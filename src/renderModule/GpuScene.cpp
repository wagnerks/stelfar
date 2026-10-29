#include "GpuScene.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <tuple>
#include <unordered_map>

#include "assetsModule/modelModule/MeshVaoRegistry.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/OutlineComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "renderModule/GpuMaterialTable.h"
#include "renderModule/MaterialRules.h"
#include "systemsModule/systems/RenderSystem.h"

namespace SFE::Render {
	// Mirrors CullView in shaders/gpu_cull.cs; gpu_glass.cs reads the part up to the shapes.
	struct GpuScene::CullParams {
		uint32_t mode = 0; // 0: planes, 1: spheres
		uint32_t shapeCount = 0;
		uint32_t batchCount = 0;
		// Each step doubles the error on screen a level may have. 1 for the shadow views: a silhouette cast on the
		// floor hides what the simplifier changed, and the cascades draw the whole scene four more times.
		uint32_t lodBias = 0;
		float shapes[GpuScene::kMaxSpheres * 4]{};
		float viewProj[16]{};
		uint32_t occlusion = 0; // 0 none, 1 and 2 the phases of cullVisibleLastFrame and cullOccluded
		uint32_t depthWidth = 0;
		uint32_t depthHeight = 0;
		uint32_t levels = 0;
		uint32_t levelOffsets[16]{};
		// xyz the player camera, w the pixels one unit of radius covers at one unit of distance. Every view
		// gets the player's, shadow cascades included: a cascade that picked its levels from the light's
		// distance would draw the coarsest mesh into the shadow right under the camera.
		float lodOrigin[4]{};
		// A shadow cascade's texels per metre, 0 for any other view: casters under minTexels across are left out,
		// and no level of detail finer than a texel is drawn.
		float texelsPerMetre = 0.f;
		float minTexels = 0.f;
		// The ground's batches in the G-buffer list, groundFirst up to groundEnd, none in any other, and the metres from
		// the camera past which its chunks are drawn a level coarser than their size on screen asks for.
		uint32_t groundFirst = 0;
		uint32_t groundEnd = 0;
		float groundLodDistance = 0.f;
		// The pixels on screen a coarser level may change (GraphicsSettings::lodPixels), and as many texels in a shadow map.
		float lodPixels = 2.f;
	};

	namespace {
		using SceneInstance = GpuScene::InstanceRecord;
		using Batch = GpuScene::BatchRecord;
		static_assert(sizeof(GpuMesh) == 80 && sizeof(SceneInstance) == 16 && sizeof(Batch) == 16);

		struct HizStep {
			uint32_t srcOffset = 0;
			uint32_t srcWidth = 0;
			uint32_t srcHeight = 0;
			uint32_t dstOffset = 0;
			uint32_t dstWidth = 0;
			uint32_t dstHeight = 0;
			uint32_t fromTexture = 0; // 0 the level below, 1 a depth texture, 2 a layer of a depth array
			uint32_t layer = 0;
		};

		constexpr uint32_t kLocalSize = 16;
		constexpr uint32_t kGlassLocalSize = 64;
		constexpr uint32_t kCommandStride = 20;

		void retire(const BufferHandle buffer) {
			if (!buffer) {
				return;
			}
			Engine::instance()->rhi()->destroyBuffer(buffer);
		}

		BufferHandle createBuffer(const size_t size, const BufferUsage usage, const CpuAccess access, const void* data, const char* name) {
			BufferDesc desc;
			desc.size = (std::max)(size, size_t{ 16 });
			desc.usage = usage;
			desc.cpuAccess = access;
			desc.debugName = name;
			return Engine::instance()->rhi()->createBuffer(desc, data);
		}

		uint32_t frameSlots() {
			return (std::max)(1u, Engine::instance()->rhi()->framesInFlight());
		}

		void setPlane(float* out, const FrustumModule::Plane& plane) {
			out[0] = plane.normal.x;
			out[1] = plane.normal.y;
			out[2] = plane.normal.z;
			out[3] = plane.distance;
		}
	}

	GpuScene::View::~View() {
		release();
	}

	GpuScene::View& GpuScene::View::operator=(View&& other) noexcept {
		if (this != &other) {
			release();
			static_cast<ViewState&>(*this) = std::exchange(static_cast<ViewState&>(other), {});
		}
		return *this;
	}

	void GpuScene::View::release() {
		// None once the engine has gone: its RHI took them with it.
		auto* rhi = Singleton<Engine>::isAlive() ? Engine::instance()->rhi() : nullptr;
		if (!rhi) {
			return;
		}
		for (const auto buffer : { visible, commands, keys, lastVisible, pyramid, counts }) {
			if (buffer) {
				rhi->destroyBuffer(buffer);
			}
		}
		for (const auto* list : { &params, &packParams }) {
			for (const auto buffer : *list) {
				if (buffer) {
					rhi->destroyBuffer(buffer);
				}
			}
		}
		for (const auto* list : { &sets, &pyramidSets, &packSets }) {
			for (const auto set : *list) {
				if (set) {
					rhi->freeDescriptorSet(set);
				}
			}
		}
		static_cast<ViewState&>(*this) = {};
	}

	void GpuScene::init() {
		auto* rhi = Engine::instance()->rhi();
		DescriptorSetLayoutDesc layout;
		layout.bindings = {
			{ 1, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 2, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 26, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 27, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 28, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 29, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 30, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 31, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 32, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 33, DescriptorType::StorageBuffer, ShaderStage::Compute },
		};
		mCullLayout = rhi->createDescriptorSetLayout(layout);
		ComputePipelineDesc desc;
		desc.compute = rhi->loadShader(ShaderStage::Compute, "shaders/gpu_cull.cs");
		desc.descriptorSetLayouts = { mCullLayout };
		mCullPipeline = rhi->createComputePipeline(desc);
		DescriptorSetLayoutDesc packLayout;
		packLayout.bindings = {
			{ 30, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 34, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 35, DescriptorType::StorageBuffer, ShaderStage::Compute },
		};
		mPackLayout = rhi->createDescriptorSetLayout(packLayout);
		ComputePipelineDesc packDesc;
		packDesc.compute = rhi->loadShader(ShaderStage::Compute, "shaders/gpu_pack.cs");
		packDesc.descriptorSetLayouts = { mPackLayout };
		mPackPipeline = rhi->createComputePipeline(packDesc);
		mEmptyBuffer =createBuffer(16, BufferUsage::Storage, CpuAccess::None, nullptr, "GpuScene empty");

		DescriptorSetLayoutDesc hizLayout;
		hizLayout.bindings = {
			{ 1, DescriptorType::CombinedImageSampler, ShaderStage::Compute },
			{ 2, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 3, DescriptorType::CombinedImageSampler, ShaderStage::Compute },
		};
		mHizLayout = rhi->createDescriptorSetLayout(hizLayout);
		ComputePipelineDesc hizDesc;
		hizDesc.compute = rhi->loadShader(ShaderStage::Compute, "shaders/hiz.cs");
		hizDesc.descriptorSetLayouts = { mHizLayout };
		hizDesc.pushConstantSize = sizeof(HizStep);
		mHizPipeline = rhi->createComputePipeline(hizDesc);

		TextureDesc emptyDepth;
		emptyDepth.width = 1;
		emptyDepth.height = 1;
		emptyDepth.format = TextureFormat::DEPTH_COMPONENT32;
		emptyDepth.usage = TextureUsage::Sampled | TextureUsage::DepthStencilAttachment;
		mEmptyDepth = rhi->createTexture(emptyDepth);
		emptyDepth.dimension = TextureDimension::Array;
		emptyDepth.arrayLayers = 1;
		mEmptyDepthArray = rhi->createTexture(emptyDepth);
		SamplerDesc emptySampler;
		emptySampler.minFilter = FilterMode::Nearest;
		emptySampler.magFilter = FilterMode::Nearest;
		mEmptySampler = rhi->createSampler(emptySampler);

		DescriptorSetLayoutDesc glassLayout;
		glassLayout.bindings = {
			{ 1, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 26, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 27, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 29, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 30, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 31, DescriptorType::StorageBuffer, ShaderStage::Compute },
			{ 32, DescriptorType::StorageBuffer, ShaderStage::Compute },
		};
		mGlassLayout = rhi->createDescriptorSetLayout(glassLayout);
		ComputePipelineDesc glassDesc;
		glassDesc.compute = rhi->loadShader(ShaderStage::Compute, "shaders/gpu_glass.cs");
		glassDesc.descriptorSetLayouts = { mGlassLayout };
		glassDesc.pushConstantSize = 24;
		mGlassPipeline = rhi->createComputePipeline(glassDesc);
	}

	void GpuScene::update(GpuMaterialTable& materials) {
		mMaterials = &materials;
		std::vector<ecss::EntityId> pending;
		{
			std::lock_guard lock(mPendingMutex);
			pending.swap(mPending);
		}

		// OutlineComponent is added and removed straight on the registry, with no event, so the outlined set is
		// compared every frame (a handful of entities) and whoever joined or left it is read again.
		std::vector<ecss::EntityId> outlined;
		for (auto [entity, outline] : ECSHandler::registry().view<const OutlineComponent>()) {
			outlined.push_back(entity);
		}
		std::sort(outlined.begin(), outlined.end());
		if (outlined != mOutlined) {
			std::set_symmetric_difference(outlined.begin(), outlined.end(), mOutlined.begin(), mOutlined.end(), std::back_inserter(pending));
			mOutlined = std::move(outlined);
		}

		mAdded.clear();
		bool rebuilt = mDirty.exchange(false, std::memory_order_acq_rel);
		if (!rebuilt) {
			std::sort(pending.begin(), pending.end());
			pending.erase(std::unique(pending.begin(), pending.end()), pending.end());
			// Remove, then read again: a destroyed entity has nothing left to add, and a reused id adds its new self.
			for (const auto entity : pending) {
				removeEntity(entity);
				addEntity(entity, materials);
				if (mEntityRefs.contains(entity)) {
					mAdded.push_back(entity);
				}
			}
			// Moves leave slots behind; once those outnumber the live ones the list is packed again. Spare room
			// inside a batch or group is not left behind, or compact's own spare would trigger it every frame.
			for (auto& data : mLists) {
				const size_t leftInstances = data.instanceMirror.size() - data.ownedInstances;
				const size_t leftBatches = data.batchMirror.size() - data.ownedBatches;
				if (leftInstances > (std::max)(size_t{ 4096 }, static_cast<size_t>(data.liveInstances))
					|| leftBatches > (std::max)(size_t{ 1024 }, data.ownedBatches)) {
					compact(data);
				}
			}
		}
		if (rebuilt) {
			rebuildAll(materials);
			for (const auto& [entity, refs] : mEntityRefs) {
				mAdded.push_back(entity);
			}
			std::sort(mAdded.begin(), mAdded.end());
		}
		// Nearest first again once the camera is a few metres from where it was last sorted, or a tenth more has joined
		// since: what joins sits at the end of its batch, and a wood streaming in round a camera standing still would
		// otherwise stay in the order it arrived. About a millisecond for fifty thousand instances.
		if (auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>()) {
			constexpr float kResortDistance = 4.f;
			const auto camera = renderSystem->getRenderData().mCameraPos;
			const auto moved = camera - mSortedFrom;
			const size_t instances = mLists[static_cast<size_t>(List::GBuffer)].liveInstances;
			if (rebuilt || Math::dot(moved, moved) > kResortDistance * kResortDistance || instances > mSortedInstances + mSortedInstances / 10) {
				sortNearestFirst(camera);
				mSortedFrom = camera;
				mSortedInstances = instances;
			}
		}
		uploadList(mLists[static_cast<size_t>(List::GBuffer)], "GpuScene gbuffer");
		uploadList(mLists[static_cast<size_t>(List::Shadow)], "GpuScene shadow");
		uploadList(mLists[static_cast<size_t>(List::Glass)], "GpuScene glass");
		uploadList(mLists[static_cast<size_t>(List::Outline)], "GpuScene outline");

		// After the lists: every mesh an instance refers to already exists by then.
		uploadMeshes();
	}

	void GpuScene::sortNearestFirst(const Math::Vec3& from) {
		auto& data = mLists[static_cast<size_t>(List::GBuffer)];
		auto& registry = ECSHandler::registry();
		std::vector<std::pair<float, uint32_t>> order;
		std::vector<InstanceRecord> records;
		std::vector<Owner> owners;
		for (size_t b = 0; b < data.batchMirror.size() && b < data.batchStates.size(); ++b) {
			auto& state = data.batchStates[b];
			const auto count = static_cast<uint32_t>(state.owners.size());
			if (count < 2) {
				continue;
			}
			order.clear();
			for (uint32_t i = 0; i < count; ++i) {
				float distance = std::numeric_limits<float>::max();
				if (const auto transform = registry.pinComponent<const ComponentsModule::TransformMatComp>(state.owners[i].entity)) {
					const auto& at = transform->mTransform[3];
					const auto offset = Math::Vec3{ at.x, at.y, at.z } - from;
					distance = Math::dot(offset, offset);
				}
				order.emplace_back(distance, i);
			}
			if (std::is_sorted(order.begin(), order.end())) {
				continue;
			}
			std::sort(order.begin(), order.end());
			// The slots move with their owners, and every owner's reference follows its slot.
			const uint32_t first = data.batchMirror[b].firstInstance;
			records.resize(count);
			owners.resize(count);
			for (uint32_t i = 0; i < count; ++i) {
				records[i] = data.instanceMirror[first + order[i].second];
				owners[i] = state.owners[order[i].second];
			}
			for (uint32_t i = 0; i < count; ++i) {
				data.instanceMirror[first + i] = records[i];
				state.owners[i] = owners[i];
				mEntityRefs[owners[i].entity][owners[i].ref].slot = i;
			}
			data.instanceDirty.add(first, first + count);
		}
	}

	void GpuScene::uploadMeshes() {
		auto* registry = MeshVaoRegistry::instance();
		if (registry->meshCount() == mMeshesUploaded) {
			return;
		}
		// Entries never change once added, so only the new ones go up, unless the buffer is replaced.
		const size_t count = registry->meshCount();
		size_t first = mMeshesUploaded;
		if (!mMeshes || count > mMeshCapacity) {
			retire(mMeshes);
			mMeshCapacity = (std::max)(mMeshCapacity, size_t{ 64 });
			while (mMeshCapacity < count) {
				mMeshCapacity *= 2;
			}
			mMeshes = createBuffer(mMeshCapacity * sizeof(GpuMesh), BufferUsage::Storage, CpuAccess::None, nullptr, "GpuScene meshes");
			first = 0;
		}
		const auto table = registry->meshTable(first);
		Engine::instance()->rhi()->updateBuffer(mMeshes, first * sizeof(GpuMesh), table.size() * sizeof(GpuMesh), table.data());
		mMeshesUploaded = first + table.size();
	}

	void GpuScene::markEntity(const ecss::EntityId entity) {
		std::lock_guard lock(mPendingMutex);
		mPending.push_back(entity);
	}

	void GpuScene::rebuildAll(GpuMaterialTable& materials) {
		// Every instance acquires its material again below.
		materials.reset();
		for (auto& data : mLists) {
			data.groups.clear();
			data.groupStates.clear();
			data.groupByKey.clear();
			data.instanceMirror.clear();
			data.batchMirror.clear();
			data.batchStates.clear();
			data.liveInstances = 0;
			data.ownedInstances = 0;
			data.ownedBatches = 0;
			// The mirrors are uploaded whole, into the buffers they already have when those are large enough.
			data.instanceDirty.all();
			data.batchDirty.all();
		}
		mEntityRefs.clear();
		for (auto [entity, transform, meshComp] : ECSHandler::registry().view<const ComponentsModule::TransformMatComp, const MeshComponent>()) {
			if (transform && meshComp) {
				addEntity(entity, materials);
			}
		}
		// Adding one at a time grew every batch by doubling; lay them out again at the size they ended up.
		for (auto& data : mLists) {
			compact(data);
		}
	}

	size_t GpuScene::batchesOfMesh(const List list, const uint32_t mesh) const {
		const auto& data = mLists[static_cast<size_t>(list)];
		size_t count = 0;
		for (const auto& group : data.groups) {
			for (uint32_t i = 0; i < group.batchCount; ++i) {
				const auto& batch = data.batchMirror[group.firstBatch + i];
				count += batch.mesh == mesh && batch.instanceCount > 0;
			}
		}
		return count;
	}

	uint32_t GpuScene::largestBatch(const List list) const {
		const auto& data = mLists[static_cast<size_t>(list)];
		uint32_t largest = 0;
		for (const auto& batch : data.batchMirror) {
			largest = (std::max)(largest, batch.instanceCount);
		}
		return largest;
	}

	bool GpuScene::validate() const {
		for (const auto& [entity, refs] : mEntityRefs) {
			for (const auto& ref : refs) {
				const auto& data = mLists[static_cast<size_t>(ref.list)];
				if (ref.group >= data.groups.size() || ref.batch >= data.groups[ref.group].batchCount) {
					return false;
				}
				const uint32_t global = data.groups[ref.group].firstBatch + ref.batch;
				if (global >= data.batchMirror.size() || ref.slot >= data.batchMirror[global].instanceCount) {
					return false;
				}
				const auto& batch = data.batchMirror[global];
				if (batch.firstInstance + ref.slot >= data.instanceMirror.size()
					|| data.instanceMirror[batch.firstInstance + ref.slot].entity != entity
					|| data.batchStates[global].owners[ref.slot].entity != entity) {
					return false;
				}
			}
		}
		for (const auto& data : mLists) {
			size_t live = 0;
			size_t ownedInstances = 0;
			for (size_t b = 0; b < data.batchMirror.size(); ++b) {
				if (data.batchStates[b].owners.size() != data.batchMirror[b].instanceCount
					|| data.batchMirror[b].instanceCount > data.batchStates[b].capacity) {
					return false;
				}
				live += data.batchMirror[b].instanceCount;
				ownedInstances += data.batchStates[b].capacity;
			}
			size_t ownedBatches = 0;
			for (const auto& state : data.groupStates) {
				ownedBatches += state.batchCapacity;
			}
			const auto sameRecords = [](const auto& a, const auto& b) {
				return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0;
			};
			if (mTrackUploads && (!sameRecords(data.instanceMirror, data.uploadedInstances) || !sameRecords(data.batchMirror, data.uploadedBatches))) {
				return false;
			}
			if (live != data.liveInstances || ownedInstances != data.ownedInstances || ownedBatches != data.ownedBatches
				|| data.ownedInstances > data.instanceMirror.size() || data.ownedBatches > data.batchMirror.size()) {
				return false;
			}
		}
		return true;
	}

	void GpuScene::compact(ListData& data) {
		++mCompactions;
		// Groups back to back and their batches with them, each with a quarter spare. References hold a group,
		// a batch inside it and a slot inside that, so none of them moves.
		std::vector<InstanceRecord> packed;
		std::vector<BatchRecord> batches;
		std::vector<BatchState> states;
		packed.reserve(data.liveInstances + data.liveInstances / 4 + data.batchMirror.size() * 4);
		data.ownedInstances = 0;
		data.ownedBatches = 0;
		for (size_t g = 0; g < data.groups.size(); ++g) {
			auto& draw = data.groups[g];
			const auto first = static_cast<uint32_t>(batches.size());
			const uint32_t batchCapacity = draw.batchCount + draw.batchCount / 4 + 4;
			batches.resize(first + batchCapacity);
			states.resize(first + batchCapacity);
			for (uint32_t i = 0; i < draw.batchCount; ++i) {
				auto batch = data.batchMirror[draw.firstBatch + i];
				auto& state = data.batchStates[draw.firstBatch + i];
				const auto start = static_cast<uint32_t>(packed.size());
				packed.insert(packed.end(), data.instanceMirror.begin() + batch.firstInstance, data.instanceMirror.begin() + batch.firstInstance + batch.instanceCount);
				state.capacity = batch.instanceCount + batch.instanceCount / 4 + 4;
				packed.resize(start + state.capacity);
				batch.firstInstance = start;
				data.ownedInstances += state.capacity;
				batches[first + i] = batch;
				states[first + i] = std::move(state);
			}
			draw.firstBatch = first;
			data.groupStates[g].batchCapacity = batchCapacity;
			data.ownedBatches += batchCapacity;
		}
		data.instanceMirror = std::move(packed);
		data.batchMirror = std::move(batches);
		data.batchStates = std::move(states);
		data.instanceDirty.all();
		data.batchDirty.all();
	}

	void GpuScene::addEntity(const ecss::EntityId entity, GpuMaterialTable& materials) {
		static const ComponentsModule::MaterialComponent kNoMaterial;
		auto& registry = ECSHandler::registry();
		const auto meshComp = registry.pinComponent<const MeshComponent>(entity);
		if (!meshComp || !registry.hasComponent<ComponentsModule::TransformMatComp>(entity)) {
			return;
		}
		const auto matComp = registry.pinComponent<const MaterialComponent>(entity);
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		const uint32_t boneOffset = renderSystem ? renderSystem->boneRange(entity).offset : 0;
		auto& refs = mEntityRefs[entity];

		meshComp->forEachDrawMesh(matComp.get(), [&](const RenderMeshData& mesh, const MaterialComponent* mat) {
			const auto& material = mat ? *mat : kNoMaterial;
			const bool transparent = mat && mat->transparent;
			const auto glassShader = transparent ? MaterialRules::glassShader(material) : std::string{};
			// One reference per instance, released when that instance is removed.
			const auto materialIndex = [&] { return materials.acquire(material); };

			// Shadow casters: glass casts through the glass proxy when there is one; a transparent surface of a shader of
			// its own, its own shape.
			const auto& caster = glassShader == ComponentsModule::DefaultGlassFS && mGlassShadowProxy ? mGlassShadowProxy : mesh;
			if (caster.indexCount != 0 && material.castsShadow) {
				const bool dithered = material.opacity > 0.f && material.opacity < 0.999f;
				const float cutoff = MaterialRules::cutoutCutoff(material.alphaCutoff, material.alphaFromMaps, material.fragmentShader);
				const bool cutout = material.alphaFromMaps || cutoff > 0.f || dithered;
				DrawGroup group;
				group.maskTexture = !cutout ? TextureHandle{}
					: material.alphaFromMaps ? MaterialRules::maskTexture(material.materials) : MaterialRules::diffuseTexture(material.materials);
				group.cutout = cutout;
				group.transparent = transparent;
				const auto key = std::to_string(group.maskTexture.id) + (cutout ? "|c" : "|o") + (transparent ? "t" : "s");
				const auto index = groupFor(List::Shadow, key, std::move(group));
				for (uint32_t level = 0; level < (std::max)(1u, caster.lodCount); ++level) {
					const uint32_t levelMesh = caster.meshId + level;
					addInstance(refs, entity, List::Shadow, index, levelMesh, { entity, levelMesh, materialIndex(), boneOffset });
				}
			}

			if (mesh.indexCount == 0 || material.shadowOnly) {
				return;
			}
			if (transparent) {
				// A group and one batch a pair of shaders whatever the mesh, and another for it solid: sortGlass orders each
				// one's instances by depth. The vertex shader the material's own (GlassPass::pipelineFor).
				DrawGroup group;
				group.vertexShader = material.vertexShader.empty() ? std::string(ComponentsModule::DefaultGBufferVS) : material.vertexShader;
				group.fragmentShader = glassShader;
				group.solid = material.solidGlass;
				const auto key = group.vertexShader + '|' + glassShader;
				const auto index = groupFor(List::Glass, material.solidGlass ? key + "|solid" : key, std::move(group));
				addInstance(refs, entity, List::Glass, index, 0, { entity, mesh.meshId, materialIndex(), boneOffset });
				return;
			}
			DrawGroup group;
			group.vertexShader = material.vertexShader.empty() ? std::string(ComponentsModule::DefaultGBufferVS) : material.vertexShader;
			group.fragmentShader = material.fragmentShader.empty() ? std::string(ComponentsModule::DefaultGBufferFS) : material.fragmentShader;
			// Whether the G-buffer shader discards: GpuMaterialTable gives it a cutoff exactly then.
			group.cutout = material.alphaDither || MaterialRules::cutoutCutoff(material.alphaCutoff, material.alphaFromMaps, material.fragmentShader) > 0.f;
			group.drawClass = MaterialRules::drawClass(material);
			if (!group.cutout && group.fragmentShader == ComponentsModule::DefaultGBufferFS) {
				group.fragmentShader = ComponentsModule::DefaultGBufferSolidFS;
			}
			group.textures = material.materials;
			group.textureKey = MaterialRules::textureKey(material.materials);
			group.oneSided = mesh.oneSided;
			const auto key = group.vertexShader + '|' + group.fragmentShader + '|' + std::to_string(group.textureKey)
				+ (group.oneSided ? "|one" : "|two") + (group.drawClass == ComponentsModule::DrawClass::Ground ? "|ground" : "");
			const auto index = groupFor(List::GBuffer, key, std::move(group));
			// Once under every level of the chain. The levels are consecutive mesh ids, they share the draw
			// group, and the culling keeps the one whose level matches the size on screen, so this costs a
			// slot per level and a draw whose instance count is zero for the levels not in use.
			for (uint32_t level = 0; level < (std::max)(1u, mesh.lodCount); ++level) {
				const uint32_t levelMesh = mesh.meshId + level;
				addInstance(refs, entity, List::GBuffer, index, levelMesh, { entity, levelMesh, materialIndex(), boneOffset });
			}
		});

		// Outlined entities draw every submesh again into the outline list, with no material of their own.
		if (registry.hasComponent<OutlineComponent>(entity)) {
			const auto group = groupFor(List::Outline, "outline", DrawGroup{});
			for (const auto& node : meshComp->meshGraph) {
				if (node.value && node.value.indexCount != 0) {
					addInstance(refs, entity, List::Outline, group, node.value.meshId, { entity, node.value.meshId, materials.acquire(kNoMaterial), boneOffset });
				}
			}
		}

		if (refs.empty()) {
			mEntityRefs.erase(entity);
			return;
		}
		mMaxEntity = (std::max)(mMaxEntity, entity);
	}

	void GpuScene::removeEntity(const ecss::EntityId entity) {
		const auto it = mEntityRefs.find(entity);
		if (it == mEntityRefs.end()) {
			return;
		}
		// Each removal can move another slot into the hole and rewrites that slot's reference, this entity's own
		// included, so every reference is read at the moment it is removed.
		auto& refs = it->second;
		for (size_t i = 0; i < refs.size(); ++i) {
			removeInstance(refs[i]);
		}
		mEntityRefs.erase(it);
	}

	uint32_t GpuScene::groupFor(const List list, const std::string& key, DrawGroup&& prototype) {
		auto& data = mLists[static_cast<size_t>(list)];
		const auto [it, inserted] = data.groupByKey.try_emplace(key, static_cast<uint32_t>(data.groups.size()));
		if (inserted) {
			prototype.firstBatch = static_cast<uint32_t>(data.batchMirror.size());
			prototype.batchCount = 0;
			data.groups.push_back(std::move(prototype));
			data.groupStates.push_back({});
			growGroup(data, it->second, 4);
		}
		return it->second;
	}

	void GpuScene::addInstance(std::vector<InstanceRef>& refs, const ecss::EntityId entity, const List list, const uint32_t group,
		const uint32_t batchKey, const InstanceRecord& record)
	{
		auto& data = mLists[static_cast<size_t>(list)];
		auto& groupState = data.groupStates[group];
		const uint32_t maxInstances = list == List::Glass ? UINT32_MAX : kMaxBatchInstances;
		auto& keyBatches = data.groupStates[group].batchesByKey[batchKey];
		uint32_t local = UINT32_MAX;
		// The newest first: that is where instances were last added, and the others are usually full.
		for (auto it = keyBatches.rbegin(); it != keyBatches.rend(); ++it) {
			if (data.batchMirror[data.groups[group].firstBatch + *it].instanceCount < maxInstances) {
				local = *it;
				break;
			}
		}
		if (local == UINT32_MAX) {
			if (data.groups[group].batchCount == groupState.batchCapacity) {
				growGroup(data, group, groupState.batchCapacity * 2);
			}
			local = data.groups[group].batchCount++;
			keyBatches.push_back(local);
			const uint32_t global = data.groups[group].firstBatch + local;
			data.batchMirror[global] = { static_cast<uint32_t>(data.instanceMirror.size()), 0, record.mesh, 0 };
			data.batchStates[global] = {};
		}

		const uint32_t global = data.groups[group].firstBatch + local;
		auto& state = data.batchStates[global];
		if (data.batchMirror[global].instanceCount == state.capacity) {
			growBatch(data, global, (std::min)((std::max)(state.capacity * 2, 4u), maxInstances));
		}
		auto& batch = data.batchMirror[global];
		const uint32_t slot = batch.instanceCount++;
		const uint32_t index = batch.firstInstance + slot;
		data.instanceMirror[index] = record;
		state.owners.push_back({ entity, static_cast<uint32_t>(refs.size()) });
		refs.push_back({ list, group, local, slot });
		++data.liveInstances;

		data.instanceDirty.add(index, index + 1);
		data.batchDirty.add(global, global + 1);
	}

	void GpuScene::removeInstance(const InstanceRef& ref) {
		auto& data = mLists[static_cast<size_t>(ref.list)];
		const uint32_t global = data.groups[ref.group].firstBatch + ref.batch;
		auto& batch = data.batchMirror[global];
		auto& state = data.batchStates[global];
		const uint32_t last = batch.instanceCount - 1;
		if (mMaterials) {
			mMaterials->release(data.instanceMirror[batch.firstInstance + ref.slot].material);
		}
		if (ref.slot != last) {
			// The last instance fills the hole, and its owner is told where it went.
			data.instanceMirror[batch.firstInstance + ref.slot] = data.instanceMirror[batch.firstInstance + last];
			const auto moved = state.owners[last];
			state.owners[ref.slot] = moved;
			mEntityRefs[moved.entity][moved.ref].slot = ref.slot;
			const uint32_t index = batch.firstInstance + ref.slot;
			data.instanceDirty.add(index, index + 1);
		}
		state.owners.pop_back();
		--batch.instanceCount;
		--data.liveInstances;
		data.batchDirty.add(global, global + 1);
	}

	void GpuScene::growBatch(ListData& data, const uint32_t batch, const uint32_t capacity) {
		auto& record = data.batchMirror[batch];
		const uint32_t start = static_cast<uint32_t>(data.instanceMirror.size());
		data.instanceMirror.resize(start + capacity);
		std::copy_n(data.instanceMirror.begin() + record.firstInstance, record.instanceCount, data.instanceMirror.begin() + start);
		record.firstInstance = start;
		data.ownedInstances += capacity - data.batchStates[batch].capacity;
		data.batchStates[batch].capacity = capacity;
		data.instanceDirty.add(start, start + capacity);
		data.batchDirty.add(batch, batch + 1);
	}

	void GpuScene::growGroup(ListData& data, const uint32_t group, const uint32_t capacity) {
		auto& draw = data.groups[group];
		const uint32_t first = static_cast<uint32_t>(data.batchMirror.size());
		data.batchMirror.resize(first + capacity);
		data.batchStates.resize(first + capacity);
		for (uint32_t i = 0; i < draw.batchCount; ++i) {
			data.batchMirror[first + i] = data.batchMirror[draw.firstBatch + i];
			data.batchStates[first + i] = std::move(data.batchStates[draw.firstBatch + i]);
			// The old slot stays behind as an empty batch: culled to a command with no instances, owning nothing.
			data.batchMirror[draw.firstBatch + i] = {};
			data.batchStates[draw.firstBatch + i] = {};
		}
		if (draw.batchCount) {
			data.batchDirty.add(draw.firstBatch, draw.firstBatch + draw.batchCount);
		}
		data.batchDirty.add(first, first + capacity);
		draw.firstBatch = first;
		data.ownedBatches += capacity - data.groupStates[group].batchCapacity;
		data.groupStates[group].batchCapacity = capacity;
	}

	void GpuScene::DirtyRanges::add(const uint32_t lo, const uint32_t hi) {
		// Merged across gaps this small: a separate upload costs more than resending the slots in between.
		constexpr uint32_t kMergeGap = 64;
		constexpr size_t kMaxRanges = 16;
		if (lo >= hi) {
			return;
		}
		auto it = std::lower_bound(ranges.begin(), ranges.end(), lo, [](const auto& range, uint32_t value) { return uint64_t{ range.second } + kMergeGap < value; });
		auto merged = std::make_pair(lo, hi);
		auto last = it;
		while (last != ranges.end() && last->first <= uint64_t{ hi } + kMergeGap) {
			merged.first = (std::min)(merged.first, last->first);
			merged.second = (std::max)(merged.second, last->second);
			++last;
		}
		it = ranges.erase(it, last);
		ranges.insert(it, merged);
		while (ranges.size() > kMaxRanges) {
			size_t closest = 0;
			for (size_t i = 1; i + 1 < ranges.size(); ++i) {
				if (ranges[i + 1].first - ranges[i].second < ranges[closest + 1].first - ranges[closest].second) {
					closest = i;
				}
			}
			ranges[closest].second = ranges[closest + 1].second;
			ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(closest) + 1);
		}
	}

	void GpuScene::uploadList(ListData& data, const char* name) {
		auto* rhi = Engine::instance()->rhi();
		const auto upload = [&](BufferHandle& buffer, size_t& capacity, const size_t count, const size_t stride, const auto* bytes,
			DirtyRanges& dirty, auto& uploaded)
		{
			if (mTrackUploads) {
				uploaded.resize(count);
			}
			if (!buffer || count > capacity) {
				retire(buffer);
				capacity = (std::max)(capacity, size_t{ 256 });
				while (capacity < count) {
					capacity *= 2;
				}
				buffer = createBuffer(capacity * stride, BufferUsage::Storage, CpuAccess::None, nullptr, name);
				dirty.all();
			}
			for (const auto [lo, rangeHi] : dirty.ranges) {
				const uint32_t hi = (std::min)(rangeHi, static_cast<uint32_t>(count));
				if (lo < hi) {
					rhi->updateBuffer(buffer, lo * stride, (hi - lo) * stride, static_cast<const uint8_t*>(static_cast<const void*>(bytes)) + lo * stride);
					if (mTrackUploads) {
						std::copy(bytes + lo, bytes + hi, uploaded.begin() + lo);
					}
				}
			}
			dirty.ranges.clear();
		};
		upload(data.instances, data.instanceGpuCapacity, data.instanceMirror.size(), sizeof(InstanceRecord), data.instanceMirror.data(),
			data.instanceDirty, data.uploadedInstances);
		upload(data.batches, data.batchGpuCapacity, data.batchMirror.size(), sizeof(BatchRecord), data.batchMirror.data(),
			data.batchDirty, data.uploadedBatches);
		data.instanceCount = static_cast<uint32_t>(data.instanceMirror.size());
		data.batchCount = static_cast<uint32_t>(data.batchMirror.size());
	}
	GpuScene::CullParams GpuScene::frustumParams(const FrustumModule::Frustum& frustum) const {
		CullParams params;
		params.shapeCount = 6;
		setPlane(params.shapes + 0, frustum.leftFace);
		setPlane(params.shapes + 4, frustum.rightFace);
		setPlane(params.shapes + 8, frustum.topFace);
		setPlane(params.shapes + 12, frustum.bottomFace);
		setPlane(params.shapes + 16, frustum.nearFace);
		setPlane(params.shapes + 20, frustum.farFace);
		return params;
	}

	bool GpuScene::cullFrustum(const CommandListHandle cmd, const List list, View& view, const FrustumModule::Frustum& frustum) {
		auto params = frustumParams(frustum);
		return cull(cmd, list, view, params);
	}

	bool GpuScene::cullShadowCasters(const CommandListHandle cmd, View& view, const FrustumModule::Frustum& volume,
		const FrustumModule::Frustum* camera, const Math::Vec3& lightTravel, const float texelsPerMetre) {
		auto params = frustumParams(volume);
		if (camera) {
			for (const auto* plane : { &camera->leftFace, &camera->rightFace, &camera->topFace, &camera->bottomFace, &camera->nearFace, &camera->farFace }) {
				// Travelling with the light never takes a point further inside this plane: a caster wholly outside it
				// casts nowhere inside. A metre of margin for the jitter and the cascade's own padding.
				if (Math::dot(plane->normal, lightTravel) <= 0.f && params.shapeCount < kMaxSpheres) {
					auto widened = *plane;
					widened.distance += 1.f;
					setPlane(params.shapes + params.shapeCount * 4, widened);
					++params.shapeCount;
				}
			}
		}
		params.texelsPerMetre = texelsPerMetre;
		// Half a texel across: a caster that small rarely covers a texel centre at all, so the map is the same
		// without it.
		params.minTexels = 0.5f;
		return cull(cmd, List::Shadow, view, params);
	}

	bool GpuScene::cullVisibleLastFrame(const CommandListHandle cmd, const List list, View& view, const FrustumModule::Frustum& frustum) {
		auto params = frustumParams(frustum);
		params.occlusion = 1;
		return cull(cmd, list, view, params);
	}

	bool GpuScene::cullOccluded(const CommandListHandle cmd, const List list, View& view, const FrustumModule::Frustum& frustum, const Math::Mat4& viewProj) {
		if (!view.pyramid) {
			return false;
		}
		auto params = frustumParams(frustum);
		std::memcpy(params.viewProj, &viewProj, sizeof(params.viewProj));
		params.occlusion = 2;
		params.depthWidth = view.depthWidth;
		params.depthHeight = view.depthHeight;
		params.levels = view.levels;
		std::copy(view.levelOffsets.begin(), view.levelOffsets.end(), params.levelOffsets);
		return cull(cmd, list, view, params);
	}

	void GpuScene::buildDepthPyramid(const CommandListHandle cmd, View& view, const TextureHandle depth, const SamplerHandle sampler, const uint32_t width, const uint32_t height,
		const uint32_t layer) {
		auto* rhi = Engine::instance()->rhi();
		if (!mHizPipeline || !depth || width == 0 || height == 0) {
			return;
		}
		// Level 0 is half the texture, and every level halves the one before, rounding down, to 1x1.
		const auto levelSize = [&](uint32_t level) {
			uint32_t w = (std::max)(width >> 1, 1u);
			uint32_t h = (std::max)(height >> 1, 1u);
			while (level--) {
				w = (std::max)(w >> 1, 1u);
				h = (std::max)(h >> 1, 1u);
			}
			return std::make_pair(w, h);
		};
		if (!view.pyramid || view.depthWidth != width || view.depthHeight != height) {
			view.depthWidth = width;
			view.depthHeight = height;
			view.levels = 0;
			size_t total = 0;
			while (view.levels < view.levelOffsets.size()) {
				const auto [w, h] = levelSize(view.levels);
				view.levelOffsets[view.levels++] = static_cast<uint32_t>(total);
				total += size_t{ w } * h;
				if (w == 1 && h == 1) {
					break;
				}
			}
			retire(view.pyramid);
			view.pyramid = createBuffer(total * sizeof(float), BufferUsage::Storage, CpuAccess::None, nullptr, "GpuScene depth pyramid");
		}

		const uint32_t slot = rhi->frameInFlight() % frameSlots();
		if (view.pyramidSets.size() < frameSlots()) {
			view.pyramidSets.resize(frameSlots());
			view.pyramidBoundIds.resize(frameSlots());
		}
		if (!view.pyramidSets[slot]) {
			view.pyramidSets[slot] = rhi->allocateDescriptorSet(mHizLayout);
		}
		const bool fromArray = layer != UINT32_MAX;
		const std::array<uint32_t, 3> ids = { depth.id, view.pyramid.id, fromArray ? 1u : 0u };
		if (ids != view.pyramidBoundIds[slot]) {
			view.pyramidBoundIds[slot] = ids;
			rhi->updateDescriptorSets({
				{ .set = view.pyramidSets[slot], .binding = 1, .images{ { fromArray ? mEmptyDepth : depth, fromArray ? mEmptySampler : sampler } } },
				{ .set = view.pyramidSets[slot], .binding = 2, .buffers{ { view.pyramid } } },
				{ .set = view.pyramidSets[slot], .binding = 3, .images{ { fromArray ? depth : mEmptyDepthArray, fromArray ? sampler : mEmptySampler } } },
			});
		}

		rhi->cmdBindPipeline(cmd, mHizPipeline);
		rhi->cmdBindDescriptorSets(cmd, mHizPipeline, 0, &view.pyramidSets[slot], 1);
		for (uint32_t level = 0; level < view.levels; ++level) {
			HizStep step;
			const auto [w, h] = levelSize(level);
			if (level == 0) {
				step.fromTexture = fromArray ? 2u : 1u;
				step.layer = fromArray ? layer : 0u;
				step.srcWidth = width;
				step.srcHeight = height;
			}
			else {
				std::tie(step.srcWidth, step.srcHeight) = levelSize(level - 1);
				step.srcOffset = view.levelOffsets[level - 1];
			}
			step.dstOffset = view.levelOffsets[level];
			step.dstWidth = w;
			step.dstHeight = h;
			rhi->cmdPushConstants(cmd, mHizPipeline, ShaderStage::Compute, &step, sizeof(step));
			rhi->cmdDispatch(cmd, (w + kLocalSize - 1) / kLocalSize, (h + kLocalSize - 1) / kLocalSize, 1);
			rhi->cmdPipelineBarrier(cmd, { PipelineStage::Compute, PipelineStage::Compute, AccessMask::ShaderWrite, AccessMask::ShaderRead });
		}
	}

	bool GpuScene::cullSpheres(const CommandListHandle cmd, const List list, View& view, const std::vector<Math::Vec4>& spheres) {
		const auto count = static_cast<uint32_t>((std::min)(spheres.size(), size_t{ kMaxSpheres }));
		CullParams params;
		params.mode = 1;
		params.shapeCount = count;
		for (uint32_t i = 0; i < count; ++i) {
			for (int c = 0; c < 4; ++c) {
				params.shapes[i * 4 + c] = spheres[i][c];
			}
		}
		return count != 0 && cull(cmd, list, view, params);
	}

	// Where the levels are chosen from, and how many pixels of screen height a unit of radius covers a unit
	// away. The player's camera and projection whatever the view being culled: a shadow cascade picking its
	// levels from the light would put the coarsest mesh in the shadow directly under the camera.
	void GpuScene::fillLodOrigin(CullParams& params) {
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		if (!renderSystem) {
			return;
		}
		const auto& data = renderSystem->getRenderData();
		const auto& screen = Engine::instance()->getWindow()->getScreenData();
		params.lodOrigin[0] = data.mCameraPos.x;
		params.lodOrigin[1] = data.mCameraPos.y;
		params.lodOrigin[2] = data.mCameraPos.z;
		// projection[1][1] is 1 / tan(fovY / 2).
		params.lodOrigin[3] = static_cast<float>(screen.renderH) * 0.5f * data.current.projection[1][1];
		params.lodPixels = (std::max)(data.graphics.lodPixels, 0.01f);
	}

	uint32_t GpuScene::prepareView(View& view, const uint32_t instances, const uint32_t commands, const CullParams& params) {
		static_assert(sizeof(CullParams) == 456);
		auto* rhi = Engine::instance()->rhi();
		if (!view.visible || view.visibleCapacity < instances) {
			retire(view.visible);
			view.visibleCapacity = instances + instances / 2;
			view.visible = createBuffer(view.visibleCapacity * 4 * sizeof(uint32_t), BufferUsage::Storage, CpuAccess::None, nullptr, "GpuScene visible");
		}
		if (!view.commands || view.commandCapacity < commands) {
			retire(view.commands);
			view.commandCapacity = commands + commands / 2;
			view.commands = createBuffer(view.commandCapacity * kCommandStride, BufferUsage::Storage | BufferUsage::Indirect, CpuAccess::None, nullptr, "GpuScene commands");
		}

		const uint32_t slot = rhi->frameInFlight() % frameSlots() * 2 + (params.occlusion == 2 ? 1u : 0u);
		if (view.params.size() < frameSlots() * 2) {
			view.params.resize(frameSlots() * 2);
			view.sets.resize(frameSlots() * 2);
			view.boundIds.resize(frameSlots() * 2);
		}
		if (!view.params[slot]) {
			view.params[slot] = createBuffer(sizeof(CullParams), BufferUsage::Storage, CpuAccess::Write, nullptr, "GpuScene view");
		}
		rhi->updateBuffer(view.params[slot], 0, sizeof(params), &params);
		return slot;
	}

	bool GpuScene::cullFrusta(const CommandListHandle cmd, const List list, const std::vector<View*>& views,
		const std::vector<FrustumModule::Frustum>& frusta) {
		const size_t count = (std::min)(views.size(), frusta.size());
		std::vector<int> slots(count);
		for (size_t i = 0; i < count; ++i) {
			auto params = frustumParams(frusta[i]);
			slots[i] = dispatchCull(cmd, list, *views[i], params);
			if (slots[i] < 0) {
				return false;
			}
		}
		if (count == 0) {
			return false;
		}
		auto* rhi = Engine::instance()->rhi();
		rhi->cmdPipelineBarrier(cmd, { PipelineStage::Compute, PipelineStage::Compute, AccessMask::ShaderWrite, AccessMask::ShaderRead | AccessMask::ShaderWrite });
		for (size_t i = 0; i < count; ++i) {
			pack(cmd, mLists[static_cast<size_t>(list)], *views[i], static_cast<uint32_t>(slots[i]), false);
		}
		rhi->cmdPipelineBarrier(cmd, {
			PipelineStage::Compute,
			PipelineStage::DrawIndirect | PipelineStage::VertexShader,
			AccessMask::ShaderWrite,
			AccessMask::IndirectCommandRead | AccessMask::ShaderRead,
		});
		return true;
	}

	bool GpuScene::cull(const CommandListHandle cmd, const List list, View& view, CullParams& params) {
		const int slot = dispatchCull(cmd, list, view, params);
		if (slot < 0) {
			return false;
		}
		auto* rhi = Engine::instance()->rhi();
		rhi->cmdPipelineBarrier(cmd, { PipelineStage::Compute, PipelineStage::Compute, AccessMask::ShaderWrite, AccessMask::ShaderRead | AccessMask::ShaderWrite });
		pack(cmd, mLists[static_cast<size_t>(list)], view, static_cast<uint32_t>(slot), params.occlusion == 2);
		rhi->cmdPipelineBarrier(cmd, {
			PipelineStage::Compute,
			PipelineStage::DrawIndirect | PipelineStage::VertexShader,
			AccessMask::ShaderWrite,
			AccessMask::IndirectCommandRead | AccessMask::ShaderRead,
		});
		return true;
	}

	int GpuScene::dispatchCull(const CommandListHandle cmd, const List list, View& view, CullParams& params) {
		const auto& data = mLists[static_cast<size_t>(list)];
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		const auto transforms = renderSystem ? renderSystem->transformsBuffer() : BufferHandle{};
		const auto bones = renderSystem ? renderSystem->bonesBuffer() : BufferHandle{};
		if (data.batchCount == 0 || !mCullPipeline || !mMeshes || !transforms || !bones) {
			return -1;
		}
		auto* rhi = Engine::instance()->rhi();
		params.batchCount = data.batchCount;
		const bool occlusion = params.occlusion != 0;
		if (occlusion && (!view.lastVisible || view.lastVisibleCapacity < data.instanceCount)) {
			// Starts with anything in it: that only picks the phase an instance is drawn in, for one frame.
			retire(view.lastVisible);
			view.lastVisibleCapacity = data.instanceCount + data.instanceCount / 2;
			view.lastVisible = createBuffer(view.lastVisibleCapacity * sizeof(uint32_t), BufferUsage::Storage, CpuAccess::None, nullptr, "GpuScene last visible");
		}
		fillLodOrigin(params);
		params.lodBias = view.lodBias + (list == List::Shadow ? 1u : 0u);
		// The G-buffer only: the shadow views draw the ground a level or two coarser already.
		if (list == List::GBuffer) {
			for (const auto& group : data.groups) {
				if (group.drawClass == ComponentsModule::DrawClass::Ground) {
					params.groundFirst = group.firstBatch;
					params.groundEnd = group.firstBatch + group.batchCount;
					params.groundLodDistance = renderSystem->getRenderData().graphics.terrainLodDistance;
					break;
				}
			}
		}
		view.secondPhaseOffset = data.batchCount;
		const uint32_t slot = prepareView(view, data.instanceCount, occlusion ? data.batchCount * 2 : data.batchCount, params);
		if (!view.sets[slot]) {
			view.sets[slot] = rhi->allocateDescriptorSet(mCullLayout);
		}
		const auto lastVisible = occlusion ? view.lastVisible : mEmptyBuffer;
		const auto pyramid = params.occlusion == 2 ? view.pyramid : mEmptyBuffer;

		// Rewritten only when a buffer was replaced: a write to a set that is already bound costs a copy.
		const std::array<uint32_t, 10> ids = { transforms.id, mMeshes.id, data.instances.id, data.batches.id, view.visible.id, view.commands.id, view.params[slot].id, lastVisible.id, pyramid.id, bones.id };
		if (ids != view.boundIds[slot]) {
			view.boundIds[slot] = ids;
			const auto set = view.sets[slot];
			rhi->updateDescriptorSets({
				{ .set = set, .binding = 1, .buffers{ { transforms } } },
				{ .set = set, .binding = 2, .buffers{ { bones } } },
				{ .set = set, .binding = 26, .buffers{ { mMeshes } } },
				{ .set = set, .binding = 27, .buffers{ { data.instances } } },
				{ .set = set, .binding = 28, .buffers{ { data.batches } } },
				{ .set = set, .binding = 29, .buffers{ { view.visible } } },
				{ .set = set, .binding = 30, .buffers{ { view.commands } } },
				{ .set = set, .binding = 31, .buffers{ { view.params[slot] } } },
				{ .set = set, .binding = 32, .buffers{ { lastVisible } } },
				{ .set = set, .binding = 33, .buffers{ { pyramid } } },
			});
		}

		rhi->cmdBindPipeline(cmd, mCullPipeline);
		rhi->cmdBindDescriptorSets(cmd, mCullPipeline, 0, &view.sets[slot], 1);
		rhi->cmdDispatch(cmd, (data.batchCount + kLocalSize - 1) / kLocalSize, 1, 1);
		view.list = list;
		return static_cast<int>(slot);
	}

	void GpuScene::pack(const CommandListHandle cmd, const ListData& data, View& view, const uint32_t slot, const bool secondPhase) {
		auto* rhi = Engine::instance()->rhi();
		const auto groups = static_cast<uint32_t>(data.groups.size());
		if (!view.counts || view.countCapacity < groups * 2) {
			retire(view.counts);
			view.countCapacity = groups * 2 + 32;
			view.counts = createBuffer(view.countCapacity * sizeof(uint32_t), BufferUsage::Storage | BufferUsage::Indirect, CpuAccess::None, nullptr, "GpuScene counts");
		}
		if (view.packCapacity < groups) {
			for (auto& params : view.packParams) {
				retire(params);
				params = {};
			}
			view.packCapacity = groups + 16;
		}
		if (view.packParams.size() < view.params.size()) {
			view.packParams.resize(view.params.size());
			view.packSets.resize(view.params.size());
			view.packBoundIds.resize(view.params.size());
		}
		if (!view.packParams[slot]) {
			view.packParams[slot] = createBuffer((4 + view.packCapacity * 2) * sizeof(uint32_t), BufferUsage::Storage, CpuAccess::Write, nullptr, "GpuScene pack");
		}
		std::vector<uint32_t> words(4 + groups * 2);
		words[0] = groups;
		words[1] = secondPhase ? view.secondPhaseOffset : 0u;
		words[2] = secondPhase ? groups : 0u;
		for (uint32_t g = 0; g < groups; ++g) {
			words[4 + g * 2] = data.groups[g].firstBatch;
			words[5 + g * 2] = data.groups[g].batchCount;
		}
		rhi->updateBuffer(view.packParams[slot], 0, words.size() * sizeof(uint32_t), words.data());
		if (!view.packSets[slot]) {
			view.packSets[slot] = rhi->allocateDescriptorSet(mPackLayout);
		}
		const std::array<uint32_t, 3> ids = { view.commands.id, view.counts.id, view.packParams[slot].id };
		if (ids != view.packBoundIds[slot]) {
			view.packBoundIds[slot] = ids;
			rhi->updateDescriptorSets({
				{ .set = view.packSets[slot], .binding = 30, .buffers{ { view.commands } } },
				{ .set = view.packSets[slot], .binding = 34, .buffers{ { view.counts } } },
				{ .set = view.packSets[slot], .binding = 35, .buffers{ { view.packParams[slot] } } },
			});
		}
		rhi->cmdBindPipeline(cmd, mPackPipeline);
		rhi->cmdBindDescriptorSets(cmd, mPackPipeline, 0, &view.packSets[slot], 1);
		rhi->cmdDispatch(cmd, groups, 1, 1);
	}

	bool GpuScene::sortGlass(const CommandListHandle cmd, View& view, const uint32_t group, const FrustumModule::Frustum& frustum,
		const Math::Vec3& cameraPos, const Math::Vec3& viewDir) {
		const auto& data = mLists[static_cast<size_t>(List::Glass)];
		auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
		const auto transforms = renderSystem ? renderSystem->transformsBuffer() : BufferHandle{};
		const auto [base, count] = glassRange(group);
		if (count == 0 || !mGlassPipeline || !mMeshes || !transforms) {
			return false;
		}
		auto* rhi = Engine::instance()->rhi();

		uint32_t padded = 1;
		while (padded < count) {
			padded <<= 1;
		}
		if (!view.keys || view.keyCapacity < padded) {
			retire(view.keys);
			view.keyCapacity = padded;
			view.keys = createBuffer(padded * 2 * sizeof(uint32_t), BufferUsage::Storage, CpuAccess::None, nullptr, "GpuScene glass keys");
		}

		float shapes[32]{};
		setPlane(shapes + 0, frustum.leftFace);
		setPlane(shapes + 4, frustum.rightFace);
		setPlane(shapes + 8, frustum.topFace);
		setPlane(shapes + 12, frustum.bottomFace);
		setPlane(shapes + 16, frustum.nearFace);
		setPlane(shapes + 20, frustum.farFace);
		for (int i = 0; i < 3; ++i) {
			shapes[24 + i] = cameraPos[i];
			shapes[28 + i] = viewDir[i];
		}
		CullParams params;
		params.shapeCount = 8;
		std::memcpy(params.shapes, shapes, sizeof(shapes));
		const uint32_t slot = prepareView(view, count, count, params);
		if (!view.sets[slot]) {
			view.sets[slot] = rhi->allocateDescriptorSet(mGlassLayout);
		}

		const std::array<uint32_t, 10> ids = { transforms.id, mMeshes.id, data.instances.id, view.keys.id, view.visible.id, view.commands.id, view.params[slot].id, 1, 0, 0 };
		if (ids != view.boundIds[slot]) {
			view.boundIds[slot] = ids;
			const auto set = view.sets[slot];
			rhi->updateDescriptorSets({
				{ .set = set, .binding = 1, .buffers{ { transforms } } },
				{ .set = set, .binding = 26, .buffers{ { mMeshes } } },
				{ .set = set, .binding = 27, .buffers{ { data.instances } } },
				{ .set = set, .binding = 29, .buffers{ { view.visible } } },
				{ .set = set, .binding = 30, .buffers{ { view.commands } } },
				{ .set = set, .binding = 31, .buffers{ { view.params[slot] } } },
				{ .set = set, .binding = 32, .buffers{ { view.keys } } },
			});
		}

		struct GlassStep {
			uint32_t stage = 0;
			uint32_t count = 0;
			uint32_t paddedCount = 0;
			uint32_t j = 0;
			uint32_t k = 0;
			uint32_t base = 0;
		} step;
		step.count = count;
		step.base = base;
		step.paddedCount = padded;

		rhi->cmdBindPipeline(cmd, mGlassPipeline);
		rhi->cmdBindDescriptorSets(cmd, mGlassPipeline, 0, &view.sets[slot], 1);
		const auto dispatch = [&](const uint32_t invocations) {
			rhi->cmdPushConstants(cmd, mGlassPipeline, ShaderStage::Compute, &step, sizeof(step));
			rhi->cmdDispatch(cmd, (invocations + kGlassLocalSize - 1) / kGlassLocalSize, 1, 1);
			// Each stage reads what the previous one wrote.
			rhi->cmdPipelineBarrier(cmd, { PipelineStage::Compute, PipelineStage::Compute, AccessMask::ShaderWrite, AccessMask::ShaderRead });
		};

		dispatch(padded);
		step.stage = 1;
		for (step.k = 2; step.k <= padded; step.k <<= 1) {
			for (step.j = step.k >> 1; step.j > 0; step.j >>= 1) {
				dispatch(padded);
			}
		}
		step.stage = 2;
		dispatch(count);

		rhi->cmdPipelineBarrier(cmd, {
			PipelineStage::Compute,
			PipelineStage::DrawIndirect | PipelineStage::VertexShader,
			AccessMask::ShaderWrite,
			AccessMask::IndirectCommandRead | AccessMask::ShaderRead,
		});
		return true;
	}

	std::pair<uint32_t, uint32_t> GpuScene::glassRange(const uint32_t group) const {
		const auto& data = mLists[static_cast<size_t>(List::Glass)];
		if (group >= data.groups.size() || data.groups[group].batchCount == 0) {
			return { 0, 0 };
		}
		const auto& batch = data.batchMirror[data.groups[group].firstBatch];
		return { batch.firstInstance, batch.instanceCount };
	}

	void GpuScene::drawSorted(const CommandListHandle cmd, const View& view, const uint32_t group) const {
		const auto count = glassRange(group).second;
		if (count == 0) {
			return;
		}
		Engine::instance()->rhi()->cmdDrawIndexedIndirect(cmd, view.commands, 0, count, kCommandStride);
		SystemsModule::RenderDebugInfo::instance()->drawCalls++;
	}

	void GpuScene::draw(const CommandListHandle cmd, const View& view, const DrawGroup& group, const bool secondPhase) const {
		if (group.batchCount == 0) {
			return;
		}
		const auto first = group.firstBatch + (secondPhase ? view.secondPhaseOffset : 0u);
		const auto& groups = mLists[static_cast<size_t>(view.list)].groups;
		const auto index = static_cast<size_t>(&group - groups.data());
		if (view.counts && index < groups.size()) {
			Engine::instance()->rhi()->cmdDrawIndexedIndirectCount(cmd, view.commands, first * kCommandStride, view.counts,
				(index + (secondPhase ? groups.size() : 0)) * sizeof(uint32_t), group.batchCount, kCommandStride);
		}
		else {
			Engine::instance()->rhi()->cmdDrawIndexedIndirect(cmd, view.commands, first * kCommandStride, group.batchCount, kCommandStride);
		}
		SystemsModule::RenderDebugInfo::instance()->drawCalls++;
	}
}
