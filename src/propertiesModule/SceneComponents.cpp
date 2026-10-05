#include "propertiesModule/ModelBinding.h"
#include "propertiesModule/SceneComponents.h"

#include <utility>

#include "assetsModule/modelModule/Model.h"
#include "assetsModule/modelModule/ModelLoader.h"
#include "assetsModule/modelModule/Terrain.h"
#include "componentsModule/CameraComponent.h"
#include "componentsModule/MeshComponent.h"
#include "core/ECSHandler.h"
#include "propertiesModule/PropertiesSystem.h"
#include "systemsModule/systems/CameraSystem.h"
#include "systemsModule/TasksManager.h"

namespace SFE::PropertiesModule {
	using namespace SFE::ComponentsModule;

	namespace {
		// Written by name rather than by ordinal, so inserting a light type into the enum does
		// not silently turn every saved point light into something else.
		constexpr std::pair<std::string_view, eLightType> kLightTypes[] = {
			{ "none",		 eLightType::NONE },
			{ "directional", eLightType::DIRECTIONAL },
			{ "point",		 eLightType::POINT },
			{ "perspective", eLightType::PERSPECTIVE },
			{ "world",		 eLightType::WORLD },
		};

		std::string lightTypeName(eLightType type) {
			for (const auto& [name, value] : kLightTypes) {
				if (value == type) {
					return std::string(name);
				}
			}
			return std::string(kLightTypes[0].first);
		}

		eLightType lightTypeFromName(const Json::Value& data) {
			const auto name = data.asString();
			for (const auto& [candidate, value] : kLightTypes) {
				if (candidate == name) {
					return value;
				}
			}
			return eLightType::NONE;
		}
	}

	// ===== TransformComponent =================================================

	void ComponentSerializer<TransformComponent>::save(const TransformComponent& component, Json::Value& data) {
		const auto& scale = component.getScale();
		data["Scale"].append(scale.x);
		data["Scale"].append(scale.y);
		data["Scale"].append(scale.z);

		const auto& pos = component.getPos();
		data["Pos"].append(pos.x);
		data["Pos"].append(pos.y);
		data["Pos"].append(pos.z);

		const auto& rotate = component.getRotate();
		data["Rotate"].append(rotate.x);
		data["Rotate"].append(rotate.y);
		data["Rotate"].append(rotate.z);
	}

	void ComponentSerializer<TransformComponent>::load(ecss::EntityId entity, const Json::Value& data) {
		auto component = ECSHandler::registry().addComponent<TransformComponent>(entity, entity);
		if (!component) {
			return;
		}

		if (auto val = JsonUtils::getValueArray(data, "Scale")) {
			component->setScale(JsonUtils::getVec3(*val));
		}

		if (auto val = JsonUtils::getValueArray(data, "Pos")) {
			component->setPos(JsonUtils::getVec3(*val));
		}

		if (auto val = JsonUtils::getValueArray(data, "Rotate")) {
			component->setRotate(JsonUtils::getVec3(*val));
		}
	}

	// ===== ModelComponent =====================================================

	void ComponentSerializer<ModelComponent>::save(const ModelComponent& component, Json::Value& data) {
		data["ModelPath"] = component.mPath;
		if (component.collider) {
			data["Collider"] = true;
		}
	}

	void ComponentSerializer<ModelComponent>::load(ecss::EntityId entity, const Json::Value& data) {
		auto component = ECSHandler::registry().addComponent<ModelComponent>(entity, entity);
		if (!component) {
			return;
		}

		AssetsModule::Model* model = nullptr;
		if (data.isMember("ModelPath")) {
			component->mPath = data["ModelPath"].asString();
			model = AssetsModule::ModelLoader::instance()->load(component->mPath);
		}
		component->collider = data.get("Collider", false).asBool();

		if (model) {
			component->boneMatrices = model->getDefaultBoneMatrices();
			component->armature = model->getArmature();
			component->addMeshData(model->getLODs());
		}
	}

	// ===== LightSourceComponent ===============================================

	void ComponentSerializer<LightSourceComponent>::save(const LightSourceComponent& component, Json::Value& data) {
		data["type"] = lightTypeName(component.getType());
		data["intensity"] = component.getIntensity();

		const auto& color = component.getLightColor();
		data["color"] = Json::arrayValue;
		data["color"].append(color.x);
		data["color"].append(color.y);
		data["color"].append(color.z);

		data["radius"] = component.mRadius;
		data["near"] = component.mNear;
		data["linear"] = component.mLinear;
		data["quadratic"] = component.mQuadratic;
		data["with_shadows"] = component.mWithShadows;
		data["flicker"] = component.mFlicker;

		data["bias"] = component.getBias();
		data["samples"] = component.getSamples();

		const auto& texelSize = component.getTexelSize();
		data["texel_size"] = Json::arrayValue;
		data["texel_size"].append(texelSize.x);
		data["texel_size"].append(texelSize.y);
	}

	void ComponentSerializer<LightSourceComponent>::load(ecss::EntityId entity, const Json::Value& data) {
		// The light type is a constructor argument, so it is read before the component exists.
		auto component = ECSHandler::addComponent<LightSourceComponent>(entity, entity, lightTypeFromName(data["type"]));
		if (!component) {
			return;
		}

		if (data.isMember("intensity")) {
			component->setIntensity(data["intensity"].asFloat());
		}
		if (auto val = JsonUtils::getValueArray(data, "color")) {
			component->setLightColor(JsonUtils::getVec3(*val));
		}
		if (data.isMember("radius")) {
			component->mRadius = data["radius"].asFloat();
		}
		if (data.isMember("near")) {
			component->mNear = data["near"].asFloat();
		}
		if (data.isMember("linear")) {
			component->mLinear = data["linear"].asFloat();
		}
		if (data.isMember("quadratic")) {
			component->mQuadratic = data["quadratic"].asFloat();
		}
		if (data.isMember("with_shadows")) {
			component->mWithShadows = data["with_shadows"].asBool();
		}
		if (data.isMember("flicker")) {
			component->mFlicker = data["flicker"].asFloat();
		}
		if (data.isMember("bias")) {
			component->setBias(data["bias"].asFloat());
		}
		if (data.isMember("samples")) {
			component->setSamples(data["samples"].asInt());
		}
		if (auto val = JsonUtils::getValueArray(data, "texel_size")) {
			component->setTexelSize(JsonUtils::getVec2(*val));
		}
	}

	// ===== CascadeShadowComponent =============================================

	void ComponentSerializer<CascadeShadowComponent>::save(const CascadeShadowComponent& component, Json::Value& data) {
		data["cascaded_levels"] = Json::arrayValue;
		for (auto level : component.shadowCascadeLevels) {
			data["cascaded_levels"].append(level);
		}

		data["resolution"] = Json::arrayValue;
		data["resolution"].append(component.resolution.x);
		data["resolution"].append(component.resolution.y);

		data["cascades"] = Json::arrayValue;
		for (auto& cascade : component.cascades) {
			Json::Value cascadeData = Json::objectValue;
			cascadeData["bias"] = cascade.bias;
			cascadeData["samples"] = cascade.samples;

			cascadeData["texel_size"].append(cascade.texelSize.x);
			cascadeData["texel_size"].append(cascade.texelSize.y);

			cascadeData["z_mult"].append(cascade.zMult.x);
			cascadeData["z_mult"].append(cascade.zMult.y);

			data["cascades"].append(cascadeData);
		}

		data["shadow_intensity"] = component.shadowIntensity;
	}

	void ComponentSerializer<CascadeShadowComponent>::load(ecss::EntityId entity, const Json::Value& data) {
		auto component = ECSHandler::registry().addComponent<CascadeShadowComponent>(entity, entity);
		if (!component) {
			return;
		}

		// The cascade split points are derived from the camera the shadows are rendered for,
		// so the file only carries the per-cascade tuning laid over them.
		auto cameraSystem = ECSHandler::getSystem<SFE::SystemsModule::CameraSystem>();
		auto camera = cameraSystem ? ECSHandler::registry().pinComponent<const CameraComponent>(cameraSystem->getCurrentCamera()) : ecss::PinnedComponent<const CameraComponent>{};
		if (!camera) {
			return;
		}

		component->shadowCascadeLevels.clear();
		for (auto level : data["cascaded_levels"]) {
			component->shadowCascadeLevels.emplace_back(level.asFloat());
		}

		component->resolution = { data["resolution"][0].asFloat(), data["resolution"][1].asFloat() };

		component->updateCascades(camera->getProjection());

		int i = 0;
		for (auto& cascade : component->cascades) {
			auto& cascadeData = data["cascades"][i++];
			cascade.bias = cascadeData["bias"].asFloat();
			cascade.samples = cascadeData["samples"].asInt();

			cascade.texelSize = { cascadeData["texel_size"][0].asFloat(), cascadeData["texel_size"][1].asFloat() };
			cascade.zMult = { cascadeData["z_mult"][0].asFloat(), cascadeData["z_mult"][1].asFloat() };
		}

		component->shadowIntensity = data["shadow_intensity"].asFloat();
	}

	// ===== MaterialComponent ==================================================

	void ComponentSerializer<MaterialComponent>::save(const MaterialComponent& component, Json::Value& data) {
		data["transparent"] = component.transparent;
		data["fragment_shader"] = component.fragmentShader;
		data["casts_shadow"] = component.castsShadow;
		data["opacity"] = component.opacity;
		data["ior"] = component.ior;
		data["tint"] = Json::arrayValue;
		data["tint"].append(component.tint.x);
		data["tint"].append(component.tint.y);
		data["tint"].append(component.tint.z);
	}

	void ComponentSerializer<MaterialComponent>::load(ecss::EntityId entity, const Json::Value& data) {
		auto pinned = ECSHandler::registry().pinComponent<MaterialComponent>(entity);
		MaterialComponent* material = pinned.get();
		if (!material) {
			material = ECSHandler::addComponent<MaterialComponent>(entity);
		}
		if (!material) {
			return;
		}

		if (data.isMember("transparent")) {
			material->transparent = data["transparent"].asBool();
		}
		if (data.isMember("fragment_shader")) {
			material->fragmentShader = data["fragment_shader"].asString();
		}
		if (data.isMember("casts_shadow")) {
			material->castsShadow = data["casts_shadow"].asBool();
		}
		if (data.isMember("opacity")) {
			material->opacity = data["opacity"].asFloat();
		}
		if (data.isMember("ior")) {
			material->ior = data["ior"].asFloat();
		}
		if (data.isMember("tint") && data["tint"].isArray() && data["tint"].size() >= 3) {
			material->tint = {
				data["tint"][0].asFloat(),
				data["tint"][1].asFloat(),
				data["tint"][2].asFloat()
			};
		}

		// The mesh carries its own copy of the material values, built from the asset by
		// ModelBinding::bind(); the scene's overrides have to reach it too.
		if (auto mesh = ECSHandler::registry().pinComponent<MeshComponent>(entity)) {
			for (auto& meshMaterial : mesh->meshMaterials) {
				if (data.isMember("transparent") && material->transparent) {
					meshMaterial.transparent = true;
				}
				if (data.isMember("casts_shadow")) {
					meshMaterial.castsShadow = material->castsShadow;
				}
				if (material->transparent) {
					if (data.isMember("fragment_shader")) {
						meshMaterial.fragmentShader = material->fragmentShader;
					}
					if (data.isMember("opacity")) {
						meshMaterial.opacity = material->opacity;
					}
					if (data.isMember("ior")) {
						meshMaterial.ior = material->ior;
					}
					if (data.isMember("tint")) {
						meshMaterial.tint = material->tint;
					}
				}
			}
			// The renderer took the mesh's materials when it was bound, before these: it takes them again.
			SFE::SystemsModule::TasksManager::instance()->notify({ entity, SFE::SystemsModule::TaskType::MATERIAL_UPDATED });
		}
	}

	// ===== SkyComponent =======================================================

	void ComponentSerializer<SkyComponent>::save(const SkyComponent& component, Json::Value& data) {
		data["sun_elevation"] = component.sunElevation;
		data["sun_azimuth"] = component.sunAzimuth;
		data["cloud_coverage"] = component.cloudCoverage;
		data["mist"] = component.mist;
		data["mist_height"] = component.mistHeight;
		data["haze"] = component.haze;
	}

	void ComponentSerializer<SkyComponent>::load(ecss::EntityId entity, const Json::Value& data) {
		auto component = ECSHandler::registry().addComponent<SkyComponent>(entity);
		if (!component) {
			return;
		}
		component->sunElevation = data.get("sun_elevation", component->sunElevation).asFloat();
		component->sunAzimuth = data.get("sun_azimuth", component->sunAzimuth).asFloat();
		component->cloudCoverage = data.get("cloud_coverage", component->cloudCoverage).asFloat();
		component->mist = data.get("mist", component->mist).asFloat();
		component->mistHeight = data.get("mist_height", component->mistHeight).asFloat();
		component->haze = data.get("haze", component->haze).asFloat();
	}

	// ===== TerrainComponent ===================================================

	void ComponentSerializer<TerrainComponent>::save(const TerrainComponent& component, Json::Value& data) {
		data["size"] = component.size;
		data["resolution"] = component.resolution;
		data["lake_width"] = component.lakeWidth;
		data["lake_length"] = component.lakeLength;
		data["lake_depth"] = component.lakeDepth;
		data["hill_height"] = component.hillHeight;
		data["seed"] = component.seed;
		data["road"] = Json::arrayValue;
		for (const auto& point : component.road) {
			Json::Value item = Json::arrayValue;
			item.append(point.x);
			item.append(point.y);
			data["road"].append(item);
		}
		data["clearings"] = Json::arrayValue;
		for (const auto& clearing : component.clearings) {
			Json::Value item = Json::arrayValue;
			item.append(clearing.x);
			item.append(clearing.y);
			item.append(clearing.z);
			data["clearings"].append(item);
		}
		data["paths"] = Json::arrayValue;
		for (const auto& path : component.paths) {
			Json::Value item;
			item["width"] = path.width;
			item["wear"] = path.wear;
			item["points"] = Json::arrayValue;
			for (const auto& point : path.points) {
				Json::Value at = Json::arrayValue;
				at.append(point.x);
				at.append(point.y);
				item["points"].append(at);
			}
			data["paths"].append(item);
		}
		data["patches"] = Json::arrayValue;
		for (const auto& patch : component.patches) {
			Json::Value item = Json::arrayValue;
			item.append(patch.x);
			item.append(patch.y);
			item.append(patch.z);
			item.append(patch.w);
			data["patches"].append(item);
		}
	}

	void ComponentSerializer<TerrainComponent>::load(ecss::EntityId entity, const Json::Value& data) {
		auto component = ECSHandler::registry().addComponent<TerrainComponent>(entity);
		if (!component) {
			return;
		}
		component->size = data.get("size", component->size).asFloat();
		component->resolution = data.get("resolution", component->resolution).asInt();
		component->lakeWidth = data.get("lake_width", component->lakeWidth).asFloat();
		component->lakeLength = data.get("lake_length", component->lakeLength).asFloat();
		component->lakeDepth = data.get("lake_depth", component->lakeDepth).asFloat();
		component->hillHeight = data.get("hill_height", component->hillHeight).asFloat();
		component->seed = data.get("seed", component->seed).asInt();
		component->road.clear();
		for (const auto& point : data["road"]) {
			component->road.push_back({ point[0].asFloat(), point[1].asFloat() });
		}
		component->clearings.clear();
		for (const auto& clearing : data["clearings"]) {
			component->clearings.push_back({ clearing[0].asFloat(), clearing[1].asFloat(), clearing[2].asFloat() });
		}
		component->paths.clear();
		for (const auto& item : data["paths"]) {
			auto& path = component->paths.emplace_back();
			path.width = item.get("width", path.width).asFloat();
			path.wear = item.get("wear", path.wear).asFloat();
			for (const auto& point : item["points"]) {
				path.points.push_back({ point[0].asFloat(), point[1].asFloat() });
			}
		}
		component->patches.clear();
		for (const auto& patch : data["patches"]) {
			component->patches.push_back({ patch[0].asFloat(), patch[1].asFloat(), patch[2].asFloat(),
				patch.size() > 3 ? patch[3].asFloat() : kDefaultWear });
		}

		auto* model = AssetsModule::getOrCreateTerrain(*component);
		auto modelComp = ECSHandler::registry().addComponent<ModelComponent>(entity, entity);
		if (!model || !modelComp) {
			return;
		}
		modelComp->mPath = model->assetPath;
		modelComp->setModel(model->getLODs());
		ModelBinding::bind(entity);

		// Painted by its own shader from world position and height, with the ground materials as texture arrays.
		if (auto mesh = ECSHandler::registry().pinComponent<MeshComponent>(entity)) {
			for (auto& material : mesh->meshMaterials) {
				material.fragmentShader = AssetsModule::kTerrainFS;
			}
		}
		SFE::SystemsModule::TasksManager::instance()->notify({ entity, SFE::SystemsModule::TaskType::MATERIAL_UPDATED });
	}

	// ===== WaterComponent =====================================================

	void ComponentSerializer<WaterComponent>::save(const WaterComponent& component, Json::Value& data) {
		data["size"] = component.size;
		data["clarity"] = component.clarity;
		data["drift"] = component.drift;
	}

	void ComponentSerializer<WaterComponent>::load(ecss::EntityId entity, const Json::Value& data) {
		auto component = ECSHandler::registry().addComponent<WaterComponent>(entity);
		if (!component) {
			return;
		}
		component->size = data.get("size", component->size).asFloat();
		component->clarity = data.get("clarity", component->clarity).asFloat();
		component->drift = data.get("drift", component->drift).asFloat();
	}

	// ===== CharacterComponent =================================================

	// ===== CharacterAppearanceComponent =======================================

	// ===== VegetationComponent ================================================


	namespace {
		constexpr std::pair<std::string_view, Sway> kSways[] = { { "none", Sway::None }, { "tree", Sway::Tree }, { "plant", Sway::Plant } };

		std::string_view swayName(Sway sway) {
			for (const auto& [name, value] : kSways) {
				if (value == sway) {
					return name;
				}
			}
			return kSways[0].first;
		}

		Sway swayFromName(const std::string& name) {
			for (const auto& [candidate, value] : kSways) {
				if (candidate == name) {
					return value;
				}
			}
			return Sway::None;
		}
	}

	// ===== WindComponent ======================================================

	void ComponentSerializer<WindComponent>::save(const WindComponent& component, Json::Value& data) {
		data["direction"] = component.direction;
		data["strength"] = component.strength;
	}

	void ComponentSerializer<WindComponent>::load(ecss::EntityId entity, const Json::Value& data) {
		auto component = ECSHandler::registry().addComponent<WindComponent>(entity);
		if (!component) {
			return;
		}
		component->direction = data.get("direction", component->direction).asFloat();
		component->strength = data.get("strength", component->strength).asFloat();
	}

	// ===== AudioSourceComponent ===============================================

	void ComponentSerializer<AudioSourceComponent>::save(const AudioSourceComponent& component, Json::Value& data) {
		data["path"] = component.mPath;
		data["volume"] = component.mVolume;
		data["pitch"] = component.mPitch;
		data["min_distance"] = component.mMinDistance;
		data["max_distance"] = component.mMaxDistance;
		data["loop"] = component.mLoop;
		data["spatial"] = component.mSpatial;
		data["stream"] = component.mStream;
		data["playing"] = component.mPlaying;
	}

	void ComponentSerializer<AudioSourceComponent>::load(ecss::EntityId entity, const Json::Value& data) {
		auto component = ECSHandler::registry().addComponent<AudioSourceComponent>(entity);
		if (!component) {
			return;
		}
		component->mPath = data.get("path", component->mPath).asString();
		component->mVolume = data.get("volume", component->mVolume).asFloat();
		component->mPitch = data.get("pitch", component->mPitch).asFloat();
		component->mMinDistance = data.get("min_distance", component->mMinDistance).asFloat();
		component->mMaxDistance = data.get("max_distance", component->mMaxDistance).asFloat();
		component->mLoop = data.get("loop", component->mLoop).asBool();
		component->mSpatial = data.get("spatial", component->mSpatial).asBool();
		component->mStream = data.get("stream", component->mStream).asBool();
		component->mPlaying = data.get("playing", component->mPlaying).asBool();
	}

	// ===== SoundscapeComponent ================================================

	// ===== GarbageComponent ===================================================

	// ===== GrassComponent =====================================================

	// ===== The table ==========================================================

	namespace {
		// The engine's own, and the games' after them: a game's component may be read only once the
		// engine's are in, and the order here is the order both directions walk.
		std::vector<ComponentRegistry::Entry>& table() {
			static std::vector<ComponentRegistry::Entry> table = ComponentRegistry::engineEntries();
			return table;
		}
	}

	void ComponentRegistry::add(Entry entry) {
		table().push_back(entry);
	}

	const std::vector<ComponentRegistry::Entry>& ComponentRegistry::entries() {
		return table();
	}

	std::vector<ComponentRegistry::Entry> ComponentRegistry::engineEntries() {
		return {
			entryFor<ModelComponent>(LoadPhase::Direct),
			entryFor<TransformComponent>(LoadPhase::Direct),
			entryFor<LightSourceComponent>(LoadPhase::Direct),
			// Saved for inspection only: restoring it needs the camera projection the cascades
			// are split against, which a scene load does not have yet. CascadedShadowPass
			// restores it from its own file, through deserializeProperty().
			entryFor<CascadeShadowComponent>(LoadPhase::Never),
			// After ModelBinding::bind(): the values are written through into the mesh materials
			// it just built.
			entryFor<MaterialComponent>(LoadPhase::AfterBind),
			entryFor<SkyComponent>(LoadPhase::Direct),
			// After bind: it builds its own mesh and binds it.
			entryFor<TerrainComponent>(LoadPhase::AfterBind),
			entryFor<WaterComponent>(LoadPhase::Direct),
			entryFor<WindComponent>(LoadPhase::Direct),
			entryFor<AudioSourceComponent>(LoadPhase::Direct),
		};
	}
}
