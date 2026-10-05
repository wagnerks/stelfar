#pragma once
#include <functional>
#include <string>

#include "ecss/Types.h"

namespace AssetsModule {
	class Animation;
	class Model;
}

namespace SFE::PropertiesModule {
	// What an entity with a ModelComponent is drawn and animated with, made from the model it names: its meshes and
	// their materials, its skeleton, a clip to play. Main thread.
	namespace ModelBinding {
		// Binds the entity to its model. Again on one already bound, it is rebuilt from the model its path names now,
		// and what was changed since the last binding is kept: the entity's own material, and each mesh's material
		// where it differs from what the model gave it (a shader, a tint, a mesh hidden) -- when the model still has
		// as many meshes; the clip it plays.
		void bind(ecss::EntityId entity);
		// The entity given `model` anew -- its own read again, or another in its place: its meshes, bones and bounds
		// taken from it, and bound again. Between frames.
		void setModel(ecss::EntityId entity, AssetsModule::Model& model);
		// The model at `path` read again from its file, and every entity made from it bound again, playing the clip of
		// the same name as before. Then the listeners told, for what they took of the model themselves. Between
		// frames. How many entities; -1 when the file does not read.
		int reload(const std::string& path);
		void onReloaded(std::function<void(const std::string& path)> listener);
		// The clip a model plays when nothing says which: a dance, a walk, an idle, a run, else the first that moves.
		const AssetsModule::Animation* playbackClip(const AssetsModule::Model* model);
	}
}
