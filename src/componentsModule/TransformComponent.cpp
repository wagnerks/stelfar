#include "TransformComponent.h"

#include <array>
#include <functional>
#include <mutex>
#include <optional>

#include "Hierarchy.h"
#include "core/ECSHandler.h"
#include "multithreading/ThreadPool.h"
#include "systemsModule/SystemManager.h"
#include "systemsModule/systems/TransformSystem.h"

namespace SFE::ComponentsModule {
	namespace {
		// Outside the component, so it stays trivially copyable. Striped by entity: a collision only makes two
		// entities wait on each other, and no code holds two of these at once.
		std::mutex& transformLock(const ecss::EntityId entity) {
			static std::array<std::mutex, 64> locks;
			return locks[entity % locks.size()];
		}
	}

	const Math::Vec3& TransformComponent::getPos(bool global) const {
		if (global) {
			return mTransform[3].xyz;
		}
		return mPos;
	}

	void TransformComponent::setX(float x) {
		setPos({ x, mPos.y, mPos.z });
	}
	void TransformComponent::setY(float y) {
		setPos({ mPos.x, y, mPos.z });
	}
	void TransformComponent::setZ(float z) {
		setPos({ mPos.x, mPos.y, z });
	}
	void TransformComponent::setPos(const Math::Vec3& pos) {
		if (mPos != pos) {
			change([&] { mPos = pos; });
		}
	}

	//x - pitch, y - yaw, z - roll
	const Math::Vec3& TransformComponent::getRotate() const {
		return mRotate;
	}

	void TransformComponent::setPitch(float x) {
		setRotate({ x, mRotate.y, mRotate.z });
	}
	void TransformComponent::setYaw(float y) {
		setRotate({ mRotate.x, y, mRotate.z });
	}
	void TransformComponent::setRoll(float z) {
		setRotate({ mRotate.x, mRotate.y, z });
	}
	void TransformComponent::setRotate(const SFE::Math::Vec3& rotate) {
		if (mRotate != rotate) {
			change([&] { mRotate = rotate; });
		}
	}

	const Math::Vec3& TransformComponent::getScale() const {
		return mScale;
	}

	Math::Vec3 TransformComponent::getGlobalScale() const {
		return { Math::length(mTransform[0]), Math::length(mTransform[1]), Math::length(mTransform[2]) };
	}
	void TransformComponent::setScaleX(float x) {
		setScale({ x, mScale.y, mScale.z });
	}
	void TransformComponent::setScaleY(float y) {
		setScale({ mScale.x, y, mScale.z });
	}
	void TransformComponent::setScaleZ(float z) {
		setScale({ mScale.x, mScale.y, z });
	}
	void TransformComponent::setScale(const SFE::Math::Vec3& scale) {
		if (mScale != scale) {
			change([&] { mScale = scale; });
		}
	}

	const Math::Mat4& TransformComponent::getTransform() const {
		return mTransform;
	}

	Math::Mat4 TransformComponent::copyTransform() const {
		std::lock_guard lock(transformLock(getEntityId()));
		return mTransform;
	}

	void TransformComponent::setTransform(const SFE::Math::Mat4& transform) {
		std::lock_guard lock(transformLock(getEntityId()));
		mTransform = transform;
	}

	Math::Mat4 TransformComponent::getRotationMatrix() const {
		return mRotateQuaternion.toMat4();
	}

	const Math::Quaternion<float>& TransformComponent::getQuaternion() const {
		return mRotateQuaternion;
	}

	// The camera and the runtime reload on the main thread while TransformSystem's worker reloads the same component,
	// so the values, the flag and the matrix change under the component's lock. The parent's matrix is copied and
	// the children are marked outside it, so no thread ever holds two locks.
	void TransformComponent::reloadTransform() {
		std::optional<Math::Mat4> parent;
		if (const auto parentTransform = ECSHandler::registry().pinComponent<const TransformComponent>(Hierarchy::parent(getEntityId()))) {
			parent = parentTransform->copyTransform();
		}

		{
			std::lock_guard lock(transformLock(getEntityId()));
			if (!mDirty) {
				return;
			}
			mDirty = false;
			mRotateQuaternion = Math::Quaternion<float>::fromEulerDegrees(mRotate);
			const auto local = calculateLocalTransform();
			mTransform = parent ? *parent * local : local;
		}

		for (const auto child : Hierarchy::children(getEntityId())) {
			if (auto transformPtr = ECSHandler::registry().pinComponent<TransformComponent>(child)) {
				transformPtr->markDirty();
			}
		}
	}

	SFE::Math::Mat4 TransformComponent::calculateLocalTransform() const {
		return Math::translate(Math::Mat4{1.f}, mPos) * mRotateQuaternion.toMat4() * Math::scale(Math::Mat4{ 1.f }, mScale);
	}

	SFE::Math::Mat4 TransformComponent::getViewMatrix() const {
		return Math::inverseAffine(copyTransform());
	}

	SFE::Math::Vec3 TransformComponent::getRight() const {
		return mTransform[0];
	}

	SFE::Math::Vec3 TransformComponent::getUp() const {
		return mTransform[1];
	}

	SFE::Math::Vec3 TransformComponent::getBackward() const {
		return mTransform[2];
	}

	SFE::Math::Vec3 TransformComponent::getForward() const {
		return -mTransform[2];
	}

	// Setters call this after writing the value. TransformSystem reloads on a worker as soon as it is notified,
	// and a reload that ran between the flag and the write rebuilt the matrix from the old value and cleared the
	// flag, so the new one was never applied: a random handful of spawned models kept their unrotated matrix.
	// Every change notifies, even when the flag is already up: the flag may belong to an event the worker has
	// already taken, and skipping the notify left the change with no reload at all -- now and then the sun kept an
	// identity rotation. The system merges repeated notifications for one entity. A new component does not come
	// through here: told from its constructor, before the registry had stored it, a worker that took the event at
	// once found nothing to reload and dropped it, and an entity no setter touched again was never drawn.
	void TransformComponent::markDirty() {
		change([] {});
	}

	void TransformComponent::change(const std::function<void()>& write) {
		{
			std::lock_guard lock(transformLock(getEntityId()));
			write();
			mDirty = true;
		}
		// Outside the lock: a worker woken by it may reload this very component at once.
		SystemsModule::TasksManager::instance()->notify({ getEntityId(), SystemsModule::TRANSFORM_UPDATE });
	}

	bool TransformComponent::isDirty() const {
		std::lock_guard lock(transformLock(getEntityId()));
		return mDirty;
	}
}