#pragma once
#include <atomic>
#include <cassert>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "containersModule/Singleton.h"
#include "debugModule/Profiler.h"

namespace SFE {

	class ThreadSynchronizer {
	public:
		ThreadSynchronizer() = default;

		ThreadSynchronizer(const ThreadSynchronizer& other) {}
		ThreadSynchronizer(ThreadSynchronizer&& other) noexcept {}
		ThreadSynchronizer& operator=(const ThreadSynchronizer& other) { return *this; }
		ThreadSynchronizer& operator=(ThreadSynchronizer&& other) noexcept { return *this;}

		inline std::unique_lock<std::shared_mutex> writeLock() { return std::unique_lock(mMutex); }
		inline std::shared_lock<std::shared_mutex> readLock() { return std::shared_lock(mMutex); }

		inline void writeLock(std::function<void()> task) { if (task) { auto lock = std::unique_lock(mMutex); task(); } }
		inline void readLock(std::function<void()> task) { if (task) { auto lock = std::shared_lock(mMutex); task(); } }
	private:
		std::shared_mutex mMutex{};
	};

	struct FuturesBunch {
		mutable std::vector<std::shared_future<void>> futures;

		void reserve(size_t capacity) const {
			futures.reserve(capacity);
		}

		void add(const std::shared_future<void>& future) const {
			futures.push_back(future);
		}

		void waitAll() const {
			for (auto& future : futures) {
				if (future.valid()) {
					future.wait();
				}
			}
			futures.clear();
		}
	};

	// A task, and what the profiler calls it (Debug::Profiler).
	struct PoolTask {
		const char* label = "Task";
		std::packaged_task<void()> work;
	};

	template<size_t Size>
	struct WorkersPool {
		// `name`: the threads' in the profiler, each numbered.
		explicit WorkersPool(const char* name) {
			mWorkers.reserve(Size);

			for (auto i = 0u; i < Size; i++) {
				mWorkers.emplace_back(std::thread([this, name, i]() {
					Debug::Profiler::nameThread(name, static_cast<int>(i) + 1);
					while (true) {
						PoolTask task;
						{
							std::unique_lock lock(mMutex);
							mCondition.wait(lock, [this] {
								return mTerminating.load(std::memory_order_acquire) || !mTasksQueue.empty();
							});
							if (mTasksQueue.empty()) {
								if (mTerminating.load(std::memory_order_acquire)) {
									break;
								}
								continue;
							}
							task = std::move(mTasksQueue.front());
							mTasksQueue.pop();
						}
						if (task.work.valid()) {
							SFE_PROFILE_ZONE(task.label);
							task.work();
						}
						mPending.fetch_sub(1, std::memory_order_acq_rel);
					}
				}));
			}
		}

		~WorkersPool() {
			shutdown();
		}

		void requestStop() {
			mTerminating.store(true, std::memory_order_release);
			mCondition.notify_all();
		}

		void join() {
			for (auto& worker : mWorkers) {
				if (worker.joinable()) {
					worker.join();
				}
			}
			mWorkers.clear();
		}

		void shutdown() {
			requestStop();
			join();
		}

		std::shared_future<void> addTask(std::function<void()>&& task, const char* label) {
			if (mTerminating.load(std::memory_order_acquire)) {
				return {};
			}
			std::lock_guard lock(mMutex);
			if (mTerminating.load(std::memory_order_acquire)) {
				return {};
			}
			mPending.fetch_add(1, std::memory_order_acq_rel);
			mTasksQueue.push({ label, std::packaged_task<void()>(std::move(task)) });
			auto future = mTasksQueue.back().work.get_future();
			mCondition.notify_one();

			return future;
		}

		// Queued plus running.
		bool idle() const { return mPending.load(std::memory_order_acquire) == 0; }

	private:
		std::vector<std::thread> mWorkers;
		std::atomic<int> mPending{ 0 };
		std::queue<PoolTask> mTasksQueue;

		std::mutex mMutex;
		std::condition_variable mCondition;

		std::atomic<bool> mTerminating{ false };
	};

	enum class WorkerType {
		COMMON,
		RENDER,
		SYNC,
		RESOURCE_LOADING
	};

	class ThreadPool : public Singleton<ThreadPool> {
	public:
		ThreadPool();
		~ThreadPool();
		
		template<WorkerType Type = WorkerType::COMMON>
		FuturesBunch addBatchTasks(size_t size, size_t batchSize, std::function<void(size_t)> task) {
			FuturesBunch futures;

			const size_t count = size / batchSize + static_cast<size_t>((size % batchSize) > 0);
			futures.reserve(count);

			for (auto i = 0u; i < count; i++) {
				futures.add(addTask<Type>([task, it = i * batchSize, last = (i + 1) * batchSize > size ? size : (i + 1) * batchSize]() mutable {
					for (; it < last; it++) {
						task(it);
					}
				}));
			}
			
			return futures;
		}

		// `label`: what the profiler calls the task (Debug::Profiler); a literal or a type's name, kept for good.
		template<WorkerType Type = WorkerType::COMMON>
		std::shared_future<void> addTask(std::function<void()>&& task, const char* label = "Task") {
			switch (Type) {
			case WorkerType::COMMON:
				return mCommonWorkers.addTask(std::move(task), label);
			case WorkerType::RENDER:
				if (mRenderTasksInline.load(std::memory_order_acquire)) {
					task();
					std::promise<void> done;
					done.set_value();
					return done.get_future().share();
				}
				return mRenderWorkers.addTask(std::move(task), label);
			case WorkerType::SYNC:
				return addTaskToSynchronization(std::move(task), label);
			case WorkerType::RESOURCE_LOADING:
				return addLoadingTask(std::move(task), label);
			}

			assert(false);
			return {};
		}

		void syncUpdate();
		// A frame capture runs render tasks on the calling thread: when a worker finishes then depends
		// on scheduling, and that timing leaks into what the captured frame draws.
		void setRenderTasksInline(bool inlineTasks) { mRenderTasksInline.store(inlineTasks, std::memory_order_release); }
		// Blocks until every worker pool is empty, pumping main-thread tasks meanwhile. A capture calls it
		// before each frame so asynchronous work, asset loading above all, lands on a fixed frame.
		void waitIdle();
		void shutdown();
		bool isShuttingDown() const { return mShuttingDown.load(std::memory_order_acquire); }

	private:
		std::atomic<bool> mRenderTasksInline = false;
		std::shared_future<void> addTaskToSynchronization(std::function<void()>&& task, const char* label);
		std::shared_future<void> addLoadingTask(std::function<void()>&& task, const char* label);
		void drainSync();

		constexpr static inline uint8_t MAX_WORKERS = 128;
		constexpr static inline uint8_t RENDER_WORKERS = 16;
		constexpr static inline uint8_t LOADING_WORKERS = 8;

		WorkersPool<MAX_WORKERS> mCommonWorkers{ "Common" };
		WorkersPool<RENDER_WORKERS> mRenderWorkers{ "Render" };
		WorkersPool<LOADING_WORKERS> mLoadingWorkers{ "Loading" };

		std::queue<PoolTask> mSyncTasks;
		std::mutex mSyncMtx;
		std::atomic<bool> mShuttingDown{ false };

	};
}