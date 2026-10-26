#pragma once

#include <cstdint>

namespace SFE {
	// Where one mesh lives inside MeshVaoRegistry's shared vertex and index buffers. Holds no buffer handle:
	// the shared buffers are replaced when they grow, and copies of this struct outlive that.
	struct RenderMeshData {
		friend bool operator<(const RenderMeshData& lhs, const RenderMeshData& rhs) { return lhs.meshId < rhs.meshId; }
		friend bool operator==(const RenderMeshData& lhs, const RenderMeshData& rhs) { return lhs.meshId == rhs.meshId; }
		friend bool operator!=(const RenderMeshData& lhs, const RenderMeshData& rhs) { return !(lhs == rhs); }

		explicit operator bool() const { return meshId != 0 && (vertexCount != 0 || indexCount != 0); }

		uint32_t meshId = 0;       // 0: not uploaded
		uint32_t firstVertex = 0;  // base vertex: indices are local to the mesh
		uint32_t firstIndex = 0;
		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
		// Levels in the chain including this one. The coarser levels are the mesh ids right after this one,
		// so a caller walks them without another lookup.
		uint32_t lodCount = 1;
		// Mesh::oneSided: drawn with its back faces culled.
		bool oneSided = false;
	};
}
