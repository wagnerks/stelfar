#include "propertiesModule/ModelBinding.h"

#include <cstring>
#include <vector>

#include "assetsModule/modelModule/MeshVaoRegistry.h"
#include "assetsModule/modelModule/ModelLoader.h"
#include "assetsModule/modelModule/ModelRelease.h"
#include "componentsModule/ArmatureComponent.h"
#include "componentsModule/MaterialComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/ModelComponent.h"
#include "core/ECSHandler.h"
#include "systemsModule/TasksManager.h"
#include "systemsModule/systems/RenderSystem.h"

namespace SFE::PropertiesModule::ModelBinding {
	namespace {
		std::vector<std::function<void(const std::string&)>>& reloadListeners() {
			static std::vector<std::function<void(const std::string&)>> listeners;
			return listeners;
		}

		bool sameTextures(const SFE::ComponentsModule::Materials& a, const SFE::ComponentsModule::Materials& b) {
			if (a.materialsCount != b.materialsCount) {
				return false;
			}
			for (uint8_t i = 0; i < a.materialsCount; ++i) {
				if (a.material[i].slot != b.material[i].slot || a.material[i].texture.id != b.material[i].texture.id
					|| a.material[i].uvSet != b.material[i].uvSet) {
					return false;
				}
			}
			return true;
		}

		// Into `fresh`, what `changed` has that differs from `given`, the material it was made from. Every field of
		// MaterialComponent, one by one: a new field belongs here too.
		void keepChanges(const MaterialComponent& given, const MaterialComponent& changed, MaterialComponent& fresh) {
			const auto keep = [](const auto& was, const auto& now, auto& into) {
				if (!(now == was)) {
					into = now;
				}
			};
			if (!sameTextures(given.materials, changed.materials)) {
				fresh.materials = changed.materials;
			}
			keep(given.vertexShader, changed.vertexShader, fresh.vertexShader);
			keep(given.fragmentShader, changed.fragmentShader, fresh.fragmentShader);
			keep(given.transparent, changed.transparent, fresh.transparent);
			keep(given.alphaFromMaps, changed.alphaFromMaps, fresh.alphaFromMaps);
			keep(given.alphaDither, changed.alphaDither, fresh.alphaDither);
			keep(given.opacity, changed.opacity, fresh.opacity);
			keep(given.ior, changed.ior, fresh.ior);
			keep(given.roughness, changed.roughness, fresh.roughness);
			keep(given.metallic, changed.metallic, fresh.metallic);
			keep(given.alphaCutoff, changed.alphaCutoff, fresh.alphaCutoff);
			keep(given.specFactor, changed.specFactor, fresh.specFactor);
			keep(given.clearcoat, changed.clearcoat, fresh.clearcoat);
			keep(given.clearcoatRough, changed.clearcoatRough, fresh.clearcoatRough);
			keep(given.tint, changed.tint, fresh.tint);
			keep(given.emissive, changed.emissive, fresh.emissive);
			keep(given.sway, changed.sway, fresh.sway);
			keep(given.hidden, changed.hidden, fresh.hidden);
			keep(given.shadowOnly, changed.shadowOnly, fresh.shadowOnly);
			if (std::memcmp(given.triplanar, changed.triplanar, sizeof(given.triplanar)) != 0) {
				std::memcpy(fresh.triplanar, changed.triplanar, sizeof(fresh.triplanar));
			}
		}
	}

	void bind(ecss::EntityId entity) {
		auto& registry = ECSHandler::registry();
		auto modelComp = registry.pinComponent<ModelComponent>(entity);
		if (!modelComp) {
			return;
		}
		// As it is: one whose textures were let go of bound with the copies of their small levels (releaseModel), not
		// read back by binding it; spawnModel reads them back first.
		AssetsModule::Model* model = modelComp->mPath.empty() ? nullptr : AssetsModule::modelAsItIs(modelComp->mPath);
		if (modelComp->getModel().meshes.empty() && model) {
			modelComp->boneMatrices = model->getDefaultBoneMatrices();
			modelComp->armature = model->getArmature();
			modelComp->setModel(model->getLODs());
		}
		if (modelComp->getModel().meshes.empty()) {
			return;
		}

		// A binding again: the mesh materials as the last one left them, and as the model gave them at that binding.
		std::vector<MaterialComponent> changed;
		std::vector<MaterialComponent> given;
		if (auto bound = registry.pinComponent<const MeshComponent>(entity); bound && bound->meshModel) {
			changed = bound->meshMaterials;
			given = bound->meshGiven;
		}

		auto meshComp = registry.addComponent<MeshComponent>(entity);
		if (!registry.hasComponent<MaterialComponent>(entity)) {
			ECSHandler::addComponent<MaterialComponent>(entity);
		}
		auto materialComp = registry.pinComponent<MaterialComponent>(entity);
		const SFE::Material* srcMat = nullptr;
		bool allGlass = true;
		bool anyMesh = false;
		if (model) {
			SFE::Graph<SFE::RenderMeshData> meshData;
			meshData.fill<SFE::MeshObject3D>(model->getMeshTree(), [&](const SFE::MeshObject3D& meshObj) {
				meshComp->meshMaterials.push_back(SFE::ComponentsModule::fromAssetMaterial(meshObj.material));
				if (!meshObj.mesh.vertices.empty()) {
					anyMesh = true;
					if (!meshObj.material.transparent) {
						allGlass = false;
					}
				}
				if (!srcMat && !meshObj.material.materialTextures.empty()) {
					srcMat = &meshObj.material;
				}
				return std::move(SFE::MeshVaoRegistry::instance()->get(const_cast<SFE::Mesh3D*>(&meshObj.mesh)));
			});
			meshComp->meshGraph = meshData;
			meshComp->meshModel = model;
			meshComp->meshGiven = meshComp->meshMaterials;
			if (anyMesh && allGlass) {
				materialComp->transparent = true;
			}
			if (!changed.empty() && changed.size() == given.size() && given.size() == meshComp->meshMaterials.size()) {
				for (size_t i = 0; i < given.size(); ++i) {
					keepChanges(given[i], changed[i], meshComp->meshMaterials[i]);
				}
			}
		}
		else {
			meshComp->meshGraph.root().value = SFE::MeshVaoRegistry::instance()->get(&modelComp->getModel().meshes[0]->mesh);
			meshComp->meshMaterials.push_back(SFE::ComponentsModule::fromAssetMaterial(modelComp->getModel().meshes[0]->material));
		}
		SFE::SystemsModule::TasksManager::instance()->notify({ entity, SFE::SystemsModule::TaskType::MESH_UPDATED });
		if (srcMat) {
			materialComp->materials.clearMaterials();
			for (auto& mat : srcMat->materialTextures) {
				materialComp->materials.addMaterial({ mat.second.uniformSlot, mat.second.texture });
			}
		}
		SFE::SystemsModule::TasksManager::instance()->notify({ entity, SFE::SystemsModule::TaskType::MATERIAL_UPDATED });

		if (modelComp->armature.bones.size()) {
			auto armatureComp = ECSHandler::addComponent<SFE::ComponentsModule::ArmatureComponent>(entity);
			armatureComp->armature = modelComp->armature;
		}

		if (!modelComp->boneMatrices.empty()) {
			auto armatureBonesComp = registry.addComponent<SFE::ComponentsModule::ArmatureBonesComponent>(entity);
			armatureBonesComp->boneMatrices = modelComp->boneMatrices;
			SFE::SystemsModule::TasksManager::instance()->notify({ entity, SFE::SystemsModule::TaskType::ARMATURE_UPDATED });
		}

		if (model && !model->getAnimations().empty()
			&& !registry.pinComponent<const ComponentsModule::AnimationComponent>(entity)) {
			auto* anim = registry.addComponent<ComponentsModule::AnimationComponent>(entity);
			anim->mCurrentAnimation = playbackClip(model);
			anim->mLoop = true;
			anim->play(0.f);
			SFE::SystemsModule::TasksManager::instance()->notify({ entity, SFE::SystemsModule::TaskType::ANIMATION_UPDATED });
		}
	}

	void onReloaded(std::function<void(const std::string& path)> listener) {
		reloadListeners().push_back(std::move(listener));
	}

	void setModel(ecss::EntityId entity, AssetsModule::Model& model) {
		auto& registry = ECSHandler::registry();
		if (auto component = registry.pinComponent<ModelComponent>(entity)) {
			component->boneMatrices = model.getDefaultBoneMatrices();
			component->armature = model.getArmature();
			component->setModel(model.getLODs());
		}
		if (auto bounds = registry.pinComponent<ComponentsModule::AABBComponent>(entity)) {
			bounds->defaultAabbs.clear();
		}
		if (auto* render = ECSHandler::getSystem<SystemsModule::RenderSystem>()) {
			render->forgetBones(entity);
		}
		bind(entity);
	}

	int reload(const std::string& path) {
		auto* fresh = AssetsModule::ModelLoader::instance()->reload(path);
		if (!fresh) {
			return -1;
		}
		auto& registry = ECSHandler::registry();
		std::vector<ecss::EntityId> users;
		for (const auto& [entity, model] : registry.view<const ModelComponent>()) {
			if (model && model->mPath == path) {
				users.push_back(entity);
			}
		}
		const auto sameName = [&](const AssetsModule::Animation* clip) -> const AssetsModule::Animation* {
			if (!clip) {
				return nullptr;
			}
			for (const auto& candidate : fresh->getAnimations()) {
				if (candidate.getName() == clip->getName()) {
					return &candidate;
				}
			}
			return clip;
		};
		for (const auto entity : users) {
			setModel(entity, *fresh);
			if (auto animation = registry.pinComponent<ComponentsModule::AnimationComponent>(entity)) {
				animation->mCurrentAnimation = sameName(animation->mCurrentAnimation);
				animation->mFromAnimation = sameName(animation->mFromAnimation);
				SFE::SystemsModule::TasksManager::instance()->notify({ entity, SFE::SystemsModule::TaskType::ANIMATION_UPDATED });
			}
		}
		for (const auto& listener : reloadListeners()) {
			listener(path);
		}
		return static_cast<int>(users.size());
	}

	const AssetsModule::Animation* playbackClip(const AssetsModule::Model* model) {
		if (!model) {
			return nullptr;
		}
		const auto& anims = model->getAnimations();
		if (anims.empty()) {
			return nullptr;
		}

		auto findSub = [&](const char* sub) -> const AssetsModule::Animation* {
			for (const auto& anim : anims) {
				if (anim.getName().find(sub) != std::string::npos) {
					return &anim;
				}
			}
			return nullptr;
		};

		for (const char* sub : { "dance", "Dance", "walk", "idle", "run" }) {
			if (const auto* clip = findSub(sub)) {
				return clip;
			}
		}
		for (const auto& anim : anims) {
			const auto& name = anim.getName();
			if (name.find("Static") == std::string::npos && name.find("static") == std::string::npos) {
				return &anim;
			}
		}
		return &anims.front();
	}
}
