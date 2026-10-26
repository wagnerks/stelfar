#pragma once

#include <set>

#include "Mesh.h"
#include "containersModule/Tree.h"

namespace AssetsModule::ModelText {
	// Text written on a model: a node of it with the extras
	//   text     what it says, its lines apart on lines of their own;
	//   size     "w h", the box it is fitted into, metres: as large as fits, keeping its proportions, its ink centred on
	//            the node;
	//   colour   "r g b", the paint's, sRGB 0..1 (white if left out);
	//   wear     0..1, how worn the paint is (none if left out);
	//   font     the font's file, as content paths go (kDefaultFont if left out)
	// is given a mesh of its glyphs as its child: in the node's x (right) and y (up), kLift out along its z, toward
	// whoever reads it. Each glyph a quad of its signed distance field (SdfFont), cut where the field reads half by
	// kTextFS (more as it is worn), so it is sharp however near it is seen; casting no shadow, being paint. Rigid like
	// any mesh under a node: what moves the node's bone moves it.
	inline constexpr const char* kDefaultFont = "fonts/Roboto-Medium.ttf";
	inline constexpr const char* kTextFS = "shaders/g_buffer_text.fs";
	inline constexpr float kLift = 0.001f;

	// Every node of `meshes` with a `text` given its mesh. Any thread, as models load.
	void add(SFE::Tree<SFE::MeshObject3D>& meshes);
}
