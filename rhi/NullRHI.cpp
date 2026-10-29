#include "rhi/NullRHI.h"

namespace SFE::Render {
	size_t NullRHI::live(const Kind kind) const {
		std::lock_guard lock(mLiveMutex);
		return mLive[static_cast<size_t>(kind)].size();
	}

	uint32_t NullRHI::made(const Kind kind) {
		const uint32_t id = nextId();
		std::lock_guard lock(mLiveMutex);
		mLive[static_cast<size_t>(kind)].insert(id);
		return id;
	}

	void NullRHI::gone(const Kind kind, const uint32_t id) {
		std::lock_guard lock(mLiveMutex);
		mLive[static_cast<size_t>(kind)].erase(id);
	}

	BufferHandle NullRHI::createBuffer(const BufferDesc& desc, const void*) {
		const BufferHandle handle{ made(Kind::Buffer) };
		std::lock_guard lock(mBuffersMutex);
		mBufferSizes[handle.id] = desc.size;
		return handle;
	}

	void NullRHI::destroyBuffer(const BufferHandle handle) {
		gone(Kind::Buffer, handle.id);
		std::lock_guard lock(mBuffersMutex);
		mBufferSizes.erase(handle.id);
		mMapped.erase(handle.id);
	}

	void* NullRHI::mapBuffer(const BufferHandle handle, MapMode) {
		std::lock_guard lock(mBuffersMutex);
		const auto size = mBufferSizes.find(handle.id);
		if (size == mBufferSizes.end()) {
			return nullptr;
		}
		auto& memory = mMapped[handle.id];
		memory.resize(size->second);
		return memory.data();
	}

	TransientAllocation NullRHI::allocTransient(const size_t size) {
		auto& memory = mTransient.emplace_back(size == 0 ? 1 : size);
		return { mTransientBuffer, 0, memory.data() };
	}

	void NullRHI::beginFrame() {
		mTransient.clear();
	}
}
