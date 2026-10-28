#include "Core.h"

#include "ECSHandler.h"
#include "Engine.h"
#include "imgui.h"
#include "InputHandler.h"
#include "multithreading/ThreadPool.h"
#include "assetsModule/modelModule/ModelLoader.h"
#include "assetsModule/TextureHandler.h"
#include "debugModule/imguiDecorator.h"
#include "systemsModule/SystemManager.h"
#include "assetsModule/AssetsManager.h"
#include "backends/imgui_impl_opengl3.h"
#include "componentsModule/TransformComponent.h"
#include "debugModule/Benchmark.h"
#include "logsModule/logger.h"
#include "mathModule/Forward.h"
#include "renderModule/TextRenderer.h"
#include "systemsModule/TasksManager.h"

namespace SFE::CoreModule {
	void Core::update(float dt) {
		FUNCTION_BENCHMARK;

		ThreadPool::instance()->syncUpdate();
		{
			SFE_PROFILE_ZONE("Systems");
			ECSHandler::systemManager().update(dt);
		}
		ThreadPool::instance()->syncUpdate();
		SFE_PROFILE_ZONE("Registry");
		ECSHandler::registry().update();
	}

	void Core::init() {
		// ecss names every component whose array loses the memmove paths. Its own reporter
		// writes to stderr, which a windowed build has nowhere to show, so send it to the log.
		ecss::setTrivialityReporter([](std::string_view component) {
			LogsModule::Logger::LOG_WARNING(
				"ecss: component '%.*s' is not trivially copyable -- its array falls back to "
				"per-element move + destroy for inserts and defragmentation",
				static_cast<int>(component.size()), component.data());
		});
		ECSHandler::registry().setAccessTracking(true);

		// Systems that keep state per entity hear of every destroy, including one made straight on the registry: one
		// that went unheard left its instances drawn where it stood.
		ECSHandler::registry().setDestroyListener([](ecss::EntityId entity, void*) {
			SFE::SystemsModule::TasksManager::instance()->notify({ entity, SFE::SystemsModule::TaskType::ENTITY_DESTROYED });
		});
		// A new transform is reloaded, and its render copy made, once the registry has stored it: TransformSystem's worker
		// may take the event at once, and one told before the store found nothing there and dropped it.
		ECSHandler::registry().setAddListener<TransformComponent>([](ecss::EntityId entity, void*) {
			SFE::SystemsModule::TasksManager::instance()->notify({ entity, SFE::SystemsModule::TaskType::TRANSFORM_UPDATE });
		});
	}

	void Core::shutdown() {
		if (mShutdown) {
			return;
		}
		mShutdown = true;
		// Systems / GPU assets still need Engine::rhi() and the GL context.
		if (ECSHandler::isAlive()) {
			ECSHandler::terminate();
		}
		AssetsModule::TextureHandler::terminate();
		AssetsModule::ModelLoader::terminate();
		ThreadPool::terminate();
		AssetsModule::AssetsManager::terminate();
	}

	Core::~Core() {
		shutdown();
	}
}