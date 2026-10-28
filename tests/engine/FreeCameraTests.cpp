#include <gtest/gtest.h>

#include "EngineTestFixture.h"
#include "componentsModule/TransformComponent.h"
#include "core/InputHandler.h"
#include "ecss/Registry.h"
#include "systemsModule/FreeFly.h"
#include "systemsModule/systems/CameraSystem.h"
#include "systemsModule/systems/FreeCameraController.h"

namespace SFE::EngineTests {
	namespace {
		void expectNear(const Math::Vec3& got, const Math::Vec3& wanted, float within, const char* what) {
			EXPECT_NEAR(got.x, wanted.x, within) << what;
			EXPECT_NEAR(got.y, wanted.y, within) << what;
			EXPECT_NEAR(got.z, wanted.z, within) << what;
		}
	}

	// Each held key moves by the velocity along its own axis: forward and back along where the camera looks, left and
	// right across it, up and down the world's.
	TEST(FreeFly, EachKeyMovesAlongItsAxis) {
		using FreeFly::Move;
		const Math::Vec3 forward{ 0.6f, -0.8f, 0.f };
		const Math::Vec3 right{ 0.f, 0.f, 1.f };
		constexpr float v = 2.5f;
		expectNear(FreeFly::offset(Move::Forward, forward, right, v), forward * v, 0.f, "forward");
		expectNear(FreeFly::offset(Move::Backward, forward, right, v), forward * -v, 0.f, "backward");
		expectNear(FreeFly::offset(Move::Left, forward, right, v), right * -v, 0.f, "left");
		expectNear(FreeFly::offset(Move::Right, forward, right, v), right * v, 0.f, "right");
		expectNear(FreeFly::offset(Move::Up, forward, right, v), Math::Vec3{ 0.f, v, 0.f }, 0.f, "up");
		expectNear(FreeFly::offset(Move::Down, forward, right, v), Math::Vec3{ 0.f, -v, 0.f }, 0.f, "down");
	}

	// The mouse turns the yaw against its x and the pitch with its y, each by the sensitivity; the pitch stops at straight
	// up and straight down when held so, and the roll is none.
	TEST(FreeFly, LookTurnsYawAgainstTheMouseAndHoldsThePitch) {
		const auto turned = FreeFly::look({ 10.f, 20.f, 5.f }, 30.f, 40.f, 0.1f);
		EXPECT_FLOAT_EQ(turned.x, 14.f);
		EXPECT_FLOAT_EQ(turned.y, 17.f);
		EXPECT_EQ(turned.z, 0.f) << "a roll was kept";
		EXPECT_FLOAT_EQ(FreeFly::look({ 85.f, 0.f, 0.f }, 0.f, 100.f, 0.1f).x, 90.f);
		EXPECT_FLOAT_EQ(FreeFly::look({ -85.f, 0.f, 0.f }, 0.f, -100.f, 0.1f).x, -90.f);
		EXPECT_FLOAT_EQ(FreeFly::look({ 85.f, 0.f, 0.f }, 0.f, 100.f, 0.1f, false).x, 95.f) << "held where it was not to be";
	}

	// The wheel narrows the view as it turns up and widens it as it turns down, from 1 degree to 90.
	TEST(FreeFly, ZoomStaysWithinOneAndNinetyDegrees) {
		EXPECT_FLOAT_EQ(FreeFly::zoom(45.f, 5.f), 40.f);
		EXPECT_FLOAT_EQ(FreeFly::zoom(45.f, -5.f), 50.f);
		EXPECT_FLOAT_EQ(FreeFly::zoom(3.f, 5.f), 1.f);
		EXPECT_FLOAT_EQ(FreeFly::zoom(88.f, -5.f), 90.f);
	}

	// The free camera flies the current camera by the keys held while it is on -- one frame of W, its speed times the
	// step along where it looks -- and leaves it be while it is off, as when something else has the camera.
	TEST(FreeCamera, FliesTheCurrentCameraOnlyWhileOn) {
		auto* cameras = ECSHandler::getSystem<SystemsModule::CameraSystem>();
		auto* fly = ECSHandler::getSystem<SystemsModule::FreeCameraController>();
		ASSERT_NE(cameras, nullptr);
		ASSERT_NE(fly, nullptr) << "the fixture makes the free camera";
		ASSERT_TRUE(fly->enabled()) << "off, though on as it is made";
		const auto camera = cameras->getCurrentCamera();
		const auto where = [camera] {
			auto transform = ECSHandler::registry().pinComponent<const ComponentsModule::TransformComponent>(camera);
			return transform ? transform->getPos() : Math::Vec3{};
		};
		Math::Vec3 pos{};
		Math::Vec3 rotate{};
		Math::Vec3 forward{};
		{
			auto transform = ECSHandler::registry().pinComponent<const ComponentsModule::TransformComponent>(camera);
			ASSERT_TRUE(transform);
			pos = transform->getPos();
			rotate = transform->getRotate();
			forward = transform->getForward();
		}
		const int w = static_cast<int>(CoreModule::InputKey::KEY_W);
		const Finally putBack([&] {
			CoreModule::InputHandler::injectKey(w, static_cast<int>(CoreModule::InputEventType::RELEASE));
			fly->setEnabled(true);
			cameras->placeCamera(pos, rotate);
			step();
		});

		CoreModule::InputHandler::injectKey(w, static_cast<int>(CoreModule::InputEventType::PRESS));
		step();
		const auto flown = where();
		expectNear(flown, pos + forward * (100.f * kFrameTime), 1e-4f, "one frame of W");

		fly->setEnabled(false);
		step(3);
		expectNear(where(), flown, 0.f, "flown while off");
	}
}
