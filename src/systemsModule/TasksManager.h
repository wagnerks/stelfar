#pragma once
#include <array>
#include <initializer_list>
#include <atomic>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

#include "containersModule/Singleton.h"
#include "ecss/Types.h"

namespace SFE::SystemsModule {

	enum TaskType : size_t {
		TRANSFORM_UPDATE,
		TRAHSFORM_RELOADED,
		AABB_UPDATED,
		CAMERA_UPDATED,
		ARMATURE_UPDATED,
		MESH_UPDATED,
		// An entity's material or mesh changed (MeshComponent::meshMaterials, its MaterialComponent): any thread; the renderer
		// reads the entity again on its next frame. Its listeners run on the caller's thread, which may hold the entity's
		// components pinned.
		MATERIAL_UPDATED,
		ANIMATION_UPDATED,
		// Sent before the entity is destroyed; its id is reused right after.
		ENTITY_DESTROYED,

		COUNT
	};
	
	struct Task {
		ecss::EntityId entity;
		TaskType type;
		void* customData = nullptr;
	};

	struct TaskWorker;

	class TasksManager : public Singleton<TasksManager>{
	public:

		void notify(Task task) const;
		void addWorker(TaskWorker* worker, TaskType type);

	private:
		// Systems subscribe while workers already notify, and a notify can notify again from inside a worker, so
		// no lock is held while calling out: each type's list is an immutable snapshot, replaced whole on subscribe.
		using Workers = std::vector<TaskWorker*>;
		std::array<std::atomic<std::shared_ptr<const Workers>>, TaskType::COUNT> workers{};
	};

	struct TaskWorker {
		TaskWorker(std::initializer_list<TaskType> types = {}) : mTaskTypes(types) {}

		void bindWorkers() {
			for (auto type : mTaskTypes) {
				TasksManager::instance()->addWorker(this, type);
			}
		}

		virtual ~TaskWorker() = default;
		virtual void notify(Task task) = 0;

	private:
		std::vector<TaskType> mTaskTypes;
	};
}
