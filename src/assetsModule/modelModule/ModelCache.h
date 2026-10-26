#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Animation.h"
#include "Armature.h"
#include "Mesh.h"
#include "containersModule/Tree.h"

namespace Assimp {
	class Importer;
}

namespace AssetsModule::ModelCache {
	// What a model file reads to -- its parts with their materials, armature and animations, as ModelLoader leaves them
	// before they go to the device -- kept in cache/ so the next load skips the importer, the tangents and the levels of
	// detail. Kept with every file the read looked at, there or not, and read back only while each is as it was then.

	// A file as it was when looked at: whether it was there, its size and when it was written.
	struct Source {
		std::string path;
		bool exists = false;
		uint64_t size = 0;
		int64_t time = 0;
	};
	Source source(const std::string& path);

	// The importer's files, every one it opens or looks for, added to `sources`, which has to outlive the read.
	void watch(Assimp::Importer& importer, std::vector<Source>& sources);

	// The cache file of the model at `path`.
	std::string file(const std::string& path);

	// False when there is none, or a file it was read from has changed since: then nothing in the arguments is to be used.
	// `sources`, given, gets the files it was read from.
	bool read(const std::string& path, SFE::Tree<SFE::MeshObject3D>& meshes, Armature& armature, std::vector<Animation>& animations,
		std::vector<Source>* sources = nullptr);
	void write(const std::string& path, const SFE::Tree<SFE::MeshObject3D>& meshes, const Armature& armature,
		const std::vector<Animation>& animations, const std::vector<Source>& sources);
}
