#pragma once

#include <string>

namespace SFE::Network {
	struct Settings;
}

namespace SFE::Render {
	class PassList;
}

// The engine's ready bricks: an application calls those it wants, in its order (its entry point puts them together:
// engine/README.md, "Putting the engine together").
namespace SFE::Modules {
	void addSceneCore();   // TransformSystem, OcTreeSystem, AABBSystem: task workers, before any entity they must hear of
	void addCamera();      // CameraSystem: the current camera, a default one made (reads the window: after createWindow)
	void addFreeCamera();  // FreeCameraController, on (after addCamera; before whatever takes the camera over)
	bool loadScene(const std::string& path);  // the scene whole ("scene" in the load trace); empty: nothing, true
	void addRenderer(Render::PassList passes); // RenderSystem(passes): after addCamera (CascadedShadowPass::init reads it)
	void addPhysics();     // PhysicsSystem, LoosePhysicsSystem (ticked 60/s), PhysicsDrawSystem
	void addAudio();       // AudioSystem
	void addNetwork(const Network::Settings& settings);  // NetworkSystem(settings)
	// Roots, in the frame's order, the engine's systems that were made: the session first (what arrived handled before
	// anything of the frame is decided), then at the frame's end the physics shapes to the debug pass, the free camera,
	// what is heard follows the camera, the frame drawn. An application puts its own before them
	// (addRootSystemsBefore<PhysicsDrawSystem, ...>, or whichever of them it has).
	void schedule();
}
