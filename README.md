# Stellar Forge Engine (STELFORGE ENGINE // SFE)
![image](https://github.com/stalker1177/KillRealGameEngine/assets/44020451/d3b12f9f-cecf-4a9e-a9c9-94c58e93d26e)

![image](https://github.com/stalker1177/KillRealGameEngine/assets/44020451/478cfff6-a3db-4673-a4a7-0b7a4c3cc3ba)

there is just some piece of pet project, you can find a lot of draft and junk code here, but i will fix every module after finishing working on it

The main goal is to create and to try modern fast game engine included draw and physics parts, with dod ecs, with high quality light system. Thinking about doing it specifically for some piece of open world space survival game.   

Which means really big open world, atmospheres, stars, lights, may be some procedural algorithm for planets and star systems, and I want it looks as much as possible.

# GOALS  

- [x] Entity Component System with Data Oriented Design
- [x] OpenGL Scene Rendering  
- [x] 3d models loading
- [x] imgui
- [x] deferred shading
- [x] directional ortho light shadows
- [ ] perspective ortho light
- [x] point light
- [ ] sky (don't like current realisation) 
- [ ] invent engine name (KillReallGameEngine not bad, but is the best?)
- [x] frustum culling
- [ ] depth culling
- [x] LOD
- [ ] LOD depending on screen space size instead distane
- [ ] batcher (existing just draft)
- [ ] own 3d math module
- [ ] custom allocator for game data (models, sprites etc)
- [x] custom allocator for ECS
- [ ] entities actions
- [x] ability to draw object via custom shader which ok with deferred shading
- [ ] precompile models format and faster models loading in game
- [ ] own gui (imgui good, but not very fast)
- [ ] add specular and other maps to the models
- [ ] skeleton animation
- [ ] (possible some 3d tools for converting normals in needed for me way)
- [x] added converting normals to engine coordinates system
- [ ] 3d objects visual redactor utils
- [ ] drag and drop
- [x] choose object by click
- [x] outlines
- [x] multithread integration, first of all add ability to update systems in parallel(almost done but no pushed yet)
- [x] Shadows (don't like how it looks now, but it works) 
- [ ] SSAO(current algorithm is not fast enough, and have some glitches while fast moving)
- [ ] SSR
- [x] HDR
- [ ] volumetric FOG
- [x] logs
- [ ] logs to file
- [ ] volumetric clouds with shadows
- [ ] dynamic meshes
- [ ] vulkan 
- [ ] directx
- [ ] ray tracing
- [ ] grass  

I will add new goals as soon as find somenew

# Putting the engine together

The engine is a library: it makes no world, no systems and no render passes of its own accord. Whatever runs it -- a
game's `main.cpp`, an editor that runs the engine in its own process -- puts it together on the main thread, in this
order:

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
   `addSceneCore()`, `addCamera()`, `addFreeCamera()` (a free-flying camera, if it wants one), `loadScene(path)`,
   `addRenderer(passes)`, `addPhysics()`, `addAudio()`, `addNetwork(settings)`. The renderer draws the list it is given:
   `Render::defaultPasses()` with the application's own passes put in by name (`Render::PassList`,
   `renderModule/PassList.h`; a name not in the list is an error). Then the application's own systems
   (`ECSHandler::systemManager().createSystem<T>(...)`) and the frame's order: `Modules::schedule()` roots the engine's
   systems that were made, and the application puts its own before them
   (`addRootSystemsBefore<PhysicsDrawSystem, ...>()`, or `addTickSystems<...>(rate)`). Scheduling never makes a system;
   a system made with an update of its own but in no schedule is named in `load_times.log`. Then what else it brings:
   bridge commands (`RuntimeBridge::addCommand`), windows in the debug UI (`debugModule/DebugPanels.h`), upright
   capsules for both physics worlds (`PhysicsSystem::addStaticCapsules`).
5. `Engine::start()`: how the tick systems run (on threads, or stepped from the frame under a fixed step or a capture),
   the input, the engine's own bridge commands after the application's.
6. `while (engine->isAlive()) engine->update();`, then `Engine::terminate()`.

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
