#pragma once

#include "Mesh.h"
#include "Vertex.h"

namespace SFE {
	// Merges vertices that are identical in every attribute. Meshes arrive from the importer with a separate
	// vertex per triangle corner, which is not just three times the vertex buffer: with no shared vertices
	// there is no topology at all, every edge reads as the border of the surface, and anything downstream
	// that needs to know which triangles touch -- simplification above all -- can do nothing.
	void weldMesh(Mesh<Vertex3D>& mesh);

	struct MeshLodOptions {
		// Fraction of the mesh's size a vertex may move in the first level, twice as much in each level on: each is drawn
		// at about half the size on screen of the one before (gpu_cull.cs), where twice the error shows as much.
		float errorPerLevel = 0.03f;
		// The open edges stay where they are: a piece of a larger surface, such as a terrain chunk, then meets its
		// neighbour at any level of either.
		bool lockBorder = false;
	};

	// Fills mesh.lodIndices with coarser index sets over the mesh's own vertices. Index only: a vertex is
	// either kept with everything it carries -- bone weights included -- or no longer referenced, so a
	// skinned mesh simplifies without touching the skinning. Weld first.
	void buildMeshLods(Mesh<Vertex3D>& mesh, const MeshLodOptions& options = {});

	// Sets mesh.oneSided: whether its back side is never seen. A closed surface facing out is not, in any pose. Nor is
	// a rigid mesh of which, drawn from all round at full detail, hardly any of what is seen is back faces: a tin
	// bucket closed but for the ends of its handle's wire. A sheet, an open box or a mesh facing in is seen from behind.
	void markOneSided(Mesh<Vertex3D>& mesh);
}
