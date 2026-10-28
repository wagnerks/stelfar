#pragma once

#include "ComponentSerializer.h"

#include "componentsModule/AudioSourceComponent.h"
#include "componentsModule/CascadeShadowComponent.h"
#include "componentsModule/LightSourceComponent.h"
#include "componentsModule/MaterialComponent.h"
#include "componentsModule/ModelComponent.h"
#include "componentsModule/ShelterComponent.h"
#include "componentsModule/SkyComponent.h"
#include "componentsModule/TransformComponent.h"
#include "componentsModule/WaterComponent.h"
#include "componentsModule/WindComponent.h"

namespace SFE::PropertiesModule {
#define SCENE_COMPONENT_SERIALIZER(type)												\
	template<>																			\
	struct ComponentSerializer<ComponentsModule::type> {								\
		static void save(const ComponentsModule::type& component, Json::Value& data);	\
		static void load(ecss::EntityId entity, const Json::Value& data);				\
	};

	SCENE_COMPONENT_SERIALIZER(TransformComponent)
	SCENE_COMPONENT_SERIALIZER(ModelComponent)
	SCENE_COMPONENT_SERIALIZER(LightSourceComponent)
	SCENE_COMPONENT_SERIALIZER(CascadeShadowComponent)
	SCENE_COMPONENT_SERIALIZER(MaterialComponent)
	SCENE_COMPONENT_SERIALIZER(SkyComponent)
	SCENE_COMPONENT_SERIALIZER(WaterComponent)
	SCENE_COMPONENT_SERIALIZER(WindComponent)
	SCENE_COMPONENT_SERIALIZER(ShelterComponent)
	SCENE_COMPONENT_SERIALIZER(AudioSourceComponent)

#undef SCENE_COMPONENT_SERIALIZER
}
