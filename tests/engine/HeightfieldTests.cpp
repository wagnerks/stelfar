#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "EngineTestFixture.h"
#include "assetsModule/modelModule/Material.h"
#include "assetsModule/modelModule/Model.h"
#include "assetsModule/modelModule/Terrain.h"
#include "componentsModule/HeightfieldComponent.h"

// A heightfield as the engine has it (AssetsModule::heightfieldHeight, heightfieldModel): its height between samples is
// the triangle its mesh draws there, and its mesh is its heights, in chunks, each with the material it was given.
namespace SFE::EngineTests {
	namespace {
		// A tilted plane, y = 0.1 x + 0.2 z + 1, sampled `n` a side over `size` metres centred on the origin, rows along z,
		// as a heightfield holds its heights; with `bumps`, each sample up or down by as much as that, at random.
		std::vector<float> plane(const int n, const float size, const float bumps = 0.f) {
			std::vector<float> heights(static_cast<size_t>(n) * n);
			std::mt19937 random(11);
			std::uniform_real_distribution<float> bump(-bumps, bumps);
			const float step = size / static_cast<float>(n - 1);
			for (int j = 0; j < n; ++j) {
				for (int i = 0; i < n; ++i) {
					const float x = -size * 0.5f + static_cast<float>(i) * step;
					const float z = -size * 0.5f + static_cast<float>(j) * step;
					heights[static_cast<size_t>(j) * n + i] = 0.1f * x + 0.2f * z + 1.f + (bumps > 0.f ? bump(random) : 0.f);
				}
			}
			return heights;
		}

		// The height at x/z of the plane through three points, and whether x/z is inside the triangle they make.
		struct OnTriangle {
			bool inside = false;
			float height = 0.f;
		};
		OnTriangle onTriangle(const Math::Vec3& p0, const Math::Vec3& p1, const Math::Vec3& p2, const float x, const float z) {
			const float det = (p1.x - p0.x) * (p2.z - p0.z) - (p2.x - p0.x) * (p1.z - p0.z);
			if (std::fabs(det) < 1e-12f) {
				return {};
			}
			const float u = ((x - p0.x) * (p2.z - p0.z) - (p2.x - p0.x) * (z - p0.z)) / det;
			const float v = ((p1.x - p0.x) * (z - p0.z) - (x - p0.x) * (p1.z - p0.z)) / det;
			constexpr float kEdge = 1e-5f;
			return { u >= -kEdge && v >= -kEdge && u + v <= 1.f + kEdge, p0.y + u * (p1.y - p0.y) + v * (p2.y - p0.y) };
		}

		// FileSystem's root a folder of this test's own, outside every real one, for as long as it lives, then put back
		// and the folder gone: what a heightfield's mesh keeps on disk is made there. No frame runs meanwhile.
		class ScratchRoot {
		public:
			ScratchRoot() : mWasRoot(FileSystem::root()), mDir(std::filesystem::temp_directory_path()
				/ ("sfe_heightfield_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
				ThreadPool::instance()->waitIdle();
				std::filesystem::create_directories(mDir);
				FileSystem::setRoot(mDir.string());
			}
			~ScratchRoot() {
				ThreadPool::instance()->waitIdle();
				FileSystem::setRoot(mWasRoot.string());
				std::error_code ec;
				std::filesystem::remove_all(mDir, ec);
			}
			ScratchRoot(const ScratchRoot&) = delete;
			ScratchRoot& operator=(const ScratchRoot&) = delete;

			const std::filesystem::path& dir() const { return mDir; }

		private:
			std::filesystem::path mWasRoot;
			std::filesystem::path mDir;
		};

		// 41 x 41 samples 2 m apart, bumpy: 40 cells a side, so 2 x 2 chunks, of 32 cells and of 8.
		constexpr int kSamples = 41;
		constexpr float kSize = 80.f;
		constexpr uint64_t kKey = 0x100;
		constexpr const char* kAsset = "builtin:heightfield tests:mesh";

		const std::shared_ptr<const std::vector<float>>& bumpyHeights() {
			static const auto heights = std::make_shared<const std::vector<float>>(plane(kSamples, kSize, 0.5f));
			return heights;
		}

		ComponentsModule::HeightfieldComponent bumpyField() {
			return { kSamples, kSize, bumpyHeights(), kKey };
		}

		SFE::Material groundMaterial() {
			SFE::Material material;
			material.roughness = 0.37f;
			material.fragmentShader = AssetsModule::kTerrainFS;
			return material;
		}
	}

	// Anywhere in a cell, on either of its triangles, a plane's heights give the plane and its slopes. With one sample
	// raised: at that sample its height; at the middle of a cell's diagonal from its +x corner to its +z one, the mean
	// of those two; on either side of that diagonal the plane through that triangle's corners. Fewer than 2 samples, or
	// fewer heights than samples squared: 0.
	TEST(Heightfield, TheHeightIsTheDrawnTriangles) {
		constexpr int kN = 9;
		constexpr float kSide = 16.f;
		auto heights = plane(kN, kSide);
		const auto planeAt = [](const float x, const float z) { return 0.1f * x + 0.2f * z + 1.f; };
		for (const auto& [x, z] : { std::pair{ 0.3f, 0.4f }, std::pair{ 1.7f, 1.6f }, std::pair{ -7.9f, 7.9f }, std::pair{ 5.f, -3.f },
			std::pair{ -2.5f, -6.2f } }) {
			float slopeX = 0.f;
			float slopeZ = 0.f;
			EXPECT_NEAR(AssetsModule::heightfieldHeight(heights, kN, kSide, x, z, &slopeX, &slopeZ), planeAt(x, z), 1e-4f) << x << ", " << z;
			EXPECT_NEAR(slopeX, 0.1f, 1e-4f) << x << ", " << z;
			EXPECT_NEAR(slopeZ, 0.2f, 1e-4f) << x << ", " << z;
		}

		// The sample at the origin (4, 4) a metre up. The cell from x 0 to 2 and z -2 to 0 has it for its +z corner.
		heights[4 * kN + 4] += 1.f;
		const auto sample = [&](const int i, const int j) {
			return Math::Vec3{ -8.f + 2.f * static_cast<float>(i), heights[static_cast<size_t>(j) * kN + i], -8.f + 2.f * static_cast<float>(j) };
		};
		EXPECT_NEAR(AssetsModule::heightfieldHeight(heights, kN, kSide, 0.f, 0.f), planeAt(0.f, 0.f) + 1.f, 1e-5f);
		const auto a = sample(4, 3);
		const auto b = sample(5, 3);
		const auto c = sample(4, 4);
		const auto d = sample(5, 4);
		EXPECT_NEAR(AssetsModule::heightfieldHeight(heights, kN, kSide, 1.f, -1.f), (b.y + c.y) * 0.5f, 1e-5f);
		EXPECT_NEAR(AssetsModule::heightfieldHeight(heights, kN, kSide, 0.5f, -1.5f), onTriangle(a, b, c, 0.5f, -1.5f).height, 1e-5f);
		EXPECT_NEAR(AssetsModule::heightfieldHeight(heights, kN, kSide, 1.5f, -0.5f), onTriangle(b, d, c, 1.5f, -0.5f).height, 1e-5f);
		EXPECT_GT(AssetsModule::heightfieldHeight(heights, kN, kSide, 1.5f, -0.5f), planeAt(1.5f, -0.5f) + 0.2f) << "the raised corner's triangle";

		float slopeX = 1.f;
		float slopeZ = 1.f;
		EXPECT_EQ(AssetsModule::heightfieldHeight(heights, 1, kSide, 0.f, 0.f, &slopeX, &slopeZ), 0.f);
		EXPECT_EQ(slopeX, 0.f);
		EXPECT_EQ(slopeZ, 0.f);
		EXPECT_EQ(AssetsModule::heightfieldHeight(std::vector<float>(5), kN, kSide, 0.f, 0.f), 0.f);
	}

	// Its mesh: square chunks of kTerrainChunkCells cells, every vertex a sample where the heights put it, texCoords its
	// x/z and texCoords1.x its height, every chunk with the material given and levels of detail of its own, kept on disk
	// by the field's key with the chunk's cells in it. The model lists the chunks from the middle out, the one round the
	// middle first, so the ground nearer the camera is drawn first. Asked again for the same asset, the same model.
	TEST(Heightfield, TheMeshIsItsHeights) {
		const ScratchRoot root;
		const auto field = bumpyField();
		const auto& heights = *field.heights;
		auto* model = AssetsModule::heightfieldModel(field, groundMaterial(), kAsset);
		ASSERT_NE(model, nullptr);
		const auto& meshes = (*model->getLODs())[0].meshes;
		ASSERT_EQ(meshes.size(), 4u);
		const float step = kSize / static_cast<float>(kSamples - 1);
		const float half = kSize * 0.5f;
		size_t vertices = 0;
		float fromMiddleBefore = -1.f;
		for (const auto* chunk : meshes) {
			ASSERT_NE(chunk, nullptr);
			ASSERT_FALSE(chunk->mesh.vertices.empty());
			EXPECT_FALSE(chunk->mesh.lodIndices.empty()) << "a chunk without levels of detail";
			EXPECT_EQ(chunk->material.roughness, 0.37f);
			EXPECT_EQ(chunk->material.fragmentShader, AssetsModule::kTerrainFS);
			for (const auto& v : chunk->mesh.vertices) {
				const int i = static_cast<int>(std::lround((v.position.x + half) / step));
				const int j = static_cast<int>(std::lround((v.position.z + half) / step));
				ASSERT_TRUE(i >= 0 && i < kSamples && j >= 0 && j < kSamples) << v.position.x << ", " << v.position.z;
				EXPECT_EQ(v.position.x, -half + static_cast<float>(i) * step);
				EXPECT_EQ(v.position.z, -half + static_cast<float>(j) * step);
				EXPECT_EQ(v.position.y, heights[static_cast<size_t>(j) * kSamples + i]);
				EXPECT_EQ(v.texCoords.x, v.position.x);
				EXPECT_EQ(v.texCoords.y, v.position.z);
				EXPECT_EQ(v.texCoords1.x, v.position.y);
			}
			vertices += chunk->mesh.vertices.size();
			// Twice the chunk's middle, from its opposite corners, as the mesh orders its chunks by.
			const float x = chunk->mesh.vertices.front().position.x + chunk->mesh.vertices.back().position.x;
			const float z = chunk->mesh.vertices.front().position.z + chunk->mesh.vertices.back().position.z;
			EXPECT_GE(x * x + z * z, fromMiddleBefore) << "the chunks are not listed from the middle out";
			fromMiddleBefore = x * x + z * z;
		}
		// 33 + 9 samples a side, the row between two chunks in both.
		EXPECT_EQ(vertices, static_cast<size_t>(42 * 42));
		EXPECT_EQ(meshes.front()->mesh.vertices.front().position.x, -half) << "the chunk round the middle is not the first";
		EXPECT_EQ(meshes.front()->mesh.vertices.front().position.z, -half);
		std::error_code ec;
		EXPECT_TRUE(std::filesystem::exists(root.dir() / "cache" / "terrain_chunk_lods2_0000000000000120.bin", ec))
			<< "its levels of detail not kept by the key 0x100 ^ 32";
		EXPECT_EQ(AssetsModule::heightfieldModel(field, groundMaterial(), kAsset), model);
		EXPECT_EQ(AssetsModule::heightfieldModel({ 1, kSize, field.heights, kKey }, groundMaterial(), "builtin:heightfield tests:none"), nullptr);
	}

	// What it answers between samples is the height of the triangle its mesh draws there: the mesh splits each cell
	// along the diagonal the query does, which the colliders (TerrainCollider) and what stands on the ground go by.
	TEST(Heightfield, TheMeshIsTheHeightItAnswers) {
		const ScratchRoot root;
		const auto field = bumpyField();
		auto* model = AssetsModule::heightfieldModel(field, groundMaterial(), kAsset);
		ASSERT_NE(model, nullptr);
		const auto& meshes = (*model->getLODs())[0].meshes;
		std::mt19937 random(5);
		std::uniform_real_distribution<float> along(-kSize * 0.5f + 0.01f, kSize * 0.5f - 0.01f);
		for (int k = 0; k < 50; ++k) {
			const float x = along(random);
			const float z = along(random);
			bool found = false;
			float drawn = 0.f;
			for (const auto* chunk : meshes) {
				const auto& vertices = chunk->mesh.vertices;
				const auto& indices = chunk->mesh.indices;
				for (size_t t = 0; t + 2 < indices.size() && !found; t += 3) {
					const auto on = onTriangle(vertices[indices[t]].position, vertices[indices[t + 1]].position, vertices[indices[t + 2]].position, x, z);
					if (on.inside) {
						found = true;
						drawn = on.height;
					}
				}
			}
			ASSERT_TRUE(found) << "no triangle of the mesh under " << x << ", " << z;
			EXPECT_NEAR(AssetsModule::heightfieldHeight(*field.heights, kSamples, kSize, x, z), drawn, 1e-4f) << x << ", " << z;
		}
	}
}
