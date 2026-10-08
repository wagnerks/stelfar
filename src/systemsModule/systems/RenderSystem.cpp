#include "RenderSystem.h"
#include "renderModule/Utils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include "renderModule/Atmosphere.h"
#include "renderModule/FrameCapture.h"

#include "CameraSystem.h"
#include "OcTreeSystem.h"
#include "systemsModule/SystemManager.h"
#include "core/GameModule.h"
#include "systemsModule/SystemsPriority.h"
#include "componentsModule/ArmatureComponent.h"
#include "componentsModule/CameraComponent.h"
#include "componentsModule/IsDrawableComponent.h"
#include "componentsModule/LightSourceComponent.h"
#include "componentsModule/ModelComponent.h"
#include "componentsModule/OutlineComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "debugModule/Profiler.h"
#include "ecss/Registry.h"
#include "renderModule/Utils.h"
#include "renderModule/renderPasses/CascadedShadowPass.h"
#include "renderModule/renderPasses/LightingPass.h"
#include "renderModule/renderPasses/SSAOPass.h"
#include "renderModule/renderPasses/DebugPass.h"
#include "renderModule/renderPasses/PointLightPass.h"
#include "runtimeBridge/RuntimeBridge.h"

#include "componentsModule/TransformComponent.h"
#include "renderModule/TextRenderer.h"
#include "renderModule/renderPasses/GUIPass.h"
#include <debugModule/SystemLoadHelper.h>

#include "componentsModule/MaterialComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/ArmatureComponent.h"
#include "renderModule/renderPasses/GeometryPass.h"
#include "renderModule/renderPasses/AnimationComputePass.h"
#include "containersModule/Vector.h"

namespace SFE::SystemsModule {

	namespace {
		// Writes one PNG per pass output plus a crc index, so a comparison script can find the
		// FIRST pass where two backends diverge instead of guessing from the final image.
		class PassDumpWriter {
		public:
			explicit PassDumpWriter(std::string dir) : mDir(std::move(dir)) {
				if (mDir.empty()) {
					return;
				}
				std::error_code ec;
				std::filesystem::create_directories(mDir, ec);
				mIndex.open(std::filesystem::path(mDir) / "passes.csv", std::ios::trunc);
				if (mIndex) {
					mIndex << "index,pass,slot,width,height,crc\n";
				}
			}

			// Stage 0: what every slot holds before any pass has run. Written, so a comparison always has a
			// baseline for a slot that one backend changes in a pass and the other leaves as it was.
			void seed(const Render::FrameBlackboard& frame) {
				if (mDir.empty()) {
					return;
				}
				dumpAll(0, "seed", frame);
			}

			void write(size_t passIndex, const char* passName, const Render::FrameBlackboard& frame) {
				if (mDir.empty()) {
					return;
				}
				dumpAll(passIndex + 1, passName, frame);
			}

		private:
			// Every texture on the blackboard, so each intermediate stage is compared, not a hand-picked
			// few. dump() skips a slot whose content has not changed, so a pass only reports what it wrote.
			void dumpAll(size_t passIndex, const char* passName, const Render::FrameBlackboard& frame) {
				for (const auto& name : frame.textureNames()) {
					const auto tex = frame.texture(name.c_str());
					if (name == Render::FrameId::ShadowCsmDepth) {
						// A depth array: the RHI cannot report its layer count, and the engine uses four cascades.
						for (uint32_t cascade = 0; cascade < 4; ++cascade) {
							dump(passIndex, passName, name + ".cascade" + std::to_string(cascade), tex, cascade);
						}
						continue;
					}
					dump(passIndex, passName, name, tex);
				}
			}

			void dump(size_t passIndex, const char* passName, const std::string& slot, Render::TextureHandle tex, uint32_t layer = 0) {
				auto* rhi = Engine::instance()->rhi();
				if (!tex || !rhi) {
					return;
				}
				std::vector<uint8_t> rgba;
				int width = 0;
				int height = 0;
				if (!rhi->readbackTexture(tex, rgba, width, height, layer)) {
					return;
				}

				// A slot a pass did not change is not written again; the comparison carries the last dump forward.
				const uint32_t crc = Render::crcOfPixels(rgba);
				auto& last = mLastCrc[slot];
				if (last == crc) {
					return;
				}
				last = crc;

				char name[160];
				std::snprintf(name, sizeof(name), "%02d_%s_%s.png", static_cast<int>(passIndex), passName, slot.c_str());
				const auto path = (std::filesystem::path(mDir) / name).string();
				if (Render::writePng(path, rgba, width, height) && mIndex) {
					mIndex << passIndex << ',' << passName << ',' << slot << ','
						<< width << ',' << height << ',' << crc << '\n';
					mIndex.flush();
				}
			}

			std::string mDir;
			std::ofstream mIndex;
			std::unordered_map<std::string, uint32_t> mLastCrc;
		};
	}

	RenderSystem::RenderSystem() : System({ SFE::SystemsModule::TaskType::TRAHSFORM_RELOADED , SFE::SystemsModule::TaskType::ARMATURE_UPDATED, ANIMATION_UPDATED, MATERIAL_UPDATED, MESH_UPDATED, ENTITY_DESTROYED }) {
		auto* rhi = Engine::instance()->rhi();
		if (!Engine::instance()->temporalAccumulation()) {
			mRenderData.graphics.taa = false;
		}
		mGpuFrames = (std::max)(1u, rhi->framesInFlight());
		mCameraUbo.resize(mGpuFrames);
		mFrameSet.resize(mGpuFrames);
		constexpr size_t maxDrawSize = 4096;
		mGpuCapacity = maxDrawSize;

		{
			Render::DescriptorSetLayoutDesc frameLayout;
			frameLayout.bindings = {
				{ 5, Render::DescriptorType::UniformBuffer, Render::ShaderStage::Vertex | Render::ShaderStage::Fragment | Render::ShaderStage::Geometry },
				{ 1, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex },
				{ 2, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex | Render::ShaderStage::Compute },
				{ 3, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex | Render::ShaderStage::Fragment },
				{ 8, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex },
				{ 9, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex | Render::ShaderStage::Compute },
				// Vertex too: the G-buffer vertex shader reads how a material sways in the wind.
				{ 7, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex | Render::ShaderStage::Fragment },
				// The area tint map (AreaTintFrame): the terrain's bank goes by it.
				{ 22, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment },
			};
			frameSetLayout = rhi->createDescriptorSetLayout(frameLayout);
			mRenderData.frameSetLayout = frameSetLayout;
		}

		createSceneBuffers(maxDrawSize);
		growBones(4096);
		mScene.init();
		for (uint32_t slot = 0; slot < mGpuFrames; ++slot) {
			createGpuFrameResources(slot);
		}
		bindGpuFrame();

		SFE::Engine::reproTrace("RenderSystem passes");
		const auto& passes = GameModule::passes();
		mRenderPasses.reserve(passes.size());
		for (const auto& entry : passes) {
			if (!entry.enabled || !entry.create) {
				continue;
			}
			SFE::Engine::reproTrace(entry.name);
			addRenderPass(entry.name, std::unique_ptr<Render::RenderPass>(entry.create()));
		}
		SFE::Engine::reproTrace("RenderSystem passes ok");

		// ChunksSystem seeds before this ctor; ARMATURE_UPDATED never reached us. Transforms are refreshed on
		// the first frame, and GpuScene starts dirty.
		auto& live = ECSHandler::registry();
		for (auto id : live.getAllEntities()) {
			if (live.hasComponent<ComponentsModule::ArmatureBonesComponent>(id)) {
				markGpuDirty(mGpuDirtyBones, id);
			}
			if (live.hasComponent<ComponentsModule::AnimationComponent>(id)) {
				markGpuDirty(mDirtyAnimations, id);
			}
		}
	}

	namespace {
		constexpr int kTaaJitterPhases = 8;

		// Radical inverse of `index` in `base`, in [0, 1).
		float halton(int index, int base) {
			float f = 1.f;
			float r = 0.f;
			while (index > 0) {
				f /= static_cast<float>(base);
				r += f * static_cast<float>(index % base);
				index /= base;
			}
			return r;
		}
	}

	void RenderSystem::update(float_t dt) {
		FUNCTION_BENCHMARK;

		auto* rhi = Engine::instance()->rhi();
		// Systems that ran earlier this frame may have queued asynchronous work (AABB, octree) that the
		// culling below reads; a capture lets it finish so what is drawn does not depend on scheduling.
		if (Engine::instance()->capturingFrame()) {
			ThreadPool::instance()->waitIdle();
		}
		// The frame slot was waited for in Engine::update, before the frame's time was taken.
		bindGpuFrame();
		auto frameLock = tryLockWorldRead();

		// This frame's camera, read before it becomes current: taken after, it was the previous frame's camera drawing
		// this frame's objects, and anything the camera follows shook by however long the last frame took.
		{
			auto playerCamera = ECSHandler::getSystem<CameraSystem>()->getCurrentCamera();
			const auto cameraComp = ECSHandler::registry().pinComponent<const CameraComponent>(playerCamera);
			// The copy the transform worker publishes, not TransformComponent itself: the worker rewrites that
			// matrix while this thread reads it, and a moving camera now and then got a half-written view.
			// Replacing the copy waits for pins, so a pinned copy is whole.
			Math::Mat4 cameraTransform{ 1.f };
			if (const auto copy = ECSHandler::registry().pinComponent<const ComponentsModule::TransformMatComp>(playerCamera)) {
				cameraTransform = copy->mTransform;
			}
			else if (const auto transformComp = ECSHandler::registry().pinComponent<const TransformComponent>(playerCamera)) {
				cameraTransform = transformComp->copyTransform(); // before the first reload reached RenderSystem
			}

			mRenderData.nextCameraProjection = cameraComp->getProjection();
			const float viewDistance = (std::max)(mRenderData.graphics.viewDistance, mRenderData.nextCameraProjection.getNear() + 1.f);
			mRenderData.nextCameraProjection.setFar((std::min)(mRenderData.nextCameraProjection.getFar(), viewDistance));
			mRenderData.next.projection = mRenderData.nextCameraProjection.getProjectionsMatrix();
			mRenderData.next.view = inverse(cameraTransform);
			mRenderData.next.PV = mRenderData.next.projection * mRenderData.next.view;

			mRenderData.mNextCameraPos = cameraTransform[3].xyz;
			const Math::Vec3 forward = -cameraTransform[2];
			mRenderData.mNextViewDir = normalize(forward);
			mRenderData.mNextCamFrustum = FrustumModule::createFrustum(mRenderData.next.PV);
		}

		mRenderData.prevPV = mRenderData.current.PV;
		const auto windBefore = mRenderData.current.wind;
		mRenderData.current = mRenderData.next;
		{
			// Wrapped so the float keeps its precision in a long session; the sway jumps once an hour.
			const float seconds = static_cast<float>(std::fmod(Engine::instance()->animationClock(), 3600.0));
			const auto wind = Render::Atmosphere::sceneWind();
			const auto along = wind ? wind->blowing() : Math::Vec2{ 1.f, 0.f };
			mRenderData.current.wind = { along.x, wind ? wind->strength : 0.f, along.y, seconds };
			mRenderData.current.windBefore = windBefore.w != 0.f ? windBefore : mRenderData.current.wind;
		}
		mRenderData.cameraProjection = mRenderData.nextCameraProjection;

		mRenderData.mCameraPos = mRenderData.mNextCameraPos;
		mRenderData.mViewDir = mRenderData.mNextViewDir;
		mRenderData.mCamFrustum = mRenderData.mNextCamFrustum;

		++mRenderData.taaFrame;
		mRenderData.taaJitterX = 0.f;
		mRenderData.taaJitterY = 0.f;

		// TAA resolves nothing without a sub-pixel offset per frame: every frame would sample the
		// same points and the history would already equal the current image. Only the matrices the
		// GPU rasterizes with are jittered -- mRenderData.current stays clean, so prevPV, culling
		// and the post-TAA debug overlay keep working in unjittered screen space.
		RenderMatrices gpuMatrices = mRenderData.current;
		// The motion vectors compare these two, so neither carries the jitter the rasterizer uses.
		gpuMatrices.PVNoJitter = mRenderData.current.PV;
		gpuMatrices.prevPVNoJitter = mRenderData.prevPV;
		if (mRenderData.graphics.taa) {
			const auto& screen = Engine::instance()->getWindow()->getScreenData();
			if (screen.renderW > 0 && screen.renderH > 0) {
				const int phase = mRenderData.taaFrame % kTaaJitterPhases + 1;
				mRenderData.taaJitterX = halton(phase, 2) - 0.5f;
				mRenderData.taaJitterY = halton(phase, 3) - 0.5f;

				// perspectiveRH_NO gives w = -z_view, so a term on the z column survives the
				// perspective divide as a constant NDC shift, unlike one on the translation column.
				gpuMatrices.projection[2][0] -= 2.f * mRenderData.taaJitterX / static_cast<float>(screen.renderW);
				gpuMatrices.projection[2][1] -= 2.f * mRenderData.taaJitterY / static_cast<float>(screen.renderH);
				gpuMatrices.PV = gpuMatrices.projection * gpuMatrices.view;
			}
		}

		gpuMatrices.invProjection = Math::inverse(gpuMatrices.projection);
		gpuMatrices.invView = Math::inverse(gpuMatrices.view);
		gpuMatrices.style = { mRenderData.graphics.stylise, 0.f, 0.f, 0.f };
		Engine::instance()->rhi()->updateBuffer(cameraMatricesUBO, 0, sizeof(RenderMatrices), &gpuMatrices);


		if (frameLock) {
			if (mRefreshTransforms) {
				// A reload that finished before we subscribed -- or while the constructor ran -- never reached
				// notify, so the copies are taken from the source once, now that every later reload does.
				mRefreshTransforms = false;
				auto& live = ECSHandler::registry();
				for (auto [id, matComp, transform] : live.view<ComponentsModule::TransformMatComp, const TransformComponent>()) {
					if (matComp && transform) {
						matComp->mTransform = transform->copyTransform();
						markGpuDirty(mGpuDirtyTransforms, id);
					}
				}
			}
			{
				SFE_PROFILE_ZONE("Render dirty upload");
				uploadDirtyGpuData();
			}
			{
				SFE_PROFILE_ZONE("Render scene");
				mScene.update(mMaterials);
			}
			SFE_PROFILE_ZONE("Render added");
			// An entity can join the scene after this frame's dirty transforms were taken, its id past the
			// buffer; the grow uploads every live transform, its own included.
			ensureGpuCapacity(mScene.maxEntity());
			// The dirty transforms were taken before the scene read its pending entities, and a transform the worker
			// published in between would reach the GPU a frame after its instances. Most of these went up already;
			// sending them again is cheap and closes that gap.
			uploadTransforms(mScene.added(), true);
			// Before the frame is recorded, so new materials go up with the frame's queued uploads; the passes'
			// own upload() calls then find nothing to send. Sent after the list began, Vulkan would submit and
			// wait for each one.
			SFE_PROFILE_ZONE("Render materials");
			mMaterials.upload();
		}

		renderFrame();
	}

	void RenderSystem::debugUpdate(float dt) {
	}

	void RenderSystem::bindGpuFrame() {
		if (mGpuFrames == 0) {
			return;
		}
		auto* rhi = Engine::instance()->rhi();
		const uint32_t slot = rhi->frameInFlight() % mGpuFrames;
		cameraMatricesUBO = mCameraUbo[slot];
		frameSet = mFrameSet[slot];
		mRenderData.transformsSSBO = transforms;
		mRenderData.bonesSSBO = bones;
		mRenderData.prevTransformsSSBO = prevTransforms;
		mRenderData.prevBonesSSBO = prevBones;
		mRenderData.cameraMatricesUBO = cameraMatricesUBO;
	}

	void RenderSystem::updateAreaTint() {
		auto* rhi = Engine::instance()->rhi();
		const auto* tint = mRenderData.frame.data<Render::AreaTintFrame>(Render::FrameId::AreaTint);
		const Math::Vec4 place[] = { tint ? tint->area : Math::Vec4{}, tint ? tint->shore : Math::Vec4{} };
		rhi->updateBuffer(cameraMatricesUBO, offsetof(RenderMatrices, tintArea), sizeof(place), place);
		// Rewritten only when the game makes a new map, which it does as the litter's bounds change.
		const auto map = Render::Utils::areaTintMap(mRenderData.frame);
		const auto [written, fresh] = mFrameSetTint.try_emplace(frameSet.id, map.first.id);
		if (fresh || written->second != map.first.id) {
			written->second = map.first.id;
			rhi->updateDescriptorSets({ { .set = frameSet, .binding = 22, .images{ { map.first, map.second } } } });
		}
	}

	void RenderSystem::createGpuFrameResources(const uint32_t slot) {
		auto* rhi = Engine::instance()->rhi();
		Render::BufferDesc uboDesc;
		uboDesc.size = sizeof(RenderMatrices);
		uboDesc.usage = Render::BufferUsage::Uniform;
		uboDesc.cpuAccess = Render::CpuAccess::Write;
		uboDesc.debugName = "CameraMatricesUBO";
		mCameraUbo[slot] = rhi->createBuffer(uboDesc);

		mFrameSet[slot] = rhi->allocateDescriptorSet(frameSetLayout);
		rhi->updateDescriptorSets({
			{ mFrameSet[slot], 5, {{ mCameraUbo[slot] }} },
			{ mFrameSet[slot], 1, {{ transforms }} },
			{ mFrameSet[slot], 2, {{ bones }} },
			{ mFrameSet[slot], 8, {{ prevTransforms }} },
			{ mFrameSet[slot], 9, {{ prevBones }} },
		});
	}

	void RenderSystem::createSceneBuffers(const size_t capacity) {
		auto* rhi = Engine::instance()->rhi();
		// CpuAccess::None: device-local, written only through updateBuffer, which on Vulkan is copied
		// in the frame's own command list instead of racing the frames still in flight.
		Render::BufferDesc transformDesc;
		transformDesc.size = sizeof(Math::Mat4) * capacity;
		transformDesc.usage = Render::BufferUsage::Storage;
		transformDesc.cpuAccess = Render::CpuAccess::None;
		transformDesc.debugName = "TransformsSSBO";
		transforms = rhi->createBuffer(transformDesc);
		transformDesc.debugName = "PrevTransformsSSBO";
		prevTransforms = rhi->createBuffer(transformDesc);

		mGpuCapacity = capacity;
	}

	void RenderSystem::ensureGpuCapacity(ecss::EntityId id) {
		const size_t needed = static_cast<size_t>(id) + 1;
		if (needed <= mGpuCapacity) {
			return;
		}

		size_t newCap = mGpuCapacity < 4096 ? 4096 : mGpuCapacity;
		while (newCap < needed) {
			newCap *= 2;
		}

		auto* rhi = Engine::instance()->rhi();
		// The RHI keeps a destroyed buffer until the frames that may still bind it have finished.
		rhi->destroyBuffer(transforms);
		rhi->destroyBuffer(prevTransforms);
		createSceneBuffers(newCap);
		for (uint32_t slot = 0; slot < mGpuFrames; ++slot) {
			rhi->updateDescriptorSets({
				{ mFrameSet[slot], 1, {{ transforms }} },
				{ mFrameSet[slot], 8, {{ prevTransforms }} },
			});
		}
		bindGpuFrame();

		// The new buffers start empty: everything live goes up again.
		std::vector<ecss::EntityId> ids;
		for (auto [entId, comp] : ECSHandler::registry().view<const ComponentsModule::TransformMatComp>()) {
			ids.push_back(entId);
		}
		std::sort(ids.begin(), ids.end());
		uploadTransforms(ids);
	}

	RenderSystem::BoneRange RenderSystem::boneRange(const ecss::EntityId entity) {
		// An id destroyed and taken again before the release was processed: the range held is the old entity's,
		// and releasing it later would free it under the new one.
		bool destroyed = false;
		{
			std::lock_guard lock(mGpuDirtyMutex);
			if (const auto it = std::find(mDestroyedBones.begin(), mDestroyedBones.end(), entity); it != mDestroyedBones.end()) {
				mDestroyedBones.erase(it);
				destroyed = true;
			}
		}
		if (destroyed) {
			releaseBones(entity);
		}
		if (const auto it = mBoneRanges.find(entity); it != mBoneRanges.end()) {
			return it->second;
		}
		auto& registry = ECSHandler::registry();
		uint32_t count = 0;
		// Without the armature its meshes' bounds slots are unknown, so it gets them all.
		uint32_t bounds = AssetsModule::Armature::kMaxPoseBounds;
		if (const auto armature = registry.pinComponent<const ComponentsModule::ArmatureComponent>(entity)) {
			count = static_cast<uint32_t>(armature->armature.bones.size());
			bounds = armature->armature.poseBoundsCount;
		}
		if (count == 0) {
			if (const auto pose = registry.pinComponent<const ComponentsModule::ArmatureBonesComponent>(entity)) {
				count = static_cast<uint32_t>(pose->boneMatrices.size());
			}
		}
		if (count == 0) {
			return {};
		}

		// Free ranges count raw slots: the bounds and the bones together.
		const uint32_t slots = bounds + count;
		BoneRange range{ 0, count, bounds };
		const auto fit = std::find_if(mFreeBoneRanges.begin(), mFreeBoneRanges.end(), [slots](const BoneRange& r) { return r.count >= slots; });
		if (fit != mFreeBoneRanges.end()) {
			range.offset = fit->offset + bounds;
			fit->offset += slots;
			fit->count -= slots;
			if (fit->count == 0) {
				mFreeBoneRanges.erase(fit);
			}
		}
		else {
			range.offset = static_cast<uint32_t>(mBonesUsed) + bounds;
			mBonesUsed += slots;
			if (mBonesUsed > mBoneCapacity) {
				size_t capacity = mBoneCapacity;
				while (capacity < mBonesUsed) {
					capacity *= 2;
				}
				mBoneRanges.emplace(entity, range);
				growBones(capacity); // uploads every range, this one included
				return range;
			}
		}
		mBoneRanges.emplace(entity, range);
		// Its pose goes up with the next upload, until an animation writes it on the GPU.
		markGpuDirty(mGpuDirtyBones, entity);
		return range;
	}

	void RenderSystem::releaseBones(const ecss::EntityId entity) {
		mLastBones.erase(entity);
		const auto it = mBoneRanges.find(entity);
		if (it == mBoneRanges.end()) {
			return;
		}
		const BoneRange range{ it->second.offset - it->second.bounds, it->second.count + it->second.bounds };
		mBoneRanges.erase(it);
		auto next = std::lower_bound(mFreeBoneRanges.begin(), mFreeBoneRanges.end(), range.offset,
			[](const BoneRange& r, uint32_t offset) { return r.offset < offset; });
		next = mFreeBoneRanges.insert(next, range);
		if (next + 1 != mFreeBoneRanges.end() && next->offset + next->count == (next + 1)->offset) {
			next->count += (next + 1)->count;
			mFreeBoneRanges.erase(next + 1);
		}
		if (next != mFreeBoneRanges.begin() && (next - 1)->offset + (next - 1)->count == next->offset) {
			(next - 1)->count += next->count;
			mFreeBoneRanges.erase(next);
		}
	}

	void RenderSystem::growBones(const size_t capacity) {
		auto* rhi = Engine::instance()->rhi();
		// The RHI keeps a destroyed buffer until the frames that may still bind it have finished.
		rhi->destroyBuffer(bones);
		rhi->destroyBuffer(prevBones);
		Render::BufferDesc boneDesc;
		boneDesc.size = sizeof(Math::Mat4) * capacity;
		boneDesc.usage = Render::BufferUsage::Storage;
		boneDesc.cpuAccess = Render::CpuAccess::None;
		boneDesc.debugName = "BonesSSBO";
		bones = rhi->createBuffer(boneDesc);
		boneDesc.debugName = "PrevBonesSSBO";
		prevBones = rhi->createBuffer(boneDesc);
		mBoneCapacity = capacity;
		for (uint32_t slot = 0; slot < mGpuFrames; ++slot) {
			if (mFrameSet[slot]) {
				rhi->updateDescriptorSets({
					{ mFrameSet[slot], 2, {{ bones }} },
					{ mFrameSet[slot], 9, {{ prevBones }} },
				});
			}
		}
		bindGpuFrame();

		// Offsets stay where they were; the new buffer starts empty, so every pose goes up again.
		std::vector<ecss::EntityId> ids;
		ids.reserve(mBoneRanges.size());
		for (const auto& [entity, range] : mBoneRanges) {
			ids.push_back(entity);
		}
		uploadBones(ids);
	}

	void RenderSystem::markGpuDirty(std::vector<ecss::EntityId>& list, const ecss::EntityId id) {
		std::lock_guard lock(mGpuDirtyMutex);
		list.push_back(id);
	}

	void RenderSystem::uploadTransforms(const std::vector<ecss::EntityId>& sortedIds, const bool previousToo) {
		auto* rhi = Engine::instance()->rhi();
		auto& registry = ECSHandler::registry();
		// Consecutive ids go up as one range, so a spawned chunk is one copy rather than one per entity.
		std::vector<Math::Mat4> run;
		ecss::EntityId runStart = 0;
		const auto flush = [&] {
			if (!run.empty()) {
				rhi->updateBuffer(transforms, static_cast<size_t>(runStart) * sizeof(Math::Mat4), run.size() * sizeof(Math::Mat4), run.data());
				if (previousToo) {
					rhi->updateBuffer(prevTransforms, static_cast<size_t>(runStart) * sizeof(Math::Mat4), run.size() * sizeof(Math::Mat4), run.data());
				}
				run.clear();
			}
		};
		for (const auto id : sortedIds) {
			if (static_cast<size_t>(id) >= mGpuCapacity) {
				break;
			}
			const auto comp = registry.pinComponent<const ComponentsModule::TransformMatComp>(id);
			if (!comp) {
				flush();
				continue;
			}
			if (!run.empty() && id != runStart + run.size()) {
				flush();
			}
			if (run.empty()) {
				runStart = id;
			}
			run.push_back(comp->mTransform);
		}
		flush();
	}

	namespace {
		// The pose boxes of a skeleton posed by hand (`skins`, the bone matrices as the component has them), grown as
		// animateBones.cs grows a played one's: every posed joint by the farthest its bone's vertices lie from it, into the
		// boxes of the meshes it moves -- `slots`' first `bounds`, the last box first. Left empty, the cull would keep to
		// the bind pose, and a part posed far from it, a tool in the hands, would be culled where it is drawn.
		void growPoseBounds(ecss::EntityId entity, const std::vector<Math::Mat4>& skins, size_t count, uint32_t bounds, std::vector<Math::Mat4>& slots) {
			const auto skeleton = ECSHandler::registry().pinComponent<const ComponentsModule::ArmatureComponent>(entity);
			if (!skeleton || bounds == 0) {
				return;
			}
			const auto& rig = skeleton->armature;
			for (uint32_t j = 0; j < bounds; ++j) {
				slots[j][0] = Math::Vec4(Math::Vec3(3.0e38f), 1.f);
				slots[j][1] = Math::Vec4(Math::Vec3(-3.0e38f), 1.f);
			}
			for (const auto& bone : rig.bones) {
				if (bone.id >= count || bone.id >= rig.boneBoundsMask.size() || rig.boneBoundsMask[bone.id] == 0u) {
					continue;
				}
				const auto posed = skins[bone.id] * Math::inverse(bone.offset);
				const auto length = [&](int axis) { return Math::length(Math::Vec3(posed[axis])); };
				const Math::Vec3 reach(rig.boneRadius[bone.id] * (std::max)({ length(0), length(1), length(2) }));
				const Math::Vec3 at(posed[3]);
				for (uint32_t j = 0; j < bounds; ++j) {
					if ((rig.boneBoundsMask[bone.id] & (1u << j)) != 0u) {
						auto& box = slots[bounds - 1 - j];
						for (int k = 0; k < 3; ++k) {
							box[0][k] = (std::min)(box[0][k], at[k] - reach[k]);
							box[1][k] = (std::max)(box[1][k], at[k] + reach[k]);
						}
					}
				}
			}
		}
	}

	void RenderSystem::uploadBones(const std::vector<ecss::EntityId>& sortedIds) {
		auto* rhi = Engine::instance()->rhi();
		auto& registry = ECSHandler::registry();
		for (const auto id : sortedIds) {
			const auto armature = registry.pinComponent<const ComponentsModule::ArmatureBonesComponent>(id);
			if (!armature || armature->boneMatrices.empty()) {
				continue;
			}
			const auto range = boneRange(id);
			const size_t boneCount = (std::min)(armature->boneMatrices.size(), static_cast<size_t>(range.count));
			if (boneCount == 0) {
				continue;
			}
			// Empty bounds, so the cull falls back to the bind pose until an animation writes them. An animation,
			// playing or stopped, writes the bones straight into the buffer in AnimationComputePass, after this upload.
			const bool animated = [&] {
				const auto anim = registry.pinComponent<const ComponentsModule::AnimationComponent>(id);
				// Posed by hand, the clip is not what is drawn: the bones written into the component are.
				return anim && anim->mCurrentAnimation && !anim->mPosedByHand;
			}();
			std::vector<Math::Mat4> slots(range.bounds + (animated ? 0 : boneCount));
			for (uint32_t i = 0; i < range.bounds; ++i) {
				slots[i][0] = Math::Vec4(1.f);
				slots[i][1] = Math::Vec4(-1.f);
			}
			if (!animated) {
				std::copy_n(armature->boneMatrices.begin(), boneCount, slots.begin() + range.bounds);
				growPoseBounds(id, armature->boneMatrices, boneCount, range.bounds, slots);
			}
			if (!slots.empty()) {
				const auto offset = static_cast<size_t>(range.offset - range.bounds) * sizeof(Math::Mat4);
				rhi->updateBuffer(bones, offset, slots.size() * sizeof(Math::Mat4), slots.data());
				// What the bones were last frame, for the motion vectors. A pose written here and left alone does not
				// move, so last frame's is this one -- without that, the first frames of a static pose would have
				// motion vectors pointing at whatever the buffer held. A pose written afresh every frame does move,
				// though, and handing it its own bones as the last frame's tells the temporal pass the skin stood
				// still while it swept across the screen, which smears it. So that one is remembered.
				auto& was = mLastBones[id];
				const bool moving = !animated && was.size() == slots.size();
				rhi->updateBuffer(prevBones, offset, slots.size() * sizeof(Math::Mat4),
					moving ? was.data() : slots.data());
				if (animated) {
					mLastBones.erase(id);   // the device writes this one's bones; nothing to remember here
				}
				else {
					was = slots;
				}
			}
		}
	}

	void RenderSystem::uploadDirtyGpuData() {
		std::vector<ecss::EntityId> dirtyTransforms;
		std::vector<ecss::EntityId> dirtyBones;
		std::vector<ecss::EntityId> destroyed;
		{
			std::lock_guard lock(mGpuDirtyMutex);
			dirtyTransforms.swap(mGpuDirtyTransforms);
			dirtyBones.swap(mGpuDirtyBones);
			destroyed.swap(mDestroyedBones);
		}
		// Before the uploads: a reused id takes a range of its own when it is next asked for.
		for (const auto entity : destroyed) {
			releaseBones(entity);
		}
		if (dirtyTransforms.empty() && dirtyBones.empty()) {
			return;
		}
		for (auto* ids : { &dirtyTransforms, &dirtyBones }) {
			std::sort(ids->begin(), ids->end());
			ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
		}

		const size_t capacityBefore = mGpuCapacity;
		ecss::EntityId maxId = 0;
		if (!dirtyTransforms.empty()) {
			maxId = dirtyTransforms.back();
		}
		ensureGpuCapacity(maxId);
		if (mGpuCapacity == capacityBefore) {
			uploadTransforms(dirtyTransforms); // otherwise the grow already uploaded everything live
		}
		uploadBones(dirtyBones);
	}

	void RenderSystem::notify(Task task) {
		if (task.type == TRAHSFORM_RELOADED) {
			auto& registry = ECSHandler::registry();
			const bool first = !registry.hasComponent<ComponentsModule::TransformMatComp>(task.entity);
			registry.addComponent<ComponentsModule::TransformMatComp>(task.entity, static_cast<TransformComponent*>(task.customData)->copyTransform());
			markGpuDirty(mGpuDirtyTransforms, task.entity);
			if (first) {
				// The scene skips an entity with no transform yet, so one whose mesh came first is read again now.
				mScene.markEntity(task.entity);
			}
		}
		else if (task.type == ARMATURE_UPDATED) {
			markGpuDirty(mGpuDirtyBones, task.entity);
			markGpuDirty(mDirtyAnimations, task.entity);
			// Its instances carry the bone offset, which the armature decides.
			mScene.markEntity(task.entity);
		}
		else if (task.type == ANIMATION_UPDATED) {
			markGpuDirty(mDirtyAnimations, task.entity);
		}
		else if (task.type == MATERIAL_UPDATED) {
			mScene.markEntity(task.entity);
		}
		else if (task.type == MESH_UPDATED) {
			mScene.markEntity(task.entity);
		}
		else if (task.type == ENTITY_DESTROYED) {
			mScene.markEntity(task.entity);
			std::lock_guard lock(mGpuDirtyMutex);
			mDestroyedBones.push_back(task.entity);
			mDirtyAnimations.push_back(task.entity);
		}
	}

	std::vector<ecss::EntityId> RenderSystem::takeDirtyAnimations() {
		std::vector<ecss::EntityId> dirty;
		std::lock_guard lock(mGpuDirtyMutex);
		dirty.swap(mDirtyAnimations);
		return dirty;
	}

	bool RenderSystem::addRenderPass(std::string name, std::unique_ptr<Render::RenderPass> pass, const std::string_view after) {
		if (!pass || std::find(mPassNames.begin(), mPassNames.end(), name) != mPassNames.end()) {
			return false;
		}
		auto position = mPassNames.size();
		if (!after.empty()) {
			if (const auto it = std::find(mPassNames.begin(), mPassNames.end(), after); it != mPassNames.end()) {
				position = static_cast<size_t>(it - mPassNames.begin()) + 1;
			}
		}
		pass->init(mRenderData);
		mRenderPasses.insert(mRenderPasses.begin() + static_cast<std::ptrdiff_t>(position), std::move(pass));
		mPassZones.insert(mPassZones.begin() + static_cast<std::ptrdiff_t>(position), Debug::Profiler::intern(name));
		mPassNames.insert(mPassNames.begin() + static_cast<std::ptrdiff_t>(position), std::move(name));
		return true;
	}

	Render::RenderPass* RenderSystem::renderPass(const std::string_view name) const {
		const auto it = std::find(mPassNames.begin(), mPassNames.end(), name);
		return it == mPassNames.end() ? nullptr : mRenderPasses[static_cast<size_t>(it - mPassNames.begin())].get();
	}

	bool RenderSystem::removeRenderPass(const std::string_view name) {
		const auto it = std::find(mPassNames.begin(), mPassNames.end(), name);
		if (it == mPassNames.end()) {
			return false;
		}
		const auto index = it - mPassNames.begin();
		mRenderPasses.erase(mRenderPasses.begin() + index);
		mPassZones.erase(mPassZones.begin() + index);
		mPassNames.erase(it);
		return true;
	}

	void RenderSystem::resize() {
		Engine::instance()->rhi()->waitIdle();
		for (auto& pass : mRenderPasses) {
			pass->resize(mRenderData);
		}
	}

	bool RenderSystem::shouldRunPass(size_t passIndex) const
	{
		if (passIndex >= mPassNames.size()) {
			return true;
		}

		const char* name = mPassNames[passIndex].c_str();
		const auto& gfx = mRenderData.graphics;
		if (std::strcmp(name, "SSAO") == 0) {
			return gfx.ssao;
		}
		if (std::strcmp(name, "SSR") == 0) {
			return gfx.ssr;
		}
		if (std::strcmp(name, "Volumetric") == 0) {
			return gfx.volumetric;
		}
		if (std::strcmp(name, "TAA") == 0) {
			return gfx.taa;
		}
		if (std::strcmp(name, "DoF") == 0) {
			return gfx.dof;
		}
		if (std::strcmp(name, "SSS") == 0) {
			return gfx.sss;
		}
		if (std::strcmp(name, "ContactGI") == 0) {
			return gfx.contactGi;
		}

		if (std::strcmp(name, "ImGui") == 0) {
			return Engine::instance()->presentToScreen();
		}

		auto* engine = Engine::instance();
		if (engine->publishEditorDisplay() && !engine->presentToScreen()) {
			if (std::strcmp(name, "Debug") == 0) {
				return false;
			}
		}
		return true;
	}

	void RenderSystem::renderFrame()
	{
		using Clock = std::chrono::steady_clock;
		const auto ms = [](Clock::time_point from, Clock::time_point to) {
			return std::chrono::duration<float, std::milli>(to - from).count();
		};
		// A span of the frame for the profiler, from the same readings of the clock as the times kept here.
		const auto profile = [](const char* name, Clock::time_point from, Clock::time_point to) {
			Debug::Profiler::zone(name, from.time_since_epoch().count(), to.time_since_epoch().count());
		};

		auto* info = RenderDebugInfo::instance();
		info->objectsRendered = 0;
		info->verticesRendered = 0;
		info->drawCalls = 0;
		info->passCount = 0;

		const auto tFrame = Clock::now();

		auto rhi = Engine::instance()->rhi();
		info->takeGpuSpans(rhi->gpuSpans());
		profile("GPU spans", tFrame, Clock::now());

		// One diagnostic frame can snapshot every pass in turn. That needs each pass submitted on
		// its own so the GPU has actually produced the image before it is read back, which is far
		// too expensive for a normal frame -- hence the narrow condition.
		++mRenderedFrames;
		auto* engine = Engine::instance();
		const bool dumpPasses = engine->capturingFrame()
			&& !engine->passDumpDir().empty()
			&& mRenderedFrames == engine->captureFrame();
		PassDumpWriter passDump(dumpPasses ? engine->passDumpDir() : std::string{});
		passDump.seed(mRenderData.frame);

		// No frame-wide bind here: every pass binds its own sets against its pipeline, as Vulkan requires, and a bind
		// with no pipeline would be kept by OpenGL alone and hide a pass that forgot one.
		// Each pass's prepare a zone of its own, "Prepare <pass>": a long "Prepare passes" names the pass it was in.
		static std::unordered_map<const char*, const char*> prepareZones;
		for (size_t i = 0; i < mRenderPasses.size(); ++i) {
			if (shouldRunPass(i)) {
				const auto tPrepare = Clock::now();
				mRenderPasses[i]->prepare(mRenderData);
				auto& zone = prepareZones[mPassZones[i]];
				if (!zone) {
					zone = Debug::Profiler::intern(std::string("Prepare ") + mPassZones[i]);
				}
				profile(zone, tPrepare, Clock::now());
			}
		}
		// After prepare: the game makes the area tint's map in its own, and may have just replaced it.
		const auto tTint = Clock::now();
		updateAreaTint();
		const auto tBegin = Clock::now();
		profile("Area tint", tTint, tBegin);
		info->prepareMs = ms(tFrame, tBegin);
		profile("Prepare passes", tFrame, tBegin);
		auto cmd = rhi->beginCommandList(Render::CommandListType::Graphics);
		const auto tBegun = Clock::now();
		info->waitMs = ms(tBegin, tBegun);
		profile(Debug::Profiler::kWaitGpu, tBegin, tBegun);
		rhi->cmdGpuMark(cmd, nullptr, false);
		for (size_t i = 0; i < mRenderPasses.size(); ++i) {
			if (!shouldRunPass(i)) {
				continue;
			}
			const auto tPass = Clock::now();
			mRenderPasses[i]->render(mRenderData, cmd);
			const auto tPassEnd = Clock::now();
			profile(mPassZones[i], tPass, tPassEnd);
			rhi->cmdGpuMark(cmd, mPassZones[i], true);
			if (info->passCount < static_cast<int>(std::size(info->passTimes))) {
				info->passTimes[info->passCount] = { mPassZones[i], ms(tPass, tPassEnd), info->gpuMsOf(mPassZones[i]) };
				++info->passCount;
			}

			if (dumpPasses) {
				rhi->endCommandList(cmd);
				rhi->submit(cmd);
				rhi->waitIdle();
				passDump.write(i, mPassNames[i].c_str(), mRenderData.frame);
				cmd = rhi->beginCommandList(Render::CommandListType::Graphics);
			}
		}
		// What this frame drew with becomes what the next one calls the previous frame. The whole buffer
		// rather than the entities that moved: an entity is uploaded when it changes, so one that stopped
		// moving -- or never moved since it spawned -- would keep whatever its previous slot held forever,
		// and for a static entity that was the identity matrix the buffer was allocated with. Its motion
		// vector then pointed from the world origin, TAA pulled that pixel's history from the other side of
		// the screen, and the models washed out into the floor behind them.
		if (transforms && prevTransforms && mGpuCapacity > 0) {
			rhi->cmdCopyBuffer(cmd, transforms, prevTransforms, 0, 0, mGpuCapacity * sizeof(Math::Mat4));
			rhi->cmdPipelineBarrier(cmd, {
				Render::PipelineStage::Transfer,
				Render::PipelineStage::VertexShader,
				Render::AccessMask::TransferWrite,
				Render::AccessMask::ShaderRead,
			});
		}
		const auto tEnd = Clock::now();
		rhi->endCommandList(cmd);

		const auto tSubmit = Clock::now();
		profile("End list", tEnd, tSubmit);
		rhi->submit(cmd);
		const auto tSubmitted = Clock::now();
		info->submitMs = ms(tSubmit, tSubmitted);
		profile("Submit", tSubmit, tSubmitted);
		info->gpuMs = static_cast<float>(rhi->lastGpuMs());
		{
			auto presentTex = mRenderData.frame.texture(Render::FrameId::Final);
			auto presentSampler = mRenderData.frame.sampler(Render::FrameId::Final);
			if (!presentTex) {
				presentTex = mRenderData.frame.texture(Render::FrameId::GBufferAlbedo);
				presentSampler = mRenderData.frame.sampler(Render::FrameId::GBufferAlbedo);
			}
			if (presentTex) {
				rhi->setPresentBlitSource(presentTex, presentSampler);
			}
		}
		if (Engine::instance()->publishEditorDisplay()) {
			SFE_PROFILE_ZONE("Stream frame");
			if (RuntimeBridge::RuntimeBridge::isAlive()) {
				RuntimeBridge::RuntimeBridge::instance()->streamPresentedFrame();
			}
		}

		const auto tPresent = Clock::now();
		rhi->present();
		const auto tPresented = Clock::now();
		info->presentMs = ms(tPresent, tPresented);
		profile(Debug::Profiler::kPresent, tPresent, tPresented);
		info->cpuMs = ms(tFrame, tPresented);
		// The whole frame, not just what this function took: the graph is about what the player feels.
		info->pushFrame(Engine::instance()->getDeltaTime() * 1000.f, info->gpuMs);
	}

	void RenderDebugInfo::takeGpuSpans(const std::vector<Render::RenderHardwareInterface::GpuSpan>& spans) {
		static constexpr const char* kOther = "other";
		gpuTimes.clear();
		size_t first = 0; // where the pass being read starts in gpuTimes
		float whole = 0.f;
		bool byShader = false;
		for (const auto& span : spans) {
			const auto ms = static_cast<float>(span.ms);
			whole += ms;
			// A pipeline's part of the pass, or the pass's rest; each once a pass, its spans summed.
			const char* part = span.endsPass || !span.label ? kOther : span.label;
			byShader |= !span.endsPass && span.label;
			auto it = std::find_if(gpuTimes.begin() + static_cast<std::ptrdiff_t>(first), gpuTimes.end(),
				[&](const GpuTime& time) { return time.shader == part; });
			if (it == gpuTimes.end()) {
				it = gpuTimes.insert(gpuTimes.end(), { nullptr, part, 0.f });
			}
			it->ms += ms;
			if (!span.endsPass) {
				continue;
			}
			// The pass ends here: its parts kept if it was marked by pipeline, and its whole after them.
			if (byShader) {
				for (size_t i = first; i < gpuTimes.size(); ++i) {
					gpuTimes[i].pass = span.label;
				}
			}
			else {
				gpuTimes.resize(first);
			}
			gpuTimes.push_back({ span.label, nullptr, whole });
			first = gpuTimes.size();
			whole = 0.f;
			byShader = false;
		}
		gpuTimes.resize(first);
	}

	float RenderDebugInfo::gpuMsOf(const char* pass) const {
		for (const auto& time : gpuTimes) {
			if (!time.shader && time.pass == pass) {
				return time.ms;
			}
		}
		return 0.f;
	}

	RenderSystem::~RenderSystem() = default;
}
