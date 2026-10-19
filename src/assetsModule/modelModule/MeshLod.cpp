#include "MeshLod.h"

#include <cmath>

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
	}

	void weldMesh(Mesh<Vertex3D>& mesh) {
		if (mesh.indices.empty() || mesh.vertices.empty()) {
			return;
		}
		// Copies of a corner apart only by float noise, a normal a hundredth of a degree off, are one vertex: they are
		// compared on their attributes rounded to steps no eye tells apart, and one copy is kept. A mesh exported a
		// triangle at a time (the camp's van) otherwise keeps three vertices a triangle, shaded three times over, and no
		// edge shared for the simplifier to collapse.
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
}
