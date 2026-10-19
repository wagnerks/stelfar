#include "GpuMaterialTable.h"

#include <algorithm>

#include "assetsModule/modelModule/Terrain.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "logsModule/logger.h"
#include "renderModule/MaterialRules.h"
#include "systemsModule/systems/RenderSystem.h"

namespace SFE::Render {
	namespace {
		// The rules the passes used to apply per draw, resolved once per material.
		GpuMaterial makeGpuMaterial(const ComponentsModule::MaterialComponent& src) {
			const float cutoff = MaterialRules::cutoutCutoff(src.alphaCutoff, src.alphaFromMaps, src.fragmentShader);
			GpuMaterial m;
			m.roughness = src.roughness;
			m.metallic = src.metallic;
			m.mapMask = MaterialRules::gBufferMapMask(src.materials, src.alphaFromMaps);
			// How it moves in the wind, for the vertex shaders: bit 32 a tree, bit 64 a plant.
			m.mapMask += src.sway == ComponentsModule::Sway::Tree ? 32.f : src.sway == ComponentsModule::Sway::Plant ? 64.f : 0.f;
			// Bit 128 the ground, whose mesh is put out of sight where it is drawn dense (sfeGroundUnderPatch).
			m.mapMask += src.fragmentShader == AssetsModule::kTerrainFS ? 128.f : 0.f;
			m.alphaCutoff = src.alphaDither ? 1.78f : cutoff;
			m.uvSel = MaterialRules::gBufferUvSel(src.materials);
			m.specFactor = src.specFactor;
			m.clearcoat = src.clearcoat;
			m.clearcoatRough = src.clearcoatRough;
			m.tint[0] = src.tint.x;
			m.tint[1] = src.tint.y;
			m.tint[2] = src.tint.z;
			m.opacity = src.opacity > 0.f ? src.opacity : 0.22f;
			m.ior = src.ior > 1.f ? src.ior : 1.52f;
			m.casterOpacity = (src.opacity > 0.f && src.opacity < 0.999f) ? src.opacity : 1.f;
			m.shadowCutoff = src.alphaFromMaps ? -0.18f : cutoff;
			std::copy(std::begin(src.triplanar[0].uv), std::end(src.triplanar[0].uv), m.triplanarUv);
			std::copy(std::begin(src.triplanar[1].uv), std::end(src.triplanar[1].uv), m.triplanarUv2);
			for (size_t i = 0; i < ComponentsModule::MaterialComponent::kTriplanarLayers; ++i) {
				m.triplanarRepeat[i] = src.triplanar[i].metres > 0.f ? 1.f / src.triplanar[i].metres : 0.f;
				m.triplanarRotated += src.triplanar[i].rotated ? static_cast<float>(1u << i) : 0.f;
			}
			m.wetBelow = src.wetBelow;
			for (size_t w = 0; w < ComponentsModule::MaterialComponent::kMudWheels; ++w) {
				for (size_t i = 0; i < 4; ++i) {
					m.mudWheels[4 * w + i] = src.mudWheels[w][i];
				}
			}
			for (size_t i = 0; i < 4; ++i) {
				m.mudTreads[i] = src.mudTreads[i];
				m.mudMap[i] = src.mudMap[i];
			}
			m.bare = src.bare;
			m.emissive[0] = src.emissive.x;
			m.emissive[1] = src.emissive.y;
			m.emissive[2] = src.emissive.z;
			return m;
		}
	}

	uint32_t GpuMaterialTable::acquire(const ComponentsModule::MaterialComponent& material) {
		const auto hash = MaterialRules::materialHash(material);
		std::lock_guard lock(mMutex);
		if (const auto it = mIndexByHash.find(hash); it != mIndexByHash.end()) {
			++mStates[it->second].refs;
			return it->second;
		}

		uint32_t index = 0;
		if (!mFree.empty()) {
			index = mFree.back();
			mFree.pop_back();
		}
		else {
			index = static_cast<uint32_t>(mEntries.size());
			mEntries.emplace_back();
			mStates.emplace_back();
		}
		auto entry = makeGpuMaterial(material);
		auto& state = mStates[index];
		state = { hash, 1, 0 };
		if (material.transparent) {
			state.glassSlot = acquireGlassSlot(MaterialRules::diffuseTexture(material.materials));
			entry.albedoSlot = static_cast<float>(state.glassSlot);
		}
		mEntries[index] = entry;
		mIndexByHash.emplace(hash, index);
		mDirty.push_back(index);
		return index;
	}

	void GpuMaterialTable::release(const uint32_t index) {
		std::lock_guard lock(mMutex);
		if (index >= mStates.size() || mStates[index].refs == 0) {
			return;
		}
		auto& state = mStates[index];
		if (--state.refs > 0) {
			return;
		}
		mIndexByHash.erase(state.hash);
		releaseGlassSlot(state.glassSlot);
		state = {};
		mFree.push_back(index);
	}

	void GpuMaterialTable::reset() {
		std::lock_guard lock(mMutex);
		mIndexByHash.clear();
		mEntries.clear();
		mStates.clear();
		mFree.clear();
		mDirty.clear();
		mGlassTextures.assign(1, TextureHandle{});
		mGlassRefs.assign(1, 0);
	}

	size_t GpuMaterialTable::liveEntries() {
		std::lock_guard lock(mMutex);
		return mIndexByHash.size();
	}

	uint32_t GpuMaterialTable::acquireGlassSlot(const TextureHandle albedo) {
		if (!albedo) {
			return 0;
		}
		for (size_t i = 1; i < mGlassTextures.size(); ++i) {
			if (mGlassRefs[i] > 0 && mGlassTextures[i].id == albedo.id) {
				++mGlassRefs[i];
				return static_cast<uint32_t>(i);
			}
		}
		for (size_t i = 1; i < mGlassTextures.size(); ++i) {
			if (mGlassRefs[i] == 0) {
				mGlassTextures[i] = albedo;
				mGlassRefs[i] = 1;
				return static_cast<uint32_t>(i);
			}
		}
		if (mGlassTextures.size() == kGlassTextureSlots) {
			LogsModule::Logger::LOG_WARNING("GpuMaterialTable: more than %zu glass albedo textures in use, the rest draw untextured", kGlassTextureSlots - 1);
			return 0;
		}
		mGlassTextures.push_back(albedo);
		mGlassRefs.push_back(1);
		return static_cast<uint32_t>(mGlassTextures.size() - 1);
	}

	void GpuMaterialTable::releaseGlassSlot(const uint32_t slot) {
		if (slot == 0 || slot >= mGlassRefs.size() || mGlassRefs[slot] == 0) {
			return;
		}
		if (--mGlassRefs[slot] == 0) {
			mGlassTextures[slot] = {};
		}
	}

	std::vector<TextureHandle> GpuMaterialTable::glassTextures() {
		std::lock_guard lock(mMutex);
		return mGlassTextures;
	}

	BufferHandle GpuMaterialTable::upload() {
		std::lock_guard lock(mMutex);
		auto* rhi = Engine::instance()->rhi();
		if (!mBuffer || mEntries.size() > mCapacity) {
			if (mBuffer) {
				// Frames already recorded may still read it; the RHI keeps it until they have finished.
				rhi->destroyBuffer(mBuffer);
			}
			mCapacity = mCapacity ? mCapacity : 256;
			while (mCapacity < mEntries.size()) {
				mCapacity *= 2;
			}
			BufferDesc desc;
			desc.size = mCapacity * sizeof(GpuMaterial);
			desc.usage = BufferUsage::Storage;
			desc.cpuAccess = CpuAccess::None;
			desc.debugName = "GpuMaterialTable";
			mBuffer = rhi->createBuffer(desc);
			mDirty.resize(mEntries.size());
			for (uint32_t i = 0; i < mDirty.size(); ++i) {
				mDirty[i] = i;
			}
		}
		// In runs of neighbouring entries.
		std::sort(mDirty.begin(), mDirty.end());
		mDirty.erase(std::unique(mDirty.begin(), mDirty.end()), mDirty.end());
		for (size_t i = 0; i < mDirty.size();) {
			size_t end = i + 1;
			while (end < mDirty.size() && mDirty[end] == mDirty[end - 1] + 1) {
				++end;
			}
			const uint32_t first = mDirty[i];
			const size_t count = end - i;
			if (first + count <= mEntries.size()) {
				rhi->updateBuffer(mBuffer, first * sizeof(GpuMaterial), count * sizeof(GpuMaterial), mEntries.data() + first);
			}
			i = end;
		}
		mDirty.clear();
		return mBuffer;
	}
}
