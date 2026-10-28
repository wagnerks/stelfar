#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/EngineConfig.h"
#include "editorProtocol/EditorProtocol.h"
#include "networkModule/NetworkSettings.h"
#include "rhi/GraphicsBackend.h"

// The engine's command line: what a run of it is, as an application's main gives it the flags it was started with.
// Included by an application's main and the tests only.
namespace SFE {
	// --capture path [--capture-frame n] [--capture-every n --capture-count m]: the frame written as a PNG, or every n-th
	// from it, m of them (the path's "%d" the capture's number, from 0), the run going on between them.
	struct CaptureSchedule {
		std::string path;   // empty: no capture
		int frame = 240;
		int every = 0;
		int count = 1;

		bool enabled() const;
		// After frame `frames` (from 1), `taken` captures written so far: the path to write now, if one is due.
		std::optional<std::string> due(int frames, int taken) const;
		// With `taken` written (the count after this one): whether that was the last, and the run is to end.
		bool last(int taken) const;
	};

	// A number of SFE::Params held for the run (Params::Session::Run), whatever the numbers file has, and never saved.
	struct SessionNumber {
		std::string group;
		std::string name;
		double value = 0.0;
	};

	// The flags of the engine, read once from the command line: the step, the frame limit, the capture, the window, the
	// network, the roots, the scene, the numbers held for the run. The application's own flags are left for it in `rest`.
	struct RunOptions {
		using Env = std::function<const char*(const char*)>;

		// The frames a second of a run whose backend draws nothing: no GPU frame bounds it, and its ticks and its
		// network go at 60.
		static constexpr int kNothingDrawnFps = 60;

		bool ok = true;                       // false: a backend name not known (fromCommandLine)
		float fixedDeltaTime = 0.f;           // --fixed-dt s
		int maxFps = EngineConfig{}.maxFps;   // kNothingDrawnFps when the backend draws nothing
		CaptureSchedule capture;              // its frame at least 1 when a path is given
		std::string passDumpDir;              // --capture-passes dir: each pass of the captured frame, with a capture only
		std::vector<SessionNumber> numbers;   // --no-taa, --debug-term n: in the order given
		std::string contentDir;               // --content-dir dir
		std::string scene;                    // --scene path; empty: the application's own
		Network::Settings network;            // --host [port], --connect address[:port], --net-lag/-jitter/-loss, --players
		std::string cameraPos;                // --camera x,y,z
		std::string cameraRot;                // --camera-rot x,y,z (degrees)
		int frameLimit = 0;                   // --frames n: leave after n frames; 0 runs until closed
		bool serve = false;                   // --serve: to the editor (RuntimeBridge), hidden unless --visible after it
		uint16_t servePort = EditorProtocol::kDefaultPort;   // --serve-port
		bool visible = true;                  // --headless, --visible, --serve, as they come; a capture hides unless --visible
		bool explicitlyVisible = false;       // --visible given
		// argv[0], then every argument not taken here, in order: the application's flags and their values.
		std::vector<char*> rest;

		// Every argument read, nothing else: no global or environment. `backend` is the one the run has, which sets the
		// frame limit.
		static RunOptions parse(int argc, char** argv, Render::GraphicsBackend backend);
		// The backend chosen first (Render::configureGraphicsBackend: STELFORGE_BACKEND, then --backend), then parse with
		// it. `ok` false when a backend's name is not one.
		static RunOptions fromCommandLine(int argc, char** argv);

		// The application's content root: --content-dir, else STELFORGE_CONTENT, else FileSystem::guessContentRoot().
		std::filesystem::path contentRoot(const Env& env = systemEnv) const;
		// Once, before anything reads them: FileSystem's roots -- the content root, and the engine's own assets behind it
		// (STELFORGE_ENGINE_CONTENT, else what `engineRootFallback` makes of the content root: where the application keeps
		// the engine) -- then the numbers held for the run, in the order given.
		void apply(const std::function<std::filesystem::path(const std::filesystem::path& contentRoot)>& engineRootFallback,
			const Env& env = systemEnv) const;
		// After apply: the step, the frame limit and the capture, a relative pass folder found under the roots. The
		// numbers file is the application's to name.
		EngineConfig engineConfig() const;
		// After the scene is loaded and before Engine::start(): --camera and --camera-rot on the current camera.
		void placeCamera() const;

		static const char* systemEnv(const char* name);
	};
}
