#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace SFE {
	// How long each part of a load took. Stages are named and may nest; the time of a name adds up over every stage
	// that carried it, so loading twenty models shows as one "models" line rather than twenty.
	//
	// Meant to stay in the build. A load that grew slower is then one table to read -- which stage, how much, what
	// share of the whole -- instead of a bisect through the history. The table goes to the log and, so two builds can
	// be put side by side, to load_times.log next to the executable.
	//
	//     {
	//         SFE::LoadTrace::Step step("terrain");
	//         ...
	//     }                                  // the stage closes here
	//     SFE::LoadTrace::report("scene.json");
	class LoadTrace {
	public:
		// A stage, lasting as long as this object. Nested stages are timed on their own; the time is not taken off
		// the stage around them, so shares may add up past a hundred per cent, and that is the point: an outer stage
		// tells where the time went, an inner one tells why.
		class Step {
		public:
			explicit Step(std::string name);
			~Step();
			Step(const Step&) = delete;
			Step& operator=(const Step&) = delete;

		private:
			std::string mName;
			std::chrono::steady_clock::time_point mStart;
		};

		// Time measured elsewhere, added under a name. For work that is spread over threads or callbacks, where a
		// scope does not fit.
		static void add(std::string_view name, double seconds);
		// Something worth seeing beside the times: how many models, how many textures, how big a terrain.
		static void count(std::string_view name, long long many);

		// A C++ type name as typeid gives it, cut down to the plain class name: "class SFE::X::RenderSystem" -> "RenderSystem".
		static std::string plainName(std::string_view typeName);

		// Print what has been gathered and start afresh. `what` names the load, usually the scene's path.
		static void report(std::string_view what);
		// Throw away what has been gathered without printing it.
		static void reset();
		// Off by default for everything but the first load of a scene; a game may turn it on where it likes.
		static void listen(bool on);
		static bool listening();
	};
}
