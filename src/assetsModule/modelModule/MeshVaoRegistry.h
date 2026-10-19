#pragma once
#include <mutex>
#include <unordered_map>

#include "Mesh.h"
#include "assetsModule/RenderMeshData.h"
#include "containersModule/Singleton.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE {
	// One entry per meshId, as the GPU culling reads it (std430 GpuMesh in shaders/gpu_cull.cs).
	struct GpuMesh {
		static constexpr size_t kMaxLodErrors = 6;
		uint32_t firstIndex = 0;
		uint32_t indexCount = 0;
		uint32_t firstVertex = 0;
		uint32_t vertexCount = 0;
		float boundsMin[3]{};
		uint32_t skinned = 0; // 0 rigid, 1 skinned, 2 + n skinned with pose bounds in slot n
		float boundsMax[3]{};
		uint32_t lodIndex = 0;  // 0 is the full detail mesh
		uint32_t lodCount = 1;  // levels in this mesh's chain, the same in every entry of it
		// Levels 1 on: how far each strays from the full mesh, a share of its extent (Mesh::lodErrors). The same in every
		// entry of the chain.
		float lodErrors[kMaxLodErrors]{};
		uint32_t pad = 0;
	};

	// Every mesh shares one vertex buffer and one index buffer, so a draw differs from the next only in
	// its ranges -- which is what lets one indirect call cover many meshes.
	class MeshVaoRegistry : public Singleton<MeshVaoRegistry> {
	public:
		const RenderMeshData& get(Mesh<Vertex3D>* mesh);
		const RenderMeshData& initMesh(Mesh<Vertex3D>* mesh);
		// All of a model's meshes in one trip to the main thread: from a loader thread, initMesh waits for the main
		// thread once per mesh, and a terrain of a thousand chunks waited a thousand times.
		void initMeshes(const std::vector<Mesh<Vertex3D>*>& meshes);

		// Replaced when the buffers grow: fetch per frame, do not cache.
		Render::BufferHandle vertexBuffer() const { return mVertexBuffer; }
		Render::BufferHandle indexBuffer() const { return mIndexBuffer; }

		// Indexed by meshId; entry 0 is unused. Grows only, so its size doubles as a version.
		// Entries from `first` on. An entry never changes once added: offsets survive a grow.
		std::vector<GpuMesh> meshTable(size_t first = 0);
		size_t meshCount();

	private:
		// `uploadNow` false for a caller that uploads a batch itself.
		void allocate(Mesh<Vertex3D>* mesh, RenderMeshData& data, bool uploadNow = true);
		void grow(size_t vertexCapacity, size_t indexCapacity);
		void upload(const Mesh<Vertex3D>* mesh, const RenderMeshData& data) const;

		std::mutex mMutex;
		std::unordered_map<Mesh<Vertex3D>*, RenderMeshData> mMeshVAO;
		std::vector<GpuMesh> mMeshTable{ 1 };

		Render::BufferHandle mVertexBuffer;
		Render::BufferHandle mIndexBuffer;
		size_t mVertexCapacity = 0;
		size_t mIndexCapacity = 0;
		uint32_t mVertexUsed = 0;
		uint32_t mIndexUsed = 0;
		uint32_t mNextMeshId = 1;
	};
}
