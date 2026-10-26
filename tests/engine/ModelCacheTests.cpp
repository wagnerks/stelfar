#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "assetsModule/modelModule/ModelCache.h"
#include "assetsModule/modelModule/ModelLoader.h"

// What ModelLoader keeps of a model file comes back as it went in -- every part in its place among its siblings, its
// vertices, levels, material and extras, the armature and the animations -- and stops coming back once a file it was
// read from changes (ModelCache).
namespace SFE::ModelCacheTests {
	using namespace AssetsModule;

	constexpr const char* kPath = "models/tests/model_cache_sample.gltf";

	MeshObject3D part(const std::string& name, const int vertices) {
		MeshObject3D p;
		p.nodeName = name;
		for (int i = 0; i < vertices; ++i) {
			Vertex3D v;
			v.position = { static_cast<float>(i), static_cast<float>(i * 2), static_cast<float>(-i) };
			v.texCoords = { static_cast<float>(i) * 0.1f, static_cast<float>(i) * 0.2f };
			v.normal = { 0.f, 1.f, 0.f };
			v.boneIDs[0] = i % 3;
			v.weights[0] = 1.f;
			p.mesh.vertices.push_back(v);
		}
		for (int i = 0; i + 2 < vertices; ++i) {
			p.mesh.indices.insert(p.mesh.indices.end(), { static_cast<unsigned>(i), static_cast<unsigned>(i + 1), static_cast<unsigned>(i + 2) });
		}
		p.mesh.lodIndices = { { 0, 1, 2 }, { 0, 2, 1 } };
		p.mesh.lodErrors = { 0.25f, 0.5f };
		p.mesh.poseBounds = 3;
		p.mesh.oneSided = vertices % 2 == 1;
		p.material.name = name + " material";
		p.material.fragmentShader = "shaders/" + name + ".fs";
		p.material[DIFFUSE].path = "textures/" + name + ".png";
		p.material[NORMALS].path = "textures/" + name + "_normal.png";
		p.material[NORMALS].uvSet = 1;
		p.material.alphaCutout = true;
		p.material.roughness = 0.7f;
		p.material.baseColor = { 0.2f, 0.4f, 0.6f };
		p.extras["kind"] = name;
		p.aabb = FrustumModule::AABB(Math::Vec3{ -1.f }, Math::Vec3{ 2.f });
		return p;
	}

	// R -> { A -> { A1 }, B }: addChild puts a child in front, so the parts are walked R B A A1.
	Tree<MeshObject3D> sampleMeshes() {
		Tree<MeshObject3D> root;
		root.value.nodeName = "R";
		root.addChild(part("A", 5)).addChild(part("A1", 4));
		root.addChild(part("B", 7));
		return root;
	}

	Armature sampleArmature() {
		Armature armature;
		armature.name = "rig";
		for (uint32_t i = 0; i < 3; ++i) {
			Bone bone;
			bone.name = "bone" + std::to_string(i);
			bone.id = i;
			bone.pos = { static_cast<float>(i), 0.f, 0.f };
			bone.parentBoneIdx = i == 0 ? std::numeric_limits<uint32_t>::max() : i - 1;
			if (i < 2) {
				bone.childrenBones = { i + 1 };
			}
			armature.bones.push_back(bone);
		}
		armature.poseBoundsCount = 2;
		armature.boneRadius = { 0.1f, 0.2f, 0.3f };
		armature.boneBoundsMask = { 1u, 2u, 3u };
		return armature;
	}

	std::vector<Animation> sampleAnimations() {
		Animation walk("walk", 2.f, 30.f);
		walk.keysOf("bone0").positions.push_back({ { 1.f, 2.f, 3.f }, 0.5f });
		walk.keysOf("bone1").scales.push_back({ { 2.f, 2.f, 2.f }, 1.5f });
		KeyRotation turn{};
		turn.timeStamp = 1.f;
		walk.keysOf("bone1").rotations.push_back(turn);
		return { walk };
	}

	std::string names(const Tree<MeshObject3D>& meshes) {
		std::string order;
		for (const auto& node : meshes) {
			order += node.value.nodeName + " ";
		}
		return order;
	}

	// A file of its own outside the content, there or not as the test needs.
	std::string tempFile(const std::string& name) {
		const auto file = std::filesystem::temp_directory_path() / ("sfe_model_cache_" + name + ".txt");
		std::filesystem::remove(file);
		return file.string();
	}

	void writeText(const std::string& file, const std::string& text) {
		std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
	}

	TEST(ModelCache, ReadsBackWhatWasWritten) {
		const auto meshes = sampleMeshes();
		const auto armature = sampleArmature();
		const auto animations = sampleAnimations();
		ModelCache::write(kPath, meshes, armature, animations, {});

		Tree<MeshObject3D> readMeshes;
		Armature readArmature;
		std::vector<Animation> readAnimations;
		ASSERT_TRUE(ModelCache::read(kPath, readMeshes, readArmature, readAnimations));

		EXPECT_EQ(names(readMeshes), "R B A A1 ");
		auto was = meshes.begin();
		for (const auto& node : readMeshes) {
			const auto& a = (*was).value;
			const auto& b = node.value;
			ASSERT_EQ(a.mesh.vertices.size(), b.mesh.vertices.size()) << a.nodeName;
			EXPECT_EQ(0, std::memcmp(a.mesh.vertices.data(), b.mesh.vertices.data(), a.mesh.vertices.size() * sizeof(Vertex3D))) << a.nodeName;
			EXPECT_EQ(a.mesh.indices, b.mesh.indices) << a.nodeName;
			EXPECT_EQ(a.mesh.lodIndices, b.mesh.lodIndices) << a.nodeName;
			EXPECT_EQ(a.mesh.lodErrors, b.mesh.lodErrors) << a.nodeName;
			EXPECT_EQ(a.mesh.poseBounds, b.mesh.poseBounds) << a.nodeName;
			EXPECT_EQ(a.mesh.oneSided, b.mesh.oneSided) << a.nodeName;
			EXPECT_EQ(a.material.name, b.material.name) << a.nodeName;
			EXPECT_EQ(a.material.fragmentShader, b.material.fragmentShader) << a.nodeName;
			EXPECT_EQ(a.material.materialTextures.size(), b.material.materialTextures.size()) << a.nodeName;
			for (const auto& [type, texture] : a.material.materialTextures) {
				ASSERT_TRUE(b.material.materialTextures.contains(type)) << a.nodeName;
				EXPECT_EQ(texture.path, b.material.materialTextures.at(type).path) << a.nodeName;
				EXPECT_EQ(texture.uvSet, b.material.materialTextures.at(type).uvSet) << a.nodeName;
			}
			EXPECT_EQ(a.material.alphaCutout, b.material.alphaCutout) << a.nodeName;
			EXPECT_EQ(a.material.roughness, b.material.roughness) << a.nodeName;
			EXPECT_EQ(a.material.baseColor.z, b.material.baseColor.z) << a.nodeName;
			EXPECT_EQ(a.extras, b.extras) << a.nodeName;
			EXPECT_EQ(a.aabb.center.x, b.aabb.center.x) << a.nodeName;
			EXPECT_EQ(a.aabb.extents.y, b.aabb.extents.y) << a.nodeName;
			++was;
		}

		ASSERT_EQ(readArmature.bones.size(), armature.bones.size());
		EXPECT_EQ(readArmature.name, armature.name);
		EXPECT_EQ(readArmature.bones[2].name, "bone2");
		EXPECT_EQ(readArmature.bones[2].parentBoneIdx, 1u);
		EXPECT_EQ(readArmature.bones[1].childrenBones, armature.bones[1].childrenBones);
		EXPECT_EQ(readArmature.bones[1].pos.x, 1.f);
		EXPECT_EQ(readArmature.poseBoundsCount, armature.poseBoundsCount);
		EXPECT_EQ(readArmature.boneRadius, armature.boneRadius);
		EXPECT_EQ(readArmature.boneBoundsMask, armature.boneBoundsMask);

		ASSERT_EQ(readAnimations.size(), 1u);
		const auto& walk = readAnimations[0];
		EXPECT_EQ(walk.getName(), "walk");
		EXPECT_EQ(walk.getDuration(), 2.f);
		EXPECT_EQ(walk.getTicksPerSecond(), 30.f);
		EXPECT_EQ(walk.keys().size(), 2u);
		ASSERT_NE(walk.getBoneAnimationInfo("bone0"), nullptr);
		EXPECT_EQ(walk.getBoneAnimationInfo("bone0")->positions[0].position.z, 3.f);
		ASSERT_NE(walk.getBoneAnimationInfo("bone1"), nullptr);
		EXPECT_EQ(walk.getBoneAnimationInfo("bone1")->scales[0].timeStamp, 1.5f);
		EXPECT_EQ(walk.getBoneAnimationInfo("bone1")->rotations.size(), 1u);
	}

	TEST(ModelCache, IsNotReadOnceAFileItWasReadFromChanges) {
		const auto there = tempFile("there");
		const auto missing = tempFile("missing");
		writeText(there, "first");
		const auto meshes = sampleMeshes();
		const auto armature = sampleArmature();
		const auto animations = sampleAnimations();
		const auto keep = [&] {
			ModelCache::write(kPath, meshes, armature, animations, { ModelCache::source(there), ModelCache::source(missing) });
		};
		const auto readable = [&] {
			Tree<MeshObject3D> m;
			Armature a;
			std::vector<Animation> n;
			return ModelCache::read(kPath, m, a, n);
		};

		keep();
		EXPECT_TRUE(readable());

		writeText(there, "second, longer");
		EXPECT_FALSE(readable()) << "a file it was read from was written since";

		keep();
		EXPECT_TRUE(readable());
		writeText(missing, "now here");
		EXPECT_FALSE(readable()) << "a file it looked for and did not find is there now";

		std::filesystem::remove(there);
		std::filesystem::remove(missing);
	}

	std::string bytesOf(const std::string& file) {
		std::ifstream in(file, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	// Real files of each kind the importer reads -- an FBX and a Collada with a skeleton and animations, a glTF with its
	// buffer and PBR maps, an OBJ with its .mtl and maps beside it -- read and kept by ModelLoader, come back from the
	// cache as they were kept: written again, byte for byte the same.
	TEST(ModelCache, RealModelsComeBackAsTheyWereKept) {
		for (const std::string path : { "models/animatedPeasant.fbx", "models/dance/dancing_vampire.dae",
			"models/backpack/scene.gltf", "models/gas-tank/Gas Tank.obj" }) {
			// Read from its files, unless the cache has it already.
			ASSERT_NE(ModelLoader::instance()->load(path), nullptr) << path;
			const auto file = ModelCache::file(path);
			const auto kept = bytesOf(file);
			ASSERT_FALSE(kept.empty()) << path << " was not kept";

			Tree<MeshObject3D> meshes;
			Armature armature;
			std::vector<Animation> animations;
			std::vector<ModelCache::Source> sources;
			ASSERT_TRUE(ModelCache::read(path, meshes, armature, animations, &sources)) << path;
			EXPECT_FALSE(sources.empty()) << path << ": the files it was read from";
			ModelCache::write(path, meshes, armature, animations, sources);
			EXPECT_EQ(bytesOf(file), kept) << path;
		}
	}

	TEST(ModelCache, KeepsNoModelWithText) {
		auto meshes = sampleMeshes();
		meshes.value.extras["text"] = "hello";
		ModelCache::write(std::string(kPath) + ".text", meshes, sampleArmature(), sampleAnimations(), {});
		Tree<MeshObject3D> m;
		Armature a;
		std::vector<Animation> n;
		EXPECT_FALSE(ModelCache::read(std::string(kPath) + ".text", m, a, n));
	}
}
