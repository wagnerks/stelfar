# Stellar Forge Engine (STELFORGE ENGINE // SFE)

A C++ game engine as a static library, `StelForgeEngine`, which an application links and puts together itself. The
engine holds no world, no content, no player and no game systems: the application makes the world from its own scene,
chooses the engine's systems and passes, puts its own among them and brings its own content.

## What is in it

- An ECS with data-oriented storage (`src/submodules/ecss`) and the frame's order of systems (`SystemManager`): root
  systems run in the frame, tick systems at a rate of their own, on threads or stepped from the frame.
- A render hardware interface (`rhi/`) with three backends: Vulkan, OpenGL and None, which draws nothing (tests,
  bots). Shaders are GLSL; for Vulkan they are compiled to SPIR-V at run time and cached.
- A GPU-driven deferred renderer (`renderModule/`, `RenderSystem`): culling and mesh LOD on the GPU, cascaded sun
  shadows, sky light from a procedural sky, SSAO, SSR, water, mist, glass, particles, TAA and the rest of
  `Render::defaultPasses()`.
- Assets: models through assimp, with a model cache and mesh LODs (meshoptimizer); textures compressed to BC7; scenes
  in JSON (`propertiesModule/`).
- Physics on Jolt in two worlds (`PhysicsSystem`, and `LoosePhysicsSystem` ticked 60 times a second), audio on
  miniaudio with Steam Audio, a network on enet.
- The runtime bridge (`runtimeBridge/`), through which a running program takes commands, 13 of them the engine's own;
  the ImGui debug UI and its windows; `SFE::Params`, the registry of tunable numbers.

## Build and link

An application adds the engine with `add_subdirectory`, links `StelForgeEngine` and calls `sfe_stage_app(MyApp)`,
which puts the runtime libraries beside its executable. `SFE_BUILD_ENGINE_TESTS` adds `engine_tests` (googletest: the
engine on the None backend, with no window, its content `STELFORGE_CONTENT=engine/bin`); `SFE_BUILD_RHI_TESTS` adds the
RHI's conformance tests.

# Putting the engine together

The engine is a library: it makes no world, no systems and no render passes of its own accord. Whatever runs it -- a
game's `main.cpp`, an editor that runs the engine in its own process -- puts it together on the main thread, in this
order:

0. The command line, if it takes the engine's: `RunOptions::fromCommandLine(argc, argv)` (`runtime/RunOptions.h`)
   chooses the graphics backend and reads the engine's flags -- `--fixed-dt`, `--capture[=]`, `--capture-frame[=]`,
   `--capture-every`, `--capture-count`, `--capture-passes[=]`, `--no-taa`, `--debug-term[=]`, `--content-dir[=]`,
   `--scene`, `--camera`, `--camera-rot`, `--frames`, `--serve`, `--serve-port`, `--headless`, `--visible`, `--host`,
   `--connect`, `--net-lag`, `--net-jitter`, `--net-loss`, `--players`, `--backend[=]` -- leaving the application's
   own in `rest`, in order. `apply(engineRootFallback)` then sets the roots (step 1) and the numbers held for the run,
   `engineConfig()` gives step 2's config but its numbers file, and `placeCamera()` the camera after the scene. A
   backend that draws nothing runs at 60 frames a second, any other at `EngineConfig::maxFps`'s 600.
1. The roots, before the engine exists: `FileSystem::setRoot` (the application's content, searched first and written
   under) and `FileSystem::setEngineRoot` (`engine/bin`, behind it), and the graphics backend
   (`Render::configureGraphicsBackend`). The application's scene components are registered
   (`PropertiesModule::ComponentRegistry`, `propertiesModule/ComponentSerializer.h`) before any scene is read, and its
   frame data is declared (`Render::UserFrameSet::instance().declare`, `renderModule/UserFrameSet.h`) before the
   renderer is made.
2. `Engine::instance()->init(config)` with an `EngineConfig` (`core/EngineConfig.h`): what the run is, the same to its
   end -- a fixed frame step, the frame cap, a frame capture and where its passes are dumped, the application's numbers
   file (`SFE::Params`; none named, every number stays as the code has it). Once, before `createWindow`: it makes the
   RHI's object for the backend chosen.
3. `createWindow(...)`, then `initThread()`: the graphics device, the numbers read, the registry's listeners.
4. The systems, from the bricks of `core/DefaultSystems.h` (`SFE::Modules`), those it wants, in this order:
   `addSceneCore()`, `addCamera()`, `addFreeCamera()` (a free-flying camera, if it wants one), `addRenderer(passes)`,
   `addPhysics()`, `addAudio()`, `addNetwork(settings)`. The renderer draws the list it is given:
   `Render::defaultPasses()` with the application's own passes put in by name (`Render::PassList`,
   `renderModule/PassList.h`; a name not in the list is an error). Then the application's own systems
   (`ECSHandler::systemManager().createSystem<T>(...)`) and the frame's order: `Modules::schedule()` roots the engine's
   systems that were made, and the application puts its own before them
   (`addRootSystemsBefore<PhysicsDrawSystem, ...>()`, or `addTickSystems<...>(rate)`). Scheduling never makes a system;
   a system made with an update of its own but in no schedule is named in `load_times.log`. Then what else it brings:
   bridge commands (`RuntimeBridge::addCommand`), windows in the debug UI (`debugModule/DebugPanels.h`).
5. The scene, `loadScene(path)`: anywhere after `addSceneCore()` and before `start()`, before the systems or after
   them -- the world they make of it is the same, and so is what is drawn. Every engine system and pass goes by one rule
   (`propertiesModule/SceneEvents.h`): it takes what is there when it is made, and is told of every scene loaded after
   (`SceneLoaded`) -- the physics its static colliders, laid in the same order either way, the renderer its sky light
   and its sun -- and an application's own systems and passes go by the same rule. Before anyone is told, every
   transform of the scene stands where the file puts it, the render copy and the component's own matrix both; the
   listeners are told in the order they subscribed, that is the order they were made in, with the scene's entities in
   the order the file lists them, depth first. Loaded after the systems, it is still loaded before the world first
   steps, so the physics lays it in at once and the systems told after the physics already meet its colliders. Upright
   capsules for both physics worlds (`PhysicsSystem::addStaticCapsules`) go after the load when the application's own
   takes must not meet them (loose bodies laid before the trunks stand): given before, the
   physics, told before the application's systems, lays them in with the scene.
6. `Engine::start()`: how the tick systems run (on threads, or stepped from the frame under a fixed step or a capture),
   the input, the engine's own bridge commands after the application's, and the frame limit.
7. `while (engine->isAlive()) engine->update();`, then `Engine::terminate()`. The engine paces the frames itself, with
   the same waiter as the tick systems' (`core/Pacing.h`): while the step is the wall clock's, `update()` ends on a grid
   of 1 / `maxFps` from `start()`, a late frame waiting for nothing and the grid starting again from it; a fixed step or
   a capture is never held back. A run in real time, whatever its limit, waits on the clock -- its frames for the
   limit, its tick systems for their ticks -- so `start()` asks the OS for naps of a millisecond for it
   (`timeBeginPeriod(1)` on Windows); a stepped run asks for nothing. The application adds no wait of its own.

Embedded in an editor, the editor's flags are the bridge's: `RuntimeBridge::setEmbeddedInEditor` and
`setPublishEditorDisplay` (`runtimeBridge/RuntimeBridge.h`); whether the window shows the picture is
`Engine::setPresentToScreen`.

Gone from `Engine.h`, with no stand-in left there: `setScenePath` (now `Modules::loadScene`), `setFixedDeltaTime`,
`setCapturingFrame`, `setCaptureFrame`, `setPassDumpDir` and the frame cap (now `EngineConfig`), `setNetwork` (now
`addNetwork`), `setEmbeddedInEditor`, `isEmbeddedInEditor`, `setPublishEditorDisplay` (now `RuntimeBridge`'s),
`setLightingDebugTerm` and `setTemporalAccumulation` (numbers of `SFE::Params`: `Render::debugView()`,
`GraphicsSettings::taa`), `initRender` (now `start`), `setEscapeCloses` and the window closing on Escape (the
application's to do), `setProceduralSpheres`, `setProceduralRooms` and the demo scenes; besides,
`ECSHandler::initSystems` (now the modules and the application's own calls) and `GameModule` (now `PassList`,
`DebugPanels` and the application's own `createSystem` calls).

# Extending the engine

What an application adds to the engine goes through the channels below, none of them named after a game's feature. Each
is described in its header.

- **One world rule** (`propertiesModule/SceneEvents.h`): a system or pass takes what is there when it is made, and is
  told of every scene loaded after (`SceneLoaded`). The engine's own go by it, and an application's should too.
- **Files** (`core/FileSystem.h`): two roots. A relative path is read from the application's root first and from the
  engine's (`engine/bin`) behind it, so a file of the same path stands in for the engine's; a shader's `#include "x"`
  is found the same way, as `shaders/x`, and beside the including file only when neither root has it. Whatever the
  engine writes -- caches, compiled SPIR-V, captures, `sfe_fatal.log` -- goes under the application's root.
- **Shader hooks**: the files `bin/shaders/sfe_user_*.glsl` are empty defaults, each saying where it is called and what
  the engine does with what it returns; an application puts its own copy under `shaders/` in its root. Surface
  (`sfeUserSurface`, early, and `sfeUserSurfaceLate`); dissolve (`sfeUserDissolveKeeps`, `sfeUserDissolved`: what of a
  dissolving piece stays and in what colour, in the surface and in the shadow passes); glass (`sfeUserGlass`);
  lighting (`sfeUserLitAlbedo`, `sfeUserAir`); water (`sfeUserWater`, `sfeUserWaterFilm`); mist (`sfeUserMist`);
  ground (`sfeUserGroundOffset`). A G-buffer shader of the application's own that defines `SFE_GBUFFER_DEPTH` or
  `SFE_GBUFFER_STYLE` before it includes `sfe_gbuffer_surface.glsl` gives the surface a depth or a look of its own.
- **Frame data** (`renderModule/UserFrameSet.h`): set 3, bindings 40-63, declared before the renderer is made, each with
  a stand-in (zero or white) until something is published to it, laid out and bound by every pipeline that compiles a
  hook or reads it.
- **Entity words** (`renderModule/EntityUser.h`): eight 32-bit words an entity keeps for the application's shaders
  (`sfe_words.glsl`: `vWords`, `vWordsMore`); word 7 is the writer's kind.
- **Ground patch** (`renderModule/TerrainPatch.h`): the ground drawn dense near the camera, its heights given by a
  producer, the depth it is pressed in by `sfeUserGroundOffset`.
- **Material data** (`componentsModule/MaterialComponent.h`, `assetsModule/modelModule/Material.h`): two user `vec4` a
  material (`user`), four texture slots `USER0`-`USER3`, `dissolve`, and the draw class (`DrawClass`: `Solid`,
  `Cutout`, `Ground`). A system that changes an entity's material or mesh says so with `TasksManager`'s
  `MATERIAL_UPDATED`.
- **Heightfield** (`componentsModule/HeightfieldComponent.h`): heights of any producer, which the physics collides with;
  its mesh `AssetsModule::heightfieldModel` and its height `heightfieldHeight` (`assetsModule/modelModule/Terrain.h`).
- **Sky** (`componentsModule/SkyComponent.h`): the sun's path is the scene's data -- `latitude`, `declination`, `noon`
  and `ground_albedo`.
- **Passes** (`renderModule/PassList.h`): the renderer draws the list it is given, `Render::defaultPasses()` with the
  application's own put in by name (`insertBefore`, `insertAfter`, `replace`, `disable`, `remove`; a name not in the
  list is an error), each with a predicate of when it runs. A pass that casts shadows implements `ShadowCasterPass`
  (`renderModule/renderPasses/RenderPass.h`), and the shadow pass finds it among the passes.
- **Systems** (`core/DefaultSystems.h`, `SFE::Modules`): the engine's systems as bricks the application calls in its
  own order, and the frame's order: `Modules::schedule()` roots the engine's, and the application puts its own before
  them (`addRootSystemsBefore<PhysicsDrawSystem, ...>`) or ticks them at a rate (`addTickSystems<...>(rate)`).
  `FreeCameraController` (`systemsModule/systems/FreeCameraController.h`), made by `addFreeCamera()`, flies the camera
  until whatever takes the camera over turns it off.
- **Numbers** (`core/Params.h`, `SFE::Params`): one registry for the engine's numbers and the application's, by group
  and name, with a range and a help text; one numbers file the application names (`EngineConfig::numbersFile`), read
  before any system is made; a session layer (the bridge's `tweak`, the command line) that is never saved. A reset goes
  back to the file's value, or the code's where the file has none. The engine's code defaults are neutral: a look is
  the file's.
- **Bridge commands** (`runtimeBridge/RuntimeBridge.h`): the engine's own are `bakeSurface`, `debugTerm`, `display`,
  `meshLods`, `press`, `profile`, `reloadModels`, `reloadScene`, `reloadShaders`, `reloadTextures`, `screenshot`,
  `setTransform` and `tweak`, registered in `start()` after the application's (`addCommand`). A name registered twice
  is logged as a warning, and the later one stands.
- **Debug UI** (`debugModule/DebugPanels.h`): the application's windows.
- **Scene components** (`propertiesModule/ComponentSerializer.h`, `ComponentRegistry::add<T>()`): a component a scene
  file may carry, read and saved.
- **LOD** (`renderModule/LodRule.h`): the rule of which level of a mesh's chain is drawn, the text of
  `shaders/sfe_lod.glsl` compiled as C++, for a pass that picks the levels of its own instances on the CPU.
- **Command line** (`runtime/RunOptions.h`): the engine's flags read, the application's own left in order.

Deliberately not offered, since no second application asks for them: replacing an engine system with a subclass,
rooting systems after the engine's, a `start()` of each system, a table of physics layers an application extends.
