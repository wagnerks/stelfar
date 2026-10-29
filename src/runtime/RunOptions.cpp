#include "runtime/RunOptions.h"

#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/FileSystem.h"
#include "core/Params.h"
#include "renderModule/DebugView.h"
#include "systemsModule/systems/CameraSystem.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace SFE {
	bool CaptureSchedule::enabled() const {
		return !path.empty();
	}

	std::optional<std::string> CaptureSchedule::due(const int frames, const int taken) const {
		if (!enabled() || frames < frame + taken * every) {
			return std::nullopt;
		}
		std::string file = path;
		if (const auto mark = file.find("%d"); mark != std::string::npos) {
			file.replace(mark, 2, std::to_string(taken));
		}
		return file;
	}

	bool CaptureSchedule::last(const int taken) const {
		return taken >= count || every <= 0;
	}

	const char* RunOptions::systemEnv(const char* name) {
		return std::getenv(name);
	}

	RunOptions RunOptions::parse(const int argc, char** argv, const Render::GraphicsBackend backend) {
		RunOptions o;
		o.maxFps = backend == Render::GraphicsBackend::None ? kNothingDrawnFps : EngineConfig{}.maxFps;
		o.rest.push_back(argc > 0 && argv ? argv[0] : nullptr);
		// A flag's value is the argument after it; a flag given last, with none, is not taken.
		const auto valued = [&](const int i) { return i + 1 < argc && argv[i + 1]; };
		const auto after = [](const std::string_view arg, const std::string_view flag) { return std::string(arg.substr(flag.size())); };
		for (int i = 1; i < argc; ++i) {
			if (!argv[i]) {
				continue;
			}
			const std::string_view arg{ argv[i] };
			if (arg == "--serve") {
				o.serve = true;
				o.visible = false;
			}
			// Served on another port than the editor's own: a second game on the same machine, to try online play with.
			else if (arg == "--serve-port" && valued(i)) {
				o.servePort = static_cast<uint16_t>(std::atoi(argv[++i]));
			}
			else if (arg == "--headless") {
				o.visible = false;
			}
			else if (arg == "--visible") {
				o.visible = true;
				o.explicitlyVisible = true;
			}
			else if (arg == "--capture" && valued(i)) {
				o.capture.path = argv[++i];
			}
			else if (arg.starts_with("--capture=")) {
				o.capture.path = after(arg, "--capture=");
			}
			else if (arg == "--capture-frame" && valued(i)) {
				o.capture.frame = std::atoi(argv[++i]);
			}
			else if (arg.starts_with("--capture-frame=")) {
				o.capture.frame = std::atoi(after(arg, "--capture-frame=").c_str());
			}
			else if (arg == "--capture-every" && valued(i)) {
				o.capture.every = std::atoi(argv[++i]);
			}
			else if (arg == "--capture-count" && valued(i)) {
				o.capture.count = std::atoi(argv[++i]);
			}
			else if (arg == "--camera" && valued(i)) {
				o.cameraPos = argv[++i];
			}
			else if (arg == "--camera-rot" && valued(i)) {
				o.cameraRot = argv[++i];
			}
			// The picture without TAA: the engine's number held so for this run, whatever the numbers file has.
			else if (arg == "--no-taa") {
				o.numbers.push_back({ "Graphics", "taa", 0.0 });
			}
			else if (arg == "--scene" && valued(i)) {
				o.scene = argv[++i];
			}
			// What the lighting draws (0 the picture; the views the debugTerm command lists), held for this run the same way.
			else if (arg == "--debug-term" && valued(i)) {
				o.numbers.push_back({ Render::kDebugViewGroup, Render::kDebugViewName, static_cast<double>(std::atoi(argv[++i])) });
			}
			else if (arg.starts_with("--debug-term=")) {
				o.numbers.push_back({ Render::kDebugViewGroup, Render::kDebugViewName,
					static_cast<double>(std::atoi(after(arg, "--debug-term=").c_str())) });
			}
			else if (arg == "--capture-passes" && valued(i)) {
				o.passDumpDir = argv[++i];
			}
			else if (arg.starts_with("--capture-passes=")) {
				o.passDumpDir = after(arg, "--capture-passes=");
			}
			else if (arg == "--frames" && valued(i)) {
				o.frameLimit = std::atoi(argv[++i]);
			}
			else if (arg == "--fixed-dt" && valued(i)) {
				o.fixedDeltaTime = static_cast<float>(std::atof(argv[++i]));
			}
			// Online play: host a session (on the default port, or the one given), or join one at address[:port].
			else if (arg == "--host") {
				o.network.role = Network::Role::Host;
				if (valued(i) && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) {
					o.network.port = static_cast<uint16_t>(std::atoi(argv[++i]));
				}
			}
			else if (arg == "--connect" && valued(i)) {
				o.network.role = Network::Role::Client;
				const std::string_view target = argv[++i];
				const auto colon = target.rfind(':');
				o.network.address = std::string(target.substr(0, colon));
				if (colon != std::string_view::npos) {
					o.network.port = static_cast<uint16_t>(std::atoi(std::string(target.substr(colon + 1)).c_str()));
				}
			}
			// A worse network to try the game over: round trip added and its jitter (ms), messages lost (percent).
			else if (arg == "--net-lag" && valued(i)) {
				o.network.conditions.lagMs = static_cast<float>(std::atof(argv[++i]));
			}
			else if (arg == "--net-jitter" && valued(i)) {
				o.network.conditions.jitterMs = static_cast<float>(std::atof(argv[++i]));
			}
			else if (arg == "--net-loss" && valued(i)) {
				o.network.conditions.lossPercent = static_cast<float>(std::atof(argv[++i]));
			}
			else if (arg == "--players" && valued(i)) {
				o.network.maxPlayers = static_cast<uint32_t>((std::max)(1, std::atoi(argv[++i])));
			}
			else if (arg == "--content-dir" && valued(i)) {
				o.contentDir = argv[++i];
			}
			else if (arg.starts_with("--content-dir=")) {
				o.contentDir = after(arg, "--content-dir=");
			}
			// Read by Render::configureGraphicsBackend; taken here so the application does not meet it.
			else if ((arg == "--backend" && valued(i)) || arg.starts_with("--backend=")) {
				if (arg == "--backend") {
					++i;
				}
			}
			else {
				o.rest.push_back(argv[i]);
			}
		}

		// A capture is meant to be reproducible from a script, so it defaults to a hidden window: nothing can steal focus
		// or occlude it, and the pixels come from the render target anyway.
		if (o.capture.enabled()) {
			o.capture.frame = (std::max)(o.capture.frame, 1);
			if (!o.explicitlyVisible) {
				o.visible = false;
			}
		}
		return o;
	}

	RunOptions RunOptions::fromCommandLine(const int argc, char** argv) {
		if (!Render::configureGraphicsBackend(argc, argv)) {
			RunOptions failed;
			failed.ok = false;
			return failed;
		}
		return parse(argc, argv, Render::activeGraphicsBackend());
	}

	std::filesystem::path RunOptions::contentRoot(const Env& env) const {
		if (!contentDir.empty()) {
			return contentDir;
		}
		if (const char* given = env("STELFORGE_CONTENT")) {
			return given;
		}
		return FileSystem::guessContentRoot();
	}

	void RunOptions::apply(const std::function<std::filesystem::path(const std::filesystem::path& contentRoot)>& engineRootFallback,
		const Env& env) const {
		const auto content = contentRoot(env);
		FileSystem::setRoot(content.string());
		if (const char* given = env("STELFORGE_ENGINE_CONTENT")) {
			FileSystem::setEngineRoot(given);
		}
		else if (engineRootFallback) {
			FileSystem::setEngineRoot(engineRootFallback(content).string());
		}
		// Held for the run: a name nothing has registered yet waits for its registration.
		for (const auto& number : numbers) {
			Params::setSession(number.group, number.name, number.value, Params::Session::Run);
		}
	}

	EngineConfig RunOptions::engineConfig() const {
		EngineConfig config;
		config.fixedDeltaTime = fixedDeltaTime;
		config.maxFps = maxFps;
		config.capture.enabled = capture.enabled();
		config.capture.frame = capture.frame;
		if (capture.enabled() && !passDumpDir.empty()) {
			const std::filesystem::path requested(passDumpDir);
			config.capture.passDumpDir = requested.is_absolute() ? requested.string() : FileSystem::resolve(passDumpDir).string();
		}
		return config;
	}

	// The camera placed from the command line, so a capture can frame whatever is being investigated without a
	// change to the camera's defaults.
	void RunOptions::placeCamera() const {
		if (cameraPos.empty() && cameraRot.empty()) {
			return;
		}
		const auto parseVec3 = [](const std::string& text, Math::Vec3& out) {
			return std::sscanf(text.c_str(), "%f,%f,%f", &out.x, &out.y, &out.z) == 3;
		};
		if (auto* camSys = ECSHandler::getSystem<SystemsModule::CameraSystem>()) {
			if (auto transform = ECSHandler::registry().pinComponent<ComponentsModule::TransformComponent>(camSys->getCurrentCamera())) {
				Math::Vec3 value{};
				if (!cameraPos.empty() && parseVec3(cameraPos, value)) {
					transform->setPos(value);
				}
				if (!cameraRot.empty() && parseVec3(cameraRot, value)) {
					transform->setRotate(value);
				}
				transform->reloadTransform();
			}
		}
	}
}
