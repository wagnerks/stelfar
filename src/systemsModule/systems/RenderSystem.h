#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "assetsModule/modelModule/BoundingVolume.h"
#include "componentsModule/ArmatureComponent.h"
#include "componentsModule/OutlineComponent.h"
#include "componentsModule/TransformComponent.h"
#include "systemsModule/SystemBase.h"
#include "renderModule/renderPasses/RenderPass.h"
#include "renderModule/FrameBlackboard.h"
#include "renderModule/GpuMaterialTable.h"
#include "renderModule/GpuScene.h"

namespace SFE {
	namespace Render {
		class Renderer;
	}
}

namespace SFE::SystemsModule {

	enum class RenderMode {
		DEFAULT,
		WIREFRAME,
		NORMALS,
	};

	struct RenderMatrices {
		Math::Mat4 projection = {};
		Math::Mat4 view = {};
		Math::Mat4 PV = {}; // jittered, what the rasterizer uses
		// Unjittered, this frame and the last: the G-buffer's motion vectors are the difference of the two, and
		// the jitter would show up in them as a wobble that TAA then chases.
		Math::Mat4 PVNoJitter = {};
		Math::Mat4 prevPVNoJitter = {};
		// The scene's wind for the vertex shaders: xz the unit direction it blows along, y its strength, w the clock in
		// seconds. The one before is last frame's, so a swaying leaf's motion vector is right too.
		Math::Vec4 wind = {};
		Math::Vec4 windBefore = {};
		// Of the matrices above, as the GPU gets them: screen passes put world positions back together from depth.
		Math::Mat4 invProjection = {};
		Math::Mat4 invView = {};
		// x GraphicsSettings::stylise: a shader drawing in that look can leave out what it would not show.
		Math::Vec4 style = {};
		// The area tint (AreaTintFrame): its map's place and the bank's turn to mud, as the frame's passes left them;
		// the map itself is binding 22 of the frame set. A zero area, no map.
		Math::Vec4 tintArea = {};
		Math::Vec4 tintShore = {};
		// The marks in the ground (GroundMarksFrame): its windows, sizes and look, as the frame's passes left them; the
		// map itself is binding 34 of the frame set. A zero size, no marks.
		Math::Vec4 marksWindow = {};
		Math::Vec4 marksSize = {};
		Math::Vec4 marksLook = {};
		Math::Vec4 marksChurn = {};
		// The ground drawn dense near the camera (GroundMarksFrame::grid, patch); its heights are binding 35.
		Math::Vec4 marksGrid = {};
		Math::Vec4 marksPatch = {};
	};

	struct GraphicsSettings {
		bool ssao = true;
		bool ssr = true;
		bool volumetric = true;
		bool taa = true;
		bool dof = false;
		bool bloom = true;
		bool sheen = true;
		bool sss = true;
		bool contactGi = false;

		float ssrDistance = 12.f;

		float volumetricDensity = 0.004f;
		float volumetricIntensity = 1.5f;
		float volumetricAnisotropy = 0.68f;
		float volumetricDistance = 90.f;

		// History weight. Must stay high enough that the jitter pattern averages out over its 8
		// phases -- at 0.72 the resolve settles in ~3.6 frames and the pattern shows as shimmer.
		float taaBlend = 0.92f;

		float dofFocus = 18.f;
		float dofRange = 28.f;
		float dofBlur = 3.5f;

		float bloomStrength = 0.12f;
		float exposure = 1.12f;
		float chromaticAberration = 0.f;
		// Contrast adaptive sharpening after the tonemap: 0 leaves the image as TAA resolved it, 1 is as far
		// as the filter goes before it starts ringing.
		float sharpness = 0.5f;
		// The eye following the scene's brightness: exposure then only nudges it.
		bool autoExposure = true;
		float adaptationSpeed = 1.5f; // per second
		float flareStrength = 0.6f;
		// The grade: -1 cool .. 1 warm, and saturation and contrast around 1.
		float warmth = 0.2f;
		float saturation = 1.15f;
		float contrast = 1.05f;

		float fogStart = 0.82f;
		// Metres the camera sees at most: its far plane. Past it the scene fades into the haze, and nothing further
		// out is drawn at all.
		float viewDistance = 3000.f;
		// Hours, 0..24, placing the sun of an outdoor scene; below 0 the scene's own sun.
		float timeOfDay = -1.f;
		// 0 still air, 1 a strong breeze; below 0 the scene's own wind.
		float windStrength = -1.f;
		float glassRoughness = 0.02f;

		float sheenStrength = 0.85f;
		float rimStrength = 0.7f;
		float sssWrap = 0.85f;
		float sssStrength = 0.65f;
		// Metres a tap of the light spread under the skin (sss.fs), the red reaching three taps.
		float sssRadius = 0.003f;
		float contactStrength = 0.55f;
		float contactDistance = 8.f;
		float contactThickness = 0.12f;
		float giStrength = 0.12f;
		float giRadius = 0.45f;

		float sunIntensity = 3.2f;
		float iblStrength = 1.f;
		float shadowAmbient = 0.42f;
		// The ambient occlusion (ssao.fs, horizon based): how far round a point it looks for what covers it and how thick
		// it takes what stands in front to be, in metres; the power its visibility is raised to, and the share of it the
		// sun's light takes as well (painted: a crease is dark in the sun too); the least a blade of grass keeps of it.
		float ssaoRadius = 1.5f;
		float ssaoThickness = 0.5f;
		float ssaoIntensity = 1.5f;
		float ssaoSun = 0.7f;
		float ssaoGrassFloor = 0.85f;
		// The sun's shadow (deferred_shading.fs): sharp where it leaves what casts it, wider the further it falls -- the
		// sun's width, as the penumbra's metres a metre from the caster -- up to the widest penumbra, metres.
		float shadowSunWidth = 0.04f;
		float shadowPenumbraMost = 0.5f;
		// The casters' depth pushed off the sun by this many times their own depth's change across a texel, up to the
		// most, metres (CascadedShadowPass): a triangle of a coarse mesh just turned from the sun is in the map, and lit
		// beside it compared with itself, it shadowed itself in facets along the edge the light leaves.
		float shadowSlopeBias = 3.f;
		float shadowSlopeBiasMost = 0.02f;
		// The share of the sun a tree's or a plant's leaves let through to what lies under them (cascadeShadowMap.fs).
		float shadowLeafLight = 0.f;
		// How many times a second each cascade of the sun's shadow is drawn anew at most, 0 every frame
		// (CascadedShadowPass).
		float shadowRefreshRate = 0.f;

		// The grass clumps: how many to a square metre against the default, and metres out to which they grow.
		float grassDensity = 1.f;
		float grassDistance = 90.f;
		// How far the vegetation's layers reach, against their own distances.
		float lodDetail = 1.8f;
		// The pixels on screen a coarser level of a model may change from the full one (its simplifier's error, a share of
		// its extent, times the extent in pixels) for it to be drawn: smaller keeps the detail further.
		float lodPixels = 2.f;
		// Metres from the eye to a chunk of the ground past which it is drawn a level coarser (about half its triangles):
		// far off its triangles are a few pixels and shading wastes most of each 2x2 quad; past the view, never.
		float terrainLodDistance = 200.f;
		// The foliage's cut-out leaves drawn for their depth alone before they are shaded, so each pixel of them is shaded
		// once (GeometryPass): 1 on, 0 off.
		float foliagePrepass = 1.f;

		// How far the picture is pushed from what the light really does towards a painted one: 0 leaves it, 1 puts the
		// sun's light in steps, takes the highlights off and flattens the colour.
		float stylise = 1.f;

		// How much the time of day grades the picture (warm low sun, blue night).
		float timeGrade = 1.f;
		// Shafts of light from the sun through the gaps in the clouds and the trees, gathered over the bloom pass.
		float sunRays = 0.45f;

		// Dust and pollen in the sunlight, midges and flies: whether, and how many of them show.
		bool airLife = true;
		float airLifeAmount = 1.f;
	};

	struct RenderData {
		FrustumModule::Frustum mCamFrustum;
		FrustumModule::Frustum mNextCamFrustum;

		RenderMatrices current;
		RenderMatrices next;
		Math::Mat4 prevPV = {};

		Math::Vec3 mCameraPos = {};
		Math::Vec3 mViewDir = {};
		Math::Vec3 mNextCameraPos = {};
		Math::Vec3 mNextViewDir = {};

		MathModule::PerspectiveProjection cameraProjection = {};
		MathModule::PerspectiveProjection nextCameraProjection =  {};

		Render::FrameBlackboard frame;

		Render::BufferHandle transformsSSBO;
		Render::BufferHandle bonesSSBO;
		// What the same entities and bones held last frame, for the motion vectors.
		Render::BufferHandle prevTransformsSSBO;
		Render::BufferHandle prevBonesSSBO;
		Render::BufferHandle cameraMatricesUBO;
		Render::DescriptorSetLayoutHandle frameSetLayout;

		RenderMode mRenderType = RenderMode::DEFAULT;
		GraphicsSettings graphics;
		// A sphere the nearest cascade of the sun's shadow is fitted round instead of its slice of the view: xyz its
		// centre, w its radius, 0 for none. The player's own body, so its shadows on itself -- a chin on the throat, the
		// hands on what they hold -- are as fine seen from beside it as from its eyes (CascadeShadowComponent).
		Math::Vec4 shadowFocus = {};

		int taaFrame = 0;
		float taaJitterX = 0.f;
		float taaJitterY = 0.f;
	};

		class RenderDebugInfo : public Singleton<RenderDebugInfo>
	{
	public:
		struct PassTime {
			const char* name = "";
			float cpuMs = 0.f;
			float gpuMs = 0.f; // of the latest frame the GPU finished, where the backend times passes
		};

		size_t objectsRendered = 0;
		size_t verticesRendered = 0;
		size_t drawCalls = 0;
		size_t cullConsidered = 0;
		size_t cullVisible = 0;
		size_t cullShadow = 0;
		size_t shadowMeshes = 0;
		size_t shadowFbos = 0;
		size_t shadowFbosTotal = 0;
		uint32_t shadowPipeline = 0;
		float shadowIntensity = 0.f;

		static constexpr int kMaxPasses = 64;
		PassTime passTimes[kMaxPasses]{};
		int passCount = 0;
		// The GPU's time of the latest frame it finished, by pass and, where it was marked by pipeline
		// (RenderHardwareInterface::gpuShaderMarks), by the pipelines drawn in the pass: a pass's whole has no shader, and
		// what of a marked pass no pipeline drew is its "other".
		struct GpuTime {
			const char* pass = "";
			const char* shader = nullptr;
			float ms = 0.f;
		};
		std::vector<GpuTime> gpuTimes;
		// gpuTimes made from the spans the RHI read back.
		void takeGpuSpans(const std::vector<Render::RenderHardwareInterface::GpuSpan>& spans);
		// A pass's whole in gpuTimes; 0 if it is not there.
		float gpuMsOf(const char* pass) const;
		float cpuMs = 0.f;
		float prepareMs = 0.f; // the passes' prepare(), before the command list
		float waitMs = 0.f;    // beginning the command list: on Vulkan, waiting for the frame in flight to finish
		float submitMs = 0.f;
		float presentMs = 0.f;
		float gpuMs = 0.f;

		// The last few seconds of frames, oldest at `historyCursor`, for the graph in the settings window.
		static constexpr int kHistory = 240;
		float frameMsHistory[kHistory]{};
		float gpuMsHistory[kHistory]{};
		int historyCursor = 0;
		void pushFrame(float frameMs, float gpu) {
			frameMsHistory[historyCursor] = frameMs;
			gpuMsHistory[historyCursor] = gpu;
			historyCursor = (historyCursor + 1) % kHistory;
		}
	};

	class RenderSystem : public ecss::System {
	public:
		RenderSystem();
		~RenderSystem() override;

		void update(float_t dt) override;
		void debugUpdate(float dt) override;
		inline void setRenderType(RenderMode type) { mRenderData.mRenderType = type; }
		inline RenderData& getRenderData() { return mRenderData; }
		Render::DescriptorSetHandle frameSetHandle() const { return frameSet; }
		int renderedFrames() const { return mRenderedFrames; }
		Render::DescriptorSetLayoutHandle frameSetLayoutHandle() const { return frameSetLayout; }

		void uploadDirtyGpuData();
		Render::GpuMaterialTable& materials() { return mMaterials; }
		Render::GpuScene& scene() { return mScene; }
		// Its place in the bone buffer let go, to be found again at the size its skeleton is now (a model read again).
		void forgetBones(ecss::EntityId entity) { releaseBones(entity); }
		Render::BufferHandle transformsBuffer() const { return transforms; }
		Render::BufferHandle bonesBuffer() const { return bones; }
		Render::BufferHandle prevBonesBuffer() const { return prevBones; }

		// Where an entity's bone matrices live in the bone buffer, as many as its armature has. Allocated on first
		// use for an entity with an armature and kept until it is destroyed; count 0 for one without. Main thread.
		// The `bounds` matrices before `offset` hold the pose bounds of its skinned meshes (Armature::poseBoundsCount),
		// written by AnimationComputePass and read by the cull: column 0 xyz the minimum, column 1 the maximum, a
		// minimum above the maximum when there are none.
		struct BoneRange {
			uint32_t offset = 0;
			uint32_t count = 0;
			uint32_t bounds = 0;
		};
		BoneRange boneRange(ecss::EntityId entity);

		// Entities whose animation state may have changed since the last call (sent ANIMATION_UPDATED or
		// ARMATURE_UPDATED, or were destroyed), duplicates possible. Main thread.
		std::vector<ecss::EntityId> takeDirtyAnimations();

		// Passes from game code or a plugin. The pass is initialised at once and runs every frame in list order,
		// after the pass named `after`, or last when `after` is empty or names no pass. A name already in the list
		// is refused. Main thread, between frames.
		bool addRenderPass(std::string name, std::unique_ptr<Render::RenderPass> pass, std::string_view after = {});
		// Removes and destroys the named pass; false when there is none. Main thread, between frames.
		bool removeRenderPass(std::string_view name);
		// The picture drawn at a new size (ViewportData::renderW, renderH): once the device is idle, every pass makes its
		// targets of the screen's size again (RenderPass::resize). Main thread, between frames.
		void resize();
		std::vector<std::string> renderPassNames() const { return mPassNames; }
		// The named pass, or null. Main thread.
		Render::RenderPass* renderPass(std::string_view name) const;
		// Every pass in draw order, for whoever needs to ask them all something.
		const std::vector<std::unique_ptr<Render::RenderPass>>& renderPasses() const { return mRenderPasses; }

		// Readers skip when a chunk mutate is in flight. Mutate sets the flag first, then
		// waits for in-flight readers, so a waiting mutate never blocks readers already running.
		class WorldReadGuard {
		public:
			WorldReadGuard() = default;
			~WorldReadGuard() { reset(); }
			WorldReadGuard(WorldReadGuard&& other) noexcept : mSys(other.mSys) { other.mSys = nullptr; }
			WorldReadGuard& operator=(WorldReadGuard&& other) noexcept {
				if (this != &other) {
					reset();
					mSys = other.mSys;
					other.mSys = nullptr;
				}
				return *this;
			}
			WorldReadGuard(const WorldReadGuard&) = delete;
			WorldReadGuard& operator=(const WorldReadGuard&) = delete;
			explicit operator bool() const noexcept { return mSys != nullptr; }
		private:
			friend class RenderSystem;
			explicit WorldReadGuard(RenderSystem* sys) : mSys(sys) {}
			void reset() {
				if (mSys) {
					mSys->mWorldReaders.fetch_sub(1, std::memory_order_acq_rel);
					mSys = nullptr;
				}
			}
			RenderSystem* mSys = nullptr;
		};

		class WorldMutateGuard {
		public:
			WorldMutateGuard() = default;
			~WorldMutateGuard() { reset(); }
			WorldMutateGuard(WorldMutateGuard&& other) noexcept : mSys(other.mSys) { other.mSys = nullptr; }
			WorldMutateGuard& operator=(WorldMutateGuard&& other) noexcept {
				if (this != &other) {
					reset();
					mSys = other.mSys;
					other.mSys = nullptr;
				}
				return *this;
			}
			WorldMutateGuard(const WorldMutateGuard&) = delete;
			WorldMutateGuard& operator=(const WorldMutateGuard&) = delete;
			explicit operator bool() const noexcept { return mSys != nullptr; }
		private:
			friend class RenderSystem;
			explicit WorldMutateGuard(RenderSystem* sys) : mSys(sys) {}
			void reset() {
				if (mSys) {
					mSys->mWorldMutating.store(false, std::memory_order_release);
					mSys = nullptr;
				}
			}
			RenderSystem* mSys = nullptr;
		};

		WorldReadGuard tryLockWorldRead() {
			if (mWorldMutating.load(std::memory_order_acquire)) {
				return {};
			}
			mWorldReaders.fetch_add(1, std::memory_order_acq_rel);
			if (mWorldMutating.load(std::memory_order_acquire)) {
				mWorldReaders.fetch_sub(1, std::memory_order_acq_rel);
				return {};
			}
			return WorldReadGuard{ this };
		}

		WorldMutateGuard lockWorldMutate() {
			mWorldMutating.store(true, std::memory_order_release);
			while (mWorldReaders.load(std::memory_order_acquire) != 0) {
				std::this_thread::yield();
			}
			return WorldMutateGuard{ this };
		}

		bool isShadowsDebugData() const {
			return mShadowsDebugDataDraw;
		}

		void notify(Task task) override;

		bool mShadowsDebugDataDraw = false;
	private:

		void renderFrame();
		bool shouldRunPass(size_t passIndex) const;
		void ensureGpuCapacity(ecss::EntityId id);
		void bindGpuFrame();
		// The area tint the passes' prepare left on the blackboard, into this frame's camera block and frame set.
		void updateAreaTint();
		void updateGroundMarks();
		void updateMudMaps();
		void createGpuFrameResources(uint32_t slot);
		void createSceneBuffers(size_t capacity);
		// `previousToo` also as last frame's, for an entity that has no last frame: no motion on its first one.
		void uploadTransforms(const std::vector<ecss::EntityId>& sortedIds, bool previousToo = false);
		void uploadBones(const std::vector<ecss::EntityId>& sortedIds);
		void markGpuDirty(std::vector<ecss::EntityId>& list, ecss::EntityId id);

		std::atomic<int> mWorldReaders{ 0 };
		std::atomic<bool> mWorldMutating{ false };

		// GPU/CPU profiling data
		int mCurrentGpuQuery = 0;
		double mLastGpuTimeMs = 0.0; // result from previous frame
		std::chrono::high_resolution_clock::time_point mCpuFrameStart; // per RenderSystem update
		double mLastCpuFrameMs = 0.0; // duration of last RenderSystem::update body

		RenderData mRenderData;
		int mRenderedFrames = 0; // drives the one-shot per-pass dump
		std::vector<std::unique_ptr<Render::RenderPass>> mRenderPasses;
		std::vector<std::string> mPassNames; // one per pass, same order
		std::vector<const char*> mPassZones; // the same names kept for good, the profiler's (Debug::Profiler::intern)
		std::shared_future<void> updateLock;

		Render::BufferHandle cameraMatricesUBO;

		// One per frame the RHI keeps in flight.
		uint32_t mGpuFrames = 1;
		std::vector<Render::BufferHandle> mCameraUbo;
		std::vector<Render::DescriptorSetHandle> mFrameSet;
		std::unordered_map<uint32_t, uint32_t> mFrameSetTint; // frame set id -> the tint map written to it
		std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> mFrameSetMarks; // frame set id -> the marks map and heights written to it
		std::unordered_map<uint32_t, uint32_t> mFrameSetMud; // frame set id -> the mud maps written to it

		// Scene data lives on the GPU in one buffer each, indexed by entity id. Only entities that changed
		// since the last frame are uploaded, through the backend's queued upload path.
		std::mutex mGpuDirtyMutex;
		std::vector<ecss::EntityId> mGpuDirtyTransforms;
		std::vector<ecss::EntityId> mGpuDirtyBones;
		std::vector<ecss::EntityId> mDirtyAnimations;
		Render::GpuMaterialTable mMaterials;
		Render::GpuScene mScene;
		bool mRefreshTransforms = true;

		Render::BufferHandle transforms;
		// Last frame's matrix per entity: a copy of `transforms` taken at the end of every frame, so an entity
		// that did not move has the same matrix in both and one that did has where it was.
		Render::BufferHandle prevTransforms;
		size_t mGpuCapacity = 0;

		// Bone matrices pooled by armature, not reserved per entity id: a skinned entity takes exactly its bones.
		void growBones(size_t capacity);
		void releaseBones(ecss::EntityId entity);
		Render::BufferHandle bones;
		// Written by AnimationComputePass, which copies a bone's matrix here before overwriting it.
		Render::BufferHandle prevBones;
		// The bones each hand-posed skeleton was uploaded with last frame, so the motion vectors of one that is
		// written afresh every frame say where its skin actually came from.
		std::unordered_map<ecss::EntityId, std::vector<Math::Mat4>> mLastBones;
		std::unordered_map<ecss::EntityId, BoneRange> mBoneRanges;
		std::vector<BoneRange> mFreeBoneRanges; // sorted by offset, neighbours merged
		size_t mBonesUsed = 0;
		size_t mBoneCapacity = 0;
		std::vector<ecss::EntityId> mDestroyedBones; // under mGpuDirtyMutex

		Render::DescriptorSetLayoutHandle frameSetLayout;
		Render::DescriptorSetHandle frameSet;
	};
}
