#include "renderModule/EntityUser.h"

#include <algorithm>
#include <set>

#include "logsModule/logger.h"

namespace SFE::Render {
	namespace {
		constexpr size_t kLeastCapacity = 4096;

		// What has been said already, for every store: each fault once a run, not once a frame.
		std::mutex sSaidMutex;
		std::set<std::pair<uint32_t, uint32_t>> sSaid;

		bool firstTime(const uint32_t first, const uint32_t second) {
			std::lock_guard lock(sSaidMutex);
			return sSaid.insert({ first, second }).second;
		}

		// Entities a buffer holds for `entities` ids: 4096, doubled until they fit.
		size_t capacityFor(const size_t entities) {
			size_t capacity = kLeastCapacity;
			while (capacity < entities) {
				capacity *= 2;
			}
			return capacity;
		}

		BufferHandle makeBuffer(RenderHardwareInterface& rhi, const std::vector<EntityUser::Words>& words) {
			BufferDesc desc;
			desc.size = words.size() * sizeof(EntityUser::Words);
			desc.usage = BufferUsage::Storage;
			desc.cpuAccess = CpuAccess::None;
			desc.debugName = "EntityWords";
			return rhi.createBuffer(desc, words.data());
		}
	}

	static_assert(sizeof(EntityUser::Words) == 32, "two uvec4 an entity, as g_buffer.vs reads them");

	EntityUser& EntityUser::instance() {
		static EntityUser words;
		return words;
	}

	void EntityUser::set(const ecss::EntityId entity, const Words& words) {
		if (words[kKind] == 0) {
			if (firstTime(0, 0)) {
				LogsModule::Logger::LOG_ERROR("EntityUser: words of kind 0 for entity %u: a writer's kind is a nonzero number of its own; ignored", entity);
			}
			return;
		}
		if (entity == ecss::INVALID_ID) {
			return;
		}
		std::lock_guard lock(mMutex);
		if (entity >= mWords.size()) {
			mWords.resize(static_cast<size_t>(entity) + 1, Words{});
		}
		auto& held = mWords[entity];
		if (held == words) {
			return;
		}
		if (held[kKind] != 0 && held[kKind] != words[kKind] && firstTime(held[kKind], words[kKind])) {
			LogsModule::Logger::LOG_ERROR("EntityUser: entity %u's words of kind %u replaced by kind %u's: the later writer wins", entity,
				held[kKind], words[kKind]);
		}
		held = words;
		mDirty.push_back(entity);
	}

	void EntityUser::clear(const ecss::EntityId entity, const uint32_t kind) {
		std::lock_guard lock(mMutex);
		if (entity >= mWords.size()) {
			return;
		}
		auto& held = mWords[entity];
		if (held[kKind] == 0 || (kind != 0 && held[kKind] != kind)) {
			return;
		}
		held = {};
		mDirty.push_back(entity);
	}

	EntityUser::Words EntityUser::get(const ecss::EntityId entity) const {
		std::lock_guard lock(mMutex);
		return entity < mWords.size() ? mWords[entity] : Words{};
	}

	void EntityUser::create(RenderHardwareInterface& rhi) {
		std::lock_guard lock(mMutex);
		if (mRhi) {
			LogsModule::Logger::LOG_ERROR("EntityUser: made a second time; the first stands");
			return;
		}
		mRhi = &rhi;
		// The buffer takes the store's bytes whole (createBuffer reads desc.size of them), so the store is grown to it
		// first; what was written before is in it, nothing left to send.
		mCapacity = capacityFor(mWords.size());
		mWords.resize(mCapacity, Words{});
		mBuffer = makeBuffer(rhi, mWords);
		mDirty.clear();
	}

	void EntityUser::upload() {
		std::lock_guard lock(mMutex);
		if (!mRhi || mDirty.empty()) {
			return;
		}
		if (mWords.size() > mCapacity) {
			// An id past the buffer: the whole store into one that holds it. The RHI keeps the old one until the frames
			// that may bind it have finished; RenderSystem writes binding 37 again as the id changes.
			mCapacity = capacityFor(mWords.size());
			mWords.resize(mCapacity, Words{});
			mRhi->destroyBuffer(mBuffer);
			mBuffer = makeBuffer(*mRhi, mWords);
			mDirty.clear();
			return;
		}
		for (const auto& [first, count] : runsOf(std::move(mDirty))) {
			mRhi->updateBuffer(mBuffer, static_cast<size_t>(first) * sizeof(Words), static_cast<size_t>(count) * sizeof(Words), mWords.data() + first);
		}
		mDirty.clear();
	}

	void EntityUser::destroy() {
		std::lock_guard lock(mMutex);
		if (mRhi && mBuffer) {
			mRhi->destroyBuffer(mBuffer);
		}
		mBuffer = {};
		mRhi = nullptr;
		mCapacity = 0;
		mWords.clear();
		mDirty.clear();
	}

	BufferHandle EntityUser::buffer() const {
		std::lock_guard lock(mMutex);
		return mBuffer;
	}

	size_t EntityUser::capacity() const {
		std::lock_guard lock(mMutex);
		return mCapacity;
	}

	std::vector<std::pair<uint32_t, uint32_t>> EntityUser::pendingRuns() const {
		std::lock_guard lock(mMutex);
		return runsOf(mDirty);
	}

	std::vector<std::pair<uint32_t, uint32_t>> EntityUser::runsOf(std::vector<uint32_t> ids) {
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		std::vector<std::pair<uint32_t, uint32_t>> runs;
		for (const auto id : ids) {
			if (!runs.empty() && runs.back().first + runs.back().second == id) {
				++runs.back().second;
			}
			else {
				runs.emplace_back(id, 1u);
			}
		}
		return runs;
	}
}
