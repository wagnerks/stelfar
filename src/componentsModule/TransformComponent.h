#pragma once

#include <functional>

#include "mathModule/Forward.h"
#include "mathModule/Quaternion.h"

#include "componentsModule/ComponentBase.h"

namespace SFE::ComponentsModule {
	/**
	 * @brief Position, rotation and scale of one entity.
	 *
	 * Trivially copyable, and worth keeping that way: this is the component every frame walks,
	 * so it is the one whose array benefits most from ecss relocating sectors with a memmove
	 * instead of a move constructor per element.
	 *
	 * Setters, reloadTransform and copyTransform take a lock striped by entity and kept outside the
	 * component: TransformSystem's worker reloads while the main thread sets values and reloads the
	 * camera. The reference getters are not synchronised; code on another thread than the one
	 * changing the entity reads copyTransform, or the TransformMatComp copy the worker publishes.
	 *
	 * A new one is dirty and tells no one: the registry's add listener tells TransformSystem once
	 * it is stored (ECSHandler::initSystems).
	 */
	class TransformComponent : public ecss::ComponentInterface {
	public:
		TransformComponent(ecss::SectorId id) : ComponentInterface(id) {}

		const Math::Vec3& getPos(bool global = false) const;
		void setX(float x);
		void setY(float y);
		void setZ(float z);

		void setPos(const Math::Vec3& pos);

		const Math::Vec3& getRotate() const;
		void setPitch(float x);
		void setYaw(float y);
		void setRoll(float z);
		void setRotate(const Math::Vec3& rotate);

		const Math::Vec3& getScale() const;
		Math::Vec3 getGlobalScale() const;

		void setScaleX(float x);
		void setScaleY(float y);
		void setScaleZ(float z);
		void setScale(const Math::Vec3& scale);

		const Math::Mat4& getTransform() const;
		// A whole matrix even while another thread reloads it.
		Math::Mat4 copyTransform() const;
		void setTransform(const Math::Mat4& transform);

		Math::Mat4 getRotationMatrix() const;
		const Math::Quaternion<float>& getQuaternion() const;
		Math::Mat4 calculateLocalTransform() const;
		Math::Mat4 getViewMatrix() const;

		Math::Vec3 getRight() const;
		Math::Vec3 getUp() const;
		Math::Vec3 getBackward() const;
		Math::Vec3 getForward() const;

		void reloadTransform();

		void markDirty();
		bool isDirty() const;

	private:
		// Writes a value and raises the flag under the lock, then notifies TransformSystem.
		void change(const std::function<void()>& write);

		bool mDirty = true;

		Math::Quaternion<float> mRotateQuaternion;
		Math::Mat4 mTransform = Math::Mat4{ 1.f };

		Math::Vec3 mPos = {0.f};
		Math::Vec3 mScale = { 1.f };
		Math::Vec3 mRotate = { 0.f };
	};

	struct TransformMatComp {
		Math::Mat4 mTransform = Math::Mat4{ 1.f };
	};

}

using SFE::ComponentsModule::TransformComponent;
//using SFE::ComponentsModule::DirtyTransform;