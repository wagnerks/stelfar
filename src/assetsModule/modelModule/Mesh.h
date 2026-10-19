#pragma once

#include <map>
#include <string>
#include <vector>

#include "Material.h"
#include "Vertex.h"
#include "BoundingVolume.h"

namespace SFE {
	template<class VertexType>
	struct Mesh {
		Mesh() = default;
		Mesh(std::vector<VertexType>&& vertices, std::vector<unsigned int>&& indices = {}) : indices(std::move(indices)), vertices(std::move(vertices)){}

		std::vector<unsigned int> indices;
		std::vector<VertexType> vertices;
		// Coarser index sets over the same vertices, each about half the triangles of the one before it.
		// A distant model drawn from these has triangles bigger than a pixel, which is the only thing that
		// stops its silhouette from boiling; no amount of temporal filtering fixes geometry that small.
		std::vector<std::vector<unsigned int>> lodIndices;
		// How far each of them strays from the full mesh, a share of its extent: a level is drawn once that is under a pixel.
		std::vector<float> lodErrors;
		// Skinned meshes of a model: 1 + the slot its pose bounds are written to (Armature::poseBoundsCount). 0 for
		// a rigid mesh, or a skinned one without pose bounds.
		uint32_t poseBounds = 0;
	};

	using Mesh3D = Mesh<Vertex3D>;
	using Mesh2D = Mesh<Vertex2D>;

	template<class VertexType>
	struct MeshObject {
		Mesh<VertexType> mesh;
		Material material;
		Math::Mat4 transform;
		FrustumModule::AABB aabb;
		std::string nodeName;
		// The node's own data from the file (glTF extras), numbers written as text.
		std::map<std::string, std::string> extras;
	};

	using MeshObject3D = MeshObject<Vertex3D>;
	using MeshObject2D = MeshObject<Vertex2D>;
}
