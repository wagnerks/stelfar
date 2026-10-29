#include "Engine.h"
#include "core/LoadTrace.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <GLFW/glfw3.h>
#include <json/writer.h>

#include "Clocks.h"
#include "Core.h"
#include "EngineConfig.h"
#include "InputHandler.h"
#include "KeyNames.h"
#include "Pacing.h"
#include "Params.h"
#include "assetsModule/shaderModule/ShaderBase.h"
#include "logsModule/logger.h"
#include "runtimeBridge/RuntimeBridge.h"
#include "core/ECSHandler.h"
#include "componentsModule/CameraComponent.h"
#include "componentsModule/DebugDataComponent.h"
#include "componentsModule/TransformComponent.h"
#include "systemsModule/systems/CameraSystem.h"
#include "systemsModule/systems/RenderSystem.h"
#include "windowsModule/Window.h"
#include "assetsModule/TextureHandler.h"
#include "assetsModule/modelModule/MeshVaoRegistry.h"
#include "assetsModule/modelModule/ModelLoader.h"
#include "assetsModule/modelModule/SurfaceBake.h"
#include "core/FileSystem.h"
#include "propertiesModule/ModelBinding.h"
#include "propertiesModule/PropertiesSystem.h"
#include "multithreading/ThreadPool.h"
#include "rhi/GraphicsBackend.h"
#include "rhi/RhiLog.h"
#include "rhi/RhiFactory.h"
#include "renderModule/TextRenderer.h"
#include "renderModule/FrameCapture.h"
#include "renderModule/DebugView.h"
#include "renderModule/RenderResources.h"
#include "debugModule/LongFrameSampler.h"
#include "debugModule/Profiler.h"

namespace SFE {
	using Render::activeGraphicsBackend;
	using Render::graphicsBackendName;
	using Render::GraphicsBackend;

	namespace {
		// The frame limit of the one engine a process has (Engine::instance): set by start(), waited out at the end of
		// every update (core/Pacing.h).
		FramePacer gPacer;
	}

	// Seconds since the first call. Not glfwGetTime: the None backend never initialises GLFW.
	double Engine::clockSeconds() {
		static const auto start = std::chrono::steady_clock::now();
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	}

	void Engine::setWindow(Render::Window* window) {
		if (mWindow) {
			delete mWindow;
		}
		mWindow = window;
		if (mWindow) {
			mWindow->onFramebufferResize = [](int width, int height) {
				SFE::ThreadPool::instance()->addTask<SFE::WorkerType::SYNC>([width, height]() {
					//ViewportStack::size = { width, height };
				});
			};

			checkNeedClose();
		}
	}

	void Engine::init(const EngineConfig& config) {
		if (mRHI) {
			LogsModule::Logger::LOG_ERROR("Engine::init: once only");
			return;
		}
		*mConfig = config;
		Params::setFile(config.numbersFile);
		Render::setRhiLogSink([](const Render::RhiLogLevel level, const char* message) {
			switch (level) {
			case Render::RhiLogLevel::Info: LogsModule::Logger::LOG_INFO("%s", message); break;
			case Render::RhiLogLevel::Warning: LogsModule::Logger::LOG_WARNING("%s", message); break;
			case Render::RhiLogLevel::Error: LogsModule::Logger::LOG_ERROR("%s", message); break;
			case Render::RhiLogLevel::Fatal: LogsModule::Logger::LOG_FATAL(false, "%s", message); break;
			}
		});
		mRHI = Render::createRhi(activeGraphicsBackend()).release();
		mRHI->setHost(&mRhiHost);
		LogsModule::Logger::LOG_INFO("Engine backend: %s", graphicsBackendName(activeGraphicsBackend()));
	}

	const EngineConfig& Engine::config() const {
		return *mConfig;
	}

	bool Engine::capturingFrame() const {
		return mConfig->capture.enabled;
	}

	void Engine::initThread() {
		if (!mWindow) {
			SFE::LogsModule::Logger::LOG_FATAL(false, "Try to initialize thread without a window, forgot to call createWindow?");
			return;
		}
		if (!mRHI) {
			SFE::LogsModule::Logger::LOG_FATAL(false, "Engine::init(config) before initThread");
			return;
		}

		mMainThreadID = std::this_thread::get_id();
		Debug::Profiler::nameThread("Main");
		// On this thread, the one it samples.
		if (const char* samplesPath = std::getenv("LONG_FRAME_SAMPLES")) {
			Debug::LongFrameSampler::start(samplesPath);
		}
		// A capture runs the render tasks on the thread that asks for them, so a frame's work is done in the same order
		// on every run.
		ThreadPool::instance()->setRenderTasksInline(mConfig->capture.enabled);
		// The numbers the engine and the application are tuned by, as the application's numbers file has them
		// (EngineConfig::numbersFile; none named, none read), before any system or pass is made: what reads a number once,
		// as it is made, gets the file's. The debug view first, so --debug-term's value is taken now.
		Render::debugView();
		Params::load();
		if (activeGraphicsBackend() == GraphicsBackend::OpenGL) {
			glfwMakeContextCurrent(mWindow->getWindow());
		}
		if (mRHI) {
			LoadTrace::Step step("graphics device");
			mRHI->init();
		}
		mCore.init();

		SFE::LogsModule::Logger::LOG_INFO("engine thread initialized");
	}

	void Engine::start() {
		// Settled at the first update, for every tick system whenever it was added: stepped from the frame for a fixed step
		// or a capture, each on a thread of its own in real time.
		const bool wallClock = realTime(*mConfig);
		ECSHandler::systemManager().setStepTickSystems(!wallClock);
		// After every system: the window's input goes to the observers they are.
		CoreModule::InputHandler::init(mWindow);
		addRuntimeCommands();
		if (activeGraphicsBackend() == GraphicsBackend::OpenGL) {
			glfwSwapInterval(0);
		}
		SFE::LogsModule::Logger::LOG_INFO("engine started (%s backend)",
			graphicsBackendName(activeGraphicsBackend()));
		// The frame limit from here: maxFps while the step is the wall clock's, none under a fixed step or a capture
		// (pacedFps). A run in real time waits on the clock -- its frames for the limit, its tick threads for their ticks,
		// whatever the limit -- so it has the OS's timer asked for naps of a millisecond, before the first update makes the
		// tick threads; a stepped run waits on no clock and asks for nothing.
		if (wallClock) {
			askFineNaps();
		}
		gPacer.start(pacedFps(*mConfig));
	}


	Render::Window* Engine::createWindow(int width, int height, GLFWwindow* window, const std::string& title, Render::WindowHints hints) {
		if (activeGraphicsBackend() == GraphicsBackend::None) {
			setWindow(new Render::Window(width, height, title));
			return mWindow;
		}
		if (activeGraphicsBackend() == GraphicsBackend::Vulkan) {
			hints.ints[Render::CLIENT_API] = GLFW_NO_API;
		}
		setWindow(new Render::Window(width, height, title, window, hints));

		return mWindow;
	}

	void Engine::destroyWindow() {
		delete mWindow;
		mWindow = nullptr;
	}

	// The engine's commands (RuntimeBridge): files edited while it runs taken up when asked -- by a panel's button, or a
	// command sent from outside -- rather than on a restart, and the picture as it is. Each runs at the start of a frame
	// (RuntimeBridge::runQueued).
	void Engine::addRuntimeCommands() {
		auto* bridge = RuntimeBridge::RuntimeBridge::instance();
		bridge->addCommand("profile",
			"Where the frame's time goes (Debug::Profiler; F2 shows it in the game), over the last {\"frames\"} frames, all that "
			"are kept (600) if left out, as JSON: the frame, the main thread's waits, the GPU, what holds the frame back (the "
			"GPU, the CPU or the fps limit), each zone of the CPU's work by thread, each render pass on the GPU. {\"file\": path} writes it "
			"there as text too, beside the game unless absolute; {\"on\": false} stops recording zones, true starts it again; "
			"{\"window\": true | false} opens or closes the window; {\"shaders\": true} times the GPU by the pipelines drawn "
			"in each pass too, with a timestamp at each change of one (RenderHardwareInterface::gpuShaderMarks).",
			[this](const Json::Value& args) {
				const bool given = args.isObject();
				const int frames = given ? args.get("frames", 0).asInt() : 0;
				if (given && args.isMember("on")) {
					Debug::Profiler::gOn.store(args["on"].asBool(), std::memory_order_relaxed);
				}
				if (given && args.isMember("window")) {
					Debug::Profiler::showWindow(args["window"].asBool());
				}
				if (given && args.isMember("shaders") && mRHI) {
					mRHI->gpuShaderMarks.store(args["shaders"].asBool(), std::memory_order_relaxed);
				}
				if (given && args.isMember("file") && !Debug::Profiler::writeSummary(args["file"].asString(), frames)) {
					return "not written: " + args["file"].asString();
				}
				return Debug::Profiler::summaryJson(frames);
			});
		bridge->addCommand("display",
			"The display as the Graphics panel sets it, for this run only (display.json is left as it is): {\"mode\": "
			"\"windowed\" | \"borderless\" | \"fullscreen\", \"width\", \"height\", \"scale\" (the share of it drawn), \"frameInfo\"}, "
			"what is left out kept. "
			"Put on the screen at the start of the next frame.",
			[this](const Json::Value& args) {
				const auto& screen = mWindow->getScreenData();
				auto settings = mWindow->display() ? *mWindow->display()
					: Render::DisplaySettings{ .width = screen.renderW, .height = screen.renderH };
				mWindow->setDisplay(Render::displayFromJson(args, settings));
				settings = *mWindow->display();
				return std::string(Render::kDisplayModeNames[static_cast<size_t>(settings.mode)]) + " "
					+ std::to_string(settings.width) + "x" + std::to_string(settings.height)
					+ " drawn at " + std::to_string(settings.scale)
					+ (settings.frameInfo ? ", frame info shown" : ", frame info hidden");
			});
		bridge->addCommand("screenshot", "The last frame as a PNG: {\"path\": ...}, under the content root unless absolute.",
			[](const Json::Value& args) {
				const auto path = args.isObject() ? args.get("path", "screenshot.png").asString() : std::string("screenshot.png");
				return Render::captureFrameToPng(path) ? "written to " + path : "not written: " + path;
			});
		// What the renderer holds, for a check that turning its passes on and off gives back what it took: the numbers of
		// the overlay's memory line and of the passes' own targets, read by a script.
		bridge->addCommand("passes",
			"The renderer's passes as JSON: \"passes\", each entry of its list in draw order and whether its pass exists now "
			"(\"made\"; PassEntry::on); \"memory\", what the RHI holds on the device by kind, MiB (targets, depth, textures, "
			"meshes, buffers); \"held\", the screen targets the passes hold by name (Render::Resources), with how many hold "
			"each and its MiB, and \"heldMiB\" their sum.",
			[this](const Json::Value&) -> std::string {
				auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
				if (!renderSystem) {
					return "no renderer";
				}
				constexpr double kMiB = 1024.0 * 1024.0;
				Json::Value out;
				auto& passes = out["passes"];
				passes = Json::arrayValue;
				for (const auto& name : renderSystem->renderPassNames()) {
					Json::Value pass;
					pass["name"] = name;
					pass["made"] = renderSystem->passOn(name);
					passes.append(pass);
				}
				if (mRHI) {
					const auto memory = mRHI->gpuMemory();
					auto& kinds = out["memory"];
					kinds["targets"] = static_cast<double>(memory.targets) / kMiB;
					kinds["depth"] = static_cast<double>(memory.depth) / kMiB;
					kinds["textures"] = static_cast<double>(memory.textures) / kMiB;
					kinds["meshes"] = static_cast<double>(memory.meshes) / kMiB;
					kinds["buffers"] = static_cast<double>(memory.buffers) / kMiB;
				}
				auto& held = out["held"];
				held = Json::arrayValue;
				uint64_t heldBytes = 0;
				for (const auto& target : renderSystem->resources().held()) {
					Json::Value one;
					one["name"] = target.name;
					one["holders"] = static_cast<Json::Int64>(target.holders);
					one["mib"] = static_cast<double>(target.bytes) / kMiB;
					held.append(one);
					heldBytes += target.bytes;
				}
				out["heldMiB"] = static_cast<double>(heldBytes) / kMiB;
				Json::StreamWriterBuilder writer;
				writer["indentation"] = "";
				writer["precision"] = 3;
				writer["precisionType"] = "decimal";
				return Json::writeString(writer, out);
			});
		bridge->addCommand("reloadShaders",
			"Every shader read again from its file, and what changed compiled again. One that does not compile keeps the "
			"old one, and the log says why.",
			[this](const Json::Value&) {
				const size_t made = mRHI ? mRHI->reloadShaders() : 0;
				return std::to_string(made) + " shaders compiled again";
			});
		bridge->addCommand("reloadModels",
			"Every model whose file was written since it was read, read again, and what was made from it bound to it again. "
			"{\"path\": ...} for one model, changed or not.",
			[](const Json::Value& args) {
				std::vector<std::string> paths;
				if (args.isObject() && args.isMember("path")) {
					paths.push_back(args["path"].asString());
				}
				else {
					for (const auto* model : AssetsModule::AssetsManager::instance()->changedOnDisk<AssetsModule::Model>()) {
						paths.push_back(model->assetPath);
					}
				}
				std::string said;
				for (const auto& path : paths) {
					const int entities = PropertiesModule::ModelBinding::reload(path);
					said += (said.empty() ? "" : "; ") + path + (entities < 0 ? " did not read" : ": " + std::to_string(entities) + " entities");
				}
				return paths.empty() ? std::string("no model has changed") : said;
			});
		bridge->addCommand("meshLods",
			"A model's meshes as they are drawn, each level of detail's triangles and how far it strays from the full mesh "
			"(MeshLod, a share of its extent): {\"path\": model}.",
			[](const Json::Value& args) -> std::string {
				auto* model = args.isObject() ? AssetsModule::ModelLoader::instance()->load(args["path"].asString()) : nullptr;
				if (!model || model->getLODs()->empty()) {
					return "give {\"path\": a model}";
				}
				std::string said;
				auto* meshes = MeshVaoRegistry::instance();
				for (auto* meshObject : model->getLODs()->front().meshes) {
					const auto& data = meshes->get(&meshObject->mesh);
					const auto table = meshes->meshTable(data.meshId);
					said += said.empty() ? "" : "; ";
					for (uint32_t level = 0; level < data.lodCount && level < table.size(); ++level) {
						char line[48];
						std::snprintf(line, sizeof(line), "%s%u", level ? " " : "", table[level].indexCount / 3);
						said += line;
						if (level) {
							std::snprintf(line, sizeof(line), "@%.4f", table[0].lodErrors[level - 1]);
							said += line;
						}
					}
				}
				return said;
			});
		bridge->addCommand("reloadScene",
			"The scene file read again, and what changed in it taken up: an entity moved, turned or scaled, a model "
			"swapped, an entity new to the file made and one gone from it taken out, colliders with them. What a system "
			"built once from the scene is left as it was, and named: it waits for a restart.",
			[](const Json::Value&) {
				return PropertiesModule::PropertiesSystem::reloadScene();
			});
		bridge->addCommand("reloadTextures",
			"Every texture whose file was written since it was read, read again into the same texture. One whose size "
			"changed needs a restart.",
			[](const Json::Value&) {
				std::string unchanged;
				const size_t made = AssetsModule::TextureHandler::reloadChanged(unchanged);
				return std::to_string(made) + " textures read again" + (unchanged.empty() ? "" : "; a new size, left as they were: " + unchanged);
			});
		bridge->addCommand("bakeSurface",
			"A model's meshes unrolled round a sphere and drawn flat, on the CPU (AssetsModule::bakeSurface): {\"model\": path, "
			"\"nodes\": [names, all if none], \"centre\": [x,y,z] (else the sphere fitted to them, or to the nodes of "
			"\"fitTo\"), \"up\", \"front\", "
			"\"azimuth\": [from, to], \"polar\": [from, to] degrees, \"size\": [w, h], \"normals\": \"height\" or \"mesh\", "
			"\"colour\": png, \"normal\": png (its alpha the height)}.",
			[](const Json::Value& args) -> std::string {
				if (!args.isObject() || !args.isMember("model")) {
					return "give {\"model\": path, ...}";
				}
				auto* model = AssetsModule::ModelLoader::instance()->load(args["model"].asString());
				if (!model) {
					return "no model " + args["model"].asString();
				}
				// The meshes of the nodes a list names; all of them for none.
				const auto meshesOf = [&](const Json::Value& list) {
					std::vector<std::string> names;
					for (const auto& name : list) {
						names.push_back(name.asString());
					}
					std::vector<const MeshObject3D*> found;
					std::function<void(const Tree<MeshObject3D>&)> gather = [&](const Tree<MeshObject3D>& node) {
						if (!node.value.mesh.indices.empty()
							&& (names.empty() || std::find(names.begin(), names.end(), node.value.nodeName) != names.end())) {
							found.push_back(&node.value);
						}
						for (const auto& child : node.children) {
							gather(child);
						}
					};
					gather(model->getMeshTree());
					return found;
				};
				const auto meshes = meshesOf(args["nodes"]);
				if (meshes.empty()) {
					return "none of those meshes in " + args["model"].asString();
				}
				const auto vec3 = [&](const char* key, const Math::Vec3& otherwise) {
					const auto& value = args[key];
					return value.isArray() ? Math::Vec3{ value[0].asFloat(), value[1].asFloat(), value[2].asFloat() } : otherwise;
				};
				const auto sphere = AssetsModule::fitSphere(args.isMember("fitTo") ? meshesOf(args["fitTo"]) : meshes);
				AssetsModule::SphereUnwrap unwrap;
				unwrap.centre = vec3("centre", { sphere.x, sphere.y, sphere.z });
				unwrap.up = vec3("up", unwrap.up);
				unwrap.front = vec3("front", unwrap.front);
				for (int i = 0; i < 2; ++i) {
					unwrap.azimuth[i] = args["azimuth"].get(static_cast<Json::ArrayIndex>(i), unwrap.azimuth[i]).asFloat();
					unwrap.polar[i] = args["polar"].get(static_cast<Json::ArrayIndex>(i), unwrap.polar[i]).asFloat();
				}
				const int width = args["size"].get(0u, 1024).asInt();
				const int height = args["size"].get(1u, 512).asInt();
				const auto baked = AssetsModule::bakeSurface(meshes, unwrap, width, height, args.get("normals", "height").asString() != "mesh");
				std::string written;
				for (const auto& [key, pixels] : { std::pair{ "colour", &baked.colour }, std::pair{ "normal", &baked.normal } }) {
					if (args.isMember(key)) {
						const auto path = args[key].asString();
						// Under the application's root, as every write is, never over one of the engine's own pictures.
						written += (Render::writePng((FileSystem::root() / path).lexically_normal().string(), *pixels, baked.width, baked.height) ? " " : " NOT ") + path;
					}
				}
				char said[256];
				std::snprintf(said, sizeof(said), "%zu meshes round [%.4f, %.4f, %.4f] (fitted radius %.4f): %.1f%% drawn, radius %.4f..%.4f;",
					meshes.size(), unwrap.centre.x, unwrap.centre.y, unwrap.centre.z, sphere.w, baked.covered * 100.f, baked.radiusMin, baked.radiusMax);
				return said + written;
			});
		bridge->addCommand("setTransform",
			"An entity's transform, by the scene's {\"id\": ...} or {\"entity\": n}: \"pos\" [x,y,z], \"rotate\" [x,y,z] degrees, "
			"\"scale\" [x,y,z] or one number -- whichever are given; then where it is. One a system places every frame "
			"(the player, say) goes back there.",
			[](const Json::Value& args) -> std::string {
				auto& registry = ECSHandler::registry();
				auto entity = ecss::INVALID_ID;
				if (args.isObject() && args.isMember("entity")) {
					entity = static_cast<ecss::EntityId>(args["entity"].asUInt());
				}
				else if (args.isObject() && args.isMember("id")) {
					const auto id = args["id"].asString();
					for (const auto& [candidate, debug] : registry.view<const DebugDataComponent>()) {
						if (debug && debug->stringId == id) {
							entity = candidate;
							break;
						}
					}
				}
				if (entity == ecss::INVALID_ID) {
					return "no entity by that id; give {\"id\": ...} or {\"entity\": n}";
				}
				auto transform = registry.pinComponent<ComponentsModule::TransformComponent>(entity);
				if (!transform) {
					return "entity " + std::to_string(entity) + " has no transform";
				}
				const auto vec3 = [](const Json::Value& value) {
					return value.isArray() ? Math::Vec3{ value[0].asFloat(), value[1].asFloat(), value[2].asFloat() } : Math::Vec3{ value.asFloat() };
				};
				if (args.isMember("pos")) {
					transform->setPos(vec3(args["pos"]));
				}
				if (args.isMember("rotate")) {
					transform->setRotate(vec3(args["rotate"]));
				}
				if (args.isMember("scale")) {
					transform->setScale(vec3(args["scale"]));
				}
				const auto text = [](const Math::Vec3& v) {
					char out[96];
					std::snprintf(out, sizeof(out), "[%.3f, %.3f, %.3f]", v.x, v.y, v.z);
					return std::string(out);
				};
				return "entity " + std::to_string(entity) + ": pos " + text(transform->getPos()) + ", rotate "
					+ text(transform->getRotate()) + ", scale " + text(transform->getScale());
			});
		// A number of the registry, the engine's or the application's, read or set for the session: what a script sets so
		// is never saved, and a reload, which reads the file again, takes it back. The answers are parsed by scripts: the
		// number after the last '='.
		bridge->addCommand("tweak",
			"A number of the Tweaks panel: {\"group\": ..., \"name\": ..., \"value\": a number or true/false, to set it}; then what "
			"it is. A value set so holds for this run until a reload and is never saved. {\"reload\": true} reads every number "
			"again from " + (Params::file().empty() ? std::string("the numbers file, once one is named (Params::setFile)")
				: Params::file()) + ".",
			[](const Json::Value& args) -> std::string {
				if (!args.isObject()) {
					return "give {group, name[, value]} or {reload}";
				}
				if (args.get("reload", false).asBool()) {
					const auto named = Params::file();
					if (named.empty()) {
						return "no numbers file named";
					}
					return Params::load() ? "tweaks read again from " + named : "no " + named + " to read";
				}
				const auto group = args.get("group", "").asString();
				const auto name = args.get("name", "").asString();
				Params::Seen seen{};
				if (!Params::see(group, name, seen)) {
					return "no tweak " + group + " / " + name;
				}
				if (args.isMember("value")) {
					const auto& value = args["value"];
					if ((!value.isNumeric() && !value.isBool())
						|| !Params::setSession(group, name, value.asDouble(), Params::Session::UntilReload)) {
						return "not a value for " + group + " / " + name;
					}
					Params::see(group, name, seen);
				}
				// As the number is: a float to six decimals, an int whole, a bool 1 or 0.
				const auto said = group + " / " + name + " = ";
				switch (seen.type) {
				case Params::Type::Int: return said + std::to_string(static_cast<int>(seen.value));
				case Params::Type::Bool: return said + std::to_string(seen.value != 0.0 ? 1 : 0);
				default: return said + std::to_string(static_cast<float>(seen.value));
				}
			});
		// What the lighting draws, as --debug-term sets it at the start, changed while it runs; held for the session like a
		// tweak, so a reload takes it back to what the command line said.
		bridge->addCommand("debugTerm",
			"What the lighting draws, as --debug-term: {\"term\": n} (0 the picture, 1 shadow, 4 albedo, 5 normals, 6 AO, 15 "
			"material AO); then which.",
			[](const Json::Value& args) -> std::string {
				if (args.isObject() && args.isMember("term")) {
					Params::setSession(Render::kDebugViewGroup, Render::kDebugViewName, args["term"].asInt(), Params::Session::UntilReload);
				}
				return "debug term " + std::to_string(Render::debugView());
			});
		// Keys and the mouse's buttons held and let go, and its wheel turned, by a command: the input a script sends is the
		// window's, given to every listener of it (InputHandler) as the window gives it. Only the panels, which hear the
		// window itself, do not hear it.
		bridge->addCommand("press",
			"A key or a mouse button held or let go as from the keyboard or the mouse: {\"key\": " + CoreModule::keyNames()
			+ ", or LMB, RMB, MMB (at the cursor where it is), \"down\": true or false (true if left out)}; or {\"wheel\": "
			"notches} the mouse wheel turned.",
			[](const Json::Value& args) -> std::string {
				const auto give = "give {\"key\": " + CoreModule::keyNames() + ", or LMB, RMB, MMB, \"down\": true or false}, "
					"or {\"wheel\": notches}";
				if (args.isObject() && args.isMember("wheel")) {
					if (!args["wheel"].isNumeric()) {
						return give;
					}
					CoreModule::InputHandler::injectScroll(0.0, args["wheel"].asDouble());
					return "wheel turned " + args["wheel"].asString();
				}
				const auto name = args.isObject() ? args.get("key", "").asString() : std::string();
				const auto button = CoreModule::buttonNamed(name);
				const auto key = CoreModule::keyNamed(name);
				if (!button && key == CoreModule::InputKey::UNKNOWN) {
					return give;
				}
				const bool down = args.get("down", true).asBool();
				const int action = static_cast<int>(down ? CoreModule::InputEventType::PRESS : CoreModule::InputEventType::RELEASE);
				if (button) {
					// At the cursor where it is, as the window gives a click.
					CoreModule::InputHandler::mouseBtnInput(static_cast<int>(*button), action, 0);
				}
				else {
					CoreModule::InputHandler::injectKey(static_cast<int>(key), action);
				}
				return name + (down ? " held" : " let go");
			});
	}

	void Engine::update() {
		// A frame begins for the long-frame sampler (LONG_FRAME_SAMPLES), so all of it is sampled.
		Debug::LongFrameSampler::frame();
		if (!checkNeedClose()) {
			return;
		}
		// The frame before closed for the profiler, before anything of this one is timed.
		Debug::Profiler::frame();

		// The display the player set (Window::setDisplay), put on the screen between frames. A picture of another size has
		// every pass make its targets again, and the camera takes its new shape.
		if (mWindow && mWindow->applyDisplay()) {
			if (auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>()) {
				renderSystem->resize();
			}
			if (auto* cameras = ECSHandler::getSystem<SystemsModule::CameraSystem>()) {
				if (auto camera = ECSHandler::registry().pinComponent<CameraComponent>(cameras->getCurrentCamera())) {
					const auto& screen = mWindow->getScreenData();
					camera->getProjection().setAspect(static_cast<float>(screen.width) / static_cast<float>(screen.height));
				}
			}
		}

		// The GPU first: the frame's time is taken once the frame it replaces has left the GPU, so each frame is one of
		// the GPU's, and what is read from the input is as fresh as it can be for the picture it goes into.
		if (mRHI) {
			SFE_PROFILE_ZONE(Debug::Profiler::kWaitGpu);
			mRHI->beginFrame();
		}
		// Then everything that arrived since the last frame, all of it before any system reads it: nothing polled in a
		// frame is left for the next. The window's input first, then the commands, a script's press among them.
		if (getMainWindow()) {
			SFE_PROFILE_ZONE("Input");
			glfwPollEvents();
		}
		// The commands asked for since the last frame, now that it has begun and nothing of it is recorded yet: what they
		// upload goes with the frame's own uploads.
		if (RuntimeBridge::RuntimeBridge::isAlive()) {
			SFE_PROFILE_ZONE("Bridge commands");
			RuntimeBridge::RuntimeBridge::instance()->runQueued();
		}
		updateDelta();

		mCore.update(mDeltaTime);

		//getWindow()->swapBuffers();

		if (gPacer.paced()) {
			SFE_PROFILE_ZONE(Debug::Profiler::kIdle);
			gPacer.endFrame();
		}
	}

	float Engine::getDeltaTime() const {
		return mDeltaTime;
	}

	int Engine::getFPS() const {
		return mFPS;
	}

	void Engine::updateDelta() {
		// A capture steps at a fixed rate: wall-clock deltas make everything time-dependent
		// (TAA history, volumetrics) land differently on a faster or slower run, which would
		// show up as a backend difference when it is only a timing difference.
		const float fixedDeltaTime = mConfig->fixedDeltaTime;
		const bool capturing = mConfig->capture.enabled;
		const bool stepped = !realTime(*mConfig);
		if (stepped) {
			// Assigned, not derived from an accumulated clock: a float clock loses bits as it grows, and the
			// difference of two of its values would stop being exactly the step.
			mDeltaTime = fixedDeltaTime > 0.f ? fixedDeltaTime : 1.f / 60.f;
			mLastFrame += mDeltaTime;
		}
		else {
			const double currentFrame = clockSeconds();
			const auto raw = static_cast<float>(currentFrame - mLastFrame);
			mLastFrame = currentFrame;
			// The GPU turns frames out at a steady pace, but they reach the CPU in bursts -- two quick frames and a long
			// one -- so the real duration jitters though nothing on screen does, and everything moved by it judders. The
			// step is the mean of the last few instead; over any stretch the steps still add up to the time that passed.
			// A real stall is not spread out: it is stepped as it came, and does not count towards the mean.
			float mean = 0.f;
			for (size_t i = 0; i < mRecentCount; ++i) {
				mean += mRecentDeltas[i];
			}
			mean = mRecentCount ? mean / static_cast<float>(mRecentCount) : raw;
			if (mRecentCount == mRecentDeltas.size() && raw > mean * 3.f) {
				mDeltaTime = raw;
			}
			else {
				mRecentDeltas[mRecentNext] = raw;
				mRecentNext = (mRecentNext + 1) % mRecentDeltas.size();
				mRecentCount = (std::min)(mRecentCount + 1, mRecentDeltas.size());
				mDeltaTime = 0.f;
				for (size_t i = 0; i < mRecentCount; ++i) {
					mDeltaTime += mRecentDeltas[i];
				}
				mDeltaTime /= static_cast<float>(mRecentCount);
			}
		}

		// A capture holds the pose still, unless it runs at a fixed step: then a clip plays the same way on every run. A
		// hold (Clocks::hold) stands it still at its own time whatever the step.
		mAnimationClock = Clocks::beginFrame(stepped ? mLastFrame : -1.0, !capturing || fixedDeltaTime > 0.f ? mDeltaTime : 0.f);

		mFramesCounter++;
		mFramesTimer += mDeltaTime;
		if (mFramesTimer >= 1.f) {
			mFPS = mFramesCounter;
			mFramesCounter = 0;
			mFramesTimer = 0.f;
		}
	}

	bool Engine::isAlive() const {
		return mAlive;
	}

	bool Engine::checkNeedClose() {
		if (!mWindow) {
			mAlive = false;
			return false;
		}
		if (!mPresentToScreen) {
			mAlive = true;
			return true;
		}
		mAlive = !mWindow->isClosing();
		return mAlive;
	}


	GLFWwindow* Engine::getMainWindow() const {
		return mWindow ? mWindow->getWindow() : nullptr;
	}

	Render::Window* Engine::getWindow() const {
		return mWindow;
	}

	bool Engine::isMainThread() {
		return mMainThreadID == std::this_thread::get_id();
	}

	// A default run until init(config) says what this one is; the RHI's object is made there.
	Engine::Engine() : mConfig(std::make_unique<EngineConfig>()) {}

	Engine::~Engine() {
		Debug::LongFrameSampler::stop();
		if (mRHI) {
			if (mWindow && mWindow->getWindow() && activeGraphicsBackend() == GraphicsBackend::OpenGL) {
				glfwMakeContextCurrent(mWindow->getWindow());
			}
			mRHI->waitIdle();
		}
		mCore.shutdown();
		if (ThreadPool::isAlive()) {
			ThreadPool::instance()->shutdown();
		}
		if (mRHI) {
			if (mWindow && mWindow->getWindow() && activeGraphicsBackend() == GraphicsBackend::OpenGL) {
				glfwMakeContextCurrent(mWindow->getWindow());
			}
			mRHI->cleanup();
			delete mRHI;
			mRHI = nullptr;
		}
		destroyWindow();
	}

	Render::RhiSurface Engine::RhiHostAdapter::surface() const {
		Render::RhiSurface surface;
		auto* window = mEngine->getWindow();
		if (!window || !window->getWindow()) {
			return surface;
		}
		const auto& screen = window->getScreenData();
		surface.window = window->getWindow();
		surface.visible = window->isVisible();
		surface.width = screen.width;
		surface.height = screen.height;
		surface.renderWidth = screen.renderW;
		surface.renderHeight = screen.renderH;
		surface.backbufferWidth = screen.backbufferW;
		surface.backbufferHeight = screen.backbufferH;
		return surface;
	}

	std::string Engine::RhiHostAdapter::loadShaderSource(std::string_view path) const {
		return ShaderModule::ShaderBase::loadShaderCode(path);
	}

	// Compiled SPIR-V is a cache like any other the application's runs make, kept under its root with them and never in
	// the engine's shaders: the engine's assets stay as shipped, and two applications on one engine do not overwrite each
	// other's binaries of a shader both have by name.
	std::filesystem::path Engine::RhiHostAdapter::shaderCacheDir() const {
		return (FileSystem::root() / "cache" / "spirv").lexically_normal();
	}

	// Log only. Retitling the window from here ran inside a frame, where the resulting window message
	// reached ImGui's GLFW hook mid-frame and crashed.
	void Engine::RhiHostAdapter::onShaderCompile(std::string_view name, bool finished) const {
		if (!finished) {
			LogsModule::Logger::LOG_INFO("compiling shader %s", std::string(name).c_str());
		}
	}
}
