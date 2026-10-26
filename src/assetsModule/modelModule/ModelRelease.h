#pragma once
#include <string>
#include <vector>

namespace AssetsModule {
	class Model;
	class Texture;

	// A model's textures let go of while the model stays loaded -- its meshes, its materials and the assets -- so what
	// is not near for a while gives its video memory back, drawn meanwhile with copies of their small levels;
	// ModelLoader::load reads them again first.

	// Device thread. The textures of the model `path` names that no other loaded model uses leave the device (the RHI
	// frees each once the frames in flight are done), and its materials are pointed at copies of them with only their
	// levels `most` texels across or smaller (distantTexture). What draws it was given the textures it had: given the
	// model again (ModelBinding::setModel), it draws with those copies, as what is spawned with it as it is
	// (modelAsItIs) does. Its next load, on whatever thread, reads its own
	// from their files again and points its materials back at them, as a model sharing one that loads meanwhile does as
	// it is published.
	void releaseModel(const std::string& path, int most);

	// Device thread. The models `paths` names that are not loaded yet loaded as ModelLoader::loadAll loads them, but as
	// releaseModel(path, most) leaves one: their meshes, and their materials pointed at the copies, none of their own
	// textures on the device. Their next ModelLoader::load reads those.
	void loadAllReleased(const std::vector<std::string>& paths, int most);

	// The model `path` names as it is, loaded (ModelLoader::load) only when it is not: one whose textures were let go of
	// stays so, drawn with the copies, where ModelLoader::load would read them back.
	Model* modelAsItIs(const std::string& path);

	// Device thread: the texture's image let go of; TextureHandler::loadTexture reads it again. One read from a single
	// image file only.
	void releaseTexture(Texture& texture);

	// A copy of the texture read from `path` (unflipped, compressed) with only its levels `most` texels wide and high
	// or smaller: a texture of its own, which releaseModel never lets go of, for what draws a model far off while the
	// model's own textures may be let go of. Made once a path and size, kept in the cache by the file's size and time;
	// any thread, as TextureHandler::loadTexture.
	Texture* distantTexture(const std::string& path, int most);
}
