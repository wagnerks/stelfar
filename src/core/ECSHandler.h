#pragma once
#include "containersModule/Singleton.h"
#include "ecss/Registry.h"
#include "systemsModule/SystemManager.h"

class ECSHandler final : public SFE::Singleton<ECSHandler> {
public:
	template<class SystemType>
	inline static SystemType* getSystem() {
		return systemManager().getSystem<SystemType>();
	}

	inline static ecss::SystemManager& systemManager() {
		return instance()->mSystemManager;
	}

	inline static ecss::Registry<>& registry() {
		return instance()->mRegistry;
	}

	template<typename CompType, class ...Args>
	inline static CompType* addComponent(ecss::EntityId entity, Args&&... args) {
		return instance()->mRegistry.addComponent<CompType>(entity, std::forward<Args>(args)...);
	}

	template<typename CompType>
	inline static void removeComponent(ecss::EntityId entity) {
		instance()->mRegistry.destroyComponent<CompType>(entity);
	}

	template<typename CompType>
	inline static void removeComponent(std::vector<ecss::EntityId>& entities) {
		instance()->mRegistry.destroyComponent<CompType>(entities);
	}
	
	// The same as the registry's own: either way the registry's destroy listener sends ENTITY_DESTROYED.
	inline static void destroyEntity(ecss::EntityId entity) {
		instance()->mRegistry.destroyEntity(entity);
	}

	inline static void destroyEntities(std::vector<ecss::EntityId>& entities) {
		instance()->mRegistry.destroyEntities(entities);
	}

private:
	// The registry outlives the systems: ~SystemManager joins the tick threads, and a system's destructor may still
	// walk entities.
	ecss::Registry<> mRegistry;
	ecss::SystemManager mSystemManager;
};
