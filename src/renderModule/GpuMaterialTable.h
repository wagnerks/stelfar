#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "componentsModule/MaterialComponent.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	// One material as the shaders read it (GpuMaterial.h).
	struct GpuMaterial;

	// One entry per distinct material, keyed by MaterialRules::materialHash and counted by the instances using it.
	// An index stays valid while it is held; a released one is reused, so editing a material (a slider drag gives
	// a new hash per step) does not grow the table.
	class GpuMaterialTable {
	public:
		// Defined in the .cpp, where GpuMaterial is whole.
		GpuMaterialTable();
		~GpuMaterialTable();

		// Any thread. Every acquire is matched by one release of the index it returned.
		uint32_t acquire(const ComponentsModule::MaterialComponent& material);
		void release(uint32_t index);
		// Drops every entry, for a caller about to acquire everything again.
		void reset();
		// Main thread. Uploads entries changed since the last call and returns the buffer to bind.
		BufferHandle upload();

		// The albedo of every glass material, in slot order; an empty handle means no texture. Glass binds them
		// all as one array so a single indirect call can draw every glass instance in depth order.
		static constexpr size_t kGlassTextureSlots = 8;
		std::vector<TextureHandle> glassTextures();

		// For tests.
		size_t liveEntries();
		// A copy of the entry at `index` as it is uploaded; a default one out of range.
		GpuMaterial entry(uint32_t index);

	private:
		struct EntryState {
			uint64_t hash = 0;
			uint32_t refs = 0;
			uint32_t glassSlot = 0; // 0: none
		};

		uint32_t acquireGlassSlot(TextureHandle albedo);
		void releaseGlassSlot(uint32_t slot);

		// Slot 0 is the untextured fallback and never released.
		std::vector<TextureHandle> mGlassTextures{ TextureHandle{} };
		std::vector<uint32_t> mGlassRefs{ 0 };
		std::mutex mMutex;
		std::unordered_map<uint64_t, uint32_t> mIndexByHash;
		std::vector<GpuMaterial> mEntries;
		std::vector<EntryState> mStates;
		std::vector<uint32_t> mFree;
		std::vector<uint32_t> mDirty; // entries to upload
		size_t mCapacity = 0;
		BufferHandle mBuffer;
	};
}
