#include "MeshVaoRegistry.h"

#include <algorithm>
#include <chrono>

#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "logsModule/logger.h"
#include "multithreading/ThreadPool.h"
#include "systemsModule/systems/RenderSystem.h"

namespace SFE {
	namespace {
		constexpr size_t kInitialVertices = 1u << 18;
		constexpr size_t kInitialIndices = 1u << 20;
	}

	const RenderMeshData& MeshVaoRegistry::get(Mesh<Vertex3D>* mesh) {
		assert(mesh);
		return initMesh(mesh);
	}

	const RenderMeshData& MeshVaoRegistry::initMesh(Mesh<Vertex3D>* mesh) {
		assert(mesh);

		std::unique_lock lock(mMutex);
		auto& data = mMeshVAO[mesh];

		if (mesh->vertices.empty() && mesh->indices.empty()) {
			return data;
		}
		if (data.meshId != 0) {
			return data;
		}

		// The shared buffers are written on the main thread only: the upload queue and a grow both touch
		// state the frame being recorded reads.
		if (SFE::Engine::isMainThread()) {
			allocate(mesh, data);
			return data;
		}

		lock.unlock();
		auto futur = ThreadPool::instance()->addTask<WorkerType::SYNC>([mesh]() {
			MeshVaoRegistry::instance()->initMesh(mesh);
		}, "Mesh upload");
		if (futur.valid()) {
			while (futur.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
				if (!ThreadPool::isAlive() || ThreadPool::instance()->isShuttingDown()) {
					break;
				}
			}
		}
		lock.lock();
		return mMeshVAO[mesh];
	}

	void MeshVaoRegistry::initMeshes(const std::vector<Mesh<Vertex3D>*>& meshes) {
		if (SFE::Engine::isMainThread()) {
			std::unique_lock lock(mMutex);
			// The meshes not uploaded yet, back to back: the buffers grow once for all of them, and their vertices and
			// indices go up as one copy each. Uploaded one by one, a terrain's thousand chunks took seconds.
			std::vector<Mesh<Vertex3D>*> pending;
			size_t vertexEnd = mVertexUsed;
			size_t indexEnd = mIndexUsed;
			for (auto* mesh : meshes) {
				const auto& data = mMeshVAO[mesh];
				if (data.meshId != 0 || (mesh->vertices.empty() && mesh->indices.empty())) {
					continue;
				}
				pending.push_back(mesh);
				vertexEnd += mesh->vertices.size();
				indexEnd += mesh->indices.size();
				for (const auto& level : mesh->lodIndices) {
					indexEnd += level.size();
				}
			}
			if (pending.empty()) {
				return;
			}
			if (vertexEnd <= UINT32_MAX && indexEnd <= UINT32_MAX && (!mVertexBuffer || vertexEnd > mVertexCapacity || indexEnd > mIndexCapacity)) {
				size_t vertexCapacity = mVertexCapacity ? mVertexCapacity : kInitialVertices;
				size_t indexCapacity = mIndexCapacity ? mIndexCapacity : kInitialIndices;
				while (vertexCapacity < vertexEnd) {
					vertexCapacity *= 2;
				}
				while (indexCapacity < indexEnd) {
					indexCapacity *= 2;
				}
				grow(vertexCapacity, indexCapacity);
			}
			const uint32_t firstVertex = mVertexUsed;
			const uint32_t firstIndex = mIndexUsed;
			std::vector<Vertex3D> vertices;
			std::vector<unsigned int> indices;
			vertices.reserve(vertexEnd - firstVertex);
			indices.reserve(indexEnd - firstIndex);
			for (auto* mesh : pending) {
				auto& data = mMeshVAO[mesh];
				allocate(mesh, data, false);
				if (data.meshId == 0) {
					break;
				}
				vertices.insert(vertices.end(), mesh->vertices.begin(), mesh->vertices.end());
				indices.insert(indices.end(), mesh->indices.begin(), mesh->indices.end());
				for (const auto& level : mesh->lodIndices) {
					indices.insert(indices.end(), level.begin(), level.end());
				}
			}
			auto* rhi = Engine::instance()->rhi();
			if (!vertices.empty()) {
				rhi->updateBuffer(mVertexBuffer, static_cast<size_t>(firstVertex) * sizeof(Vertex3D), vertices.size() * sizeof(Vertex3D), vertices.data());
			}
			if (!indices.empty()) {
				rhi->updateBuffer(mIndexBuffer, static_cast<size_t>(firstIndex) * sizeof(unsigned int), indices.size() * sizeof(unsigned int), indices.data());
			}
			return;
		}
		auto futur = ThreadPool::instance()->addTask<WorkerType::SYNC>([&meshes]() {
			MeshVaoRegistry::instance()->initMeshes(meshes);
		}, "Mesh upload");
		if (futur.valid()) {
			while (futur.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
				if (!ThreadPool::isAlive() || ThreadPool::instance()->isShuttingDown()) {
					break;
				}
			}
		}
	}

	void MeshVaoRegistry::allocate(Mesh<Vertex3D>* mesh, RenderMeshData& data, const bool uploadNow) {
		size_t lodIndices = 0;
		for (const auto& level : mesh->lodIndices) {
			lodIndices += level.size();
		}
		const size_t vertexEnd = static_cast<size_t>(mVertexUsed) + mesh->vertices.size();
		// The coarser levels sit right behind the full one in the same allocation, over the same vertices.
		const size_t indexEnd = static_cast<size_t>(mIndexUsed) + mesh->indices.size() + lodIndices;
		// Offsets are uint32 in RenderMeshData, the mesh table and the draw commands. Past that a mesh is left
		// unuploaded (meshId 0, never drawn) instead of wrapping onto the start of the buffers.
		if (vertexEnd > UINT32_MAX || indexEnd > UINT32_MAX) {
			LogsModule::Logger::LOG_ERROR("MeshVaoRegistry: shared buffers are full (%zu vertices, %zu indices), mesh not uploaded", vertexEnd, indexEnd);
			return;
		}
		if (!mVertexBuffer || vertexEnd > mVertexCapacity || indexEnd > mIndexCapacity) {
			size_t vertexCapacity = mVertexCapacity ? mVertexCapacity : kInitialVertices;
			size_t indexCapacity = mIndexCapacity ? mIndexCapacity : kInitialIndices;
			while (vertexCapacity < vertexEnd) {
				vertexCapacity *= 2;
			}
			while (indexCapacity < indexEnd) {
				indexCapacity *= 2;
			}
			grow(vertexCapacity, indexCapacity);
		}

		data.meshId = mNextMeshId++;
		data.firstVertex = mVertexUsed;
		data.firstIndex = mIndexUsed;
		data.vertexCount = static_cast<uint32_t>(mesh->vertices.size());
		data.indexCount = static_cast<uint32_t>(mesh->indices.size());
		data.lodCount = 1u + static_cast<uint32_t>(mesh->lodIndices.size());
		mVertexUsed = static_cast<uint32_t>(vertexEnd);
		mIndexUsed = static_cast<uint32_t>(indexEnd);
		if (uploadNow) {
			upload(mesh, data);
		}

		GpuMesh entry;
		entry.firstIndex = data.firstIndex;
		entry.indexCount = data.indexCount;
		entry.firstVertex = data.firstVertex;
		entry.vertexCount = data.vertexCount;
		if (!mesh->vertices.empty()) {
			for (int i = 0; i < 3; ++i) {
				entry.boundsMin[i] = entry.boundsMax[i] = mesh->vertices.front().position[i];
			}
			for (const auto& v : mesh->vertices) {
				for (int i = 0; i < 3; ++i) {
					entry.boundsMin[i] = (std::min)(entry.boundsMin[i], v.position[i]);
					entry.boundsMax[i] = (std::max)(entry.boundsMax[i], v.position[i]);
				}
				if (v.boneIDs[0] >= 0) {
					entry.skinned = mesh->poseBounds ? 1 + mesh->poseBounds : 1;
				}
			}
		}
		entry.lodCount = data.lodCount;
		for (size_t level = 0; level < mesh->lodErrors.size() && level < GpuMesh::kMaxLodErrors; ++level) {
			entry.lodErrors[level] = mesh->lodErrors[level];
		}
		mMeshTable.push_back(entry);

		// One id per level, consecutive, so a caller holding the full detail id reaches the rest by adding to
		// it. Bounds and skinning are the mesh's own and do not change with the level.
		uint32_t levelFirstIndex = data.firstIndex + data.indexCount;
		for (size_t level = 0; level < mesh->lodIndices.size(); ++level) {
			GpuMesh coarse = entry;
			coarse.firstIndex = levelFirstIndex;
			coarse.indexCount = static_cast<uint32_t>(mesh->lodIndices[level].size());
			coarse.lodIndex = static_cast<uint32_t>(level) + 1u;
			levelFirstIndex += coarse.indexCount;
			mMeshTable.push_back(coarse);
			++mNextMeshId;
		}
	}

	std::vector<GpuMesh> MeshVaoRegistry::meshTable(const size_t first) {
		std::lock_guard lock(mMutex);
		if (first >= mMeshTable.size()) {
			return {};
		}
		return { mMeshTable.begin() + static_cast<std::ptrdiff_t>(first), mMeshTable.end() };
	}

	size_t MeshVaoRegistry::meshCount() {
		std::lock_guard lock(mMutex);
		return mMeshTable.size();
	}

	void MeshVaoRegistry::grow(const size_t vertexCapacity, const size_t indexCapacity) {
		auto* rhi = Engine::instance()->rhi();
		const auto retire = [rhi](const Render::BufferHandle buffer) {
			if (!buffer) {
				return;
			}
			// Frames already recorded may still draw from it; the RHI keeps it until they have finished.
			rhi->destroyBuffer(buffer);
		};
		retire(mVertexBuffer);
		retire(mIndexBuffer);

		Render::BufferDesc vboDesc;
		vboDesc.usage = Render::BufferUsage::Vertex;
		vboDesc.cpuAccess = Render::CpuAccess::None;
		vboDesc.size = vertexCapacity * sizeof(Vertex3D);
		vboDesc.debugName = "MeshVaoRegistry shared VBO";
		mVertexBuffer = rhi->createBuffer(vboDesc);

		Render::BufferDesc eboDesc;
		eboDesc.usage = Render::BufferUsage::Index;
		eboDesc.cpuAccess = Render::CpuAccess::None;
		eboDesc.size = indexCapacity * sizeof(unsigned int);
		eboDesc.debugName = "MeshVaoRegistry shared EBO";
		mIndexBuffer = rhi->createBuffer(eboDesc);

		mVertexCapacity = vertexCapacity;
		mIndexCapacity = indexCapacity;

		// Offsets stay where they were, so every copy of a RenderMeshData stays valid. The meshes go up in the order they
		// lie, a chunk at a time: one by one, the terrain's thousand chunks and everything else made a grow late in a run --
		// a model read again while the game runs -- take half a minute.
		std::vector<std::pair<const Mesh<Vertex3D>*, const RenderMeshData*>> placed;
		placed.reserve(mMeshVAO.size());
		for (const auto& [mesh, data] : mMeshVAO) {
			if (data.meshId != 0) {
				placed.emplace_back(mesh, &data);
			}
		}
		constexpr size_t kChunkBytes = size_t{ 64 } << 20;
		const auto copyUp = [rhi](const Render::BufferHandle buffer, auto& chunk, size_t& start) {
			using Element = typename std::decay_t<decltype(chunk)>::value_type;
			if (!chunk.empty()) {
				rhi->updateBuffer(buffer, start * sizeof(Element), chunk.size() * sizeof(Element), chunk.data());
				chunk.clear();
			}
		};
		std::sort(placed.begin(), placed.end(), [](const auto& a, const auto& b) { return a.second->firstVertex < b.second->firstVertex; });
		std::vector<Vertex3D> vertices;
		size_t vertexStart = 0;
		for (const auto& [mesh, data] : placed) {
			if (mesh->vertices.empty()) {
				continue;
			}
			if (vertices.empty() || data->firstVertex != vertexStart + vertices.size()
				|| (vertices.size() + mesh->vertices.size()) * sizeof(Vertex3D) > kChunkBytes) {
				copyUp(mVertexBuffer, vertices, vertexStart);
				vertexStart = data->firstVertex;
			}
			vertices.insert(vertices.end(), mesh->vertices.begin(), mesh->vertices.end());
		}
		copyUp(mVertexBuffer, vertices, vertexStart);
		// Each mesh's indices, and its levels of detail after them.
		std::sort(placed.begin(), placed.end(), [](const auto& a, const auto& b) { return a.second->firstIndex < b.second->firstIndex; });
		std::vector<unsigned int> indices;
		size_t indexStart = 0;
		for (const auto& [mesh, data] : placed) {
			size_t count = mesh->indices.size();
			for (const auto& level : mesh->lodIndices) {
				count += level.size();
			}
			if (count == 0) {
				continue;
			}
			if (indices.empty() || data->firstIndex != indexStart + indices.size()
				|| (indices.size() + count) * sizeof(unsigned int) > kChunkBytes) {
				copyUp(mIndexBuffer, indices, indexStart);
				indexStart = data->firstIndex;
			}
			indices.insert(indices.end(), mesh->indices.begin(), mesh->indices.end());
			for (const auto& level : mesh->lodIndices) {
				indices.insert(indices.end(), level.begin(), level.end());
			}
		}
		copyUp(mIndexBuffer, indices, indexStart);
	}

	void MeshVaoRegistry::upload(const Mesh<Vertex3D>* mesh, const RenderMeshData& data) const {
		auto* rhi = Engine::instance()->rhi();
		if (!mesh->vertices.empty()) {
			rhi->updateBuffer(mVertexBuffer, static_cast<size_t>(data.firstVertex) * sizeof(Vertex3D),
				mesh->vertices.size() * sizeof(Vertex3D), mesh->vertices.data());
		}
		if (!mesh->indices.empty()) {
			rhi->updateBuffer(mIndexBuffer, static_cast<size_t>(data.firstIndex) * sizeof(unsigned int),
				mesh->indices.size() * sizeof(unsigned int), mesh->indices.data());
		}
		size_t offset = static_cast<size_t>(data.firstIndex) + mesh->indices.size();
		for (const auto& level : mesh->lodIndices) {
			rhi->updateBuffer(mIndexBuffer, offset * sizeof(unsigned int),
				level.size() * sizeof(unsigned int), level.data());
			offset += level.size();
		}
	}
}
