#include "DefaultSystems.h"

#include <utility>

#include "core/ECSHandler.h"
#include "core/LoadTrace.h"
#include "logsModule/logger.h"
#include "networkModule/NetworkSettings.h"
#include "networkModule/NetworkSystem.h"
#include "propertiesModule/PropertiesSystem.h"
#include "renderModule/PassList.h"
#include "systemsModule/systems/AABBSystem.h"
#include "systemsModule/systems/AudioSystem.h"
#include "systemsModule/systems/CameraSystem.h"
#include "systemsModule/systems/FreeCameraController.h"
#include "systemsModule/systems/OcTreeSystem.h"
#include "systemsModule/systems/PhysicsDrawSystem.h"
#include "systemsModule/systems/PhysicsSystem.h"
#include "systemsModule/systems/RenderSystem.h"
#include "systemsModule/systems/TransformSystem.h"

namespace SFE::Modules {
	void addSceneCore() {
		auto& systems = ECSHandler::systemManager();
		systems.createSystem<SystemsModule::TransformSystem>();
		systems.createSystem<SystemsModule::OcTreeSystem>();
		systems.createSystem<SystemsModule::AABBSystem>();
	}

	void addCamera() {
		ECSHandler::systemManager().createSystem<SystemsModule::CameraSystem>();
	}

	void addFreeCamera() {
		ECSHandler::systemManager().createSystem<SystemsModule::FreeCameraController>();
	}

	// The scene, loaded whole: its entities, their components and the render copies of their transforms. Without one the
	// world starts empty.
	bool loadScene(const std::string& path) {
		if (path.empty()) {
			return true;
		}
		LoadTrace::Step step("scene");
		if (!PropertiesModule::PropertiesSystem::loadScene(path)) {
			LogsModule::Logger::LOG_ERROR("scene not loaded: %s", path.c_str());
			return false;
		}
		return true;
	}

	void addRenderer(Render::PassList passes) {
		ECSHandler::systemManager().createSystem<SystemsModule::RenderSystem>(std::move(passes));
	}

	void addPhysics() {
		auto& systems = ECSHandler::systemManager();
		systems.createSystem<SystemsModule::PhysicsSystem>();
		systems.createSystem<SystemsModule::LoosePhysicsSystem>();
		systems.createSystem<SystemsModule::PhysicsDrawSystem>();
		// Both worlds tick 60 times a second: each on a thread of its own, or stepped from the frame for a fixed step or a
		// capture (Engine::start). A capture steps them too, so a body resting on the ground rests on it in the picture.
		systems.addTickSystems<SystemsModule::PhysicsSystem, SystemsModule::LoosePhysicsSystem>(60);
	}

	void addAudio() {
		ECSHandler::systemManager().createSystem<SystemsModule::AudioSystem>();
	}

	void addNetwork(const Network::Settings& settings) {
		ECSHandler::systemManager().createSystem<Network::NetworkSystem>(settings);
	}

	void schedule() {
		auto& systems = ECSHandler::systemManager();
		// The session first of all, so what arrived is handled before anything of the frame is decided; an application's
		// systems listen to it as they are made.
		if (systems.getSystem<Network::NetworkSystem>()) {
			systems.addRootSystems<Network::NetworkSystem>();
		}
		// Then, at the frame's end and in this order: the physics worlds' shapes are handed to the debug pass (when F3 has
		// them shown), the free camera flies (when nothing else holds it), what is heard follows the camera, and the frame
		// is drawn from where it ended up. An application roots its own ahead of these (addRootSystemsBefore).
		if (systems.getSystem<SystemsModule::PhysicsDrawSystem>()) {
			systems.addRootSystems<SystemsModule::PhysicsDrawSystem>();
		}
		if (systems.getSystem<SystemsModule::FreeCameraController>()) {
			systems.addRootSystems<SystemsModule::FreeCameraController>();
		}
		if (systems.getSystem<SystemsModule::AudioSystem>()) {
			systems.addRootSystems<SystemsModule::AudioSystem>();
		}
		if (systems.getSystem<SystemsModule::RenderSystem>()) {
			systems.addRootSystems<SystemsModule::RenderSystem>();
		}
	}
}
