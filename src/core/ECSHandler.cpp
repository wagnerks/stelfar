#include "ECSHandler.h"

#include <fstream>
#include <iostream>

#include "core/GameModule.h"
#include "assetsModule/AssetsManager.h"
#include "assetsModule/modelModule/MeshVaoRegistry.h"
#include "assetsModule/modelModule/ModelLoader.h"
#include "componentsModule/ArmatureComponent.h"
#include "componentsModule/IsDrawableComponent.h"
#include "componentsModule/MaterialComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/ModelComponent.h"
#include "componentsModule/OcTreeComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/Engine.h"
#include "core/LoadTrace.h"
#include "debugModule/Benchmark.h"
#include "logsModule/logger.h"
#include "propertiesModule/PropertiesSystem.h"

#include "systemsModule/SystemManager.h"
#include "systemsModule/TasksManager.h"

#include "systemsModule/systems/AABBSystem.h"
#include "systemsModule/systems/ActionSystem.h"
#include "systemsModule/systems/AudioSystem.h"
#include "networkModule/NetworkSystem.h"
#include "systemsModule/systems/CameraSystem.h"
#include "systemsModule/systems/LODSystem.h"
#include "systemsModule/systems/OcTreeSystem.h"
#include "systemsModule/systems/PhysicsSystem.h"
#include "systemsModule/systems/RenderSystem.h"
#include "systemsModule/systems/SkeletalAnimationSystem.h"
#include "systemsModule/systems/TransformSystem.h"
#include "systemsModule/systems/WorldTimeSystem.h"

namespace SectorsArrayTest
{
	struct Trivial;
}

namespace {
	// The scene the engine was given (Engine::scenePath), loaded whole: its entities, their components and the render
	// copies of their transforms. Without one the world starts empty. The engine loads it here until the application
	// puts the engine together and loads its scene itself.
	void loadStartScene() {
		const auto& path = SFE::Engine::instance()->scenePath();
		if (path.empty()) {
			return;
		}
		SFE::LoadTrace::Step step("scene");
		if (!SFE::PropertiesModule::PropertiesSystem::loadScene(path)) {
			SFE::LogsModule::Logger::LOG_ERROR("scene not loaded: %s", path.c_str());
		}
	}
}

void ECSHandler::initSystems() {
	// Systems that keep state per entity hear of every destroy, including one made straight on the registry: one
	// that went unheard left its instances drawn where it stood.
	mRegistry.setDestroyListener([](ecss::EntityId entity, void*) {
		SFE::SystemsModule::TasksManager::instance()->notify({ entity, SFE::SystemsModule::TaskType::ENTITY_DESTROYED });
	});

	//todo separate systems into  drawData preparing and logic update
	/*
	 * draw data preparing functions should not be updated while scene is rendering (or it should have its own ecs registries to apply double buffering (updating data in one buffer, while draw data from other buffer)
	 *
	 * todo also create separate registries for separate data, static data - which is not changes (buildings, woods etc), dynamic data(wooden tree, players etc), shared data (static but needed both for static and dynamic entities)(mesh component for example)
	 */


	//mRegistry.registerArray<TransformComponent, MeshComponent>();
	//mDrawRegistry[0].registerArray<SFE::ComponentsModule::TransformMatComp, MeshComponent, SFE::ComponentsModule::ArmatureBonesComponent>();
	//mDrawRegistry[1].registerArray<SFE::ComponentsModule::TransformMatComp, MeshComponent, SFE::ComponentsModule::ArmatureBonesComponent>();

	mSystemManager.createSystem<SFE::SystemsModule::LODSystem>();
	mSystemManager.createSystem<SFE::SystemsModule::TransformSystem>();
	mSystemManager.createSystem<SFE::SystemsModule::OcTreeSystem>();
	mSystemManager.createSystem<SFE::SystemsModule::AABBSystem>();
	mSystemManager.createSystem<SFE::SystemsModule::CameraSystem>();
	// The world before what draws it and what collides with it: RenderSystem, and PhysicsSystem below, take up what
	// is in it as they are made.
	loadStartScene();
	mSystemManager.createSystem<SFE::SystemsModule::RenderSystem>();

	//mSystemManager.createSystem<SFE::SystemsModule::Physics>();
	//mSystemManager.createSystem<SFE::SystemsModule::ActionSystem>();
	//mSystemManager.createSystem<SFE::SystemsModule::WorldTimeSystem>();
	//mSystemManager.createSystem<SFE::SystemsModule::SkeletalAnimationSystem>();


	// Tick systems advance off their own wall-clock threads, so the pose at frame N depends on
	// how fast that run happened to be. A capture has to be reproducible and comparable across
	// backends, so it holds the scene still instead. AnimationComputePass runs its own clock and
	// is frozen separately.
	// Physics is the exception: a capture still steps it, from the frame, so a character stands on the ground in it.
	// The character registers its step with the world before the physics thread starts.
	mSystemManager.createSystem<SFE::SystemsModule::PhysicsSystem>();
	mSystemManager.createSystem<SFE::SystemsModule::LoosePhysicsSystem>();
	mSystemManager.createSystem<SFE::SystemsModule::AudioSystem>();
	// The session first of all, so what arrived is handled before anything of the frame is decided; the game's systems
	// listen to it as they are made.
	mSystemManager.addRootSystems<SFE::Network::NetworkSystem>();
	// Whatever game is built with this engine, once everything it stands on exists.
	SFE::GameModule::createSystems(mSystemManager);
	if (auto* physics = getSystem<SFE::SystemsModule::PhysicsSystem>()) {
		physics->addStaticCapsuleBodies();
		if (auto* loose = getSystem<SFE::SystemsModule::LoosePhysicsSystem>()) {
			loose->shareStaticCapsules(*physics);
			loose->addStaticCapsuleBodies();
		}
	}
	const bool capturing = SFE::Engine::instance()->capturingFrame();
	// Settled at the first update, for every tick system whenever it was added: a game's, made above, runs as these do.
	mSystemManager.setStepTickSystems(capturing || SFE::Engine::instance()->fixedDeltaTime() > 0.f);
	mSystemManager.addTickSystems<SFE::SystemsModule::PhysicsSystem, SFE::SystemsModule::LoosePhysicsSystem>(60);
	if (!capturing) {
		mSystemManager.addTickSystems<SFE::SystemsModule::WorldTimeSystem>(1);
		mSystemManager.addTickSystems<SFE::SystemsModule::SkeletalAnimationSystem>(24);
	}

	// In this order: the character takes its pose from the physics thread and places the camera, the litter follows its
	// bodies, the camera flies (when the character does not hold it), the vegetation around it is brought in, sounds are heard from the camera's
	// new place, then the frame is drawn from where the camera ended up.
	// The camera flies (when nothing else holds it), what it hears follows it, then the frame is drawn from where it
	// ended up. A game adds its own ahead of these, in GameModule::addSystems.
	mSystemManager.addRootSystems<SFE::SystemsModule::CameraSystem, SFE::SystemsModule::AudioSystem, SFE::SystemsModule::RenderSystem>();
}
