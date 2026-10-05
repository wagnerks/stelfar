#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <json/value.h>

#include "SceneComponents.h"
#include "TypeName.h"
#include "core/ECSHandler.h"
#include "mathModule/Forward.h"

namespace SFE::PropertiesModule {
	class PropertiesSystem {
	public:
		static ecss::EntityId loadScene(std::string_view path);

		/// @brief The scene loadScene read last, read again from its file, and what changed in it since taken up while
		///        the game runs: an entity moved, turned or scaled, a model swapped, an entity new to the file made
		///        and one gone from it taken out, their colliders with them. A system that built something of its own
		///        from a component once cannot follow, so a change the components on an entity do not take (see
		///        addReloadable) is left as it was and named in the answer: it waits for a restart. Main thread, between
		///        frames (the reloadScene command).
		static std::string reloadScene();

		/// @brief How a component takes the scene being read again. `read` is called once the component has been read
		///        again from the file, with its JSON before; false puts that back, and the change waits for a restart.
		///        `moved` is told where the entity it is on now stands in the world, once that has been moved. Without
		///        `read` a change to the component waits for a restart, and without `moved` so does a move of its entity.
		///        An entity of nothing but a transform and a model needs neither, and may be made and taken out as well.
		struct Reloadable {
			std::function<bool(ecss::EntityId entity, const Json::Value& was)> read;
			std::function<void(ecss::EntityId entity, const Math::Mat4& world)> moved;
		};
		static void addReloadable(std::string_view component, Reloadable reloadable);

		static void applyProperties(ecss::EntityId entity, const Json::Value& properties);

		static void fillTree(ecss::EntityId entity, const Json::Value& properties);

		static Json::Value serializeEntity(const ecss::EntityId entity);

		static ecss::EntityId spawnDefaultScene();
		// A model at a place, ready to draw: loaded (once per path), bound and with its render transform seeded.
		static ecss::EntityId spawnModel(const std::string& path, const Math::Vec3& pos, const Math::Vec3& scale, const Math::Vec3& rotate);
		static void spawnSphereGrid(int chunkX, int chunkZ, float chunkSize, float spacing);
		static void spawnVampireGrid(int chunkX, int chunkZ, float chunkSize, float spacing);
		// Walled rooms with characters inside, for testing occlusion culling.
		static void spawnRoomGrid(int chunkX, int chunkZ, float chunkSize, float roomSize);
		static Json::Value serializeEntities(const std::vector<ecss::EntityId>& entities);
		static Json::Value serializeWorldSnapshot(const Json::Value& request);
		static std::vector<ecss::EntityId> loadEntities(const Json::Value& root);

		/// @brief Read one named component out of a "Properties" object, whatever phase it is
		///        registered for. For callers holding a file of their own; a scene load goes
		///        through ComponentRegistry.
		template<class T>
		static void deserializeProperty(const ecss::EntityId entity, const Json::Value& properties);

		template<class T>
		static void serializeProperty(const ecss::EntityId entity, Json::Value& properties);
	private:
	};

	template <class T>
	void PropertiesSystem::deserializeProperty(const ecss::EntityId entity, const Json::Value& properties) {
		if (!ECSHandler::registry().contains(entity)) {
			return;
		}
		const auto name = TypeName<T>::name().data();
		if (properties.isMember(name)) {
			ComponentSerializer<T>::load(entity, properties[name]);
		}
	}

	template <class T>
	void PropertiesSystem::serializeProperty(const ecss::EntityId entity, Json::Value& properties) {
		if (entity == ecss::INVALID_ID) {
			return;
		}

		if (auto component = ECSHandler::registry().pinComponent<const T>(entity)) {
			ComponentSerializer<T>::save(*component, properties[TypeName<T>::name().data()]);
		}
	}

	namespace JsonUtils {
		inline const Json::Value* getValue(const Json::Value& json, std::string_view memberId) {
			if (json.isMember(memberId.data())) {
				return &json[memberId.data()];
			}

			return nullptr;
		}

		inline const Json::Value* getValueArray(const Json::Value& json, std::string_view memberId) {
			if (auto val = getValue(json, memberId)) {
				if (val->isArray()) {
					return val;
				}
			}

			return nullptr;
		}

		inline Math::Vec3 getVec3(const Json::Value& json) {
			if (json.isArray()) {
				return { json[0].asFloat(), json[1].asFloat(), json[2].asFloat() };
			}
			return {};
		}

		inline Math::Vec2 getVec2(const Json::Value& json) {
			if (json.isArray()) {
				return { json[0].asFloat(), json[1].asFloat() };
			}
			return {};
		}
	}
}
