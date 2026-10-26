#include "propertiesModule/PropertiesSystem.h"
#include "propertiesModule/ModelBinding.h"
#include "propertiesModule/SceneEvents.h"
#include "core/FileSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "assetsModule/AssetsManager.h"
#include "assetsModule/modelModule/MeshVaoRegistry.h"
#include "assetsModule/modelModule/ModelLoader.h"
#include "componentsModule/ArmatureComponent.h"
#include "componentsModule/CascadeShadowComponent.h"
#include "componentsModule/DebugDataComponent.h"
#include "componentsModule/IsDrawableComponent.h"
#include "componentsModule/LightSourceComponent.h"
#include "componentsModule/MaterialComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/ModelComponent.h"
#include "systemsModule/systems/OcTreeSystem.h"
#include "systemsModule/systems/PhysicsSystem.h"
#include "systemsModule/systems/RenderSystem.h"
#include "componentsModule/OcTreeComponent.h"
#include "componentsModule/TransformComponent.h"
#include "componentsModule/Hierarchy.h"
#include "core/ECSHandler.h"
#include "core/LoadTrace.h"
#include "core/Engine.h"
#include "rhi/GraphicsBackend.h"
#include "multithreading/ThreadPool.h"
#include "componentsModule/CameraComponent.h"
#include "mathModule/VectorOperations.h"
#include "systemsModule/systems/CameraSystem.h"
#include "systemsModule/TasksManager.h"

namespace SFE::PropertiesModule {
	namespace {
		AssetsModule::Model* getOrCreateUnitSphere() {
			constexpr const char* kPath = "builtin:sphere";
			if (auto* existing = AssetsModule::AssetsManager::instance()->getAsset<AssetsModule::Model>(kPath)) {
				return existing;
			}

			constexpr int stacks = 32;
			constexpr int slices = 48;
			constexpr float pi = 3.14159265f;

			auto makeVert = [](float u, float v, float x, float y, float z) {
				SFE::Vertex3D vert{};
				vert.position = { x, y, z };
				vert.normal = vert.position;
				vert.texCoords = { u, 1.f - v };
				vert.texCoords1 = vert.texCoords;
				const float lenXZ = std::sqrt(x * x + z * z);
				if (lenXZ > 1e-4f) {
					vert.tangent = { -z / lenXZ, 0.f, x / lenXZ };
				}
				else {
					vert.tangent = { 1.f, 0.f, 0.f };
				}
				vert.biTangent = Math::cross(vert.normal, vert.tangent);
				return vert;
			};

			SFE::MeshObject3D meshObj;
			meshObj.mesh.vertices.push_back(makeVert(0.5f, 0.f, 0.f, 1.f, 0.f));
			for (int i = 1; i < stacks; ++i) {
				const float v = static_cast<float>(i) / static_cast<float>(stacks);
				const float phi = v * pi;
				const float sp = std::sin(phi);
				const float cp = std::cos(phi);
				for (int j = 0; j <= slices; ++j) {
					const float u = static_cast<float>(j) / static_cast<float>(slices);
					const float theta = u * 2.f * pi;
					meshObj.mesh.vertices.push_back(makeVert(u, v, sp * std::cos(theta), cp, sp * std::sin(theta)));
				}
			}
			meshObj.mesh.vertices.push_back(makeVert(0.5f, 1.f, 0.f, -1.f, 0.f));

			const unsigned ring = static_cast<unsigned>(slices + 1);
			const unsigned south = static_cast<unsigned>(meshObj.mesh.vertices.size() - 1);
			for (int j = 0; j < slices; ++j) {
				const unsigned a = 1u + static_cast<unsigned>(j);
				meshObj.mesh.indices.push_back(0);
				meshObj.mesh.indices.push_back(a);
				meshObj.mesh.indices.push_back(a + 1);
			}
			const int rings = stacks - 1;
			for (int i = 0; i < rings - 1; ++i) {
				for (int j = 0; j < slices; ++j) {
					const unsigned a = 1u + static_cast<unsigned>(i) * ring + static_cast<unsigned>(j);
					const unsigned b = a + ring;
					meshObj.mesh.indices.push_back(a);
					meshObj.mesh.indices.push_back(b);
					meshObj.mesh.indices.push_back(a + 1);
					meshObj.mesh.indices.push_back(a + 1);
					meshObj.mesh.indices.push_back(b);
					meshObj.mesh.indices.push_back(b + 1);
				}
			}
			const unsigned lastRing = 1u + static_cast<unsigned>(rings - 1) * ring;
			for (int j = 0; j < slices; ++j) {
				meshObj.mesh.indices.push_back(south);
				meshObj.mesh.indices.push_back(lastRing + static_cast<unsigned>(j) + 1);
				meshObj.mesh.indices.push_back(lastRing + static_cast<unsigned>(j));
			}
			meshObj.aabb = SFE::FrustumModule::AABB({ -1.f, -1.f, -1.f }, { 1.f, 1.f, 1.f });

			SFE::Tree<SFE::MeshObject3D> tree;
			tree.value = std::move(meshObj);
			return AssetsModule::AssetsManager::instance()->createAsset<AssetsModule::Model>(
				kPath, std::move(tree), AssetsModule::Armature{}, std::vector<AssetsModule::Animation>{});
		}

		// A cube from -1 to 1, one quad per face so the normals are flat.
		AssetsModule::Model* getOrCreateUnitBox() {
			constexpr const char* kPath = "builtin:box";
			if (auto* existing = AssetsModule::AssetsManager::instance()->getAsset<AssetsModule::Model>(kPath)) {
				return existing;
			}

			const Math::Vec3 normals[6] = { {1.f,0.f,0.f}, {-1.f,0.f,0.f}, {0.f,1.f,0.f}, {0.f,-1.f,0.f}, {0.f,0.f,1.f}, {0.f,0.f,-1.f} };
			const Math::Vec3 tangents[6] = { {0.f,0.f,-1.f}, {0.f,0.f,1.f}, {1.f,0.f,0.f}, {1.f,0.f,0.f}, {1.f,0.f,0.f}, {-1.f,0.f,0.f} };

			SFE::MeshObject3D meshObj;
			for (int face = 0; face < 6; ++face) {
				const auto n = normals[face];
				const auto t = tangents[face];
				const auto b = Math::cross(n, t);
				const unsigned first = static_cast<unsigned>(meshObj.mesh.vertices.size());
				for (int corner = 0; corner < 4; ++corner) {
					const float u = (corner == 1 || corner == 2) ? 1.f : 0.f;
					const float v = (corner >= 2) ? 1.f : 0.f;
					SFE::Vertex3D vert{};
					vert.position = n + t * (u * 2.f - 1.f) + b * (v * 2.f - 1.f);
					vert.normal = n;
					vert.tangent = t;
					vert.biTangent = b;
					vert.texCoords = { u, v };
					vert.texCoords1 = vert.texCoords;
					meshObj.mesh.vertices.push_back(vert);
				}
				for (const unsigned idx : { 0u, 1u, 2u, 0u, 2u, 3u }) {
					meshObj.mesh.indices.push_back(first + idx);
				}
			}
			meshObj.aabb = SFE::FrustumModule::AABB({ -1.f, -1.f, -1.f }, { 1.f, 1.f, 1.f });

			SFE::Tree<SFE::MeshObject3D> tree;
			tree.value = std::move(meshObj);
			return AssetsModule::AssetsManager::instance()->createAsset<AssetsModule::Model>(
				kPath, std::move(tree), AssetsModule::Armature{}, std::vector<AssetsModule::Animation>{});
		}

		void markGlass(ecss::EntityId entity, float opacity, float ior, const Math::Vec3& tint);

		ecss::EntityId spawnModelEntity(AssetsModule::Model* model, const char* path, const Math::Vec3& pos, const Math::Vec3& scale, bool glass,
			const Math::Vec3& rotate = {}) {
			if (!SFE::Engine::instance()->isAlive() || !model) {
				return {};
			}

			auto entity = ECSHandler::registry().takeEntity();
			auto aabbCmp = ECSHandler::addComponent<SFE::ComponentsModule::AABBComponent>(entity);
			aabbCmp->aabbScale = 1.f;
			ECSHandler::addComponent<OcTreeComponent>(entity);
			auto modelComp = ECSHandler::addComponent<ModelComponent>(entity, entity);
			modelComp->mPath = path;
			modelComp->boneMatrices = model->getDefaultBoneMatrices();
			modelComp->armature = model->getArmature();
			modelComp->setModel(model->getLODs());

			auto transformComp = ECSHandler::addComponent<TransformComponent>(entity, entity);
			transformComp->setPos(pos);
			transformComp->setScale(scale);
			transformComp->setRotate(rotate);
			// A model spawned before RenderSystem exists never reaches it through the transform notify, so the render
			// copy is filled here rather than left to the notify. It is computed from the spawn values, the way
			// reloadTransform does for an entity with no parent, instead of reloading the component: the setters above
			// have already woken TransformSystem, whose worker may be reloading the same component right now.
			const auto rotation = Math::Quaternion<float>::fromEulerDegrees(rotate);
			const auto seed = Math::translate(Math::Mat4{ 1.f }, pos) * rotation.toMat4() * Math::scale(Math::Mat4{ 1.f }, scale);
			ECSHandler::registry().addComponent<ComponentsModule::TransformMatComp>(entity, seed);
			ECSHandler::addComponent<IsDrawableComponent>(entity);

			ModelBinding::bind(entity);
			if (glass) {
				markGlass(entity, 0.22f, 1.52f, { 1.f, 1.f, 1.f });
			}
			return entity;
		}

		void spawnModelEntity(const char* path, const Math::Vec3& pos, const Math::Vec3& scale, bool glass) {
			spawnModelEntity(AssetsModule::ModelLoader::instance()->load(path), path, pos, scale, glass);
		}

		ecss::EntityId spawnDancingVampire(const Math::Vec3& pos, float yawDegrees, float animPhase = 0.f) {
			constexpr const char* kPath = "models/vampire.fbx";
			auto* model = AssetsModule::ModelLoader::instance()->load(kPath);
			if (!model) {
				return {};
			}

			const auto id = spawnModelEntity(model, kPath, pos, { 0.1f }, false, { 0.f, yawDegrees, 0.f });
			if (!id) {
				return {};
			}

			if (const auto* clip = ModelBinding::playbackClip(model)) {
				auto* anim = ECSHandler::registry().addComponent<ComponentsModule::AnimationComponent>(id);
				anim->mCurrentAnimation = clip;
				anim->mLoop = true;
				anim->play(animPhase);
				SFE::SystemsModule::TasksManager::instance()->notify({ id, SFE::SystemsModule::TaskType::ANIMATION_UPDATED });
			}

			if (auto debug = ECSHandler::registry().pinComponent<DebugDataComponent>(id)) {
				debug->stringId = "dancer";
			}
			return id;
		}

		void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
			h = h - std::floor(h);
			const float c = v * s;
			const float x = c * (1.f - std::fabs(std::fmod(h * 6.f, 2.f) - 1.f));
			const float m = v - c;
			float rf = 0.f, gf = 0.f, bf = 0.f;
			const int sector = static_cast<int>(h * 6.f) % 6;
			switch (sector) {
			case 0: rf = c; gf = x; break;
			case 1: rf = x; gf = c; break;
			case 2: gf = c; bf = x; break;
			case 3: gf = x; bf = c; break;
			case 4: rf = x; bf = c; break;
			default: rf = c; bf = x; break;
			}
			r = rf + m;
			g = gf + m;
			b = bf + m;
		}

		void markGlass(ecss::EntityId entity, float opacity, float ior, const Math::Vec3& tint) {
			if (auto mat = ECSHandler::registry().pinComponent<ComponentsModule::MaterialComponent>(entity)) {
				mat->transparent = true;
				mat->opacity = opacity;
				mat->ior = ior;
				mat->tint = tint;
			}
			if (auto mesh = ECSHandler::registry().pinComponent<MeshComponent>(entity)) {
				for (auto& mm : mesh->meshMaterials) {
					mm.transparent = true;
					mm.opacity = opacity;
					mm.ior = ior;
					mm.tint = tint;
				}
			}
			SFE::SystemsModule::TasksManager::instance()->notify({ entity, SFE::SystemsModule::TaskType::MATERIAL_UPDATED });
		}

		void applyGlassTint(ecss::EntityId entity, uint32_t hash) {
			if (!entity) {
				return;
			}
			const float hue = static_cast<float>(hash % 12u) / 12.f;
			const float sat = 0.55f + static_cast<float>((hash >> 4) & 3u) * 0.12f;
			const float val = 0.75f + static_cast<float>((hash >> 6) & 3u) * 0.08f;
			float r = 1.f, g = 1.f, b = 1.f;
			hsvToRgb(hue, sat, val, r, g, b);
			markGlass(entity, 0.22f, 1.52f, { r, g, b });
		}

		void makeSphereLamp(ecss::EntityId entity, float spacing) {
			if (!entity) {
				return;
			}
			auto light = ECSHandler::addComponent<LightSourceComponent>(entity, entity, ComponentsModule::eLightType::POINT);
			light->setLightColor({ 6.5f, 4.0f, 1.8f });
			light->setIntensity(1.f);
			light->mRadius = (std::max)(28.f, spacing * 5.5f);
			light->mNear = 0.2f;
			light->mLinear = 0.07f;
			light->mQuadratic = 0.017f;
			light->setSamples(8);
			light->setBias(0.0002f);
			light->setTexelSize({ 0.004f, 0.004f });
			light->mWithShadows = true;

			auto debug = ECSHandler::addComponent<DebugDataComponent>(entity);
			debug->stringId = "lamp";
		}

		// What an entity's transform makes of it, from its values as they are now: not from the rotation the transform
		// system works out from them on its worker, a frame later.
		Math::Mat4 localOf(const TransformComponent& transform) {
			return Math::translate(Math::Mat4{ 1.f }, transform.getPos())
				* Math::Quaternion<float>::fromEulerDegrees(transform.getRotate()).toMat4()
				* Math::scale(Math::Mat4{ 1.f }, transform.getScale());
		}

		// Where an entity stands in the world, as its own transform and its parents' are now.
		Math::Mat4 worldOf(ecss::EntityId entity) {
			Math::Mat4 world{ 1.f };
			for (auto at = entity; at != ecss::INVALID_ID; at = Hierarchy::parent(at)) {
				if (const auto transform = ECSHandler::registry().pinComponent<const TransformComponent>(at)) {
					world = localOf(*transform.get()) * world;
				}
			}
			return world;
		}

		// The scene loadScene read last: from where, made into what, and as it now runs -- the file as it was read, with
		// what reloadScene has taken up of it since.
		struct LoadedScene {
			std::string path;
			ecss::EntityId root = ecss::INVALID_ID;
			Json::Value read;
		};
		LoadedScene& loadedScene() {
			static LoadedScene scene;
			return scene;
		}

		// The components that take the scene being read again. The sky and the wind are read where they are used, every
		// frame, so being read again is all they need.
		std::unordered_map<std::string, PropertiesSystem::Reloadable>& reloadables() {
			static std::unordered_map<std::string, PropertiesSystem::Reloadable> table{
				{ "SkyComponent", { [](ecss::EntityId, const Json::Value&) { return true; }, {} } },
				{ "WindComponent", { [](ecss::EntityId, const Json::Value&) { return true; }, {} } },
			};
			return table;
		}

		constexpr const char* kTransform = "TransformComponent";
		constexpr const char* kModel = "ModelComponent";

		// Whether a "Properties" object holds nothing but where an entity stands and what it looks like.
		bool placedOnly(const Json::Value& properties) {
			for (const auto& name : properties.getMemberNames()) {
				if (name != kTransform && name != kModel) {
					return false;
				}
			}
			return true;
		}

		// An entity of nothing but where it stands and what it looks like, and its children the same: no system has
		// built anything of its own from it, so it may be made and taken out while the game runs.
		bool plain(const Json::Value& node) {
			if (!placedOnly(node["Properties"])) {
				return false;
			}
			for (const auto& child : node["Children"]) {
				if (!plain(child)) {
					return false;
				}
			}
			return true;
		}

		// What reloadScene did, and what it left for a restart ("id component what").
		struct Reload {
			int moved = 0;
			int swapped = 0;
			int made = 0;
			int removed = 0;
			int read = 0;
			std::vector<std::string> waiting;
			std::vector<ecss::EntityId> gone;      // to be taken out, children with them
			std::vector<ecss::EntityId> colliders; // whose static colliders are built again
		};

		// An entity as the file has it made under `parent`, as loadScene makes one: the render copy of its transform
		// seeded, its children's too.
		void make(ecss::EntityId parent, const Json::Value& node, Reload& reload) {
			auto& registry = ECSHandler::registry();
			const auto entity = registry.takeEntity();
			PropertiesSystem::fillTree(entity, node);
			Hierarchy::setParent(entity, parent);
			auto nodes = Hierarchy::descendants(entity);
			nodes.insert(nodes.begin(), entity);
			for (const auto id : nodes) {
				if (registry.hasComponent<TransformComponent>(id) && !registry.hasComponent<ComponentsModule::TransformMatComp>(id)) {
					registry.addComponent<ComponentsModule::TransformMatComp>(id, worldOf(id));
				}
				reload.colliders.push_back(id);
			}
			++reload.made;
		}

		// One entity of the scene: `was`, the file as last taken up, against `now`, the file read again. What changed is
		// taken up where it can be, and the entity answered as it now runs: as `now` has it, but as `was` had what waits.
		Json::Value reloadNode(ecss::EntityId entity, const Json::Value& was, const Json::Value& now, Reload& reload) {
			auto& registry = ECSHandler::registry();
			const auto id = now.get("id", "").asString();
			const auto& before = was["Properties"];
			const auto& after = now["Properties"];
			Json::Value kept = now;
			kept.removeMember("Children");
			const auto wait = [&](const char* name, const std::string& what) {
				reload.waiting.push_back(id + " " + name + " " + what);
				if (before.isMember(name)) {
					kept["Properties"][name] = before[name];
				}
				else {
					kept["Properties"].removeMember(name);
				}
			};

			// Moved, turned or scaled: set there, when everything else on it can follow -- as its serializer reads it,
			// what the file leaves out at its default.
			if (before[kTransform] != after[kTransform]) {
				std::vector<const PropertiesSystem::Reloadable*> following;
				std::string keeps;
				for (const auto& name : after.getMemberNames()) {
					if (name == kTransform || name == kModel) {
						continue;
					}
					const auto found = reloadables().find(name);
					if (found == reloadables().end() || !found->second.moved) {
						keeps += (keeps.empty() ? "" : ", ") + name;
						continue;
					}
					following.push_back(&found->second);
				}
				bool set = false;
				if (auto transform = registry.pinComponent<TransformComponent>(entity); transform && after.isMember(kTransform) && keeps.empty()) {
					const auto& data = after[kTransform];
					const auto value = [&](const char* key, const Math::Vec3& otherwise) {
						const auto* array = JsonUtils::getValueArray(data, key);
						return array ? JsonUtils::getVec3(*array) : otherwise;
					};
					transform->setScale(value("Scale", Math::Vec3{ 1.f }));
					transform->setPos(value("Pos", Math::Vec3{ 0.f }));
					transform->setRotate(value("Rotate", Math::Vec3{ 0.f }));
					set = true;
				}
				if (!set) {
					wait(kTransform, keeps.empty() ? "changed" : "changed, and what is on it cannot follow: " + keeps);
				}
				else {
					const auto world = worldOf(entity);
					for (const auto* follower : following) {
						follower->moved(entity, world);
					}
					reload.colliders.push_back(entity);
					for (const auto child : Hierarchy::descendants(entity)) {
						reload.colliders.push_back(child);
					}
					++reload.moved;
				}
			}

			// Another model, or its collider on or off: only on an entity of nothing else, which no system took anything
			// from.
			if (before[kModel] != after[kModel]) {
				const auto& data = after[kModel];
				const auto path = data.get("ModelPath", "").asString();
				auto* model = path.empty() ? nullptr : AssetsModule::ModelLoader::instance()->load(path);
				if (!after.isMember(kModel) || !placedOnly(after) || !model) {
					wait(kModel, !after.isMember(kModel) ? "taken off" : !model ? "does not read" : "changed");
				}
				else {
					if (auto component = registry.pinComponent<ModelComponent>(entity)) {
						component->mPath = path;
						component->collider = data.get("Collider", false).asBool();
					}
					else {
						ComponentSerializer<ModelComponent>::load(entity, data);
					}
					ModelBinding::setModel(entity, *model);
					reload.colliders.push_back(entity);
					++reload.swapped;
				}
			}

			// The rest in the order a load reads them, each as it says it takes being read again.
			for (const auto phase : { LoadPhase::Direct, LoadPhase::AfterBind }) {
				for (const auto& entry : ComponentRegistry::entries()) {
					const char* name = entry.name.data();
					if (entry.loadPhase != phase || entry.name == kTransform || entry.name == kModel || before[name] == after[name]) {
						continue;
					}
					const auto found = reloadables().find(name);
					if (!before.isMember(name) || !after.isMember(name)) {
						wait(name, after.isMember(name) ? "put on" : "taken off");
					}
					else if (found == reloadables().end() || !found->second.read) {
						wait(name, "changed");
					}
					else {
						entry.load(entity, after[name]);
						if (found->second.read(entity, before[name])) {
							++reload.read;
						}
						else {
							entry.load(entity, before[name]);
							wait(name, "changed");
						}
					}
				}
			}

			// Its children, known by their ids: one in both taken up in turn; one new to the file made and one gone from
			// it taken out, when it is plain -- else that waits too. One without an id is left as it was made.
			std::unordered_map<std::string, ecss::EntityId> running;
			for (const auto child : Hierarchy::children(entity)) {
				if (const auto debug = registry.pinComponent<const DebugDataComponent>(child)) {
					running.emplace(debug->stringId, child);
				}
			}
			std::unordered_map<std::string, const Json::Value*> earlier;
			for (const auto& child : was["Children"]) {
				earlier.emplace(child.get("id", "").asString(), &child);
			}
			Json::Value children = Json::arrayValue;
			std::unordered_set<std::string> named;
			for (const auto& child : now["Children"]) {
				const auto childId = child.get("id", "").asString();
				named.insert(childId);
				const auto old = earlier.find(childId);
				const auto live = running.find(childId);
				if (childId.empty()) {
					children.append(child);
				}
				else if (old == earlier.end()) {
					if (plain(child)) {
						make(entity, child, reload);
						children.append(child);
					}
					else {
						reload.waiting.push_back(childId + " new");
					}
				}
				else if (live != running.end()) {
					children.append(reloadNode(live->second, *old->second, child, reload));
				}
				else {
					children.append(*old->second);
				}
			}
			for (const auto& child : was["Children"]) {
				const auto childId = child.get("id", "").asString();
				const auto live = running.find(childId);
				if (childId.empty() || named.contains(childId) || live == running.end()) {
					continue;
				}
				if (plain(child)) {
					reload.gone.push_back(live->second);
					++reload.removed;
				}
				else {
					reload.waiting.push_back(childId + " taken out");
					children.append(child);
				}
			}
			if (!children.empty()) {
				kept["Children"] = std::move(children);
			}
			return kept;
		}
	}

	ecss::EntityId PropertiesSystem::loadScene(std::string_view path) {
		if (!FileSystem::isFileExists(path)) {
			return {};
		}

		auto scene = ECSHandler::registry().takeEntity();
		Json::Value read;
		{
			LoadTrace::Step step("scene file");
			read = FileSystem::readJson(path);
		}
		{
			LoadTrace::Step step("entities and components");
			fillTree(scene, read);
		}
		loadedScene() = { std::string(path), scene, read };

		// The same as spawnModelEntity: a scene loaded before RenderSystem exists never reaches it through the
		// transform notify, so the render copy of every transform is seeded here, parents before children.
		std::unordered_map<ecss::EntityId, Math::Mat4> world;
		auto nodes = Hierarchy::descendants(scene);
		nodes.insert(nodes.begin(), scene);
		{
		LoadTrace::Step seeding("world transforms");
		for (const auto id : nodes) {
			const auto transform = ECSHandler::registry().pinComponent<const TransformComponent>(id);
			if (!transform) {
				continue;
			}
			const auto local = localOf(*transform.get());
			const auto parent = world.find(Hierarchy::parent(id));
			const auto matrix = parent != world.end() ? parent->second * local : local;
			world[id] = matrix;
			if (!ECSHandler::registry().hasComponent<ComponentsModule::TransformMatComp>(id)) {
				ECSHandler::registry().addComponent<ComponentsModule::TransformMatComp>(id, matrix);
			}
		}
		}
		LoadTrace::count("entities", static_cast<long long>(nodes.size()));

		// Whatever takes something of a scene (the physics: its static colliders) takes it of this one now, every entity of
		// it standing where the file puts it; in the order they subscribed (SceneEvents.h).
		{
			LoadTrace::Step listeners("scene listeners");
			emitSceneLoaded({ std::string(path), scene, std::move(nodes) });
		}
		return scene;
	}

	std::string PropertiesSystem::reloadScene() {
		auto& scene = loadedScene();
		if (scene.root == ecss::INVALID_ID || !ECSHandler::registry().contains(scene.root)) {
			return "no scene loaded";
		}
		if (!FileSystem::isFileExists(scene.path)) {
			return "no file at " + scene.path;
		}
		const auto fresh = FileSystem::readJson(scene.path);
		if (!fresh.isObject()) {
			return scene.path + " does not read";
		}

		Reload reload;
		auto kept = reloadNode(scene.root, scene.read, fresh, reload);

		// What went from the file taken out with its children: off the octree, then destroyed.
		if (!reload.gone.empty()) {
			std::vector<ecss::EntityId> doomed;
			for (const auto entity : reload.gone) {
				const auto below = Hierarchy::descendants(entity);
				Hierarchy::detach(entity);
				doomed.push_back(entity);
				doomed.insert(doomed.end(), below.begin(), below.end());
			}
			reload.colliders.insert(reload.colliders.end(), doomed.begin(), doomed.end());
			SystemsModule::RenderSystem::WorldMutateGuard worldLock;
			if (!Engine::isMainThread()) {
				if (auto* render = ECSHandler::getSystem<SystemsModule::RenderSystem>()) {
					worldLock = render->lockWorldMutate();
				}
			}
			if (auto* octree = ECSHandler::getSystem<SystemsModule::OcTreeSystem>()) {
				octree->detach(doomed);
			}
			ECSHandler::destroyEntities(doomed);
		}

		// The static colliders of what moved, took another model, was made or went: a set of shapes for each world.
		std::ranges::sort(reload.colliders);
		reload.colliders.erase(std::unique(reload.colliders.begin(), reload.colliders.end()), reload.colliders.end());
		auto* physics = ECSHandler::getSystem<SystemsModule::PhysicsSystem>();
		auto* loose = ECSHandler::getSystem<SystemsModule::LoosePhysicsSystem>();
		for (const auto entity : reload.colliders) {
			if (physics) {
				physics->replaceStatic(entity, SystemsModule::PhysicsSystem::staticShapes(entity, worldOf(entity)));
			}
			if (loose) {
				loose->replaceStatic(entity, SystemsModule::PhysicsSystem::staticShapes(entity, worldOf(entity)));
			}
		}
		scene.read = std::move(kept);

		std::string said;
		const auto count = [&](int n, const char* what) {
			if (n > 0) {
				said += (said.empty() ? "" : ", ") + std::to_string(n) + " " + what;
			}
		};
		count(reload.moved, "moved");
		count(reload.swapped, "given another model");
		count(reload.made, "made");
		count(reload.removed, "taken out");
		count(reload.read, "read again");
		if (reload.waiting.empty()) {
			return said.empty() ? "nothing changed" : said;
		}
		said += (said.empty() ? "nothing taken up" : "") + std::string("; waiting for a restart: ");
		for (size_t i = 0; i < reload.waiting.size(); ++i) {
			said += (i ? ", " : "") + reload.waiting[i];
		}
		return said;
	}

	void PropertiesSystem::addReloadable(std::string_view component, Reloadable reloadable) {
		reloadables()[std::string(component)] = std::move(reloadable);
	}

	void PropertiesSystem::applyProperties(ecss::EntityId entity, const Json::Value& properties) {
		if (entity == ecss::INVALID_ID || !properties.isMember("Properties")) {
			return;
		}

		ComponentRegistry::load(entity, properties["Properties"], LoadPhase::Direct);
	}

	void PropertiesSystem::fillTree(ecss::EntityId entity, const Json::Value& properties) {
		if (!properties.isObject()) {
			return;
		}

		if (properties.isMember("id")) {
			auto debugData = ECSHandler::registry().addComponent<DebugDataComponent>(entity);
			debugData->stringId = properties["id"].asString();
		}

		ECSHandler::registry().addComponent<ComponentsModule::AABBComponent>(entity);
		ECSHandler::registry().addComponent<OcTreeComponent>(entity);
		applyProperties(entity, properties);
		ECSHandler::registry().addComponent<IsDrawableComponent>(entity);
		ModelBinding::bind(entity);
		if (properties.isMember("Properties")) {
			ComponentRegistry::load(entity, properties["Properties"], LoadPhase::AfterBind);
		}

		if (properties.isMember("Children") && properties["Children"].isArray()) {
			for (auto element : properties["Children"]) {
				auto child = ECSHandler::registry().takeEntity();
				fillTree(child, element);
				Hierarchy::setParent(child, entity);
			}
		}
	}

	Json::Value PropertiesSystem::serializeEntity(ecss::EntityId entity) {
		Json::Value result = Json::objectValue;
		if (entity == ecss::INVALID_ID) {
			return result;
		}

		if (auto debugData = ECSHandler::registry().pinComponent<const DebugDataComponent>(entity)) {
			result["id"] = debugData->stringId;
		}

		ComponentRegistry::save(entity, result["Properties"]);

		const auto children = Hierarchy::children(entity);
		if (!children.empty()) {
			result["Children"] = Json::arrayValue;
			auto& childrenJson = result["Children"];
			for (auto node : children) {
				childrenJson.append(serializeEntity(node));
			}
		}

		return result;
	}

	bool isSnapshotEntity(ecss::EntityId id) {
		if (!ECSHandler::registry().pinComponent<const ModelComponent>(id)) {
			return false;
		}
		if (ECSHandler::registry().pinComponent<const CameraComponent>(id)) {
			return false;
		}
		if (ECSHandler::registry().pinComponent<const CascadeShadowComponent>(id)) {
			return false;
		}
		if (Hierarchy::parent(id) != ecss::INVALID_ID) {
			return false;
		}
		return true;
	}

	Json::Value PropertiesSystem::serializeWorldSnapshot(const Json::Value& request) {
		Json::Value root = Json::objectValue;

		const size_t maxEntities = (std::max)(1u, request.get("maxEntities", 512).asUInt());
		const uint32_t detailId = request.get("detailId", 0u).asUInt();

		Math::Vec3 camPos{};
		if (auto* camSys = ECSHandler::getSystem<SystemsModule::CameraSystem>()) {
			if (auto tc = ECSHandler::registry().pinComponent<const TransformComponent>(camSys->getCurrentCamera())) {
				camPos = tc->getPos(true);
			}
		}

		std::vector<std::pair<float, ecss::EntityId>> ranked;
		ranked.reserve(4096);
		for (const auto& [id, model, transform] : ECSHandler::registry().view<const ModelComponent, const TransformComponent>()) {
			if (!isSnapshotEntity(id)) {
				continue;
			}
			const float dist = Math::distanceSqr(camPos, transform->getPos(true));
			ranked.emplace_back(dist, id);
		}

		root["entityCount"] = static_cast<Json::UInt64>(ranked.size());
		root["fps"] = Engine::instance()->getFPS();
		root["backend"] = Render::graphicsBackendName(Render::activeGraphicsBackend());

		std::ranges::sort(ranked, [](const auto& a, const auto& b) { return a.first < b.first; });

		Json::Value entities = Json::arrayValue;
		const size_t count = (std::min)(maxEntities, ranked.size());
		for (size_t i = 0; i < count; ++i) {
			const ecss::EntityId id = ranked[i].second;
			auto transform = ECSHandler::registry().pinComponent<const TransformComponent>(id);
			auto modelComp = ECSHandler::registry().pinComponent<const ModelComponent>(id);
			if (!transform || !modelComp) {
				continue;
			}

			Json::Value entry = Json::objectValue;
			entry["id"] = static_cast<Json::UInt>(static_cast<uint32_t>(id));
			entry["model"] = modelComp->mPath;
			const auto pos = transform->getPos(true);
			entry["pos"] = Json::arrayValue;
			entry["pos"].append(pos.x);
			entry["pos"].append(pos.y);
			entry["pos"].append(pos.z);
			if (auto mat = ECSHandler::registry().pinComponent<const ComponentsModule::MaterialComponent>(id)) {
				entry["glass"] = mat->transparent;
				entry["opacity"] = mat->opacity;
			}
			entities.append(std::move(entry));
		}
		root["entities"] = std::move(entities);

		if (detailId != 0) {
			const ecss::EntityId detailEntity = static_cast<ecss::EntityId>(detailId);
			if (ECSHandler::registry().contains(detailEntity)) {
				root["detail"] = serializeEntity(detailEntity);
				root["detail"]["id"] = detailId;
			}
		}

		return root;
	}

	Json::Value PropertiesSystem::serializeEntities(const std::vector<ecss::EntityId>& entities) {
		Json::Value root = Json::objectValue;
		root["entities"] = Json::arrayValue;
		for (auto id : entities) {
			if (Hierarchy::parent(id) != ecss::INVALID_ID) {
				continue;
			}
			root["entities"].append(serializeEntity(id));
		}
		return root;
	}

	std::vector<ecss::EntityId> PropertiesSystem::loadEntities(const Json::Value& root) {
		std::vector<ecss::EntityId> ids;
		if (!root.isMember("entities") || !root["entities"].isArray()) {
			return ids;
		}
		for (const auto& element : root["entities"]) {
			auto entity = ECSHandler::registry().takeEntity();
			fillTree(entity, element);
			ids.push_back(entity);
		}
		return ids;
	}

	ecss::EntityId PropertiesSystem::spawnModel(const std::string& path, const Math::Vec3& pos, const Math::Vec3& scale, const Math::Vec3& rotate) {
		return spawnModelEntity(AssetsModule::ModelLoader::instance()->load(path), path.c_str(), pos, scale, false, rotate);
	}

	ecss::EntityId PropertiesSystem::spawnModel(AssetsModule::Model* model, const std::string& path, const Math::Vec3& pos, const Math::Vec3& scale,
		const Math::Vec3& rotate) {
		return spawnModelEntity(model, path.c_str(), pos, scale, false, rotate);
	}

	ecss::EntityId PropertiesSystem::spawnDefaultScene() {
		return {};
	}

	void PropertiesSystem::spawnVampireGrid(int chunkX, int chunkZ, float chunkSize, float spacing) {
		if (spacing < 1.f) {
			spacing = 1.f;
		}

		auto* sampleModel = AssetsModule::ModelLoader::instance()->load("models/vampire.fbx");
		if (!sampleModel) {
			return;
		}
		const AssetsModule::Animation* sampleClip = ModelBinding::playbackClip(sampleModel);
		const float clipDuration = sampleClip ? sampleClip->getDuration() : 0.f;

		const float originX = static_cast<float>(chunkX) * chunkSize;
		const float originZ = static_cast<float>(chunkZ) * chunkSize;
		const float maxX = originX + chunkSize;
		const float maxZ = originZ + chunkSize;
		const int i0 = static_cast<int>(std::ceil(originX / spacing - 1e-4f));
		const int i1 = static_cast<int>(std::ceil(maxX / spacing - 1e-4f));
		const int j0 = static_cast<int>(std::ceil(originZ / spacing - 1e-4f));
		const int j1 = static_cast<int>(std::ceil(maxZ / spacing - 1e-4f));

		for (int i = i0; i < i1; ++i) {
			for (int j = j0; j < j1; ++j) {
				const float x = static_cast<float>(i) * spacing;
				const float z = static_cast<float>(j) * spacing;
				float phase = 0.f;
				if (clipDuration > 0.f) {
					const uint32_t h = static_cast<uint32_t>(i) * 374761393u + static_cast<uint32_t>(j) * 668265263u;
					phase = clipDuration * static_cast<float>(h & 1023u) / 1024.f;
				}
				spawnDancingVampire({ x, 0.f, z }, 180.f, phase);
			}
		}
	}

	// Rooms of solid walls with a doorway and a roof, a few dancing characters in each: what is inside one room is
	// hidden from the camera and from the sun by the room in front of it, which is what occlusion culling is for.
	void PropertiesSystem::spawnRoomGrid(const int chunkX, const int chunkZ, const float chunkSize, float roomSize) {
		if (roomSize < 48.f) {
			roomSize = 48.f;
		}
		auto* box = getOrCreateUnitBox();
		if (!box) {
			return;
		}
		auto* sampleModel = AssetsModule::ModelLoader::instance()->load("models/vampire.fbx");
		const AssetsModule::Animation* sampleClip = ModelBinding::playbackClip(sampleModel);
		const float clipDuration = sampleClip ? sampleClip->getDuration() : 0.f;

		const int rooms = (std::max)(1, static_cast<int>(chunkSize / roomSize));
		const float originX = static_cast<float>(chunkX) * chunkSize;
		const float originZ = static_cast<float>(chunkZ) * chunkSize;

		// A character stands about 18 units tall, so the walls have to clear that to hide anyone behind them.
		constexpr float kWallHeight = 26.f;
		constexpr float kWallThickness = 1.5f;
		constexpr float kDoorWidth = 14.f;
		// The gap between rooms is a street, so the camera starts outside and looks along one.
		constexpr float kStreet = 16.f;

		const auto spawnBox = [&](const Math::Vec3& center, const Math::Vec3& halfSize) {
			spawnModelEntity(box, "builtin:box", center, halfSize, false);
		};

		for (int i = 0; i < rooms; ++i) {
			for (int j = 0; j < rooms; ++j) {
				const float cx = originX + (static_cast<float>(i) + 0.5f) * roomSize;
				const float cz = originZ + (static_cast<float>(j) + 0.5f) * roomSize;
				const float half = roomSize * 0.5f - kStreet;
				const float segment = (half - kDoorWidth * 0.5f) * 0.5f;
				const float segmentCenter = kDoorWidth * 0.5f + segment;

				// Three solid walls and a fourth with a doorway in the middle, so the inside is visible from one side
				// only -- which side varies, so some rooms open onto the street the camera stands in and some do not.
				uint32_t room = static_cast<uint32_t>(chunkX * rooms + i) * 2246822519u + static_cast<uint32_t>(chunkZ * rooms + j) * 3266489917u;
				room ^= room >> 15;
				const uint32_t doorSide = room & 3u;
				for (uint32_t side = 0; side < 4u; ++side) {
					// Sides 0 and 1 run along x at -z and +z; sides 2 and 3 along z at -x and +x.
					const float sign = (side & 1u) ? 1.f : -1.f;
					const bool alongX = side < 2u;
					const Math::Vec3 center = alongX ? Math::Vec3{ cx, kWallHeight * 0.5f, cz + sign * half }
						: Math::Vec3{ cx + sign * half, kWallHeight * 0.5f, cz };
					const Math::Vec3 full = alongX ? Math::Vec3{ half, kWallHeight * 0.5f, kWallThickness }
						: Math::Vec3{ kWallThickness, kWallHeight * 0.5f, half };
					if (side != doorSide) {
						spawnBox(center, full);
						continue;
					}
					const Math::Vec3 piece = alongX ? Math::Vec3{ segment, kWallHeight * 0.5f, kWallThickness }
						: Math::Vec3{ kWallThickness, kWallHeight * 0.5f, segment };
					const Math::Vec3 step = alongX ? Math::Vec3{ segmentCenter, 0.f, 0.f } : Math::Vec3{ 0.f, 0.f, segmentCenter };
					spawnBox(center - step, piece);
					spawnBox(center + step, piece);
				}
				// The roof hides the room from the sun as well, so the shadow cascades have something to cull too.
				spawnBox({ cx, kWallHeight, cz }, { half + kWallThickness, kWallThickness, half + kWallThickness });

				if (!sampleModel) {
					continue;
				}
				for (int k = 0; k < 3; ++k) {
					const uint32_t h = static_cast<uint32_t>(chunkX * rooms + i) * 374761393u
						+ static_cast<uint32_t>(chunkZ * rooms + j) * 668265263u + static_cast<uint32_t>(k) * 2654435761u;
					const float phase = clipDuration > 0.f ? clipDuration * static_cast<float>(h & 1023u) / 1024.f : 0.f;
					const float dx = (static_cast<float>(k) - 1.f) * (half * 0.5f);
					const float dz = static_cast<float>((h >> 10) & 7u) * 0.5f - 2.f;
					spawnDancingVampire({ cx + dx, 0.f, cz + dz }, 180.f, phase);
				}
			}
		}
	}

	void PropertiesSystem::spawnSphereGrid(int chunkX, int chunkZ, float chunkSize, float spacing) {
		if (spacing < 4.f) {
			spacing = 4.f;
		}
		if (spacing > 64.f) {
			spacing = 64.f;
		}

		auto* model = getOrCreateUnitSphere();
		if (!model) {
			return;
		}

		int n = static_cast<int>(chunkSize / spacing);
		if (n < 1) {
			n = 1;
		}

		const float originX = static_cast<float>(chunkX) * chunkSize;
		const float originZ = static_cast<float>(chunkZ) * chunkSize;
		const float scaleV = spacing * 0.45f;
		const Math::Vec3 scale{ scaleV };

		uint32_t ch = static_cast<uint32_t>(chunkX) * 374761393u + static_cast<uint32_t>(chunkZ) * 668265263u;
		ch = (ch ^ (ch >> 13)) * 1274126177u;
		const int lampI = static_cast<int>(ch % static_cast<uint32_t>(n));
		const int lampJ = static_cast<int>((ch >> 8) % static_cast<uint32_t>(n));

		for (int i = 0; i < n; ++i) {
			for (int j = 0; j < n; ++j) {
				const int gx = chunkX * n + i;
				const int gz = chunkZ * n + j;
				uint32_t h = static_cast<uint32_t>(gx) * 374761393u + static_cast<uint32_t>(gz) * 668265263u;
				h = (h ^ (h >> 13)) * 1274126177u;
				const float y = scaleV + static_cast<float>(h & 255u) * (spacing * 1.5f / 255.f);
				const float x = originX + (static_cast<float>(i) + 0.5f) * spacing;
				const float z = originZ + (static_cast<float>(j) + 0.5f) * spacing;
				const bool isLamp = (i == lampI && j == lampJ);
				const auto id = spawnModelEntity(model, "builtin:sphere", { x, y, z }, scale, !isLamp);
				if (isLamp) {
					makeSphereLamp(id, spacing);
				}
				else {
					applyGlassTint(id, h);
				}
			}
		}
	}
}
