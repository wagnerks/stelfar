#include "Terrain.h"

#include <algorithm>
#include <cstdint>
#include <execution>
#include <numeric>
#include <string>
#include <vector>

#include "assetsModule/AssetsManager.h"
#include "assetsModule/modelModule/MeshLod.h"
#include "assetsModule/modelModule/Model.h"
#include "componentsModule/HeightfieldComponent.h"
#include "core/BinaryCache.h"
#include "logsModule/logger.h"

namespace AssetsModule {
	float heightfieldHeight(const std::vector<float>& heights, const int samples, const float size, const float x, const float z, float* slopeX, float* slopeZ) {
		if (samples < 2 || heights.size() < static_cast<size_t>(samples) * static_cast<size_t>(samples)) {
			if (slopeX) {
				*slopeX = 0.f;
			}
			if (slopeZ) {
				*slopeZ = 0.f;
			}
			return 0.f;
		}
		const int n = samples;
		const float step = size / static_cast<float>(n - 1);
		const float half = size * 0.5f;
		const float gx = std::clamp((x + half) / step, 0.f, static_cast<float>(n - 1) - 1e-3f);
		const float gz = std::clamp((z + half) / step, 0.f, static_cast<float>(n - 1) - 1e-3f);
		const int i = static_cast<int>(gx);
		const int j = static_cast<int>(gz);
		const float fx = gx - static_cast<float>(i);
		const float fz = gz - static_cast<float>(j);
		const auto corner = [&](int ci, int cj) { return heights[static_cast<size_t>(cj) * n + ci]; };
		const float a = corner(i, j);
		const float b = corner(i + 1, j);
		const float c = corner(i, j + 1);
		const float d = corner(i + 1, j + 1);
		// The cell is split along its b-c diagonal, as the mesh indexes it.
		float h = 0.f;
		float dx = 0.f;
		float dz = 0.f;
		if (fx + fz <= 1.f) {
			h = a + (b - a) * fx + (c - a) * fz;
			dx = b - a;
			dz = c - a;
		}
		else {
			h = d + (c - d) * (1.f - fx) + (b - d) * (1.f - fz);
			dx = d - c;
			dz = d - b;
		}
		if (slopeX) {
			*slopeX = dx / step;
		}
		if (slopeZ) {
			*slopeZ = dz / step;
		}
		return h;
	}

	Model* heightfieldModel(const SFE::ComponentsModule::HeightfieldComponent& field, const SFE::Material& material, const std::string& assetPath) {
		auto* assets = AssetsManager::instance();
		if (auto* existing = assets->getAsset<Model>(assetPath)) {
			return existing;
		}
		const int n = field.samples;
		if (n < 2 || !field.heights || field.heights->size() < static_cast<size_t>(n) * static_cast<size_t>(n)) {
			SFE::LogsModule::Logger::LOG_ERROR("heightfieldModel: %s is no field of heights (%d samples a side, %d heights)", assetPath.c_str(), n,
				field.heights ? static_cast<int>(field.heights->size()) : 0);
			return nullptr;
		}
		const float step = field.size / static_cast<float>(n - 1);
		const float half = field.size * 0.5f;
		const auto& heights = *field.heights;
		const auto heightOf = [&](int i, int j) {
			return heights[static_cast<size_t>(std::clamp(j, 0, n - 1)) * n + std::clamp(i, 0, n - 1)];
		};

		// Square chunks of kTerrainChunkCells cells, each a mesh of its own with its own levels of detail: the scene
		// culls them one by one and picks each one's level by its own distance. One mesh for the whole terrain was
		// always drawn whole, at full detail, into the G-buffer and every shadow cascade.
		const int chunkCells = kTerrainChunkCells;
		const int chunks = (n - 1 + chunkCells - 1) / chunkCells;
		std::vector<SFE::MeshObject3D> chunkMeshes(static_cast<size_t>(chunks) * chunks);
		for (int cz = 0; cz < chunks; ++cz) {
			for (int cx = 0; cx < chunks; ++cx) {
				auto& chunk = chunkMeshes[static_cast<size_t>(cz) * chunks + cx].mesh;
				const int i0 = cx * chunkCells;
				const int j0 = cz * chunkCells;
				const int i1 = (std::min)(i0 + chunkCells, n - 1);
				const int j1 = (std::min)(j0 + chunkCells, n - 1);
				const int side = i1 - i0 + 1;
				for (int j = j0; j <= j1; ++j) {
					for (int i = i0; i <= i1; ++i) {
						const float x = -half + i * step;
						const float z = -half + j * step;
						const float y = heightOf(i, j);
						SFE::Vertex3D v{};
						v.position = { x, y, z };
						const float dx = (heightOf(i + 1, j) - heightOf(i - 1, j)) / (2.f * step);
						const float dz = (heightOf(i, j + 1) - heightOf(i, j - 1)) / (2.f * step);
						v.normal = SFE::Math::normalize(SFE::Math::Vec3{ -dx, 1.f, -dz });
						v.tangent = SFE::Math::normalize(SFE::Math::Vec3{ 1.f, dx, 0.f });
						v.biTangent = SFE::Math::cross(v.normal, v.tangent);
						v.texCoords = { x, z };
						v.texCoords1 = { y, 0.f };
						chunk.vertices.push_back(v);
					}
				}
				for (int j = 0; j < j1 - j0; ++j) {
					for (int i = 0; i < i1 - i0; ++i) {
						const unsigned a = static_cast<unsigned>(j * side + i);
						const unsigned b = a + 1;
						const unsigned c = a + static_cast<unsigned>(side);
						const unsigned d = c + 1;
						// Counter-clockwise seen from above.
						for (const unsigned idx : { a, c, b, b, c, d }) {
							chunk.indices.push_back(idx);
						}
					}
				}
			}
		}
		// The vertices are all distinct, so there is nothing to weld. Every chunk's levels together take a while, so
		// they come from the cache when they can.
		// "2": the levels' errors kept with them.
		const auto lodFile = SFE::BinaryCache::path("terrain_chunk_lods2", field.key ^ static_cast<uint64_t>(chunkCells));
		bool cached = false;
		if (SFE::BinaryCache::Reader reader(lodFile); reader) {
			for (auto& chunk : chunkMeshes) {
				uint64_t levels = 0;
				reader.value(levels);
				chunk.mesh.lodIndices.resize(static_cast<size_t>((std::min)(levels, uint64_t{ 8 })));
				for (auto& level : chunk.mesh.lodIndices) {
					reader.vector(level);
				}
				reader.vector(chunk.mesh.lodErrors);
			}
			cached = static_cast<bool>(reader);
		}
		if (!cached) {
			// Edges locked, so neighbouring chunks at different levels still meet; and a small error, since a chunk
			// is large on screen long after an object of its size would have dropped a level.
			const SFE::MeshLodOptions options{ .errorPerLevel = 0.005f, .lockBorder = true };
			std::for_each(std::execution::par, chunkMeshes.begin(), chunkMeshes.end(), [&](SFE::MeshObject3D& chunk) {
				SFE::buildMeshLods(chunk.mesh, options);
			});
			SFE::BinaryCache::Writer writer(lodFile);
			for (const auto& chunk : chunkMeshes) {
				writer.value(static_cast<uint64_t>(chunk.mesh.lodIndices.size()));
				for (const auto& level : chunk.mesh.lodIndices) {
					writer.vector(level);
				}
				writer.vector(chunk.mesh.lodErrors);
			}
			writer.commit();
		}

		// The root holds nothing; every chunk is a child with the material and its own bounds. The model lists them from
		// the middle out, the one round the middle first: the camera is mostly near a heightfield's middle, so the ground
		// nearer it goes up and is drawn first, and the depth test turns away what it hides before it is shaded.
		// Tree::addChild puts each child in front of those before it, so they are put in the other way, from the outside
		// in.
		std::vector<size_t> order(chunkMeshes.size());
		std::iota(order.begin(), order.end(), size_t{ 0 });
		const auto fromMiddle = [&](const size_t c) {
			// Twice the chunk's middle, from its first and last vertices, its opposite corners.
			const auto& vertices = chunkMeshes[c].mesh.vertices;
			const float x = vertices.front().position.x + vertices.back().position.x;
			const float z = vertices.front().position.z + vertices.back().position.z;
			return x * x + z * z;
		};
		std::stable_sort(order.begin(), order.end(), [&](const size_t a, const size_t b) { return fromMiddle(a) < fromMiddle(b); });
		SFE::Tree<SFE::MeshObject3D> tree;
		for (auto c = order.rbegin(); c != order.rend(); ++c) {
			auto& chunk = chunkMeshes[*c];
			SFE::Math::Vec3 lo = chunk.mesh.vertices.front().position;
			SFE::Math::Vec3 hi = lo;
			for (const auto& v : chunk.mesh.vertices) {
				for (int axis = 0; axis < 3; ++axis) {
					lo[axis] = (std::min)(lo[axis], v.position[axis]);
					hi[axis] = (std::max)(hi[axis], v.position[axis]);
				}
			}
			chunk.material = material;
			chunk.aabb = SFE::FrustumModule::AABB(lo, hi);
			tree.addChild(std::move(chunk));
		}
		return assets->createAsset<Model>(assetPath, std::move(tree), Armature{}, std::vector<Animation>{});
	}
}
