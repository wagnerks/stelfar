#pragma once
#include <string>

namespace AssetsModule {
	class Texture;

	// A model's textures let go of while the model stays loaded -- its meshes, its materials and the assets -- so what
	// is not drawn for a while gives its video memory back; ModelLoader::load reads them again first.

	// Device thread. The textures of the model `path` names that no other loaded model uses leave the device (the RHI
	// frees each once the frames in flight are done). Nothing may draw the model meanwhile: its next load, on whatever
	// thread, reads them from their files again and points its materials at them, as a model sharing one that loads
	// meanwhile does as it is published.
	void releaseModel(const std::string& path);

	// Device thread: the texture's image let go of; TextureHandler::loadTexture reads it again. One read from a single
	// image file only.
	void releaseTexture(Texture& texture);
}
