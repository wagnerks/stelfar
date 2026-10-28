#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "Core.h"
#include "containersModule/Singleton.h"
#include "rhi/GraphicsBackend.h"
#include "rhi/RenderHardwareInterface.h"
#include "windowsModule/Window.h"

namespace SFE {
	struct EngineConfig;

	class Engine : public Singleton<Engine> {
		friend Singleton;

	public:
		// What this run is (EngineConfig, core/EngineConfig.h): once, before createWindow and initThread. Makes the RHI's
		// object for the backend chosen (Render::configureGraphicsBackend) and names the application's numbers file; a
		// second call is said and changes nothing.
		void init(const EngineConfig& config);
		const EngineConfig& config() const;
		// Whether this run is a deterministic frame capture (EngineConfig::capture): passes must drop anything that differs
		// from one run to the next (FPS counters, timing overlays) so two captures compare pixel for pixel.
		bool capturingFrame() const;

		Render::Window* createWindow(int width, int height, GLFWwindow* window = nullptr, const std::string& title = "Engine", Render::WindowHints hints = {});
		void setWindow(Render::Window* window);
		void destroyWindow();

		void initThread();
		// After every system is made and scheduled: how the tick systems run (EngineConfig), the input, the engine's
		// commands after the application's (a name both have stays the engine's).
		void start();

		void update();

		float getDeltaTime() const;
		// Seconds of animation time: advances by the frame step, and stands still while a frame is captured.
		double animationClock() const { return mAnimationClock; }
		int getFPS() const;

		bool isAlive() const;

		GLFWwindow* getMainWindow() const;
		Render::Window* getWindow() const;
		static bool isMainThread();
		static double clockSeconds();

		Render::RenderHardwareInterface* rhi() const { return mRHI; }

		// Whether frames go to the window -- runtime state: RuntimeBridge turns it off while it streams to an editor.
		void setPresentToScreen(bool present) { mPresentToScreen = present; }
		bool presentToScreen() const { return mPresentToScreen; }

	protected:
		Engine();
		~Engine() override;

	private:
		// Singleton's own, for instance(); the engine's is init(config).
		using Singleton::init;

		void updateDelta();
		bool checkNeedClose();
		void addRuntimeCommands();

		// Seconds, as a double: a float clock an hour in steps a quarter of a millisecond, and the frame times with it.
		double mLastFrame = 0.0;
		float mDeltaTime = 0.f;
		// The last few frames' real durations, which the step is the mean of; see updateDelta.
		std::array<float, 8> mRecentDeltas{};
		size_t mRecentCount = 0;
		size_t mRecentNext = 0;
		double mAnimationClock = 0.0;
		float mFramesTimer = 0.f;
		int mFramesCounter = 0;
		int mFPS = 0;


		bool mAlive = false;
		bool mPresentToScreen = false;
		// What init(config) was given; a default one until then.
		std::unique_ptr<EngineConfig> mConfig;

		CoreModule::Core mCore;
		
		Render::Window* mWindow = nullptr;
		inline static std::thread::id mMainThreadID;

		// The RHI asks its host for the window and the present flags instead of reaching for this
		// singleton. This is the engine answering; a test answers with its own stub.
		class RhiHostAdapter final : public Render::RhiHost {
		public:
			explicit RhiHostAdapter(Engine* engine) : mEngine(engine) {}
			Render::RhiSurface surface() const override;
			bool presentToScreen() const override { return mEngine->presentToScreen(); }
			std::string loadShaderSource(std::string_view path) const override;
			std::filesystem::path shaderCacheDir() const override;
			void onShaderCompile(std::string_view name, bool finished) const override;
		private:
			Engine* mEngine = nullptr;
		};
		RhiHostAdapter mRhiHost{ this };

		Render::RenderHardwareInterface* mRHI = nullptr;
	};
}

