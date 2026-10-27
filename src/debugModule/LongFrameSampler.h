#pragma once

// Diagnostics (LONG_FRAME_SAMPLES=<file>, read by the engine as it starts): the main thread's stack, every millisecond of
// a frame that lasts 40 ms or more, appended to the file -- where a long frame's time went, function by function, when
// the profiler's zones are too coarse to say. Windows on x64 only; elsewhere these do nothing.
namespace SFE::Debug::LongFrameSampler {
	// Called on the main thread, whose stack is sampled. What comes before the first frame (the load) is not.
	void start(const char* path);
	// A frame begins: the engine calls it first thing in each update.
	void frame();
	// Sampling ends and its thread goes; the engine calls it as it shuts down.
	void stop();
}
