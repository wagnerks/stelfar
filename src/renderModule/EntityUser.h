#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>
#include "ecss/Fwd.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	// Eight words an application keeps for each entity, for its shaders: g_buffer.vs reads them by the entity's id from
	// binding 37 of set 0 (GlassPass's set too) and hands them on flat to the fragment shader (sfe_words.glsl: vWords
	// words 0-3, vWordsMore 4-7). Word 7 is the writer's kind, a nonzero number of its own. Any system writes, any thread;
	// an entity's words go when it does (RenderSystem, ENTITY_DESTROYED, before its id can be taken again).
	class EntityUser {
	public:
		static constexpr size_t kWords = 8;
		static constexpr size_t kKind = 7;
		using Words = std::array<uint32_t, kWords>;

		static EntityUser& instance();

		// `words[kKind]` 0: logged once, ignored. Words of another kind than the entity holds replace them (the later
		// writer wins), logged once per pair of kinds (LOG_ERROR "EntityUser: ..."). Unchanged words cost a compare.
		void set(ecss::EntityId entity, const Words& words);
		// The entity's words to 0 if it holds `kind`; whatever it holds with 0.
		void clear(ecss::EntityId entity, uint32_t kind = 0);
		Words get(ecss::EntityId entity) const; // all 0 for none

		// RenderSystem: create in its constructor (a device-local buffer, CpuAccess::None, of max(4096, a power of two past
		// the largest id written) entities x 32 bytes, holding the words written so far); upload after the passes'
		// prepare, before the frame's commands (the ids written since as runs of consecutive ids, updateBuffer each,
		// queued in the frame as the transforms are; the whole store into a new buffer when an id is past the buffer, the
		// old one destroyed -- the RHI keeps it while frames still bind it); destroy in its destructor (the buffer, and every
		// entity's words). set/clear/get work before create() and after destroy(), on the CPU store alone.
		// createBuffer copies desc.size bytes of its initial data (VulkanRHIResources.cpp:557-573), so create() and the
		// grow first resize the store (zeros) to the new capacity and then make the buffer from it. upload() runs only
		// from RenderSystem's channel step, before beginCommandList: a device-local write after the frame's uploads were
		// flushed is submitted and waited on the spot and logged (VulkanRHIResources.cpp:156-163).
		void create(RenderHardwareInterface& rhi);
		void upload();
		void destroy();
		BufferHandle buffer() const;  // binding 37's, after create()
		size_t capacity() const;      // entities the buffer holds

		// For tests: the runs {first id, count} the next upload() sends; and the runs of a list of ids.
		std::vector<std::pair<uint32_t, uint32_t>> pendingRuns() const;
		static std::vector<std::pair<uint32_t, uint32_t>> runsOf(std::vector<uint32_t> ids); // sorted, unique, merged
	private:
		mutable std::mutex mMutex;
		std::vector<Words> mWords;    // by entity id
		std::vector<uint32_t> mDirty; // ids written since the last upload
		RenderHardwareInterface* mRhi = nullptr;
		BufferHandle mBuffer{};
		size_t mCapacity = 0;
	};
}
