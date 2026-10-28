#pragma once

#include <array>
#include <atomic>

#include "core/InputHandler.h"
#include "systemsModule/FreeFly.h"
#include "systemsModule/SystemBase.h"

namespace SFE::SystemsModule {
	class CameraSystem;

	// Flies the current camera (CameraSystem) by the keys held -- W, S, A, D, Space and Left Shift --, turns it with the
	// mouse while the middle button is held (or the left one with Alt) and zooms it with the wheel. In no default module:
	// an application that wants it makes it (Modules::addFreeCamera), right after the camera.
	class FreeCameraController : public ecss::System, CoreModule::InputObserver {
	public:
		FreeCameraController();
		void update(float dt) override;

		// Off: the keyboard and mouse no longer fly the current camera -- something else (an application's character) has
		// it, and the cursor is that one's. On when made.
		void setEnabled(bool enabled) { mEnabled.store(enabled, std::memory_order_relaxed); }
		bool enabled() const { return mEnabled.load(std::memory_order_relaxed); }

	private:
		void initKeyEvents();
		void refreshMouseLook();
		void processKeyboard(ecss::EntityId camera, FreeFly::Move direction, float deltaTime);
		void processMouseMovement(float xoffset, float yoffset, bool constrainPitch = true);
		void processMouseScroll(float yoffset);
		bool keyDown(CoreModule::InputKey key) const;
		void setKeyDown(CoreModule::InputKey key, bool down);

		// The cameras it flies the current one of; null when there was no CameraSystem as it was made: it flies nothing.
		CameraSystem* mCameras = nullptr;

		bool processMouse = false;
		static constexpr int kKeyCount = 512;
		std::array<std::atomic<bool>, kKeyCount> mPressed{};
		std::atomic<bool> mmbLook{ false };
		std::atomic<bool> altLook{ false };
		std::atomic<bool> lmbDown{ false };
		std::atomic<bool> mEnabled{ true };
	};
}
