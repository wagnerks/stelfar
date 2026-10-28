#include "FreeCameraController.h"

#include <GLFW/glfw3.h>

#include "CameraSystem.h"
#include "componentsModule/CameraComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "ecss/Registry.h"
#include "logsModule/logger.h"

namespace SFE::SystemsModule {
	namespace {
		// How fast the free flight goes, metres a second, and how far a pixel of the mouse turns it, degrees.
		constexpr float kMovementSpeed = 100.f;
		constexpr float kMouseSensitivity = 0.1f;
	}

	FreeCameraController::FreeCameraController() {
		mCameras = ECSHandler::getSystem<CameraSystem>();
		if (!mCameras) {
			LogsModule::Logger::LOG_ERROR("FreeCameraController: no CameraSystem made before it (Modules::addCamera): it flies nothing");
		}
		initKeyEvents();
	}

	bool FreeCameraController::keyDown(CoreModule::InputKey key) const {
		const int k = static_cast<int>(key);
		if (k < 0 || k >= kKeyCount) {
			return false;
		}
		return mPressed[static_cast<size_t>(k)].load(std::memory_order_relaxed);
	}

	void FreeCameraController::setKeyDown(CoreModule::InputKey key, bool down) {
		const int k = static_cast<int>(key);
		if (k < 0 || k >= kKeyCount) {
			return;
		}
		mPressed[static_cast<size_t>(k)].store(down, std::memory_order_relaxed);
	}

	void FreeCameraController::update(float dt) {
		// A capture holds the scene still, and its window is visible and takes focus: keys typed into another
		// window while it opens would fly the camera and make the frame depend on the user.
		if (!mCameras || Engine::instance()->capturingFrame() || !enabled()) {
			return;
		}
		auto cam = mCameras->getCurrentCamera();
		if (keyDown(CoreModule::InputKey::KEY_W)) {
			processKeyboard(cam, FreeFly::Move::Forward, dt);
		}
		if (keyDown(CoreModule::InputKey::KEY_S)) {
			processKeyboard(cam, FreeFly::Move::Backward, dt);
		}
		if (keyDown(CoreModule::InputKey::KEY_A)) {
			processKeyboard(cam, FreeFly::Move::Left, dt);
		}
		if (keyDown(CoreModule::InputKey::KEY_D)) {
			processKeyboard(cam, FreeFly::Move::Right, dt);
		}
		if (keyDown(CoreModule::InputKey::KEY_SPACE)) {
			processKeyboard(cam, FreeFly::Move::Up, dt);
		}
		if (keyDown(CoreModule::InputKey::KEY_LEFT_SHIFT)) {
			processKeyboard(cam, FreeFly::Move::Down, dt);
		}
	}

	void FreeCameraController::refreshMouseLook() {
		GLFWwindow* glfwWindow = Engine::instance()->getMainWindow();
		// Only the free camera's: while something else (an application's character) has the camera, the cursor is its.
		if (!enabled()) {
			return;
		}
		const bool want = mmbLook.load(std::memory_order_relaxed) || altLook.load(std::memory_order_relaxed);
		if (want == processMouse) {
			return;
		}
		processMouse = want;
		if (glfwWindow) {
			glfwSetInputMode(glfwWindow, GLFW_CURSOR, want ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
		}
	}

	void FreeCameraController::initKeyEvents() {
		onKeyEvent = [this](CoreModule::InputKey key, CoreModule::InputEventType type) {
			if (type == SFE::CoreModule::InputEventType::PRESS) {
				setKeyDown(key, true);
			}
			else if (type == SFE::CoreModule::InputEventType::RELEASE) {
				setKeyDown(key, false);
			}
			if (key == CoreModule::InputKey::KEY_LEFT_ALT || key == CoreModule::InputKey::KEY_RIGHT_ALT) {
				if (type == CoreModule::InputEventType::PRESS && lmbDown.load(std::memory_order_relaxed)) {
					altLook.store(true, std::memory_order_relaxed);
				}
				if (type == CoreModule::InputEventType::RELEASE) {
					altLook.store(false, std::memory_order_relaxed);
				}
				refreshMouseLook();
			}
		};

		onMouseBtnEvent = [this](Math::DVec2 mPos, CoreModule::MouseButton btn, CoreModule::InputEventType action) {
			GLFWwindow* glfwWindow = Engine::instance()->getMainWindow();
			if (!glfwWindow) {
				return;
			}
			const bool press = action == CoreModule::InputEventType::PRESS;
			const bool release = action == CoreModule::InputEventType::RELEASE;
			if (btn == CoreModule::MouseButton::MOUSE_BUTTON_LEFT) {
				lmbDown.store(press, std::memory_order_relaxed);
				if (press && (keyDown(CoreModule::InputKey::KEY_LEFT_ALT) || keyDown(CoreModule::InputKey::KEY_RIGHT_ALT))) {
					altLook.store(true, std::memory_order_relaxed);
				}
				if (release) {
					altLook.store(false, std::memory_order_relaxed);
				}
			}
			if (btn == CoreModule::MouseButton::MOUSE_BUTTON_MIDDLE) {
				mmbLook.store(press, std::memory_order_relaxed);
				if (release) {
					mmbLook.store(false, std::memory_order_relaxed);
				}
			}
			refreshMouseLook();
		};

		onMouseEvent = [this](Math::DVec2 mPos, Math::DVec2 mouseOffset) {
			processMouseMovement(static_cast<float>(mouseOffset.x), static_cast<float>(mouseOffset.y));
		};

		onScrollEvent = [this](Math::DVec2 scrollOffset) {
			processMouseScroll(static_cast<float>(scrollOffset.y));
		};
	}

	void FreeCameraController::processKeyboard(ecss::EntityId camera, FreeFly::Move direction, float deltaTime) {
		if (camera == ecss::INVALID_ID) {
			return;
		}

		Math::Mat4 viewMatrix{};
		bool moved = false;
		{
			auto tc = ECSHandler::registry().pinComponent<TransformComponent>(camera);
			if (!tc) {
				return;
			}

			const float velocity = kMovementSpeed * deltaTime;
			const Math::Vec3 dif = FreeFly::offset(direction, tc->getForward(), tc->getRight(), velocity);
			if (dif != Math::Vec3{}) {
				tc->setPos(tc->getPos() + dif);
				tc->reloadTransform();
				viewMatrix = tc->getViewMatrix();
				moved = true;
			}
		}

		if (!moved) {
			return;
		}

		if (auto cameraComp = ECSHandler::registry().pinComponent<CameraComponent>(camera)) {
			cameraComp->updateFrustum(viewMatrix);
		}
		TasksManager::instance()->notify({ camera, CAMERA_UPDATED });
	}

	void FreeCameraController::processMouseMovement(float xoffset, float yoffset, bool constrainPitch) {
		if (!processMouse || !mCameras || Engine::instance()->capturingFrame() || !enabled()) {
			return;
		}
		const auto camera = mCameras->getCurrentCamera();
		Math::Mat4 viewMatrix{};
		{
			auto tc = ECSHandler::registry().pinComponent<TransformComponent>(camera);
			if (!tc) {
				return;
			}
			tc->setRotate(FreeFly::look(tc->getRotate(), xoffset, yoffset, kMouseSensitivity, constrainPitch));
			tc->reloadTransform();
			viewMatrix = tc->getViewMatrix();
		}
		if (auto cameraComp = ECSHandler::registry().pinComponent<CameraComponent>(camera)) {
			cameraComp->updateFrustum(viewMatrix);
		}
	}

	void FreeCameraController::processMouseScroll(float yoffset) {
		if (!mCameras || Engine::instance()->capturingFrame() || !enabled()) {
			return;
		}
		const auto camera = mCameras->getCurrentCamera();
		Math::Mat4 viewMatrix{};
		{
			auto tc = ECSHandler::registry().pinComponent<TransformComponent>(camera);
			if (!tc) {
				return;
			}
			viewMatrix = tc->getViewMatrix();
		}
		auto cameraComp = ECSHandler::registry().pinComponent<CameraComponent>(camera);
		if (!cameraComp) {
			return;
		}
		cameraComp->getProjection().setFOV(FreeFly::zoom(cameraComp->getProjection().getFOV(), yoffset));
		cameraComp->updateFrustum(viewMatrix);
	}
}
