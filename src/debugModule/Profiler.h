#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// The frame profiler: what each thread spends its time on, frame by frame, cheap enough to leave on (Profiler.cpp). A
// zone is two clock reads and a write into its thread's own ring; the main thread takes the rings in once a frame
// (frame()), keeps the last frames' sums of each zone and the last frame's zones themselves, and shows them in a window
// (F2, drawWindow), answers the bridge's "profile" command and writes a summary to a file. The GPU's side, each render
// pass's time and, marked by pipeline, each pipeline's in it, is RenderDebugInfo's (RenderSystem).
namespace SFE::Debug::Profiler {
	// Off, a zone costs a load and a branch.
	inline std::atomic<bool> gOn{ true };

	// Zones the overview reads by name: the main thread waiting for the GPU, waiting for the fps limit, and handing the
	// picture to the screen (which waits for the screen under vsync). None of them is the CPU's own work.
	inline constexpr const char* kWaitGpu = "Wait GPU";
	inline constexpr const char* kIdle = "Idle (fps limit)";
	inline constexpr const char* kPresent = "Present";

	// Now, in the profiler's ticks: steady_clock's.
	int64_t now();
	// A span of work on this thread, from `begin` to `end` (now()). `name` must outlive the program: a literal, a
	// type_info's name (shown without its namespaces), or what intern() gave.
	void zone(const char* name, int64_t begin, int64_t end);
	// What this thread is called on the timeline, and which group it is counted in: "Main", "Common", "Tick"; `index` > 0
	// numbers it within the group ("Common 12"). Once, as the thread starts.
	void nameThread(const char* group, int index = 0);
	// A name kept for good, for zones named at run time (a render pass's).
	const char* intern(const std::string& name);

	// Main thread, at the start of each frame: the one before closed, its zones taken in and counted.
	void frame();
	// Main thread, inside an ImGui frame: the window, F2 opening and closing it.
	void drawWindow();
	void showWindow(bool shown);
	// The last `frames` frames (as many as are kept, 600, if 0) as JSON: the frame's time, the main thread's work and
	// waits, the GPU's time, each zone's mean, own and worst time per frame by thread, each render pass's GPU time.
	std::string summaryJson(int frames = 0);
	// The same as text, written to `path` (its folders made); false if it could not be.
	bool writeSummary(const std::string& path, int frames = 0);
	// The share of the frame the GPU worked and the main thread worked (its waits left out), over the last second or so:
	// the one near 1 holds the frame back. Recounted a few times a second, cheap to ask every frame (the overlays do).
	struct Load {
		float gpu = 0.f;
		float cpu = 0.f;
		float gpuMs = 0.f;
		float cpuMs = 0.f; // the main thread's own work a frame
	};
	Load load();

	struct Zone {
		explicit Zone(const char* name) : mName(name), mBegin(gOn.load(std::memory_order_relaxed) ? now() : 0) {}
		~Zone() {
			if (mBegin) {
				zone(mName, mBegin, now());
			}
		}
		Zone(const Zone&) = delete;
		Zone& operator=(const Zone&) = delete;

	private:
		const char* mName;
		int64_t mBegin;
	};
}

#define SFE_PROFILE_CONCAT_DETAIL(a, b) a##b
#define SFE_PROFILE_CONCAT(a, b) SFE_PROFILE_CONCAT_DETAIL(a, b)
// A zone from here to the end of the scope.
#define SFE_PROFILE_ZONE(name) const ::SFE::Debug::Profiler::Zone SFE_PROFILE_CONCAT(sfeProfileZone, __LINE__)(name)
