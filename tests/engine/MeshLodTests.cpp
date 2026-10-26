#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "assetsModule/modelModule/MeshLod.h"

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
}
