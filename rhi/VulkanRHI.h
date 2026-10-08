#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>
#define VMA_VULKAN_VERSION 1002000
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include "vk_mem_alloc.h"

#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render
{
	struct VkBufferResource {
		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		BufferDesc desc{};
		void* mapped = nullptr;
		bool deviceLocal = false;
		bool persistentMapped = false;
		uint32_t generation = 0;
		bool live = false;
	};

	struct PendingBufferUpload {
		uint32_t dstId = UINT32_MAX;
		VkDeviceSize dstOffset = 0;
		VkDeviceSize srcOffset = 0;
		VkDeviceSize size = 0;
	};

	// A texture's region written through the frame's upload ring, as a device-local buffer's is (queueTextureUpload).
	struct PendingTextureUpload {
		uint32_t dstId = UINT32_MAX;
		VkDeviceSize srcOffset = 0;
		uint32_t x = 0;
		uint32_t y = 0;
		uint32_t w = 0;
		uint32_t h = 0;
		uint32_t mip = 0;
		uint32_t layer = 0;
	};

	struct VkTextureResource {
		VkImage image = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		TextureDesc desc{};
		uint32_t generation = 0;
		bool live = false;
	};

	struct VkSamplerResource {
		VkSampler sampler = VK_NULL_HANDLE;
		SamplerDesc desc{};
		uint32_t generation = 0;
		bool live = false;
	};

	struct VkDescriptorSetLayoutResource {
		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		DescriptorSetLayoutDesc desc{};
		uint32_t generation = 0;
		bool live = false;
	};

	struct VkDescriptorSetResource {
		VkDescriptorSet set = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE; // the pool it came from, which frees it
		// What each binding was last written with, as RHI handle ids (which carry a generation) plus buffer
		// offset and range: a write repeating it is skipped, so a set bound by the frame in flight is not copied.
		std::vector<std::pair<uint32_t, std::vector<uint64_t>>> written;
		DescriptorSetLayoutHandle layoutHandle{};
		uint32_t generation = 0;
		bool live = false;
	};

	struct VkRecordedCommandList {
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkPipelineLayout boundLayout = VK_NULL_HANDLE;
		VkPipelineBindPoint boundBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		CommandListType type = CommandListType::Graphics;
		FramebufferHandle activeFramebuffer{};
		int passWidth = 0;
		int passHeight = 0;
		bool recording = false;
		uint32_t generation = 0;
		bool live = false;
		bool submitted = false;
		std::vector<PipelineBarrier> barriers;
		std::vector<VkDescriptorSet> boundSets;
	};

	struct VkShaderModuleResource {
		VkShaderModule module = VK_NULL_HANDLE;
		ShaderModuleDesc desc{};
		// Modules are shared per SPIR-V name; every create is one reference, every destroy drops one.
		uint32_t refs = 0;
		uint32_t generation = 0;
		bool live = false;
	};

	struct VkGraphicsPipelineResource {
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		GraphicsPipelineDesc desc{};
		ComputePipelineDesc computeDesc{};
		RenderPassHandle ownedRenderPass{};
		const char* label = nullptr; // in the GPU's spans (pipelineLabel)
		bool isCompute = false;
		uint32_t generation = 0;
		bool live = false;
	};

	struct VkRenderPassResource {
		VkRenderPass renderPass = VK_NULL_HANDLE;
		RenderPassDesc desc{};
		uint32_t generation = 0;
		bool live = false;
	};

		struct VkFramebufferResource {
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		VkRenderPass vkRenderPass = VK_NULL_HANDLE;
		FramebufferDesc desc{};
		RenderPassHandle ownedRenderPass{};
		// Views onto a single array layer, created when an attachment pins one. The texture's own
		// view covers the whole array, so rendering through it would ignore the requested layer.
		std::vector<VkImageView> ownedViews;
		bool swapchainTarget = false;
		uint32_t generation = 0;
		bool live = false;
	};

	class VulkanRHI final : public RenderHardwareInterface
	{
	public:
		VulkanRHI();
		~VulkanRHI() override;

		GraphicsBackend backend() const override { return GraphicsBackend::Vulkan; }
		void setPresentBlitSource(TextureHandle texture, SamplerHandle sampler) override;
		bool readbackTexture(TextureHandle tex, std::vector<uint8_t>& rgba, int& width, int& height, uint32_t layer = 0) override;
		TextureHandle presentSourceTexture() const override { return m_presentBlitTexture; }
		void beginFrame() override;
		uint32_t frameInFlight() const override { return m_currentFrame; }
		uint32_t framesInFlight() const override { return kMaxFramesInFlight; }

		void init() override;
		void cleanup() override;
		void waitIdle() override;

		PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
		PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;
		size_t reloadShaders() override;
		void destroyGraphicsPipeline(PipelineHandle pipeline) override;
		TextureHandle createTexture(const TextureDesc& desc, const void* data = nullptr) override;
		bool preparesTextures() const override { return true; }
		std::unique_ptr<PreparedTexture> prepareTexture(const TextureDesc& desc, const std::vector<const void*>& levels) override;
		TextureHandle adoptTexture(std::unique_ptr<PreparedTexture> texture) override;
		void destroyTexture(TextureHandle texture) override;
		SamplerHandle createSampler(const SamplerDesc& desc) override;
		void destroySampler(SamplerHandle sampler) override;
		ShaderModuleHandle createShaderModule(const ShaderModuleDesc& desc) override;
		void destroyShaderModule(ShaderModuleHandle handle) override;
		FramebufferHandle createFramebuffer(const FramebufferDesc& desc) override;
		void destroyFramebuffer(FramebufferHandle framebuffer) override;
		FramebufferHandle getBackbufferFramebuffer() override;
		BufferHandle createBuffer(const BufferDesc& desc, const void* initialData = nullptr) override;
		void destroyBuffer(BufferHandle handle) override;
		void updateBuffer(BufferHandle handle, size_t offset = 0, size_t size = 0, const void* data = nullptr) override;
		void* mapBuffer(BufferHandle handle, MapMode mode) override;
		TransientAllocation allocTransient(size_t size) override;
		void unmapBuffer(BufferHandle handle) override;
		RenderPassHandle createRenderPass(const RenderPassDesc& desc) override;
		void destroyRenderPass(RenderPassHandle renderPass) override;
		DescriptorSetLayoutHandle createDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override;
		void destroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) override;
		DescriptorSetHandle allocateDescriptorSet(DescriptorSetLayoutHandle layout) override;
		void freeDescriptorSet(DescriptorSetHandle set) override;
		void updateDescriptorSets(const std::vector<WriteDescriptor>& writes) override;
		CommandListHandle beginCommandList(CommandListType type) override;
		void endCommandList(CommandListHandle cmd) override;
		void cmdBeginRenderPass(CommandListHandle cmd, const RenderPassBeginInfo& beginInfo) override;
		void cmdEndRenderPass(CommandListHandle cmd) override;
		void cmdBindPipeline(CommandListHandle cmd, PipelineHandle pipeline) override;
		void cmdBindVertexBuffer(CommandListHandle cmd, uint32_t slot, BufferHandle handle, uint32_t offset = 0) override;
		void cmdBindIndexBuffer(CommandListHandle cmd, BufferHandle handle, IndexType type, uint32_t offset = 0) override;
		void cmdBindDescriptorSets(CommandListHandle cmd, PipelineHandle layout, uint32_t firstSet, const DescriptorSetHandle* sets, uint32_t setCount,
			const uint32_t* dynamicOffsets = nullptr, uint32_t dynamicOffsetCount = 0) override;
		void cmdPushConstants(CommandListHandle cmd, PipelineHandle pipeline, ShaderStage stages, const void* data, uint32_t size) override;
		void cmdDraw(CommandListHandle cmd, uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) override;
		void cmdDrawIndexed(CommandListHandle cmd, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) override;
		void cmdDrawIndirect(CommandListHandle cmd, BufferHandle buffer, size_t offset = 0, uint32_t drawCount = 1, uint32_t stride = 16) override;
		void cmdDrawIndexedIndirect(CommandListHandle cmd, BufferHandle buffer, size_t offset = 0, uint32_t drawCount = 1, uint32_t stride = 20) override;
		void cmdDrawIndexedIndirectCount(CommandListHandle cmd, BufferHandle buffer, size_t offset, BufferHandle countBuffer, size_t countOffset, uint32_t maxDrawCount, uint32_t stride = 20) override;
		void cmdDispatch(CommandListHandle cmd, uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1) override;
		void cmdBlit(CommandListHandle cmd, FramebufferHandle src, FramebufferHandle dst, BlitMask mask = BlitMask::Color, BlitFilter filter = BlitFilter::Nearest) override;
		void cmdSetScissor(CommandListHandle cmd, int32_t x, int32_t y, uint32_t width, uint32_t height) override;
		void cmdPipelineBarrier(CommandListHandle cmd, const PipelineBarrier& barrier) override;
		void cmdUpdateBuffer(CommandListHandle cmd, BufferHandle handle, size_t offset, size_t size, const void* data) override;
		void cmdCopyBuffer(CommandListHandle cmd, BufferHandle src, BufferHandle dst, size_t srcOffset, size_t dstOffset, size_t size) override;
		void submit(CommandListHandle list) override;
		void present() override;
		double lastGpuMs() const override { return mLastGpuMs; }
		void writeGpuMark(CommandListHandle cmd, const char* label, bool endsPass) override;
		const char* deviceName() const override { return m_deviceName.c_str(); }
		void updateTexture(TextureHandle tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, TextureFormat fmt, const void* data, uint32_t mip, uint32_t layer) override;
		void updateCubeFace(TextureHandle tex, uint32_t face, uint32_t mip, const void* data) override;
		void updateCubeMip(TextureHandle tex, uint32_t mip, const void* packed6Faces, size_t faceBytes) override;
		uint64_t getNativeTextureId(TextureHandle tex) const override;

	private:
		// Ids carry the slot's generation (see handleSlot): descriptor writes are cached by id, and a set written
		// for a destroyed resource must not look up to date for the one that reuses its slot.
		template<typename T>
		static uint32_t storeResource(std::vector<T>& pool, std::vector<uint32_t>& freeList, T value) {
			if (!freeList.empty()) {
				const uint32_t slot = freeList.back();
				freeList.pop_back();
				value.generation = pool[slot].generation;
				pool[slot] = std::move(value);
				return slot | (pool[slot].generation << kHandleSlotBits);
			}
			if (pool.size() >= kHandleSlotMask) {
				return UINT32_MAX;
			}
			pool.push_back(std::move(value));
			return static_cast<uint32_t>(pool.size() - 1);
		}

		template<typename T>
		static T* getLive(std::vector<T>& pool, uint32_t id) {
			const uint32_t slot = handleSlot(id);
			if (slot >= pool.size() || !pool[slot].live || pool[slot].generation != handleGeneration(id)) {
				return nullptr;
			}
			return &pool[slot];
		}

		template<typename T>
		static const T* getLive(const std::vector<T>& pool, uint32_t id) {
			const uint32_t slot = handleSlot(id);
			if (slot >= pool.size() || !pool[slot].live || pool[slot].generation != handleGeneration(id)) {
				return nullptr;
			}
			return &pool[slot];
		}

		template<typename T>
		static bool recycleResource(std::vector<T>& pool, std::vector<uint32_t>& freeList, uint32_t id) {
			if (!getLive(pool, id)) {
				return false;
			}
			const uint32_t slot = handleSlot(id);
			const uint32_t generation = pool[slot].generation;
			pool[slot] = T{};
			pool[slot].generation = generation + 1;
			// A slot whose generation has run out is never handed out again, so no id ever repeats; it costs one
			// dead slot per 4096 recycles.
			if (generation + 1 < kHandleGenerationMask) {
				freeList.push_back(slot);
			}
			return true;
		}

		uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
		void destroyBufferInternal(VkBufferResource& buffer);
		void createImage(VkTextureResource& out, const TextureDesc& desc, const void* data);
		// The image and its view, nothing in them yet. Any thread.
		void createImageObjects(VkTextureResource& out, const TextureDesc& desc, uint32_t& mipLevels, uint32_t& arrayLayers);
		void destroyTextureInternal(VkTextureResource& texture);
		void transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t layerCount = 1, uint32_t mipLevels = 1);
		void ensureTextureLayout(VkCommandBuffer cmd, VkTextureResource& texture, VkImageLayout newLayout);
		void generateMipmaps(VkCommandBuffer cmd, VkTextureResource& texture);
		void copyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage image, uint32_t width, uint32_t height, uint32_t layer = 0, uint32_t mip = 0);
		void blitTextureToTexture(VkCommandBuffer cmd, VkTextureResource& src, VkTextureResource& dst);
		void blitToSwapchain(VkCommandBuffer cmd, VkTextureResource& src);
		void ensureBackbufferHandle();
		// Records `fn` into a one-off command buffer and waits for it on the graphics queue. The shared upload pool and
		// fence are the device thread's; any other thread brings its own.
		void executeImmediate(const std::function<void(VkCommandBuffer)>& fn);
		void executeImmediate(const std::function<void(VkCommandBuffer)>& fn, VkCommandPool pool, VkFence fence);
		void createDescriptorPool();
		void destroyDescriptorPool();
		void expandSetLayouts(const std::vector<DescriptorSetLayoutHandle>& handles, std::vector<VkDescriptorSetLayout>& out) const;
		uint32_t inferFramebufferLayers(const FramebufferDesc& desc) const;
		bool realizeFramebuffer(VkFramebufferResource& fb, VkRenderPass vkPass);
		RenderPassHandle synthesizeGraphicsRenderPass(const GraphicsPipelineDesc& desc);
		VkBuffer createStagingBuffer(size_t size, VmaAllocation& allocation, const void* data);
		void copyThroughStaging(VkBuffer dst, size_t offset, size_t size, const void* data);
		void destroyStagingBuffer(VkBuffer buffer, VmaAllocation allocation);
		void createAllocator();
		void destroyAllocator();
		void createUploadRings();
		void destroyUploadRings();
		void waitForInFlightFrame();
		void createTimestampQueries();
		void destroyTimestampQueries();
		void collectGpuTimestamp();
		void queueBufferUpload(uint32_t bufferId, size_t offset, size_t size, const void* data);
		bool queueTextureUpload(uint32_t textureId, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t mip, uint32_t layer,
			const void* data, size_t size);
		void recordTextureUploads(VkCommandBuffer cmd);
		void flushBufferUploads(VkCommandBuffer cmd);

		void createInstance();
		void setupDebugMessenger();
		void createSurface();
		void pickPhysicalDevice();
		void createDevice();
		void createSwapchain();
		void createImageViews();
		void createSwapchainRenderPass();
		void createFramebuffers();
		void createCommandPool();
		void createSyncObjects();
		void createSwapchainSyncObjects();
		void destroySwapchainSyncObjects();
		void createCommandBuffers();
		void createSceneCommandPool();
		void createBlitPipeline();
		void destroyBlitPipeline();
		void createPipelineCache();
		void savePipelineCache();
		void destroyPipelineCache();
		void recordPresentBlit(VkCommandBuffer cmd, uint32_t imageIndex);
		void recycleSceneList(CommandListHandle handle);
		bool prepareInlinePresent();
		void submitInlinePresent(VkCommandBuffer cmd);
		void cleanupSwapchain();
		void recreateSwapchain();
		void recordClearCommandBuffer(uint32_t imageIndex);
		void drawFrame();

		bool m_inited = false;
		bool m_framebufferResized = false;
		bool m_samplerAnisotropy = false;
		float m_maxAnisotropy = 1.f;
		bool m_fillModeNonSolid = false;
		bool m_hostQueryReset = false;
		bool m_negativeOneToOneDepth = false;
		uint32_t m_currentFrame = 0;

		VkInstance m_instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
		VkSurfaceKHR m_surface = VK_NULL_HANDLE;
		VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
		std::string m_deviceName;
		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		VkQueue m_graphicsQueue = VK_NULL_HANDLE;
		VkQueue m_presentQueue = VK_NULL_HANDLE;
		std::mutex m_queueMutex;
		uint32_t m_graphicsQueueFamily = 0;
		uint32_t m_presentQueueFamily = 0;

		VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
		VkFormat m_swapchainFormat = VK_FORMAT_B8G8R8A8_SRGB;
		VkExtent2D m_swapchainExtent{};
		std::vector<VkImage> m_swapchainImages;
		std::vector<VkImageView> m_swapchainImageViews;
		std::vector<VkFramebuffer> m_swapchainFramebuffers;

		VkRenderPass m_swapchainRenderPass = VK_NULL_HANDLE;
		VkCommandPool m_commandPool = VK_NULL_HANDLE;
		VkCommandPool m_uploadCommandPool = VK_NULL_HANDLE;
		VkCommandPool m_sceneCommandPool = VK_NULL_HANDLE;
		VkCommandBuffer m_sceneCmds[2]{};
		bool m_swapchainTransferDst = false;
		VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
		VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
		std::vector<VkCommandBuffer> m_commandBuffers;
		VkFence m_sceneFence = VK_NULL_HANDLE;
		VkFence m_uploadFence = VK_NULL_HANDLE;

		VkPipeline m_blitPipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_blitPipelineLayout = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_blitSetLayout = VK_NULL_HANDLE;
		// One per frame slot: the other slot's present can still be pending while this one is rewritten.
		VkDescriptorSet m_blitDescriptorSets[2]{};
		VkSampler m_blitDefaultSampler = VK_NULL_HANDLE;
		TextureHandle m_presentBlitTexture{};
		SamplerHandle m_presentBlitSampler{};
		bool m_hasPendingScene = false;
		CommandListHandle m_pendingSceneCmd{};
		CommandListHandle m_inflightSceneCmd[2]{};
		uint32_t m_acquiredImageIndex = 0;
		bool m_swapchainAcquired = false;
		bool m_inlinePresent = false;
		bool m_presentedThisFrame = false;
		bool m_swapchainBlitted = false;
		bool m_frameWaited = false;
		FramebufferHandle m_backbufferHandle{};
		std::vector<VkImageLayout> m_swapchainLayouts;

		static constexpr int kMaxFramesInFlight = 2;
		static constexpr uint32_t kTimestampsPerFrame = 2;
		static constexpr VkDeviceSize kUploadRingSize = 8 * 1024 * 1024;
		VkQueryPool m_timestampPool = VK_NULL_HANDLE;
		// cmdGpuMark's timestamps: kMaxGpuMarks a frame slot, how many the slot's frame wrote, and what each one ends.
		VkQueryPool m_markPool = VK_NULL_HANDLE;
		uint32_t m_markCount[kMaxFramesInFlight]{};
		GpuMarkLabel m_markLabels[kMaxFramesInFlight][kMaxGpuMarks]{};
		float m_timestampPeriod = 0.f;
		uint32_t m_timestampValidBits = 0;
		bool m_timestampPending[kMaxFramesInFlight]{};
		// Sets replaced while still bound, freed once the frame that bound them has finished.
		std::vector<std::pair<VkDescriptorPool, VkDescriptorSet>> m_retiredSets[kMaxFramesInFlight];
		// Pools are added when the newest one is full; sets are allocated from the newest.
		std::vector<VkDescriptorPool> m_descriptorPools;
		VkDescriptorSet allocateVkDescriptorSet(VkDescriptorSetLayout layout, VkDescriptorPool& pool);
		// Destroyed buffers an in-flight frame may still bind: freed after every frame recorded before the destroy
		// has finished, which takes one wait per frame slot.
		struct RetiredBuffer {
			VkBuffer buffer = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			int waitsLeft = 0;
		};
		std::vector<RetiredBuffer> m_retiredBuffers;
		// Destroyed textures, the same way: a set an in-flight frame binds may still hold the image.
		struct RetiredTexture {
			VkTextureResource texture;
			int waitsLeft = 0;
		};
		std::vector<RetiredTexture> m_retiredTextures;
		// updateDescriptorSets' working storage. Where each accepted write's infos begin: the pointers are filled
		// in only after every push_back, since an array descriptor pushes several infos and the vectors can grow.
		struct DescriptorWriteScratch {
			struct InfoSpan {
				size_t first = 0;
				bool images = false;
			};
			std::vector<VkWriteDescriptorSet> writes;
			std::vector<VkDescriptorBufferInfo> buffers;
			std::vector<VkDescriptorImageInfo> images;
			std::vector<InfoSpan> spans;
		} m_writeScratch;
		// allocTransient: one mapped buffer with a region per frame slot, so a set written for it once stays
		// valid while each frame writes its own region. An allocation that does not fit gets a buffer of its
		// own, destroyed when the slot comes round again, and the region grows to the peak at that point.
		BufferHandle m_transientBuffer{};
		VkDeviceSize m_transientRegion = 0;
		VkDeviceSize m_transientAlign = 16;
		VkDeviceSize m_transientPeak = 0;
		VkDeviceSize m_transientUsed[kMaxFramesInFlight]{};
		std::vector<BufferHandle> m_transientOverflow[kMaxFramesInFlight];
		BufferHandle createTransientBuffer(VkDeviceSize size);
		void resetTransient();
		// Several lists can run in one frame; the frame keeps the first list's start and the last one's end.
		bool m_timestampStartWritten[kMaxFramesInFlight]{};
		bool m_timestampEndWritten[kMaxFramesInFlight]{};
		double mLastGpuMs = 0.0;
		VkBuffer m_uploadRing[kMaxFramesInFlight]{};
		VmaAllocation m_uploadRingAlloc[kMaxFramesInFlight]{};
		void* m_uploadRingMapped[kMaxFramesInFlight]{};
		VkDeviceSize m_uploadRingUsed = 0;
		bool m_uploadsFlushed = false;
		std::vector<PendingBufferUpload> m_uploadOps;
		std::vector<PendingTextureUpload> m_textureUploadOps;

		std::vector<VkSemaphore> m_imageAvailableSemaphores;
		std::vector<VkSemaphore> m_renderFinishedSemaphores;
		std::vector<VkFence> m_inFlightFences;
		std::vector<VkFence> m_imagesInFlight;

		std::vector<VkBufferResource> m_buffers;
		std::vector<uint32_t> m_freeBuffers;
		std::vector<VkTextureResource> m_textures;
		std::vector<uint32_t> m_freeTextures;
		std::vector<VkSamplerResource> m_samplers;
		std::vector<uint32_t> m_freeSamplers;
		std::vector<VkDescriptorSetLayoutResource> m_descriptorSetLayouts;
		std::vector<uint32_t> m_freeDescriptorSetLayouts;
		std::vector<VkDescriptorSetResource> m_descriptorSets;
		std::vector<uint32_t> m_freeDescriptorSets;
		std::vector<VkRecordedCommandList> m_commandLists;
		CommandListHandle m_openCommandList{};
		VkRecordedCommandList* listInPass(CommandListHandle cmd, const char* command);
		void finishPendingScene();
		bool swapchainOutOfDate() const;
		std::vector<uint32_t> compileShader(ShaderStage stage, const std::string& name, const std::string& source);
		// A pipeline's device objects from its desc, stored nowhere: what create and reloadShaders both make.
		bool buildGraphicsPipeline(const GraphicsPipelineDesc& desc, VkGraphicsPipelineResource& out);
		bool buildComputePipeline(const ComputePipelineDesc& desc, VkGraphicsPipelineResource& out);
		bool boundInUnfinishedList(VkDescriptorSet set) const;
		void detachBoundSet(VkDescriptorSetResource& set, const VkDescriptorSetLayoutResource& layout);
		std::vector<uint32_t> m_freeCommandLists;
		std::vector<VkShaderModuleResource> m_shaders;
		std::vector<uint32_t> m_freeShaders;
		std::unordered_map<std::string, ShaderModuleHandle> m_shaderByName;
		std::vector<VkGraphicsPipelineResource> m_pipelines;
		std::vector<uint32_t> m_freePipelines;
		std::vector<VkRenderPassResource> m_renderPasses;
		std::vector<uint32_t> m_freeRenderPasses;
		std::vector<VkFramebufferResource> m_framebuffers;
		std::vector<uint32_t> m_freeFramebuffers;
	};
}
