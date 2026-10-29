#include <cmath>
#include <regex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "assetsModule/modelModule/MeshLod.h"
#include "assetsModule/modelModule/MeshVaoRegistry.h"
#include "core/FileSystem.h"
#include "renderModule/LodRule.h"

// Whether a mesh's back side is never seen (markOneSided), as GeometryPass and the game's litter then cull its back
// faces: a closed surface facing out is, however its corners are split; a rigid one with a thin open tube on it, as a
// bucket with its handle's wire, is too; a sheet, an open box, one facing in, or the tubed one skinned are not.
namespace SFE::MeshLodTests {
	struct Builder {
		Mesh<Vertex3D> mesh;

		unsigned add(const float x, const float y, const float z) {
			Vertex3D v;
			v.position = { x, y, z };
			mesh.vertices.push_back(v);
			return static_cast<unsigned>(mesh.vertices.size() - 1);
		}
		void triangle(const unsigned a, const unsigned b, const unsigned c) { mesh.indices.insert(mesh.indices.end(), { a, b, c }); }
		// Counter-clockwise seen from where its front faces.
		void quad(const unsigned a, const unsigned b, const unsigned c, const unsigned d) {
			triangle(a, b, c);
			triangle(a, c, d);
		}
	};

	// A unit cube about `at`, its faces facing out; each face its own four corners when `split`, as an exporter leaves
	// them at sharp edges. `top` false leaves the top open.
	void cube(Builder& b, const float at[3], const bool split, const bool top = true) {
		const float h = 0.5f;
		unsigned shared[8];
		for (int i = 0; i < 8; ++i) {
			shared[i] = split ? 0 : b.add(at[0] + (i & 1 ? h : -h), at[1] + (i & 2 ? h : -h), at[2] + (i & 4 ? h : -h));
		}
		// Each face's corners, counter-clockwise from outside: bit 0 x, bit 1 y, bit 2 z.
		const int faces[6][4] = { { 1, 3, 7, 5 }, { 0, 4, 6, 2 }, { 2, 6, 7, 3 }, { 0, 1, 5, 4 }, { 4, 5, 7, 6 }, { 0, 2, 3, 1 } };
		for (int f = 0; f < 6; ++f) {
			if (!top && f == 2) {
				continue;
			}
			unsigned corner[4];
			for (int k = 0; k < 4; ++k) {
				const int i = faces[f][k];
				corner[k] = split ? b.add(at[0] + (i & 1 ? h : -h), at[1] + (i & 2 ? h : -h), at[2] + (i & 4 ? h : -h)) : shared[i];
			}
			b.quad(corner[0], corner[1], corner[2], corner[3]);
		}
	}

	// A thin tube along x from `from`, open at both ends, its outside facing out.
	void tube(Builder& b, const float from[3], const float length, const float radius) {
		constexpr int kSides = 8;
		unsigned ring[2][kSides];
		for (int end = 0; end < 2; ++end) {
			for (int s = 0; s < kSides; ++s) {
				const float angle = 2.f * 3.14159265f * static_cast<float>(s) / kSides;
				ring[end][s] = b.add(from[0] + length * static_cast<float>(end), from[1] + radius * std::cos(angle), from[2] + radius * std::sin(angle));
			}
		}
		for (int s = 0; s < kSides; ++s) {
			const int t = (s + 1) % kSides;
			b.quad(ring[0][s], ring[0][t], ring[1][t], ring[1][s]);
		}
	}

	void reverse(Mesh<Vertex3D>& mesh) {
		for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
			std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
		}
	}

	constexpr float kOrigin[3] = { 0.f, 0.f, 0.f };

	TEST(MeshLod, AClosedSurfaceFacingOutIsOneSided) {
		Builder shared;
		cube(shared, kOrigin, false);
		markOneSided(shared.mesh);
		EXPECT_TRUE(shared.mesh.oneSided);

		Builder split;
		cube(split, kOrigin, true);
		markOneSided(split.mesh);
		EXPECT_TRUE(split.mesh.oneSided);
	}

	TEST(MeshLod, ASurfaceFacingInIsNot) {
		Builder b;
		cube(b, kOrigin, true);
		reverse(b.mesh);
		markOneSided(b.mesh);
		EXPECT_FALSE(b.mesh.oneSided);
	}

	TEST(MeshLod, ASheetIsNot) {
		Builder b;
		b.quad(b.add(-0.5f, 0.f, -0.5f), b.add(-0.5f, 0.f, 0.5f), b.add(0.5f, 0.f, 0.5f), b.add(0.5f, 0.f, -0.5f));
		markOneSided(b.mesh);
		EXPECT_FALSE(b.mesh.oneSided);
	}

	TEST(MeshLod, AnOpenBoxIsNot) {
		Builder b;
		cube(b, kOrigin, true, false);
		markOneSided(b.mesh);
		EXPECT_FALSE(b.mesh.oneSided);
	}

	TEST(MeshLod, AClosedSurfaceWithAThinOpenTubeIsOneSidedWhileRigid) {
		Builder b;
		cube(b, kOrigin, true);
		const float handle[3] = { -0.4f, 0.7f, 0.f };
		tube(b, handle, 0.8f, 0.01f);
		markOneSided(b.mesh);
		EXPECT_TRUE(b.mesh.oneSided);

		for (auto& v : b.mesh.vertices) {
			v.boneIDs[0] = 0;
			v.weights[0] = 1.f;
		}
		markOneSided(b.mesh);
		EXPECT_FALSE(b.mesh.oneSided);
	}

	// Which level of a chain is drawn (shaders/sfe_lod.glsl, the scene's culls' and through LodRule.h a pass's of its own
	// instances): the coarsest whose error is within the allowed share of the mesh's extent, and that share growing with
	// the distance and the bias, never finer than a shadow map's texels show.
	constexpr float kErrors[GpuMesh::kMaxLodErrors] = { 0.01f, 0.02f, 0.04f, 0.08f, 0.16f, 0.32f };

	TEST(LodRule, TheCoarsestLevelWithinTheAllowedError) {
		using Render::LodRule::sfeLodLevel;
		EXPECT_EQ(sfeLodLevel(kErrors, 7u, 0.005f), 0u) << "under every level's error";
		EXPECT_EQ(sfeLodLevel(kErrors, 7u, 0.02f), 2u) << "an error equal to the allowed one is within it";
		EXPECT_EQ(sfeLodLevel(kErrors, 7u, 1.f), 6u);
		EXPECT_EQ(sfeLodLevel(kErrors, 3u, 1.f), 2u) << "past the end of a shorter chain";
		EXPECT_EQ(sfeLodLevel(kErrors, 1u, 1.f), 0u) << "a mesh with no coarser level";
	}

	TEST(LodRule, AllowedGrowsWithDistanceAndDoublesPerBias) {
		using Render::LodRule::sfeLodAllowed;
		constexpr float kPixels = 2.f;
		constexpr float kPerUnit = 1000.f;
		// 2 pixels of a mesh 1 m across seen from 10 m, 100 pixels: a share of 0.02.
		const float atTen = sfeLodAllowed(kPixels, 0u, 0.5f, kPerUnit, 10.f, 0.f);
		EXPECT_FLOAT_EQ(atTen, 0.02f);
		EXPECT_FLOAT_EQ(sfeLodAllowed(kPixels, 0u, 0.5f, kPerUnit, 20.f, 0.f), 2.f * atTen) << "twice as far, twice the share";
		EXPECT_FLOAT_EQ(sfeLodAllowed(kPixels, 1u, 0.5f, kPerUnit, 10.f, 0.f), 2.f * atTen) << "a step of bias doubles the pixels";
		EXPECT_FLOAT_EQ(sfeLodAllowed(kPixels, 0u, 1.f, kPerUnit, 10.f, 0.f), 0.5f * atTen) << "twice the size, half the share";
		// A cascade of a texel a metre shows the mesh 1 texel across, coarser than its 100 pixels; one of 1e5 a metre
		// finer, which leaves the pixels' share.
		EXPECT_FLOAT_EQ(sfeLodAllowed(kPixels, 0u, 0.5f, kPerUnit, 10.f, 1.f), 2.f) << "finer than the map's texels show";
		EXPECT_FLOAT_EQ(sfeLodAllowed(kPixels, 0u, 0.5f, kPerUnit, 10.f, 1e5f), atTen);
		const float atTheEye = sfeLodAllowed(kPixels, 0u, 0.5f, kPerUnit, 0.f, 0.f);
		EXPECT_TRUE(std::isfinite(atTheEye)) << "a distance of 0 divides by 0";
		EXPECT_GT(atTheEye, 0.f);
		EXPECT_LT(atTheEye, atTen);
	}

	// The scene's cull takes the rule instead of a copy of its own, with GpuMesh's errors as they are.
	TEST(LodRule, TheCullShaderTakesTheRule) {
		std::string cull;
		ASSERT_TRUE(FileSystem::readFile("shaders/gpu_cull.cs", cull));
		EXPECT_NE(cull.find("#include \"sfe_lod.glsl\""), std::string::npos);
		EXPECT_NE(cull.find("sfeLodAllowed("), std::string::npos);
		EXPECT_NE(cull.find("sfeLodLevel("), std::string::npos);
		EXPECT_EQ(cull.find("lodErrors["), std::string::npos) << "gpu_cull.cs picks a level by a rule of its own";

		std::string rule;
		ASSERT_TRUE(FileSystem::readFile("shaders/sfe_lod.glsl", rule));
		std::smatch errors;
		ASSERT_TRUE(std::regex_search(rule, errors, std::regex(R"(float errors\[(\d+)\])")));
		EXPECT_EQ(std::stoul(errors[1].str()), GpuMesh::kMaxLodErrors) << "sfe_lod.glsl's errors are not GpuMesh::lodErrors";
	}
}
