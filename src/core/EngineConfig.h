#pragma once

#include <string>

namespace SFE {
	// What a run of the engine is, given once as it starts (Engine::init) and the same to its end. What changes while it
	// runs is not here: whether the window shows the picture (Engine::setPresentToScreen), the numbers (SFE::Params).
	struct EngineConfig {
		// Non-zero: every frame advances exactly this many seconds, as fast as it can, and the tick systems step from the
		// frame instead of their own wall-clock threads -- frame N is the same world on every run.
		float fixedDeltaTime = 0.f;
		// Frames a second at most while the step is the wall clock's (0: no limit).
		int maxFps = 600;
		// A deterministic frame capture: passes drop what differs from one run to the next (FPS counters, timing overlays),
		// frames step 1/60 s unless fixedDeltaTime says otherwise, tick systems step from the frame, render tasks run
		// inline. `frame`: the frame it lands on; `passDumpDir`: where each pass of that frame is dumped (empty: none).
		struct Capture {
			bool enabled = false;
			int frame = 0;
			std::string passDumpDir;
		} capture;
		// The application's numbers file under the content root (SFE::Params::setFile): read before any system is made,
		// written by the Tweaks window's save. Empty: none -- every number stays as the code has it.
		std::string numbersFile;
	};
}
