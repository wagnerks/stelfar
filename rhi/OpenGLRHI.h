#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <rhi/RenderHardwareInterface.h>

namespace SFE::Render
{
	using GLenum = unsigned int;
	using GLuint = unsigned int;
	using GLint = int;
	using GLbitfield = unsigned int;

	struct GLTexture { unsigned id = 0; TextureDesc desc{}; bool live = false; };
	struct GLSampler { unsigned id = 0; SamplerDesc desc{}; bool live = false; };
	struct GLFramebuffer { unsigned id = 0; FramebufferDesc desc{}; bool live = false; };
	struct GLBuffer { unsigned id = 0; GLenum target = 0; BufferDesc desc{}; bool live = false; };
	struct GLVertexArray { GLuint id = 0; VertexLayoutDesc desc; };
	struct GLShaderModule { GLuint id = 0; ShaderModuleDesc desc{}; bool live = false; };
	struct GLRenderPass
	{
		RenderPassDesc desc{};
		bool live = false;
	};
	struct GLDescriptorSetLayout
	{
		DescriptorSetLayoutDesc desc{};
		bool live = false;
	};

	struct GLGraphicsPipeline
	{
		GLuint programId = 0;
		GLuint vaoId = 0;
		const char* label = nullptr; // in the GPU's spans (pipelineLabel)

		// кешируем GL-формы состояний
		GLenum primitiveGL;
		GLenum cullModeGL = 0;          // 0 = no cull
		GLenum frontFaceGL;
		GLenum fillModeGL;
		GLenum depthFuncGL;
		uint32_t patchControlPoints = 0;

		bool  depthTestEnabled = true;
		bool  depthWriteEnabled = true;

		bool  blendEnabled = false;
		GLenum blendSrcGL;
		GLenum blendDstGL;
		GLenum blendSrcAlphaGL;
		GLenum blendDstAlphaGL;

		GraphicsPipelineDesc desc; // опционально — для дебага / реконфига
		bool isCompute = false;
		ShaderModuleHandle computeShader{}; // what a compute pipeline was linked from, to link it again

		std::unordered_map<std::string, GLint> uniformLocations;
		bool live = false;
	};

	struct GLDescriptorBindingData
	{
		uint32_t binding = 0;
		DescriptorType type;
		std::vector<DescriptorBufferInfo> buffers;
		std::vector<DescriptorImageInfo>  images;
	};

	using GLDescriptorBindings = std::vector<GLDescriptorBindingData>;

	struct GLDescriptorSet
	{
		DescriptorSetLayoutDesc layout{};
		// Shared with the binds recorded from it: a bind keeps the contents it saw, and a later write copies
		// them first instead of every bind copying them.
		std::shared_ptr<GLDescriptorBindings> bindings = std::make_shared<GLDescriptorBindings>();
		bool live = false;
	};

	struct CmdBeginRenderPass {
		RenderPassBeginInfo info{};
		RhiVec4 clearColors[8]{};
		float clearDepth = 1.f;
		uint8_t clearStencil = 0;
		bool hasOverrideColors = false;
		bool hasOverrideDepth = false;
		bool hasOverrideStencil = false;
	};
	struct CmdEndRenderPass {};
	struct CmdBindPipeline { PipelineHandle pipeline; };
	struct CmdBindVertexBuffer { uint32_t slot = 0; BufferHandle handle; uint32_t offset = 0; };
	struct CmdBindIndexBuffer { BufferHandle handle; IndexType type = IndexType::Uint16; uint32_t offset = 0; };
	struct CmdBindDescriptorSets {
		static constexpr size_t kMaxSets = 4;
		static constexpr size_t kMaxDynamic = 16;
		std::array<std::shared_ptr<const GLDescriptorBindings>, kMaxSets> sets{};
		uint8_t setCount = 0;
		// Dynamic offsets by set and binding number, applied when the bind is replayed.
		struct DynamicOffset { uint8_t set = 0; uint32_t binding = 0; uint32_t element = 0; uint32_t offset = 0; };
		std::array<DynamicOffset, kMaxDynamic> dynamic{};
		uint8_t dynamicCount = 0;
	};
	struct CmdPushConstants { uint32_t offset = 0; uint32_t size = 0; }; // into GLCommandList::pushData
	struct CmdDraw { uint32_t vertexCount = 0, instanceCount = 1, firstVertex = 0, firstInstance = 0; };
	struct CmdDrawIndexed { uint32_t indexCount = 0, instanceCount = 1, firstIndex = 0; int32_t vertexOffset = 0; uint32_t firstInstance = 0; };
	struct CmdDrawIndirect { BufferHandle buffer; size_t offset = 0; uint32_t drawCount = 1; uint32_t stride = 16; };
	struct CmdDrawIndexedIndirect { BufferHandle buffer; size_t offset = 0; uint32_t drawCount = 1; uint32_t stride = 20; };
	struct CmdDrawIndexedIndirectCount { BufferHandle buffer; size_t offset = 0; BufferHandle countBuffer; size_t countOffset = 0; uint32_t maxDrawCount = 0; uint32_t stride = 20; };
	struct CmdDispatch { uint32_t groupCountX = 1, groupCountY = 1, groupCountZ = 1; };
	struct CmdBlit { FramebufferHandle src; FramebufferHandle dst; BlitMask mask = BlitMask::Color; BlitFilter filter = BlitFilter::Nearest; };
	struct CmdSetScissor { int32_t x = 0, y = 0; uint32_t width = 0, height = 0; };
	struct CmdPipelineBarrier { PipelineBarrier barrier; };
	struct CmdGpuMark { const char* label = nullptr; bool endsPass = false; };

	using GLCommand = std::variant<
		CmdBeginRenderPass,
		CmdEndRenderPass,
		CmdBindPipeline,
		CmdBindVertexBuffer,
		CmdBindIndexBuffer,
		CmdBindDescriptorSets,
		CmdPushConstants,
		CmdDraw,
		CmdDrawIndexed,
		CmdDrawIndirect,
		CmdDrawIndexedIndirect,
		CmdDrawIndexedIndirectCount,
		CmdDispatch,
		CmdBlit,
		CmdSetScissor,
		CmdPipelineBarrier,
		CmdGpuMark
	>;

	struct GLCommandList
	{
		CommandListType type{};
		bool recording = false;
		bool live = false;
		std::vector<GLCommand> commands;
		std::vector<uint8_t> pushData;
	};

	class OpenGLRHI : public RenderHardwareInterface
	{
	public:
		void init() override;
		void cleanup() override;
		void waitIdle() override;

		void updateTexture(TextureHandle tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, TextureFormat fmt, const void* data, uint32_t mip, uint32_t layer) override;
		void updateCubeFace(TextureHandle tex, uint32_t face, uint32_t mip, const void* data) override;
		uint64_t getNativeTextureId(TextureHandle tex) const override;

		ShaderModuleHandle createShaderModule(const ShaderModuleDesc& desc) override;
		void destroyShaderModule(ShaderModuleHandle handle) override;

		FramebufferHandle createFramebuffer(const FramebufferDesc& desc) override;
		void destroyFramebuffer(FramebufferHandle framebuffer) override;

		TextureHandle createTexture(const TextureDesc& desc, const void* data) override;
		void destroyTexture(TextureHandle texture) override;

		SamplerHandle createSampler(const SamplerDesc& desc) override;
		void destroySampler(SamplerHandle sampler) override;

		BufferHandle createBuffer(const BufferDesc& desc, const void* initialData) override;
		void destroyBuffer(BufferHandle handle) override;
		void updateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) override;
		void* mapBuffer(BufferHandle handle, MapMode mode) override;
		void unmapBuffer(BufferHandle handle) override;
		TransientAllocation allocTransient(size_t size) override;

		PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
		PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;
		size_t reloadShaders() override;
		// A program linked from modules, its uniform blocks bound where the engine puts them; 0 when it does not link.
		GLuint linkProgram(const std::vector<ShaderModuleHandle>& modules, bool compute);
		void destroyGraphicsPipeline(PipelineHandle pipeline) override;
	private:
		template<typename T>
		static uint32_t storeResource(std::vector<T>& pool, std::vector<uint32_t>& freeList, T value) {
			if (!freeList.empty()) {
				const uint32_t id = freeList.back();
				freeList.pop_back();
				pool[id] = std::move(value);
				return id;
			}
			pool.push_back(std::move(value));
			return static_cast<uint32_t>(pool.size() - 1);
		}

		template<typename T>
		static T* getLive(std::vector<T>& pool, uint32_t id) {
			if (id >= pool.size() || !pool[id].live) {
				return nullptr;
			}
			return &pool[id];
		}

		template<typename T>
		static const T* getLive(const std::vector<T>& pool, uint32_t id) {
			if (id >= pool.size() || !pool[id].live) {
				return nullptr;
			}
			return &pool[id];
		}

		template<typename T>
		static bool recycleResource(std::vector<T>& pool, std::vector<uint32_t>& freeList, uint32_t id) {
			if (id >= pool.size() || !pool[id].live) {
				return false;
			}
			pool[id] = T{};
			freeList.push_back(id);
			return true;
		}

		std::vector<GLTexture> textures;
		std::vector<GLSampler> samplers;
		std::vector<GLFramebuffer> framebuffers;

		std::vector<GLBuffer> m_buffers;
		std::vector<GLVertexArray> m_vertexArrays;
		std::vector<GLShaderModule> m_shaders;
		std::vector<GLGraphicsPipeline> m_pipelines;

		std::vector<uint32_t> m_freeTextures;
		std::vector<uint32_t> m_freeSamplers;
		std::vector<uint32_t> m_freeFramebuffers;
		std::vector<uint32_t> m_freeBuffers;
		std::vector<uint32_t> m_freeShaders;
		std::vector<uint32_t> m_freePipelines;
		std::vector<uint32_t> m_freeRenderPasses;
		std::vector<uint32_t> m_freeDescriptorSets;
		std::vector<uint32_t> m_freeDescriptorSetLayouts;

		IndexType m_currentIndexType = IndexType::Uint16;
		uint32_t m_currentIndexOffset = 0;
		GLenum m_currentPrimitive;
		PipelineHandle m_currentPipeline;
		std::vector<GLRenderPass> m_renderPasses;

		std::vector<GLDescriptorSet> m_descriptorSets;
		std::vector<GLDescriptorSetLayout> m_descriptorSetLayouts;
		std::vector<GLCommandList> m_commandLists;
		CommandListHandle m_openCommandList{};
		std::vector<uint32_t> m_freeCommandLists;

		// allocTransient: the data is kept on the CPU and uploaded before the list that reads it is replayed.
		// A replayed list has issued its commands, and GL commands see a buffer as it was when issued, so the
		// region is reused from the start after every submit. The vector is only resized between submits, so
		// no allocation's pointer moves; one that does not fit gets a buffer of its own until the submit.
		struct TransientOverflow {
			BufferHandle buffer;
			std::vector<uint8_t> data;
		};
		BufferHandle m_transientBuffer{};
		std::vector<uint8_t> m_transientData;
		size_t m_transientUsed = 0;
		size_t m_transientPeak = 0;
		size_t m_transientAlign = 16;
		std::vector<TransientOverflow> m_transientOverflow;
		// Buffers destroyed while a list was open, destroyed after its submit.
		std::vector<uint32_t> m_pendingBufferDestroys;
		// A submitted list's storage, handed to the next one so recording does not grow it from empty every frame.
		std::vector<GLCommand> m_spareCommands;
		const GLCommandList* m_replayingList = nullptr; // for commands that refer to the list's own storage
		std::vector<uint8_t> m_sparePushData;
		void uploadTransient();
		void resetTransient();

		bool m_inited = false;
		std::string m_deviceName;
		// A ring, not a pair: the driver runs frames behind -- as many as it queues while the GPU is the slower -- so a
		// query ended a few submits ago is not finished yet, and polling it came back unavailable, which left the GPU
		// time reading zero, or stale at whatever it last read.
		static constexpr int kGpuTimeQueries = 8;
		GLuint mGpuTimeQueries[kGpuTimeQueries]{};
		int mGpuQueryWrite = 0;
		int mGpuQueriesIssued = 0;
		double mLastGpuMs = 0.0;
		// cmdGpuMark's timestamps the same way: kMaxGpuMarks a submit that marks, kGpuTimeQueries such submits round,
		// read as the oldest comes to be written again; and what each mark ends.
		GLuint mGpuMarkQueries[kGpuTimeQueries][kMaxGpuMarks]{};
		uint32_t mGpuMarkCount[kGpuTimeQueries]{};
		int mGpuMarkWrite = 0;
		GpuMarkLabel mGpuMarkLabels[kGpuTimeQueries][kMaxGpuMarks]{};
		TextureHandle m_presentBlitTexture{};
		// A hidden window's frames, which nothing swaps: the fences (GLsync) of the last few presented, the oldest
		// waited for.
		static constexpr int kHiddenFrames = 2;
		void* mHiddenFrames[kHiddenFrames]{};
		int mHiddenFrame = 0;
		// A hidden window whose context shares objects with the main one, for runOnWorkerThread.
		GLFWwindow* m_workerContext = nullptr;
		std::mutex m_workerContextMutex;
		GLuint m_presentReadGlFbo = 0;
		uint32_t m_presentReadTexId = UINT32_MAX;



		GLuint m_prevFramebuffer = 0; // для сохранения/восстановления FBO в begin/endRenderPass
		GLuint m_pushConstantUBO = 0; // constant buffer for push constants emulation
		GLuint m_pushConstantPos = 15; // binding point for push constants emulation
		int m_currentPassHeight = 0;

		GLenum toGLBufferTarget(BufferUsage usage) const;
		GLenum toGLAccessFlags(CpuAccess access) const;
		GLbitfield toGLMapFlags(MapMode mode, CpuAccess access) const;
		GLenum toGLType(VertexFormat f) const;
		GLint  toGLSize(VertexFormat f) const;

		GLCommandList* recordingList(CommandListHandle cmd);
		void executeCommand(const GLCommand& command);
		void execBeginRenderPass(const CmdBeginRenderPass& cmd);
		void execEndRenderPass();
		void execBindPipeline(PipelineHandle pipeline);
		void execBindVertexBuffer(uint32_t slot, BufferHandle handle, uint32_t offset);
		void execBindIndexBuffer(BufferHandle handle, IndexType type, uint32_t offset);
		void execDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
		void execDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);
		void execDrawIndirect(BufferHandle buffer, size_t offset, uint32_t drawCount, uint32_t stride);
		void execDrawIndexedIndirect(BufferHandle buffer, size_t offset, uint32_t drawCount, uint32_t stride);
		void execDrawIndexedIndirectCount(BufferHandle buffer, size_t offset, BufferHandle countBuffer, size_t countOffset, uint32_t maxDrawCount, uint32_t stride);
		void execDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);
		void execPushConstants(const void* data, uint32_t size);
		void execBlit(FramebufferHandle src, FramebufferHandle dst, BlitMask mask, BlitFilter filter);
		void execSetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height);
		void execBindDescriptorSets(const CmdBindDescriptorSets& bind);
		void execUpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data);
		void execPipelineBarrier(const PipelineBarrier& barrier);

	public:
		RenderPassHandle createRenderPass(const RenderPassDesc& desc) override;
		void destroyRenderPass(RenderPassHandle renderPass) override;

		CommandListHandle beginCommandList(CommandListType type) override;
		void endCommandList(CommandListHandle cmd) override;
		void submit(CommandListHandle list) override;
		void present() override;
		void runOnWorkerThread(const std::function<void()>& work) override;
		void setPresentBlitSource(TextureHandle texture, SamplerHandle sampler) override;
		double lastGpuMs() const override { return mLastGpuMs; }
		void writeGpuMark(CommandListHandle cmd, const char* label, bool endsPass) override;
		const char* deviceName() const override { return m_deviceName.c_str(); }
		bool readbackTexture(TextureHandle tex, std::vector<uint8_t>& rgba, int& width, int& height, uint32_t layer = 0) override;
		TextureHandle presentSourceTexture() const override { return m_presentBlitTexture; }
		void cmdBeginRenderPass(CommandListHandle cmd, const RenderPassBeginInfo& beginInfo) override;
		void cmdEndRenderPass(CommandListHandle cmd) override;
		void cmdBindPipeline(CommandListHandle cmd, PipelineHandle pipeline) override;
		void cmdBindVertexBuffer(CommandListHandle cmd, uint32_t slot, BufferHandle handle, uint32_t offset) override;
		void cmdBindIndexBuffer(CommandListHandle cmd, BufferHandle handle, IndexType type, uint32_t offset) override;
		void cmdDraw(CommandListHandle cmd, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
		void cmdDrawIndexed(CommandListHandle cmd, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
		void cmdDrawIndirect(CommandListHandle cmd, BufferHandle buffer, size_t offset, uint32_t drawCount, uint32_t stride) override;
		void cmdDrawIndexedIndirect(CommandListHandle cmd, BufferHandle buffer, size_t offset, uint32_t drawCount, uint32_t stride) override;
		void cmdDrawIndexedIndirectCount(CommandListHandle cmd, BufferHandle buffer, size_t offset, BufferHandle countBuffer, size_t countOffset, uint32_t maxDrawCount, uint32_t stride) override;
		void cmdDispatch(CommandListHandle cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
		void cmdPushConstants(CommandListHandle cmd, PipelineHandle pipeline, ShaderStage stages, const void* data,	uint32_t size) override;
		void cmdBlit(CommandListHandle cmd, FramebufferHandle src, FramebufferHandle dst, BlitMask mask, BlitFilter filter) override;
		void cmdSetScissor(CommandListHandle cmd, int32_t x, int32_t y, uint32_t width, uint32_t height) override;
		void cmdPipelineBarrier(CommandListHandle cmd, const PipelineBarrier& barrier) override;
		void cmdUpdateBuffer(CommandListHandle cmd, BufferHandle handle, size_t offset, size_t size, const void* data) override;
		void cmdCopyBuffer(CommandListHandle cmd, BufferHandle src, BufferHandle dst, size_t srcOffset, size_t dstOffset, size_t size) override;

		DescriptorSetLayoutHandle createDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override;
		void destroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) override;
		DescriptorSetHandle allocateDescriptorSet(DescriptorSetLayoutHandle layout) override;
		void freeDescriptorSet(DescriptorSetHandle set) override;
		void updateDescriptorSets(const std::vector<WriteDescriptor>& writes) override;
		void cmdBindDescriptorSets(CommandListHandle cmd, PipelineHandle layout, uint32_t firstSet, const DescriptorSetHandle* sets, uint32_t setCount,
			const uint32_t* dynamicOffsets = nullptr, uint32_t dynamicOffsetCount = 0) override;
		FramebufferHandle getBackbufferFramebuffer() override;
	};
}
