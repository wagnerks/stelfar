#include "CameraSystem.h"

#include <algorithm>
#include <GLFW/glfw3.h>

#include "OcTreeSystem.h"
#include "PhysicsSystem.h"
#include "componentsModule/CameraComponent.h"
#include "componentsModule/ModelComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "ecss/Registry.h"

namespace SFE::SystemsModule {
	CameraSystem::CameraSystem() {
		auto aspect = static_cast<float>(Engine::instance()->getWindow()->getScreenData().width) / static_cast<float>(Engine::instance()->getWindow()->getScreenData().height);
		mDefaultCamera = ECSHandler::registry().takeEntity();

		auto transform = ECSHandler::registry().addComponent<TransformComponent>(mDefaultCamera, mDefaultCamera);
		transform->setPos({ 0.f, 2.2f, 6.5f });
		transform->setRotate({ -18.f, 0.f, 0.0f });
		transform->reloadTransform();
		ECSHandler::registry().addComponent<CameraComponent>(mDefaultCamera, 45.f, aspect, Engine::instance()->getWindow()->getScreenData().near, Engine::instance()->getWindow()->getScreenData().far)->updateFrustum(transform->getViewMatrix());
		initKeyEvents();
	}

	bool CameraSystem::keyDown(CoreModule::InputKey key) const {
		const int k = static_cast<int>(key);
		if (k < 0 || k >= kKeyCount) {
			return false;
		}
		return mPressed[static_cast<size_t>(k)].load(std::memory_order_relaxed);
	}

	void CameraSystem::setKeyDown(CoreModule::InputKey key, bool down) {
		const int k = static_cast<int>(key);
		if (k < 0 || k >= kKeyCount) {
			return;
		}
		mPressed[static_cast<size_t>(k)].store(down, std::memory_order_relaxed);
	}

	void CameraSystem::update(float_t dt) {
		// A capture holds the scene still, and its window is visible and takes focus: keys typed into another
		// window while it opens would fly the camera and make the frame depend on the user.
		if (Engine::instance()->capturingFrame() || !userControl()) {
			return;
		}
		auto cam = getCurrentCamera();
		if (keyDown(CoreModule::InputKey::KEY_W)) {
			processKeyboard(cam, FORWARD, dt);
		}
		if (keyDown(CoreModule::InputKey::KEY_S)) {
			processKeyboard(cam, BACKWARD, dt);
		}
		if (keyDown(CoreModule::InputKey::KEY_A)) {
			processKeyboard(cam, LEFT, dt);
		}
		if (keyDown(CoreModule::InputKey::KEY_D)) {
			processKeyboard(cam, RIGHT, dt);
		}
		if (keyDown(CoreModule::InputKey::KEY_SPACE)) {
			processKeyboard(cam, TOP, dt);
		}
		if (keyDown(CoreModule::InputKey::KEY_LEFT_SHIFT)) {
			processKeyboard(cam, BOTTOM, dt);
		}
	}

	void CameraSystem::placeCamera(const Math::Vec3& pos, const Math::Vec3& rotate) {
		const auto camera = getCurrentCamera();
		Math::Mat4 viewMatrix{};
		{
			auto tc = ECSHandler::registry().pinComponent<TransformComponent>(camera);
			if (!tc) {
				return;
			}
			tc->setPos(pos);
			tc->setRotate(rotate);
			tc->reloadTransform();
			viewMatrix = tc->getViewMatrix();
			// The renderer draws from the copy this publishes. Left to the transform worker it lands this frame or the
			// next, and whatever the camera follows, placed the same frame, would shake against it.
			TasksManager::instance()->notify({ camera, TRAHSFORM_RELOADED, tc.get() });
		}
		if (auto cameraComp = ECSHandler::registry().pinComponent<CameraComponent>(camera)) {
			cameraComp->updateFrustum(viewMatrix);
		}
		TasksManager::instance()->notify({ camera, CAMERA_UPDATED });
	}

	void CameraSystem::setCurrentCamera(ecss::EntityId camera) {
		mCurrentCamera = camera;
	}

	void CameraSystem::refreshMouseLook() {
		GLFWwindow* glfwWindow = Engine::instance()->getMainWindow();
		// Only the free camera's: while something else (a game's character) has the camera, the cursor is its.
		if (!userControl()) {
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

	ecss::EntityId CameraSystem::getCurrentCamera() const {
		if (mCurrentCamera != ecss::INVALID_ID) {
			return mCurrentCamera;
		}

		return mDefaultCamera;
	}
	
	void CameraSystem::initKeyEvents() {
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

	void CameraSystem::processKeyboard(ecss::EntityId camera, CameraMovement direction, float deltaTime) {
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

			const float velocity = movementSpeed * deltaTime;

			Math::Vec3 dif = {};
			if (direction == FORWARD) {
				dif += tc->getForward() * velocity;
			}

			if (direction == BACKWARD) {
				dif -= tc->getForward() * velocity;
			}

			if (direction == LEFT) {
				dif -= tc->getRight() * velocity;
			}

			if (direction == RIGHT) {
				dif += tc->getRight() * velocity;
			}

			if (direction == TOP) {
				dif += Math::Vec3{0.f, 1.f, 0.f} *velocity;
			}

			if (direction == BOTTOM) {
				dif -= Math::Vec3{0.f, 1.f, 0.f} *velocity;
			}

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


	void CameraSystem::processMouseMovement(float xoffset, float yoffset, bool constrainPitch) {
		if (!processMouse || Engine::instance()->capturingFrame() || !userControl()) {
			return;
		}
		xoffset *= mouseSensitivity;
		yoffset *= mouseSensitivity;
		const auto camera = getCurrentCamera();
		Math::Mat4 viewMatrix{};
		{
			auto tc = ECSHandler::registry().pinComponent<TransformComponent>(camera);
			if (!tc) {
				return;
			}
			auto Pitch = tc->getRotate().x;
			auto Yaw = tc->getRotate().y;
			Yaw -= xoffset;

			Pitch += yoffset;

			if (constrainPitch) {
				Pitch = (std::min)((std::max)(Pitch, -90.f), 90.f);
			}

			tc->setRotate({ Pitch, Yaw, 0.f });
			tc->reloadTransform();
			viewMatrix = tc->getViewMatrix();
		}
		if (auto cameraComp = ECSHandler::registry().pinComponent<CameraComponent>(camera)) {
			cameraComp->updateFrustum(viewMatrix);
		}
	}

	void CameraSystem::processMouseScroll(float yoffset) {
		if (Engine::instance()->capturingFrame() || !userControl()) {
			return;
		}
		const auto camera = getCurrentCamera();
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
		auto fov = cameraComp->getProjection().getFOV();
		fov -= yoffset;
		cameraComp->getProjection().setFOV((std::min)((std::max)(1.f, fov), 90.f));
		cameraComp->updateFrustum(viewMatrix);
	}
}