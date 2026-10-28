#pragma once

#include <functional>
#include <utility>

#include <gtest/gtest.h>

#include "core/DefaultSystems.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "core/EngineConfig.h"
#include "core/FileSystem.h"
#include "multithreading/ThreadPool.h"
#include "networkModule/NetworkSettings.h"
#include "renderModule/PassList.h"
#include "rhi/GraphicsBackend.h"

namespace SFE::EngineTests {
	// What a test made in the one engine all tests share, taken out on every way out of the test -- a failed ASSERT's
	// return among them -- so no test leaves anything behind for the tests after it.
	class Finally {
	public:
		explicit Finally(std::function<void()> run) : mRun(std::move(run)) {}
		~Finally() {
			if (mRun) {
				mRun();
			}
		}
		Finally(const Finally&) = delete;
		Finally& operator=(const Finally&) = delete;

	private:
		std::function<void()> mRun;
	};

	// Every frame advances exactly this much.
	inline constexpr float kFrameTime = 1.f / 60.f;

	// One engine for the whole run, put together as an application puts it together but on the None backend: no window,
	// no GPU, a fixed frame step, no numbers file, every module of the engine's. The engine is a process-wide singleton,
	// so it cannot be brought up per test.
	class EngineEnvironment : public testing::Environment {
	public:
		void SetUp() override {
			char arg0[] = "engine_tests";
			char arg1[] = "--backend";
			char arg2[] = "none";
			char* args[] = { arg0, arg1, arg2 };
			ASSERT_TRUE(Render::configureGraphicsBackend(3, args));

			const char* content = std::getenv("STELFORGE_CONTENT");
			FileSystem::setRoot(content ? std::string(content) : FileSystem::guessContentRoot().string());

			auto* engine = Engine::instance();
			EngineConfig config;
			config.fixedDeltaTime = kFrameTime;
			engine->init(config);
			engine->createWindow(1280, 720, nullptr, "engine tests");
			engine->initThread();
			Modules::addSceneCore();
			Modules::addCamera();
			Modules::addFreeCamera();
			Modules::addRenderer(Render::defaultPasses());
			Modules::addPhysics();
			Modules::addAudio();
			Modules::addNetwork({});
			Modules::schedule();
			engine->start();
			ASSERT_TRUE(engine->isAlive());
		}

		void TearDown() override {
			Engine::terminate();
		}
	};

	// Runs whole frames with nothing left in flight between them, so what a test reads after step() is the world
	// those frames produced and not a moment in the middle of a worker's job.
	inline void step(int frames = 1) {
		auto* engine = Engine::instance();
		for (int i = 0; i < frames; ++i) {
			ThreadPool::instance()->waitIdle();
			engine->update();
		}
		ThreadPool::instance()->waitIdle();
	}
}
