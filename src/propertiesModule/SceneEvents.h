#pragma once

#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#include "ecss/Fwd.h"

namespace SFE::PropertiesModule {
	// A scene file made into entities (PropertiesSystem::loadScene): the file as it was named, the entity the scene
	// hangs under, and every entity made of it in the order the file lists them, depth first -- the root first, each
	// node before its children, siblings as the file has them (fillTree's order, their ids' order). Each that has a
	// transform stands where the file puts it under its parents before any listener is told: its component's own matrix
	// and its render copy (TransformMatComp) both. A node the file gives no transform has neither.
	struct SceneLoaded {
		std::string path;
		ecss::EntityId root = ecss::INVALID_ID;
		std::vector<ecss::EntityId> entities;
	};

	// The one rule for whatever takes something of the world, a system or a render pass: take what is there when it is
	// made, and listen for the scenes loaded after. `listener` is told of every scene loaded from now on, after those
	// subscribed before it; one subscribed while the listeners are being told hears from the next scene on. `owner` is
	// what the listener belongs to, by address: forgetSceneListeners(owner) drops all of its, and must come before the
	// owner goes -- a SystemManager drops a system's before deleting it, a RenderSystem a pass's. Subscribing may be done
	// from any thread; forgetting on the main thread, where scenes are told of, so that no listener of the owner is still
	// running when it returns.
	void onSceneLoaded(const void* owner, std::function<void(const SceneLoaded&)> listener);
	void forgetSceneListeners(const void* owner);

	// The same for an object known by one of its bases (a system as ecss::System, a pass as RenderPass): its listeners,
	// whether they were subscribed with that base's address or the whole object's, which multiple inheritance may set
	// apart.
	template <class T>
	void forgetSceneListenersOf(const T* object) {
		forgetSceneListeners(object);
		if constexpr (std::is_polymorphic_v<T>) {
			if (const void* whole = dynamic_cast<const void*>(object); whole != static_cast<const void*>(object)) {
				forgetSceneListeners(whole);
			}
		}
	}

	// Every listener told of `scene`, in the order they subscribed; one forgotten meanwhile is not. Main thread:
	// PropertiesSystem::loadScene sends it once the scene stands. reloadScene does not -- it moves, swaps and takes out
	// what is already there, and builds again the colliders of what it touched itself (PhysicsSystem::replaceStatic).
	void emitSceneLoaded(const SceneLoaded& scene);
}
