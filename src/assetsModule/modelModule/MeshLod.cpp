#include "MeshLod.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <meshoptimizer.h>

namespace SFE {
	namespace {
		// Each level aims for this share of the triangles of the level above.
		constexpr float kRatio = 0.5f;
		// Past this the simplifier is collapsing the shapes the silhouette is made of, and the win no longer
		// matters anyway.
		constexpr size_t kMinIndices = 96;
		// Deep enough that a piece a few pixels across draws a hundred triangles or so, not a thousand: which level is
		// drawn is its error on screen, so a level more is never seen, only cheaper when small.
		constexpr size_t kMaxLevels = 6;
		// A level has to be meaningfully cheaper than the one above to be worth an id and an instance slot.
		constexpr float kKeepBelow = 0.9f;
		// texCoords and normal sit next to each other in Vertex3D, so one pointer covers both. Collapsing on
		// position alone folds a shoulder into a sleeve and drags the texture across the seam with it, which
		// reads as a grey smear on a model at middle distance.
		constexpr size_t kAttributeCount = 5;
		constexpr float kAttributeWeights[kAttributeCount] = { 0.5f, 0.5f, 0.25f, 0.25f, 0.25f };
		// The steps a vertex's attributes are compared on when welding: a tenth of a millimetre, about a quarter of a
		// degree of a unit vector, a ten-thousandth of a texture, a 256th of a weight or a colour.
		constexpr float kWeldPosition = 1e-4f;
		constexpr float kWeldDirection = 1.f / 256.f;
		constexpr float kWeldTexCoord = 1e-4f;
		constexpr float kWeldShare = 1.f / 256.f;
		// markOneSided draws a mesh from the six axes and the eight corners of a cube round it, each view this many pixels
		// across, and lets back faces be this share of all it sees: a tin bucket's, through the open ends of its handle's
		// wire and the slit between its bottom and its wall (0.75%), not a sheet's or an open box's. Over the models of
		// a large outdoor scene, a level of detail whose thin walls crossed was seen on 254 parts; 172 of them come out
		// one-sided by this, half a percent would have left 161, and more than this gains next to none. A part that
		// comes out so changes on at most this share of its pixels at full detail.
		constexpr int kSideViewPixels = 96;
		constexpr float kSideBackShare = 0.01f;

		float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
		void cross3(const float* a, const float* b, float* out) {
			out[0] = a[1] * b[2] - a[2] * b[1];
			out[1] = a[2] * b[0] - a[0] * b[2];
			out[2] = a[0] * b[1] - a[1] * b[0];
		}
		void normalize3(float* a) {
			const float length = std::sqrt(dot3(a, a));
			if (length > 0.f) {
				for (int i = 0; i < 3; ++i) {
					a[i] /= length;
				}
			}
		}

		// Closed and facing out: with the corners at one place one vertex (the copies a seam keeps apart differ only in
		// what they carry), every edge walked twice, once each way, and the volume the surface wraps positive.
		bool closedFacingOut(const Mesh<Vertex3D>& mesh, const size_t corners) {
			std::vector<unsigned int> shadow(corners);
			meshopt_generateShadowIndexBuffer(shadow.data(), mesh.indices.data(), corners, &mesh.vertices[0].position[0],
				mesh.vertices.size(), sizeof(float) * 3, sizeof(Vertex3D));
			struct Edge {
				uint64_t key; // the lower vertex high, the higher low
				int way;      // +1 walked from the lower, -1 from the higher
			};
			std::vector<Edge> edges;
			edges.reserve(corners);
			double volume = 0.0;
			for (size_t t = 0; t < corners; t += 3) {
				const unsigned int tri[3] = { shadow[t], shadow[t + 1], shadow[t + 2] };
				if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) {
					continue;
				}
				for (int k = 0; k < 3; ++k) {
					const unsigned int a = tri[k];
					const unsigned int b = tri[(k + 1) % 3];
					edges.push_back({ (uint64_t{ (std::min)(a, b) } << 32) | (std::max)(a, b), a < b ? 1 : -1 });
				}
				const auto& p0 = mesh.vertices[tri[0]].position;
				const auto& p1 = mesh.vertices[tri[1]].position;
				const auto& p2 = mesh.vertices[tri[2]].position;
				volume += static_cast<double>(p0[0]) * (static_cast<double>(p1[1]) * p2[2] - static_cast<double>(p1[2]) * p2[1])
					- static_cast<double>(p0[1]) * (static_cast<double>(p1[0]) * p2[2] - static_cast<double>(p1[2]) * p2[0])
					+ static_cast<double>(p0[2]) * (static_cast<double>(p1[0]) * p2[1] - static_cast<double>(p1[1]) * p2[0]);
			}
			if (edges.empty() || volume <= 0.0) {
				return false;
			}
			std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) { return x.key < y.key; });
			for (size_t i = 0; i < edges.size();) {
				size_t j = i;
				int ways = 0;
				while (j < edges.size() && edges[j].key == edges[i].key) {
					ways += edges[j++].way;
				}
				if (j - i != 2 || ways != 0) {
					return false;
				}
				i = j;
			}
			return true;
		}

		// Drawn at full detail from all round it, the nearest surface a pixel: the share of what is seen that is a back face.
		float seenBackShare(const Mesh<Vertex3D>& mesh, const size_t corners) {
			float lo[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
			float hi[3] = { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
			for (const auto& v : mesh.vertices) {
				for (int i = 0; i < 3; ++i) {
					lo[i] = (std::min)(lo[i], v.position[i]);
					hi[i] = (std::max)(hi[i], v.position[i]);
				}
			}
			const float centre[3] = { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f };
			float radius = 0.f;
			for (const auto& v : mesh.vertices) {
				const float d[3] = { v.position[0] - centre[0], v.position[1] - centre[1], v.position[2] - centre[2] };
				radius = (std::max)(radius, std::sqrt(dot3(d, d)));
			}
			if (radius <= 0.f) {
				return 1.f;
			}

			constexpr int n = kSideViewPixels;
			constexpr float kViews[14][3] = {
				{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
				{ 1, 1, 1 }, { 1, 1, -1 }, { 1, -1, 1 }, { 1, -1, -1 }, { -1, 1, 1 }, { -1, 1, -1 }, { -1, -1, 1 }, { -1, -1, -1 },
			};
			std::vector<float> depth(static_cast<size_t>(n) * n);
			std::vector<int8_t> facing(static_cast<size_t>(n) * n);
			const float scale = (n * 0.5f - 1.f) / radius;
			size_t seen = 0;
			size_t back = 0;
			for (const auto& view : kViews) {
				// Looking along `dir`; `right` and `up` span the picture.
				float dir[3] = { view[0], view[1], view[2] };
				normalize3(dir);
				const float upAxis[3] = { 0.f, std::fabs(dir[1]) > 0.99f ? 0.f : 1.f, std::fabs(dir[1]) > 0.99f ? 1.f : 0.f };
				float right[3];
				float up[3];
				cross3(dir, upAxis, right);
				normalize3(right);
				cross3(right, dir, up);
				std::fill(depth.begin(), depth.end(), std::numeric_limits<float>::max());
				std::fill(facing.begin(), facing.end(), int8_t{ 0 });
				for (size_t t = 0; t < corners; t += 3) {
					const float* p[3] = { &mesh.vertices[mesh.indices[t]].position[0], &mesh.vertices[mesh.indices[t + 1]].position[0],
						&mesh.vertices[mesh.indices[t + 2]].position[0] };
					const float e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
					const float e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
					float normal[3];
					cross3(e1, e2, normal);
					const int8_t front = dot3(normal, dir) < 0.f ? 1 : -1;
					float x[3];
					float y[3];
					float z[3];
					for (int k = 0; k < 3; ++k) {
						const float d[3] = { p[k][0] - centre[0], p[k][1] - centre[1], p[k][2] - centre[2] };
						x[k] = n * 0.5f + dot3(d, right) * scale;
						y[k] = n * 0.5f - dot3(d, up) * scale;
						z[k] = dot3(d, dir);
					}
					const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
					if (std::fabs(area) < 1e-8f) {
						continue;
					}
					const int x0 = (std::max)(0, static_cast<int>(std::floor((std::min)({ x[0], x[1], x[2] }))));
					const int x1 = (std::min)(n - 1, static_cast<int>(std::ceil((std::max)({ x[0], x[1], x[2] }))));
					const int y0 = (std::max)(0, static_cast<int>(std::floor((std::min)({ y[0], y[1], y[2] }))));
					const int y1 = (std::min)(n - 1, static_cast<int>(std::ceil((std::max)({ y[0], y[1], y[2] }))));
					for (int py = y0; py <= y1; ++py) {
						for (int px = x0; px <= x1; ++px) {
							const float cx = static_cast<float>(px) + 0.5f;
							const float cy = static_cast<float>(py) + 0.5f;
							const float w0 = ((x[1] - cx) * (y[2] - cy) - (x[2] - cx) * (y[1] - cy)) / area;
							const float w1 = ((x[2] - cx) * (y[0] - cy) - (x[0] - cx) * (y[2] - cy)) / area;
							const float w2 = 1.f - w0 - w1;
							if (w0 < 0.f || w1 < 0.f || w2 < 0.f) {
								continue;
							}
							const size_t pixel = static_cast<size_t>(py) * n + px;
							const float at = w0 * z[0] + w1 * z[1] + w2 * z[2];
							if (at < depth[pixel]) {
								depth[pixel] = at;
								facing[pixel] = front;
							}
						}
					}
				}
				for (const auto f : facing) {
					seen += f != 0 ? 1u : 0u;
					back += f < 0 ? 1u : 0u;
				}
			}
			return seen ? static_cast<float>(back) / static_cast<float>(seen) : 1.f;
		}
	}

	void weldMesh(Mesh<Vertex3D>& mesh) {
		if (mesh.indices.empty() || mesh.vertices.empty()) {
			return;
		}
		// Copies of a corner apart only by float noise, a normal a hundredth of a degree off, are one vertex: they are
		// compared on their attributes rounded to steps no eye tells apart, and one copy is kept. A mesh exported a
		// triangle at a time (a car body, as some exporters write it) otherwise keeps three vertices a triangle, shaded
		// three times over, and no edge shared for the simplifier to collapse.
		const auto round = [](float& value, const float step) { value = std::round(value / step) * step + 0.f; };
		std::vector<Vertex3D> keys = mesh.vertices;
		for (auto& v : keys) {
			for (int i = 0; i < 3; ++i) {
				round(v.position[i], kWeldPosition);
				round(v.normal[i], kWeldDirection);
				round(v.tangent[i], kWeldDirection);
				round(v.biTangent[i], kWeldDirection);
			}
			for (int i = 0; i < 2; ++i) {
				round(v.texCoords[i], kWeldTexCoord);
				round(v.texCoords1[i], kWeldTexCoord);
			}
			for (int i = 0; i < 4; ++i) {
				round(v.weights[i], kWeldShare);
				round(v.color[i], kWeldShare);
			}
		}
		std::vector<unsigned int> remap(mesh.vertices.size());
		const size_t unique = meshopt_generateVertexRemap(remap.data(), mesh.indices.data(), mesh.indices.size(),
			keys.data(), keys.size(), sizeof(Vertex3D));
		if (unique >= mesh.vertices.size()) {
			return;
		}
		std::vector<unsigned int> indices(mesh.indices.size());
		meshopt_remapIndexBuffer(indices.data(), mesh.indices.data(), mesh.indices.size(), remap.data());
		std::vector<Vertex3D> vertices(unique);
		meshopt_remapVertexBuffer(vertices.data(), mesh.vertices.data(), mesh.vertices.size(), sizeof(Vertex3D), remap.data());
		mesh.indices = std::move(indices);
		mesh.vertices = std::move(vertices);
	}

	void buildMeshLods(Mesh<Vertex3D>& mesh, const MeshLodOptions& options) {
		mesh.lodIndices.clear();
		mesh.lodErrors.clear();
		if (mesh.indices.size() < kMinIndices * 2 || mesh.vertices.empty()) {
			return;
		}

		std::vector<unsigned int> source = mesh.indices;
		// Each level is simplified from the one before, so its error from the full mesh is at most the sum of the steps'.
		float total = 0.f;
		for (size_t level = 0; level < kMaxLevels; ++level) {
			const size_t target = static_cast<size_t>(static_cast<float>(source.size()) * kRatio) / 3u * 3u;
			if (target < kMinIndices) {
				break;
			}
			std::vector<unsigned int> simplified(source.size());
			float error = 0.f;
			const size_t count = meshopt_simplifyWithAttributes(simplified.data(), source.data(), source.size(),
				&mesh.vertices[0].position[0], mesh.vertices.size(), sizeof(Vertex3D),
				&mesh.vertices[0].texCoords[0], sizeof(Vertex3D), kAttributeWeights, kAttributeCount, nullptr,
				target, options.errorPerLevel * static_cast<float>(1u << level), options.lockBorder ? meshopt_SimplifyLockBorder : 0, &error);
			// It stops early on a mesh it cannot collapse further without breaking the surface. Whatever it
			// did reach is the last level: another one would be the same triangles under a different id.
			if (count == 0 || static_cast<float>(count) > static_cast<float>(source.size()) * kKeepBelow) {
				break;
			}
			simplified.resize(count);
			source = simplified;
			mesh.lodIndices.push_back(std::move(simplified));
			total += error;
			mesh.lodErrors.push_back(total);
		}
	}

	void markOneSided(Mesh<Vertex3D>& mesh) {
		mesh.oneSided = false;
		const size_t corners = mesh.indices.size() / 3 * 3;
		if (corners == 0 || mesh.vertices.empty()) {
			return;
		}
		if (closedFacingOut(mesh, corners)) {
			mesh.oneSided = true;
			return;
		}
		// A skinned mesh takes other shapes than this one, and what is seen of this one says nothing of those.
		for (const auto& v : mesh.vertices) {
			if (v.boneIDs[0] >= 0) {
				return;
			}
		}
		mesh.oneSided = seenBackShare(mesh, corners) <= kSideBackShare;
	}
}
