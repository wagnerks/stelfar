#include "ThreadPool.h"

#include "core/Engine.h"
#include "logsModule/logger.h"
#include "rhi/GraphicsBackend.h"


namespace SFE {
	ThreadPool::ThreadPool() {}

	ThreadPool::~ThreadPool() {
		shutdown();
	}

	void ThreadPool::drainSync() {
		while (true) {
			PoolTask task;
			{
				std::lock_guard lock(mSyncMtx);
				if (mSyncTasks.empty()) {
					break;
				}
				task = std::move(mSyncTasks.front());
				mSyncTasks.pop();
			}
			if (task.work.valid()) {
				SFE_PROFILE_ZONE(task.label);
				task.work();
			}
		}
	}

	void ThreadPool::shutdown() {
		if (mShuttingDown.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		mCommonWorkers.requestStop();
		mRenderWorkers.requestStop();
		mLoadingWorkers.requestStop();

		drainSync();
		mCommonWorkers.join();
		mRenderWorkers.join();
		mLoadingWorkers.join();
		drainSync();

	}

	void ThreadPool::syncUpdate() {
		if (mShuttingDown.load(std::memory_order_acquire)) {
			return;
		}
		drainSync();
	}

	void ThreadPool::waitIdle() {
		while (!mShuttingDown.load(std::memory_order_acquire)
			&& !(mCommonWorkers.idle() && mRenderWorkers.idle() && mLoadingWorkers.idle())) {
			syncUpdate();
			std::this_thread::yield();
		}
		syncUpdate();
	}

	std::shared_future<void> ThreadPool::addTaskToSynchronization(std::function<void()>&& task, const char* label) {
		if (mShuttingDown.load(std::memory_order_acquire)) {
			return {};
		}
		std::lock_guard lock(mSyncMtx);
		if (mShuttingDown.load(std::memory_order_acquire)) {
			return {};
		}

		mSyncTasks.push({ label, std::packaged_task<void()>(std::move(task)) });
		return mSyncTasks.back().work.get_future();
	}

	std::shared_future<void> ThreadPool::addLoadingTask(std::function<void()>&& task, const char* label) {
		return mLoadingWorkers.addTask(std::move(task), label);
	}
}
