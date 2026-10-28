# No-render mode plan

Status: done. Since then the engine's own executable (`StelForgeRuntime`) and the tick systems WorldTime and SkeletalAnimation are gone, the fixed step is `EngineConfig::fixedDeltaTime` (given once to `Engine::init`), and Escape is the application's: the engine no longer closes the window on it.

Goal: run the engine with no window and no GPU, to run engine unit tests without a picture (game tests will later be part of the runtime itself). What must work:
- game systems and ECS;
- asset loading on the CPU;
- the editor bridge (`--serve`);
- a fixed time step.

Approach: a third RHI backend, `None`, the same way as Unreal `-nullrhi` and Godot `--headless`. Engine code keeps calling the RHI and does not branch on the mode. The only exception is the window, which has no GPU equivalent.

## 1. Null RHI backend (`engine/rhi`)
- [x] `GraphicsBackend::None`. `parseName` accepts `none`/`null`, and `graphicsBackendName` prints `None`. An unknown name is an error, not a silent switch to OpenGL.
- [x] `NullRHI` implements every pure virtual as a no-op and overrides `backend()`:
  - every `create*` / `allocate*` / `beginCommandList` / `getBackbufferFramebuffer` returns a unique valid id, because callers key caches on `.id`;
  - `framesInFlight()` returns 1 and `frameInFlight()` returns 0;
  - `readbackTexture` returns false;
  - `runOnWorkerThread` runs the work.
- [x] `createRhi(None)` creates `NullRHI`.
- [x] `rhi_tests`: a small smoke test that NullRHI hands out unique handles and survives a frame of calls. The pixel tests stay on OpenGL/Vulkan only, and the fixture labels become three-way.

## 2. Engine without a window
- [x] `Window` without a GLFW handle: `ScreenData` comes from a fixed size (from the command line, 1280x720 by default). Camera, cascades and passes keep reading `getScreenData()`.
- [x] Guard the few GLFW-only uses:
  - `Engine::initThread`: the no-window check, the Esc handler, `glfwPollEvents`, `checkNeedClose`;
  - `InputHandler::init`;
  - `runtime/main.cpp`: `glfwInit` failure, `createWindow`, `glfwWindowShouldClose` / `SetWindowShouldClose`.
- [x] Time without GLFW: `Engine::updateDelta` / `fpsSync` and the `SystemManager` tick threads use `std::chrono` instead of `glfwGetTime`. No `glfwInit` is needed in None mode.

## 3. Fixed time step
- [x] `--fixed-dt <seconds>` (and a setter on Engine) generalises the capture path: `updateDelta` returns the fixed value, and `fpsSync` does not wait.
- [x] Tick systems (WorldTime, SkeletalAnimation) under a fixed step: stepped from `Engine::update` with an accumulator instead of their own wall-clock threads, so frame N is the same scene on every run.
- [x] `--frames N`: leave the loop after N frames with exit code 0.

## 4. Editor bridge
- [x] `--serve --backend none`: no embedding in the editor window (there is no window), no Frame packets (readback returns false), and input injection and the rest of the protocol unchanged.
- [x] Editor `RuntimeWidget`: passes `none` through `STELFORGE_BACKEND` and shows "no picture" instead of an empty view.

## 5. Engine unit tests
- [x] `engine_tests` target (GoogleTest, like `rhi_tests`): one engine per process in None mode with a fixed step, plus helpers to step N frames and read components.
- [x] First tests:
  - a spawned entity's rotation reaches `TransformMatComp` (does not reproduce the `markDirty` race: it passes on the old code too, the window is too narrow);
  - an animation advances by exactly dt * ticks;
  - WorldTime advances in whole ticks under the fixed step.
- [x] Runtime flag path: `StelForgeRuntime --backend none --fixed-dt 0.0166667 --frames 600` runs the default scene and exits 0.

## Open questions
- Resolved: RenderSystem keeps running its passes against NullRHI. That costs about 0.13 ms of CPU per frame, so there is no reason to skip them.
- Linux CI: None mode does not touch GLFW, so no display is needed. That needs checking when a Linux build exists.
