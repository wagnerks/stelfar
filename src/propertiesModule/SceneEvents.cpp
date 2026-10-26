#include "propertiesModule/SceneEvents.h"

#include <memory>
#include <mutex>
#include <utility>

namespace SFE::PropertiesModule {
	namespace {
		struct Listener {
			const void* owner = nullptr;
			std::function<void(const SceneLoaded&)> call;
			bool forgotten = false; // under the table's mutex
		};

		// In the order they subscribed. A telling under way holds its own copy of the list, so each is shared: one
		// forgotten while the listeners are being told is marked, and skipped.
		struct Table {
			std::mutex mutex;
			std::vector<std::shared_ptr<Listener>> listeners;
		};

		Table& table() {
			static Table one;
			return one;
		}
	}

	void onSceneLoaded(const void* owner, std::function<void(const SceneLoaded&)> listener) {
		if (!listener) {
			return;
		}
		auto& all = table();
		std::lock_guard lock(all.mutex);
		all.listeners.push_back(std::make_shared<Listener>(Listener{ owner, std::move(listener) }));
	}

	void forgetSceneListeners(const void* owner) {
		auto& all = table();
		std::lock_guard lock(all.mutex);
		std::erase_if(all.listeners, [owner](const std::shared_ptr<Listener>& listener) {
			if (listener->owner != owner) {
				return false;
			}
			listener->forgotten = true;
			return true;
		});
	}

	void emitSceneLoaded(const SceneLoaded& scene) {
		// Told from a copy of the list with no lock held, so a listener may subscribe another, or forget one, as it is told.
		auto& all = table();
		std::vector<std::shared_ptr<Listener>> told;
		{
			std::lock_guard lock(all.mutex);
			told = all.listeners;
		}
		for (const auto& listener : told) {
			bool forgotten = false;
			{
				std::lock_guard lock(all.mutex);
				forgotten = listener->forgotten;
			}
			if (!forgotten) {
				listener->call(scene);
			}
		}
	}
}
