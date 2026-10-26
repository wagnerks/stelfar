#include "ModelLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/GltfMaterial.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "Animation.h"
#include "MeshUtils.h"
#include "MeshLod.h"
#include "MeshVaoRegistry.h"
#include "ModelCache.h"
#include "ModelRelease.h"
#include "ModelText.h"
#include "logsModule/logger.h"
#include "assetsModule/TextureHandler.h"
#include "componentsModule/ArmatureComponent.h"
#include "core/Engine.h"
#include "core/FileSystem.h"
#include "core/LoadTrace.h"
#include "multithreading/ThreadPool.h"
#include "mathModule/VectorOperations.h"

#include <algorithm>
#include <unordered_map>
#include <functional>
#include <set>
#include <execution>
#include <memory>
#include <numeric>
#include <vector>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_set>

using namespace AssetsModule;

// What releaseModel let go of, under a lock of its own: one never held while waiting for the device thread, which
// ModelLoader::mtx is (publish binds meshes under it), so the device thread may take it.
namespace {
	std::mutex gResidencyMutex;
	// How many loaded models use each texture, the textures each model is a user of (counted), and the models whose
	// textures were let go of.
	std::unordered_map<Texture*, int> gUsers;
	std::unordered_map<const Model*, std::vector<Texture*>> gHeld;
	std::unordered_set<const Model*> gReleased;
	// Set on the thread loading models without their textures (loadAllReleased): how many texels across at most the
	// copies they are drawn with meanwhile are (distantTexture).
	thread_local int tDistantTexels = 0;

	// The parts of a model, or of one being loaded, as Model::getLODs has them: those with vertices.
	std::vector<SFE::MeshObject3D*> partsOf(Model& model) {
		std::vector<SFE::MeshObject3D*> parts;
		for (auto& lod : *model.getLODs()) {
			parts.insert(parts.end(), lod.meshes.begin(), lod.meshes.end());
		}
		return parts;
	}

	std::vector<SFE::MeshObject3D*> partsOf(SFE::Tree<SFE::MeshObject3D>& meshes) {
		std::vector<SFE::MeshObject3D*> parts;
		for (auto& node : meshes) {
			if (!node.value.mesh.vertices.empty()) {
				parts.push_back(&node.value);
			}
		}
		return parts;
	}

	bool released(const Model* model) {
		std::lock_guard lock(gResidencyMutex);
		return gReleased.contains(model);
	}

	// The textures read from files that the parts' materials name, each once, held: one more user of each -- first those
	// loaded already, so that no model let go of meanwhile takes one away -- then each one let go of read again
	// (TextureHandler::loadTexture reads it back), or read for the first time and held too, and the materials pointed at
	// them. So a model never keeps a texture let go of, whether it was itself let go of or was loading while another one
	// was, and lets go of every one it read when it is let go of, a model from the cache too, whose textures are first
	// read here.
	std::vector<Texture*> hold(const std::vector<SFE::MeshObject3D*>& parts) {
		std::vector<Texture*> textures;
		for (const auto* part : parts) {
			for (const auto& [type, slot] : part->material.materialTextures) {
				auto* texture = slot.path.empty() ? nullptr : AssetsManager::instance()->getAsset<Texture>(slot.path);
				if (texture && std::find(textures.begin(), textures.end(), texture) == textures.end()) {
					textures.push_back(texture);
				}
			}
		}
		{
			std::lock_guard lock(gResidencyMutex);
			for (auto* texture : textures) {
				++gUsers[texture];
			}
		}
		const auto* none = &TextureHandler::instance()->mDefaultTex;
		for (auto* part : parts) {
			for (auto& [type, slot] : part->material.materialTextures) {
				if (slot.path.empty()) {
					continue;
				}
				auto* texture = TextureHandler::loadTexture(slot.path, false, TextureStorage::Compressed);
				slot.texture = texture->texture;
				if (texture != none && std::find(textures.begin(), textures.end(), texture) == textures.end()) {
					std::lock_guard lock(gResidencyMutex);
					++gUsers[texture];
					textures.push_back(texture);
				}
			}
		}
		return textures;
	}

	// The parts' materials pointed at copies of their textures with only the levels `most` texels across or smaller
	// (distantTexture), which are never let go of: what a model whose own were let go of is drawn with.
	void pointAtDistant(const std::vector<SFE::MeshObject3D*>& parts, const int most) {
		for (auto* part : parts) {
			for (auto& [type, slot] : part->material.materialTextures) {
				if (!slot.path.empty()) {
					slot.texture = distantTexture(slot.path, most)->texture;
				}
			}
		}
	}

	// A model whose textures were held (hold): their user, till releaseModel lets go of them.
	void counted(const Model& model, std::vector<Texture*> textures) {
		std::lock_guard lock(gResidencyMutex);
		gReleased.erase(&model);
		gHeld[&model] = std::move(textures);
	}

	// A model published without its textures (loadAllReleased): as releaseModel leaves one, so that its next load reads
	// them. Those put on the device as it was read from its file rather than its cache, which no counted model uses, let
	// go of again.
	void uncounted(Model& model) {
		std::vector<Texture*> textures;
		for (const auto* part : partsOf(model)) {
			for (const auto& [type, slot] : part->material.materialTextures) {
				auto* texture = slot.path.empty() ? nullptr : AssetsManager::instance()->getAsset<Texture>(slot.path);
				if (texture && std::find(textures.begin(), textures.end(), texture) == textures.end()) {
					textures.push_back(texture);
				}
			}
		}
		std::lock_guard lock(gResidencyMutex);
		gReleased.insert(&model);
		for (auto* texture : textures) {
			if (gUsers[texture] <= 0) {
				releaseTexture(*texture);
			}
		}
	}
}

namespace {
	// A glTF names its files as URIs: a space in the name as "%20", "(" as "%28".
	std::string uriDecoded(const std::string& uri) {
		std::string text;
		for (size_t i = 0; i < uri.size(); ++i) {
			if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1]))
				&& std::isxdigit(static_cast<unsigned char>(uri[i + 2]))) {
				text += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
				i += 2;
			}
			else {
				text += uri[i];
			}
		}
		return text;
	}

	bool meshHasSkin(const SFE::MeshObject3D& mesh) {
		for (const auto& v : mesh.mesh.vertices) {
			if (v.boneIDs[0] >= 0) {
				return true;
			}
		}
		return false;
	}

	std::string toLowerCopy(std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return s;
	}

	bool looksLikeHair(const std::string& lowerName) {
		static const char* kKeys[] = { "fur", "hair", "whisker", "eyelash", "feather" };
		for (const char* key : kKeys) {
			if (lowerName.find(key) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	bool looksLikeCutout(const std::string& lowerName) {
		if (looksLikeHair(lowerName)) {
			return true;
		}
		static const char* kKeys[] = { "leaf", "leaves", "grass", "foliage", "decal" };
		for (const char* key : kKeys) {
			if (lowerName.find(key) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	bool looksLikeGlass(const std::string& lowerName) {
		return lowerName.find("glass") != std::string::npos
			|| lowerName.find("window") != std::string::npos
			|| lowerName.find("transparent") != std::string::npos
			|| lowerName.find("water") != std::string::npos;
	}

	bool looksLikeEye(const std::string& lowerName) {
		if (lowerName.find("eyelash") != std::string::npos
			|| lowerName.find("eyelid") != std::string::npos
			|| lowerName.find("eyebrow") != std::string::npos
			|| lowerName.find("eyelbrow") != std::string::npos) {
			return false;
		}
		return lowerName.find("eyeball") != std::string::npos
			|| lowerName.find("iris") != std::string::npos
			|| lowerName.find("cornea") != std::string::npos
			|| lowerName.find("pupil") != std::string::npos
			|| lowerName.find("sclera") != std::string::npos
			|| lowerName.find("lens") != std::string::npos
			|| lowerName.find("eye") != std::string::npos;
	}

	bool looksLikeMetalnessMap(const Texture* tex) {
		if (!tex || !tex->isValid()) {
			return false;
		}
		// Real metalness is ~0/1. Mid-gray "metal" sidecars are gloss/AO/cavity.
		return tex->fgMean <= 0.12f || tex->fgMean >= 0.85f;
	}

	bool isCardMesh(const SFE::Mesh3D& mesh) {
		if (mesh.vertices.size() < 3) {
			return false;
		}
		SFE::Math::Vec3 mn(1e30f);
		SFE::Math::Vec3 mx(-1e30f);
		for (const auto& v : mesh.vertices) {
			mn.x = (std::min)(mn.x, v.position.x);
			mn.y = (std::min)(mn.y, v.position.y);
			mn.z = (std::min)(mn.z, v.position.z);
			mx.x = (std::max)(mx.x, v.position.x);
			mx.y = (std::max)(mx.y, v.position.y);
			mx.z = (std::max)(mx.z, v.position.z);
		}
		const float ex = mx.x - mn.x;
		const float ey = mx.y - mn.y;
		const float ez = mx.z - mn.z;
		const float thin = (std::min)(ex, (std::min)(ey, ez));
		const float wide = (std::max)(ex, (std::max)(ey, ez));
		if (wide > 1e-5f && thin / wide < 0.045f) {
			return true;
		}
		return false;
	}

	void bakeRigidMesh(SFE::MeshObject3D& mesh) {
		const auto t = mesh.transform;
		if (t == SFE::Math::Mat4(1.f)) {
			return;
		}
		for (auto& vertex : mesh.mesh.vertices) {
			vertex.position = t * vertex.position;
			const auto n = t * SFE::Math::Vec4(vertex.normal, 0.f);
			const auto tan = t * SFE::Math::Vec4(vertex.tangent, 0.f);
			const auto bit = t * SFE::Math::Vec4(vertex.biTangent, 0.f);
			vertex.normal = SFE::Math::normalize(SFE::Math::Vec3(n.x, n.y, n.z));
			vertex.tangent = { tan.x, tan.y, tan.z };
			vertex.biTangent = { bit.x, bit.y, bit.z };
		}
		mesh.transform = SFE::Math::Mat4(1.f);
	}

	void bindUnskinnedMeshes(SFE::Tree<SFE::MeshObject3D>& meshes, const Armature& armature) {
		std::map<std::string, const Bone*> byName;
		for (const auto& bone : armature.bones) {
			byName[bone.name] = &bone;
		}
		const SFE::Math::Mat4 identity(1.f);

		for (auto& node : meshes) {
			auto& mesh = node.value;
			if (mesh.mesh.vertices.empty() || meshHasSkin(mesh)) {
				continue;
			}

			const bool needBake = mesh.transform != identity;
			if (needBake) {
				bakeRigidMesh(mesh);
			}

			const Bone* nearest = nullptr;
			const Bone* deforming = nullptr;
			for (auto* p = &node; p; p = p->parent) {
				const auto it = byName.find(p->value.nodeName);
				if (it == byName.end()) {
					continue;
				}
				if (!nearest) {
					nearest = it->second;
				}
				if (it->second->offset != identity && !deforming) {
					deforming = it->second;
				}
			}
			const Bone* chosen = needBake ? (deforming ? deforming : nearest) : nearest;
			if (chosen) {
				for (auto& v : mesh.mesh.vertices) {
					v.boneIDs[0] = static_cast<int>(chosen->id);
					v.weights[0] = 1.f;
				}
			}
		}
	}
}

SFE::Math::Mat4 ModelLoader::assimpMatToMat4(const aiMatrix4x4& from) {
	SFE::Math::Mat4 to;

	to[0][0] = from.a1; to[1][0] = from.a2; to[2][0] = from.a3; to[3][0] = from.a4;
	to[0][1] = from.b1; to[1][1] = from.b2; to[2][1] = from.b3; to[3][1] = from.b4;
	to[0][2] = from.c1; to[1][2] = from.c2; to[2][2] = from.c3; to[3][2] = from.c4;
	to[0][3] = from.d1; to[1][3] = from.d2; to[2][3] = from.d3; to[3][3] = from.d4;

	return to;
}

namespace {
	const aiScene* readScene(Assimp::Importer& import, const std::string& path) {
		import.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
		const unsigned flags = aiProcess_Triangulate | aiProcess_FlipUVs
			| aiProcess_GenSmoothNormals | aiProcess_LimitBoneWeights;
		const aiScene* scene = import.ReadFile(SFE::FileSystem::resolve(path).string(), flags);
		if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
			SFE::LogsModule::Logger::LOG_ERROR("ASSIMP:: %s", import.GetErrorString());
			return nullptr;
		}
		return scene;
	}

	// Where the files read() looks at on this thread go while it reads a model for its cache (fileThere).
	thread_local std::vector<ModelCache::Source>* tSources = nullptr;

	// Whether a file is there; written down for the model's cache while one is being read (tSources).
	bool fileThere(const std::string& path) {
		if (tSources) {
			tSources->push_back(ModelCache::source(path));
		}
		return SFE::FileSystem::isFileExists(path);
	}

	// Every texture file the parts' materials name, to decode ahead, side by side: publish's hold reads them.
	std::vector<std::string> materialFiles(SFE::Tree<SFE::MeshObject3D>& meshes) {
		std::vector<std::string> files;
		for (const auto* part : partsOf(meshes)) {
			for (const auto& [type, slot] : part->material.materialTextures) {
				if (!slot.path.empty()) {
					files.push_back(slot.path);
				}
			}
		}
		return files;
	}
}

AssetsModule::Model* ModelLoader::load(const std::string& path) {
	SFE::LoadTrace::Step step("models");
	auto asset = AssetsManager::instance()->getAsset<Model>(path);
	if (asset && !released(asset)) {
		return asset;
	}

	std::unique_lock lock(mtx);
	if (loading.contains(path)) {
		// Being loaded on another thread: waited for under the lock, so its publish cannot slip in between and leave
		// this waiting for good. That thread waits on the main one for what goes to the device (SYNC tasks), so the main
		// thread does those while it waits.
		const bool main = SFE::Engine::isMainThread();
		for (auto found = loading.find(path); found != loading.end(); found = loading.find(path)) {
			if (!main) {
				found->second.wait(lock);
				continue;
			}
			found->second.wait_for(lock, std::chrono::milliseconds(1));
			if (loading.contains(path)) {
				lock.unlock();
				SFE::ThreadPool::instance()->syncUpdate();
				lock.lock();
			}
		}
		return AssetsManager::instance()->getAsset<Model>(path);
	}
	// Published, or read back, between the look above and the lock.
	if (asset = AssetsManager::instance()->getAsset<Model>(path); asset && !released(asset)) {
		return asset;
	}

	loading[path];
	lock.unlock();

	// One whose textures were let go of (releaseModel): read again, as a load reads them, before anyone draws it.
	if (asset) {
		counted(*asset, hold(partsOf(*asset)));
		lock.lock();
		loading[path].notify_all();
		loading.erase(path);
		return asset;
	}

	// Its textures decoded side by side first, as loadAll does: a character's are several large ones, and one after
	// another they were most of its load. Kept till it is published, whose hold reads those a cached one names.
	TextureHandler::Predecoded ahead;
	Loaded loaded;
	{
		SFE::LoadTrace::Step cached("model cache");
		loaded.ok = ModelCache::read(path, loaded.meshes, loaded.armature, loaded.animations);
	}
	if (loaded.ok) {
		SFE::LoadTrace::Step decoding("textures");
		ahead = TextureHandler::predecode(materialFiles(loaded.meshes));
	}
	else {
		loaded = Loaded{};
		std::vector<ModelCache::Source> sources;
		Assimp::Importer import;
		ModelCache::watch(import, sources);
		const aiScene* scene = nullptr;
		{
			SFE::LoadTrace::Step files("model files");
			scene = readScene(import, path);
		}
		if (scene) {
			SFE::LoadTrace::Step decoding("textures");
			ahead = TextureHandler::predecode(textureFiles(scene, path));
		}
		{
			SFE::LoadTrace::Step parts("model parts");
			tSources = &sources;
			loaded = read(path, scene);
			tSources = nullptr;
		}
		{
			SFE::LoadTrace::Step levels("model levels");
			finishMeshes(loaded);
		}
		if (loaded.ok) {
			SFE::LoadTrace::Step cached("model cache");
			ModelCache::write(path, loaded.meshes, loaded.armature, loaded.animations, sources);
		}
	}
	SFE::LoadTrace::Step upload("model upload");
	return publish(path, std::move(loaded));
}

void ModelLoader::loadAll(const std::vector<std::string>& paths) {
	SFE::LoadTrace::Step step("models");
	std::vector<std::string> pending;
	{
		std::lock_guard lock(mtx);
		for (const auto& path : paths) {
			if (!AssetsManager::instance()->getAsset<Model>(path) && !loading.contains(path)
				&& std::find(pending.begin(), pending.end(), path) == pending.end()) {
				loading[path];
				pending.push_back(path);
			}
		}
	}
	if (pending.empty()) {
		return;
	}

	// Everything CPU only is done side by side: reading the caches and the files, decoding their textures, the tangents
	// and levels of detail. What reaches the device stays on this thread.
	std::vector<size_t> order(pending.size());
	std::iota(order.begin(), order.end(), size_t{ 0 });
	std::vector<Loaded> loaded(pending.size());
	{
		SFE::LoadTrace::Step cached("model cache");
		std::for_each(std::execution::par, order.begin(), order.end(), [&](const size_t i) {
			if (!(loaded[i].ok = ModelCache::read(pending[i], loaded[i].meshes, loaded[i].armature, loaded[i].animations))) {
				loaded[i] = Loaded{};
			}
		});
	}
	// The rest read from their files, and their caches made.
	std::vector<char> fresh(pending.size());
	for (size_t i = 0; i < pending.size(); ++i) {
		fresh[i] = !loaded[i].ok;
	}
	std::vector<std::vector<ModelCache::Source>> sources(pending.size());
	std::vector<std::unique_ptr<Assimp::Importer>> importers(pending.size());
	std::vector<const aiScene*> scenes(pending.size(), nullptr);
	{
		SFE::LoadTrace::Step files("model files");
		std::for_each(std::execution::par, order.begin(), order.end(), [&](const size_t i) {
			if (fresh[i]) {
				importers[i] = std::make_unique<Assimp::Importer>();
				ModelCache::watch(*importers[i], sources[i]);
				scenes[i] = readScene(*importers[i], pending[i]);
			}
		});
	}

	std::vector<std::string> textures;
	for (size_t i = 0; i < pending.size(); ++i) {
		const auto files = scenes[i] ? textureFiles(scenes[i], pending[i]) : materialFiles(loaded[i].meshes);
		textures.insert(textures.end(), files.begin(), files.end());
	}
	// Kept till the models are published, whose hold reads those the cached ones name.
	TextureHandler::Predecoded ahead;
	{
		SFE::LoadTrace::Step decoding("textures");
		SFE::LoadTrace::count("texture files", static_cast<long long>(textures.size()));
		if (!tDistantTexels) {
			ahead = TextureHandler::predecode(textures);
		}
	}
	{
		SFE::LoadTrace::Step parts("model parts");
		for (size_t i = 0; i < pending.size(); ++i) {
			if (fresh[i]) {
				tSources = &sources[i];
				loaded[i] = read(pending[i], scenes[i]);
				tSources = nullptr;
				importers[i].reset();
			}
		}
	}
	{
		SFE::LoadTrace::Step levels("model levels");
		std::for_each(std::execution::par, order.begin(), order.end(), [&](const size_t i) {
			if (fresh[i]) {
				finishMeshes(loaded[i]);
			}
		});
	}
	{
		SFE::LoadTrace::Step cached("model cache");
		std::for_each(std::execution::par, order.begin(), order.end(), [&](const size_t i) {
			if (fresh[i] && loaded[i].ok) {
				ModelCache::write(pending[i], loaded[i].meshes, loaded[i].armature, loaded[i].animations, sources[i]);
			}
		});
	}
	SFE::LoadTrace::Step upload("model upload");
	for (size_t i = 0; i < pending.size(); ++i) {
		publish(pending[i], std::move(loaded[i]));
	}
}

ModelLoader::Loaded ModelLoader::read(const std::string& path, const aiScene* scene) {
	Loaded loaded;
	if (!scene) {
		return loaded;
	}
	auto model = loadModel(scene, SFE::FileSystem::resolve(path).string());
	loaded.meshes = std::move(std::get<0>(model));
	loaded.armature = std::move(std::get<1>(model));
	loaded.animations.reserve(scene->mNumAnimations);
	for (auto i = 0u; i < scene->mNumAnimations; i++) {
		loaded.animations.emplace_back(scene->mAnimations[i]);
	}
	loaded.ok = true;
	return loaded;
}

std::vector<std::string> ModelLoader::textureFiles(const aiScene* scene, const std::string& path) {
	std::vector<std::string> textures;
	const auto directory = SFE::FileSystem::resolve(path).parent_path().string();
	for (unsigned m = 0; m < scene->mNumMaterials; ++m) {
		const auto* material = scene->mMaterials[m];
		for (int type = aiTextureType_NONE; type <= AI_TEXTURE_TYPE_MAX; ++type) {
			for (unsigned t = 0; t < material->GetTextureCount(static_cast<aiTextureType>(type)); ++t) {
				aiString raw;
				material->GetTexture(static_cast<aiTextureType>(type), t, &raw);
				if (raw.length == 0 || raw.C_Str()[0] == '*') {
					continue;
				}
				for (const auto& candidate : textureFileCandidates(raw.C_Str(), directory)) {
					if (SFE::FileSystem::isFileExists(candidate)) {
						textures.push_back(candidate);
						break;
					}
				}
			}
		}
	}
	return textures;
}

// Mesh by mesh side by side: a character is two dozen parts, each welded and given its levels of detail on its own.
void ModelLoader::finishMeshes(Loaded& loaded) {
	std::vector<SFE::MeshObject3D*> parts;
	for (auto& meshNode : loaded.meshes) {
		if (!meshNode.value.mesh.vertices.empty()) {
			parts.push_back(&meshNode.value);
		}
	}
	std::for_each(std::execution::par, parts.begin(), parts.end(), [](SFE::MeshObject3D* part) {
		auto& mesh = *part;
		// Welded before the tangents: MikkTSpace gives each copy of a corner its own, a little apart, and no weld after
		// joins those again.
		SFE::weldMesh(mesh.mesh);
		int tangentUv = 0;
		if (const auto* normals = mesh.material.tryGetTexture(SFE::NORMALS)) {
			tangentUv = normals->uvSet;
		}
		SFE::MeshUtils::generateTangents(&mesh.mesh, tangentUv);

		auto minAABB = SFE::Math::Vec3(std::numeric_limits<float>::max());
		auto maxAABB = SFE::Math::Vec3(std::numeric_limits<float>::lowest());
		for (const auto& vertex : mesh.mesh.vertices) {
			minAABB.x = std::min(minAABB.x, vertex.position.x);
			minAABB.y = std::min(minAABB.y, vertex.position.y);
			minAABB.z = std::min(minAABB.z, vertex.position.z);
			maxAABB.x = std::max(maxAABB.x, vertex.position.x);
			maxAABB.y = std::max(maxAABB.y, vertex.position.y);
			maxAABB.z = std::max(maxAABB.z, vertex.position.z);
		}
		mesh.aabb = SFE::FrustumModule::AABB(minAABB, maxAABB);

		// Model::bindMeshes keeps levels a mesh already has.
		SFE::buildMeshLods(mesh.mesh);
	});
}

AssetsModule::Model* ModelLoader::reload(const std::string& path) {
	Assimp::Importer import;
	const aiScene* scene = readScene(import, path);
	if (!scene) {
		return nullptr;
	}
	auto loaded = read(path, scene);
	finishMeshes(loaded);
	if (!loaded.ok) {
		return nullptr;
	}
	auto textures = hold(partsOf(loaded.meshes));
	auto* model = new Model(std::move(loaded.meshes), std::move(loaded.armature), std::move(loaded.animations));
	counted(*model, std::move(textures));
	std::lock_guard lock(mtx);
	AssetsManager::instance()->replaceAsset(path, model);
	return model;
}

AssetsModule::Model* ModelLoader::publish(const std::string& path, Loaded&& loaded) {
	// Its textures held before anyone can see it, and outside the lock (hold may wait for the device thread): one let
	// go of since it was read (releaseModel) is read back first.
	auto textures = loaded.ok && !tDistantTexels ? hold(partsOf(loaded.meshes)) : std::vector<Texture*>{};
	if (loaded.ok && tDistantTexels) {
		pointAtDistant(partsOf(loaded.meshes), tDistantTexels);
	}
	// Made outside the lock too: made on a worker, the model waits on the main thread to bind its meshes, and the main
	// thread may be in here meanwhile with a model of its own (a web's veil) -- each waited on the other for good.
	AssetsModule::Model* asset = loaded.ok
		? AssetsManager::instance()->createAsset<Model>(path, std::move(loaded.meshes), std::move(loaded.armature), std::move(loaded.animations))
		: nullptr;
	if (asset && tDistantTexels) {
		uncounted(*asset);
	}
	else if (asset) {
		counted(*asset, std::move(textures));
	}
	std::lock_guard lock(mtx);
	loading[path].notify_all();
	loading.erase(path);
	return asset;
}

Model* AssetsModule::modelAsItIs(const std::string& path) {
	auto* model = AssetsManager::instance()->getAsset<Model>(path);
	return model ? model : ModelLoader::instance()->load(path);
}

void AssetsModule::loadAllReleased(const std::vector<std::string>& paths, const int most) {
	tDistantTexels = most;
	ModelLoader::instance()->loadAll(paths);
	tDistantTexels = 0;
}

void AssetsModule::releaseModel(const std::string& path, const int most) {
	auto* model = AssetsManager::instance()->getAsset<Model>(path);
	if (!model) {
		return;
	}
	{
		std::lock_guard lock(gResidencyMutex);
		// Once, and only one counted (published here): one still being read back is let go of already.
		const auto held = gHeld.find(model);
		if (held == gHeld.end() || !gReleased.insert(model).second) {
			return;
		}
		for (auto* texture : held->second) {
			if (auto& users = gUsers[texture]; --users <= 0) {
				users = 0;
				releaseTexture(*texture);
			}
		}
	}
	pointAtDistant(partsOf(*model), most);
}

std::tuple<SFE::Tree<SFE::MeshObject3D>, Armature> ModelLoader::loadModel(const aiScene* scene, const std::string& path) {
	const auto directory = std::filesystem::path(path).parent_path().string();

	SFE::Tree<SFE::MeshObject3D> meshes;
	Armature armat;

	processNode(scene->mRootNode, scene, directory, std::filesystem::path(path).stem().string(), meshes, armat);
	finalizeArmature(scene, armat);
	AssetsModule::ModelText::add(meshes);
	bindUnskinnedMeshes(meshes, armat);

	return { meshes, armat };
}


void ModelLoader::processNode(aiNode* node, const aiScene* scene, const std::string& directory, const std::string& atlasStem, SFE::Tree<SFE::MeshObject3D>& meshes, Armature& armature) {
	meshes.value.nodeName = node->mName.C_Str();
	if (const aiMetadata* meta = node->mMetaData) {
		for (unsigned int i = 0; i < meta->mNumProperties; i++) {
			const aiMetadataEntry& entry = meta->mValues[i];
			std::string value;
			switch (entry.mType) {
			case AI_AISTRING: value = static_cast<const aiString*>(entry.mData)->C_Str(); break;
			case AI_BOOL: value = *static_cast<const bool*>(entry.mData) ? "1" : "0"; break;
			case AI_INT32: value = std::to_string(*static_cast<const int32_t*>(entry.mData)); break;
			case AI_UINT64: value = std::to_string(*static_cast<const uint64_t*>(entry.mData)); break;
			case AI_INT64: value = std::to_string(*static_cast<const int64_t*>(entry.mData)); break;
			case AI_FLOAT: value = std::to_string(*static_cast<const float*>(entry.mData)); break;
			case AI_DOUBLE: value = std::to_string(*static_cast<const double*>(entry.mData)); break;
			default: continue;
			}
			meshes.value.extras[meta->mKeys[i].C_Str()] = std::move(value);
		}
	}
	{
		const auto local = assimpMatToMat4(node->mTransformation);
		if (meshes.parent) {
			meshes.value.transform = meshes.parent->value.transform * local;
		}
		else {
			meshes.value.transform = local;
		}
	}

	for (unsigned int i = 0; i < node->mNumMeshes; i++) {
		SFE::MeshObject3D* dst = &meshes.value;
		if (i > 0 || !meshes.value.mesh.vertices.empty()) {
			meshes.addChild({});
			meshes.children.front().value.transform = meshes.value.transform;
			meshes.children.front().value.nodeName = meshes.value.nodeName;
			dst = &meshes.children.front().value;
		}
		processMesh(scene->mMeshes[node->mMeshes[i]], scene, directory, atlasStem, *dst, armature);
	}

	for (unsigned int i = 0; i < node->mNumChildren; i++) {
		meshes.addChild({});
		processNode(node->mChildren[i], scene, directory, atlasStem, meshes.children.front(), armature);
	}
}

void ModelLoader::readBonesData(std::vector<SFE::Vertex3D>& vertices, aiMesh* mesh, Armature& armature) {
	auto& bones = armature.bones;
	bones.reserve(mesh->mNumBones);

	std::map<std::string, int> bonesIds;

	for (const auto& bone : bones) {
		bonesIds[bone.name] = bone.id;
	}

	for (auto boneIndex = 0u; boneIndex < mesh->mNumBones; ++boneIndex) {
		uint32_t boneID;
		const auto meshBone = mesh->mBones[boneIndex];
		std::string boneName = meshBone->mName.C_Str();
		auto id = bonesIds.find(boneName);
		if (id == bonesIds.end()) {
			boneID = static_cast<uint32_t>(bones.size());

			Bone bone;

			bone.offset = assimpMatToMat4(meshBone->mOffsetMatrix);
			bone.name = boneName;
			bone.id = boneID;
			
			bones.push_back(bone);

			bonesIds[boneName] = boneID;
		}
		else {
			boneID = id->second;
		}

		assert(boneID != -1);
		const auto weights = meshBone->mWeights;

		for (auto weightIndex = 0u; weightIndex < meshBone->mNumWeights; ++weightIndex) {
			const auto vertexId = weights[weightIndex].mVertexId;
			const auto weight = weights[weightIndex].mWeight;
			if (vertexId >= vertices.size()) {
				assert(vertexId < vertices.size());
				continue;
			}
			
			for (int i = 0; i < 4; ++i) {
				if (vertices[vertexId].boneIDs[i] < 0) {
					vertices[vertexId].weights[i] = weight;
					vertices[vertexId].boneIDs[i] = boneID;
					break;
				}
			}
		}
	}
}

void ModelLoader::finalizeArmature(const aiScene* scene, Armature& armature) {
	if (!scene || !scene->mRootNode || armature.bones.empty()) {
		return;
	}

	constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();
	std::map<std::string, uint32_t> bonesIds;
	for (const auto& bone : armature.bones) {
		bonesIds[bone.name] = bone.id;
	}

	auto tryAddNode = [&](const aiNode* node) {
		if (!node) {
			return;
		}
		const std::string name = node->mName.C_Str();
		if (name.empty() || bonesIds.contains(name)) {
			return;
		}
		Bone bone;
		bone.name = name;
		bone.id = static_cast<uint32_t>(armature.bones.size());
		bone.offset = SFE::Math::Mat4(1.f);
		bone.transform = assimpMatToMat4(node->mTransformation);
		armature.bones.push_back(bone);
		bonesIds[name] = bone.id;
	};

	tryAddNode(scene->mRootNode);

	const size_t deformingCount = armature.bones.size();
	for (size_t i = 0; i < deformingCount; ++i) {
		const aiNode* node = scene->mRootNode->FindNode(armature.bones[i].name.c_str());
		while (node) {
			tryAddNode(node);
			node = node->mParent;
		}
	}

	for (auto& bone : armature.bones) {
		bone.parentBoneIdx = kNone;
		bone.childrenBones.clear();
		if (const aiNode* node = scene->mRootNode->FindNode(bone.name.c_str())) {
			bone.transform = assimpMatToMat4(node->mTransformation);
			if (node->mParent) {
				const auto it = bonesIds.find(node->mParent->mName.C_Str());
				if (it != bonesIds.end()) {
					bone.parentBoneIdx = it->second;
				}
			}
		}
	}

	for (auto& bone : armature.bones) {
		if (bone.parentBoneIdx != kNone && bone.parentBoneIdx < armature.bones.size()) {
			armature.bones[bone.parentBoneIdx].childrenBones.push_back(bone.id);
		}
	}

	armature.transform = SFE::Math::Mat4(1.f);
}

void ModelLoader::readMaterialData(SFE::Material& material, aiMaterial* assimpMaterial, const std::string& directory, const std::string& atlasStem, const std::string& meshName) {
	bool diffuseHasAlpha = false;
	bool diffuseCutoutAlpha = false;
	material.name = assimpMaterial->GetName().C_Str();

	aiString alphaMode;
	const bool gltfPbr = assimpMaterial->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS;
	material.gltfPbr = gltfPbr;
	const std::string am = alphaMode.C_Str();

	auto onDiffuse = [&](const Texture* tex) {
		if (!tex) {
			return;
		}
		if (tex->hasAlpha) {
			diffuseHasAlpha = true;
		}
		if (tex->hasCutoutAlpha) {
			diffuseCutoutAlpha = true;
		}
	};

	auto bindFirst = [&](SFE::MaterialType type, const std::vector<Texture*>& maps, aiTextureType uvType) {
		if (maps.empty() || material.tryGetTexture(type)) {
			return;
		}
		if (!gltfPbr && type == SFE::METALLIC && !looksLikeMetalnessMap(maps.front())) {
			return;
		}
		int uv = 0;
		assimpMaterial->Get(AI_MATKEY_UVWSRC(uvType, 0), uv);
		material[type] = SFE::MaterialTexture{ maps.front()->texture, type, type, uv };
		material[type].path = maps.front()->assetPath;
		if (type == SFE::DIFFUSE) {
			onDiffuse(maps.front());
		}
	};

	auto tryFiles = [&](SFE::MaterialType type, const std::vector<std::string>& names) {
		if (material.tryGetTexture(type)) {
			return;
		}
		for (const auto& name : names) {
			if (name.empty()) {
				continue;
			}
			const std::string path = directory + "/" + name;
			if (!fileThere(path)) {
				continue;
			}
			auto* tex = TextureHandler::loadTexture(path, false, TextureStorage::Compressed);
			if (tex && tex->isValid()) {
				if (!gltfPbr && type == SFE::METALLIC && !looksLikeMetalnessMap(tex)) {
					continue;
				}
				material[type] = SFE::MaterialTexture{ tex->texture, type, type };
				material[type].path = tex->assetPath;
				if (type == SFE::DIFFUSE) {
					onDiffuse(tex);
				}
				return;
			}
		}
	};

	auto sidecarNames = [](const std::string& prefix, std::initializer_list<const char*> suffixes) {
		std::vector<std::string> out;
		if (prefix.empty()) {
			return out;
		}
		out.reserve(suffixes.size() * 2);
		std::string us = prefix;
		std::string sp = prefix;
		std::replace(us.begin(), us.end(), ' ', '_');
		std::replace(sp.begin(), sp.end(), '_', ' ');
		for (const char* suffix : suffixes) {
			out.push_back(prefix + suffix);
			if (us != prefix) {
				out.push_back(us + suffix);
			}
			if (sp != prefix && sp != us) {
				out.push_back(sp + suffix);
			}
		}
		return out;
	};

	auto tryPrefix = [&](const std::string& prefix) {
		if (prefix.empty()) {
			return;
		}
		tryFiles(SFE::DIFFUSE, sidecarNames(prefix, {
			"_Base_Color.png", "_BaseColor.png", "_albedo.png", "_color.png", "_diffuse.png"
		}));
		tryFiles(SFE::NORMALS, sidecarNames(prefix, { "_Normal.png", "_Normal_OpenGL.png", "_normal.png" }));
		tryFiles(SFE::ROUGHNESS, sidecarNames(prefix, { "_Roughness.png", "_rough.png" }));
		tryFiles(SFE::METALLIC, sidecarNames(prefix, { "_Metallic.png", "_metal.png" }));
		tryFiles(SFE::SPECULAR, sidecarNames(prefix, { "_Specular.png", "_spec.png" }));
		tryFiles(SFE::SUBSURFACE, sidecarNames(prefix, { "_subsur.png", "_sss.png", "_subsurface.png" }));
	};

	aiTextureType diffuseType = aiTextureType_BASE_COLOR;
	auto diffuseMaps = loadMaterialTextures(assimpMaterial, aiTextureType_BASE_COLOR, directory);
	if (diffuseMaps.empty()) {
		diffuseMaps = loadMaterialTextures(assimpMaterial, aiTextureType_DIFFUSE, directory);
		diffuseType = aiTextureType_DIFFUSE;
	}
	bindFirst(SFE::DIFFUSE, diffuseMaps, diffuseType);

	auto specularMaps = loadMaterialTextures(assimpMaterial, aiTextureType_SPECULAR, directory);
	bindFirst(SFE::SPECULAR, specularMaps, aiTextureType_SPECULAR);

	aiTextureType roughType = aiTextureType_DIFFUSE_ROUGHNESS;
	aiTextureType metalType = aiTextureType_METALNESS;
	auto roughnessMaps = loadMaterialTextures(assimpMaterial, aiTextureType_DIFFUSE_ROUGHNESS, directory);
	auto metallicMaps = loadMaterialTextures(assimpMaterial, aiTextureType_METALNESS, directory);
	auto packedMaps = loadMaterialTextures(assimpMaterial, aiTextureType_UNKNOWN, directory);
	if (roughnessMaps.empty()) {
		roughnessMaps = packedMaps;
		roughType = aiTextureType_UNKNOWN;
	}
	if (metallicMaps.empty()) {
		metallicMaps = packedMaps;
		metalType = aiTextureType_UNKNOWN;
	}
	bindFirst(SFE::ROUGHNESS, roughnessMaps, roughType);
	bindFirst(SFE::METALLIC, metallicMaps, metalType);

	// glTF only, so an FBX or OBJ lightmap never darkens a model. Assimp 5 reads the occlusionTexture as a lightmap.
	if (gltfPbr) {
		aiTextureType aoType = aiTextureType_LIGHTMAP;
		auto aoMaps = loadMaterialTextures(assimpMaterial, aoType, directory);
		if (aoMaps.empty()) {
			aoType = aiTextureType_AMBIENT_OCCLUSION;
			aoMaps = loadMaterialTextures(assimpMaterial, aoType, directory);
		}
		bindFirst(SFE::OCCLUSION, aoMaps, aoType);
	}

	aiTextureType normalType = aiTextureType_NORMALS;
	auto normalMaps = loadMaterialTextures(assimpMaterial, aiTextureType_NORMALS, directory);
	if (normalMaps.empty()) {
		normalMaps = loadMaterialTextures(assimpMaterial, aiTextureType_NORMAL_CAMERA, directory);
		normalType = aiTextureType_NORMAL_CAMERA;
	}
	bindFirst(SFE::NORMALS, normalMaps, normalType);

	auto opacityMaps = loadMaterialTextures(assimpMaterial, aiTextureType_OPACITY, directory);
	if (!opacityMaps.empty() && opacityMaps.front()->hasAlpha) {
		diffuseHasAlpha = true;
	}

	aiString matName;
	assimpMaterial->Get(AI_MATKEY_NAME, matName);
	const std::string n = matName.C_Str();
	if (!gltfPbr) {
		tryPrefix(n);
		if (!atlasStem.empty() && toLowerCopy(atlasStem) != toLowerCopy(n)) {
			tryPrefix(atlasStem);
		}
	}

	float opacity = 1.f;
	assimpMaterial->Get(AI_MATKEY_OPACITY, opacity);
	float ior = 1.5f;
	assimpMaterial->Get(AI_MATKEY_REFRACTI, ior);
	float transmission = 0.f;
	assimpMaterial->Get(AI_MATKEY_TRANSMISSION_FACTOR, transmission);

	const std::string nl = toLowerCopy(n);
	const std::string meshLower = toLowerCopy(meshName);
	const bool nameGlass = !gltfPbr && (looksLikeGlass(nl) || looksLikeGlass(meshLower));
	const bool nameHair = !gltfPbr && (looksLikeHair(nl) || looksLikeHair(meshLower));
	const bool nameCutout = !gltfPbr && (nameHair || looksLikeCutout(nl) || looksLikeCutout(meshLower));

	material.opacity = opacity;
	material.ior = ior > 1.f ? ior : 1.5f;
	material.hasDiffuseAlpha = diffuseHasAlpha;

	float roughFactor = 0.f;
	const bool hasRoughFactor = assimpMaterial->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughFactor) == AI_SUCCESS;
	if (hasRoughFactor) {
		material.roughness = (std::max)(0.04f, (std::min)(1.f, roughFactor));
	}
	else if (!material.tryGetTexture(SFE::ROUGHNESS) && !material.tryGetTexture(SFE::DIFFUSE)) {
		float shininess = 0.f;
		if (assimpMaterial->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 1.f) {
			material.roughness = (std::max)(0.04f, (std::min)(1.f, std::sqrt(2.f / (shininess + 2.f))));
		}
	}

	float metalFactor = 0.f;
	if (assimpMaterial->Get(AI_MATKEY_METALLIC_FACTOR, metalFactor) == AI_SUCCESS) {
		material.metallic = (std::max)(0.f, (std::min)(1.f, metalFactor));
	}

	float specFactor = 1.f;
	if (assimpMaterial->Get(AI_MATKEY_SPECULAR_FACTOR, specFactor) == AI_SUCCESS) {
		material.specFactor = (std::max)(0.f, (std::min)(1.f, specFactor));
	}

	float cc = 0.f;
	if (assimpMaterial->Get(AI_MATKEY_CLEARCOAT_FACTOR, cc) == AI_SUCCESS && cc > 0.f) {
		material.clearcoat = (std::max)(0.f, (std::min)(1.f, cc));
		float ccRough = 0.04f;
		assimpMaterial->Get(AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR, ccRough);
		material.clearcoatRough = (std::max)(0.04f, (std::min)(1.f, ccRough));
	}

	if (gltfPbr) {
		aiColor4D baseColor;
		if (assimpMaterial->Get(AI_MATKEY_BASE_COLOR, baseColor) == AI_SUCCESS) {
			material.baseColor = { baseColor.r, baseColor.g, baseColor.b };
		}
		// emissiveFactor times KHR_materials_emissive_strength, which Blender writes for a strength past 1.
		aiColor4D emissive;
		if (assimpMaterial->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
			float strength = 1.f;
			assimpMaterial->Get(AI_MATKEY_EMISSIVE_INTENSITY, strength);
			material.emissive = SFE::Math::Vec3{ emissive.r, emissive.g, emissive.b } * strength;
		}
		bindFirst(SFE::EMISSIVE, loadMaterialTextures(assimpMaterial, aiTextureType_EMISSIVE, directory), aiTextureType_EMISSIVE);
		material.transparent = am == "BLEND";
		material.alphaCutout = am == "MASK";
		material.alphaFromMaps = false;
		material.alphaDither = false;
		// A cutoff means something to a MASK material alone. Assimp reports glTF's default of 0.5 for every material, and
		// an opaque one taken at its word was drawn as a cut-out: no early depth test, and no metal -- cut-outs are matte.
		if (material.alphaCutout) {
			float cutoff = 0.5f;
			assimpMaterial->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff);
			material.alphaCutoff = cutoff;
		}
		// Deferred has no general alpha blend. BLEND foliage/decals/windows would
		// otherwise go through the glass IOR pass and lose their albedo.
		if (material.transparent && transmission <= 0.01f) {
			material.transparent = false;
			material.alphaCutout = true;
			material.opacity = 1.f;
			if (material.alphaCutoff <= 0.f) {
				material.alphaCutoff = 0.5f;
			}
		}
	} else {
		const bool maskMode = am == "MASK" || nameCutout || diffuseCutoutAlpha || !opacityMaps.empty();
		const bool blendMode = am == "BLEND" || nameGlass || transmission > 0.01f
			|| (opacity > 0.05f && opacity < 0.99f);
		if (maskMode && !nameGlass && transmission <= 0.01f) {
			material.transparent = false;
			material.alphaCutout = true;
			material.opacity = 1.f;
			material.alphaFromMaps = !diffuseHasAlpha && !diffuseCutoutAlpha;
			material.alphaDither = nameHair;
		}
		else {
			material.transparent = blendMode;
			material.alphaCutout = !material.transparent && (am == "MASK" || nameCutout);
		}

		if (nameHair) {
			material.metallic = 0.f;
			material.materialTextures.erase(SFE::METALLIC);
			material.materialTextures.erase(SFE::ROUGHNESS);
			if (!material.alphaFromMaps) {
				material.materialTextures.erase(SFE::SPECULAR);
			}
			material.roughness = (std::max)(material.roughness, 0.9f);
		}
	}
}

void ModelLoader::readIndicesData(std::vector<unsigned>& indices, unsigned numFaces, aiFace* faces) {
	indices.clear();
	indices.reserve(numFaces * 3);
	for (auto i = 0u; i < numFaces; i++) {
		const auto& face = faces[i];
		for (auto j = 0u; j < face.mNumIndices; j++) {
			indices.push_back(face.mIndices[j]);
		}
	}
}

void ModelLoader::readVerticesData(std::vector<SFE::Vertex3D>& vertices, unsigned numVertices, aiMesh* assimpMesh) {
	vertices.resize(numVertices);

	for (unsigned int i = 0; i < numVertices; i++) {
		auto& vertex = vertices[i];

		auto newPos =/* meshNode->mTransformation **/ assimpMesh->mVertices[i];
		// process vertex positions, normals and texture coordinates
		vertex.position.x = newPos.x;
		vertex.position.y = newPos.y;
		vertex.position.z = newPos.z;

		if (assimpMesh->mTextureCoords[0]) {
			vertex.texCoords.x = assimpMesh->mTextureCoords[0][i].x;
			vertex.texCoords.y = assimpMesh->mTextureCoords[0][i].y;
		}
		if (assimpMesh->mTextureCoords[1]) {
			vertex.texCoords1.x = assimpMesh->mTextureCoords[1][i].x;
			vertex.texCoords1.y = assimpMesh->mTextureCoords[1][i].y;
		}
		else {
			vertex.texCoords1 = vertex.texCoords;
		}

		if (assimpMesh->mNormals) {
			vertex.normal.x = assimpMesh->mNormals[i].x;
			vertex.normal.y = assimpMesh->mNormals[i].y;
			vertex.normal.z = assimpMesh->mNormals[i].z;
		}

		if (assimpMesh->mTangents) {
			vertex.tangent.x = assimpMesh->mTangents[i].x;
			vertex.tangent.y = assimpMesh->mTangents[i].y;
			vertex.tangent.z = assimpMesh->mTangents[i].z;
		}

		if (assimpMesh->mBitangents) {
			vertex.biTangent.x = assimpMesh->mBitangents[i].x;
			vertex.biTangent.y = assimpMesh->mBitangents[i].y;
			vertex.biTangent.z = assimpMesh->mBitangents[i].z;
		}
		else if (assimpMesh->mTangents && assimpMesh->mNormals) {
			vertex.biTangent = SFE::Math::cross(vertex.normal, vertex.tangent);
		}
	}
}

int ModelLoader::extractLodLevel(const std::string& meshName) {
	if (meshName.size() <= 4) {
		return 0;
	}

	auto i = meshName.size() - 4;
	for (; i > 0; i--) {
		if (meshName[i] == '_' && meshName[i + 1] == 'L' && meshName[i + 2] == 'O' && meshName[i + 3] == 'D') {
			break;
		}
	}

	if (i == 0) {
		return 0;
	}

	return std::atoi(meshName.substr(i + 4, meshName.size() - i).c_str());
}

void ModelLoader::processMesh(aiMesh* assimpMesh, const aiScene* scene, const std::string& directory, const std::string& atlasStem, SFE::MeshObject3D& meshObject, Armature& armature) {
	readVerticesData(meshObject.mesh.vertices, assimpMesh->mNumVertices, assimpMesh);
	readIndicesData(meshObject.mesh.indices, assimpMesh->mNumFaces, assimpMesh->mFaces);
	readBonesData(meshObject.mesh.vertices, assimpMesh, armature);
	std::string meshName = assimpMesh->mName.C_Str();
	if (!meshObject.nodeName.empty()) {
		meshName += " ";
		meshName += meshObject.nodeName;
	}
	readMaterialData(meshObject.material, scene->mMaterials[assimpMesh->mMaterialIndex], directory, atlasStem, meshName);
	// Tangents come later, in finishMeshes: they need nothing but the mesh and take most of a load's time.
	if (meshObject.material.gltfPbr) {
		return;
	}
	const std::string meshLower = toLowerCopy(meshName);
	if (looksLikeEye(meshLower)) {
		meshObject.material.materialTextures.erase(SFE::NORMALS);
		meshObject.material.materialTextures.erase(SFE::ROUGHNESS);
		meshObject.material.materialTextures.erase(SFE::METALLIC);
		meshObject.material.materialTextures.erase(SFE::SPECULAR);
		meshObject.material.materialTextures.erase(SFE::SUBSURFACE);
		meshObject.material.metallic = 0.f;
		meshObject.material.roughness = 0.28f;
		meshObject.material.transparent = false;
		meshObject.material.alphaCutout = false;
		meshObject.material.alphaFromMaps = false;
		meshObject.material.alphaDither = false;
	}
	else if (isCardMesh(meshObject.mesh)) {
		meshObject.material.transparent = false;
		meshObject.material.alphaCutout = true;
		meshObject.material.alphaDither = true;
		if (!meshObject.material.hasDiffuseAlpha) {
			meshObject.material.alphaFromMaps = true;
		}
		meshObject.material.metallic = 0.f;
		meshObject.material.materialTextures.erase(SFE::METALLIC);
		if (!meshObject.material.alphaFromMaps) {
			meshObject.material.materialTextures.erase(SFE::ROUGHNESS);
			meshObject.material.materialTextures.erase(SFE::SPECULAR);
			meshObject.material.roughness = (std::max)(meshObject.material.roughness, 0.9f);
		}
	}
}

std::vector<std::string> ModelLoader::textureFileCandidates(const std::string& raw, const std::string& directory) {
	std::vector<std::string> candidates;
	for (const auto& name : { raw, uriDecoded(raw) }) {
		std::string filename = name;
		if (const auto pos = name.find_last_of("/\\"); pos != std::string::npos) {
			filename = name.substr(pos + 1);
		}
		std::string relative = name;
		std::replace(relative.begin(), relative.end(), '\\', '/');
		candidates.insert(candidates.end(), { name, directory + "/" + filename, directory + "/" + relative, directory + "/textures/" + filename });
	}
	return candidates;
}

std::vector<Texture*> ModelLoader::loadMaterialTextures(aiMaterial* mat, aiTextureType type, const std::string& directory) {
	std::vector<Texture*> textures;
	auto* defaultTex = &TextureHandler::instance()->mDefaultTex;
	auto tryLoad = [defaultTex](const std::string& path) -> Texture* {
		if (path.empty() || !fileThere(path)) {
			return nullptr;
		}
		auto* tex = TextureHandler::loadTexture(path, false, TextureStorage::Compressed);
		if (tex && tex != defaultTex && tex->isValid()) {
			return tex;
		}
		return nullptr;
	};

	for (unsigned int i = 0; i < mat->GetTextureCount(type); i++) {
		aiString str;
		mat->GetTexture(type, i, &str);

		std::string raw = str.C_Str();
		if (raw.empty() || raw[0] == '*') {
			continue;
		}
		Texture* tex = nullptr;
		for (const auto& candidate : textureFileCandidates(raw, directory)) {
			if ((tex = tryLoad(candidate))) {
				break;
			}
		}
		if (tex) {
			textures.emplace_back(tex);
		}
	}
	return textures;
}
