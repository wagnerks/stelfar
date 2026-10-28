#include "CameraSystem.h"

#include "componentsModule/CameraComponent.h"
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

	ecss::EntityId CameraSystem::getCurrentCamera() const {
		if (mCurrentCamera != ecss::INVALID_ID) {
			return mCurrentCamera;
		}

		return mDefaultCamera;
	}
}
