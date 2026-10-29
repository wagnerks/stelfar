#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	// The backend for running without a window or a GPU: game logic and tests that need no picture. Every
	// call succeeds and does nothing. Resources get unique ids, never reused, because callers key caches on
	// them; a mapped buffer is plain memory of the buffer's size. Nothing is drawn, so readbackTexture keeps
	// the base class's false. What is made and not yet destroyed is counted by kind (live), for a test of who
	// frees what.
	class NullRHI final : public RenderHardwareInterface {
	public:
		// The kinds of object the RHI destroys, each counted apart.
		enum class Kind : uint8_t { Texture, Buffer, Sampler, Pipeline, RenderPass, Framebuffer, SetLayout, Set, ShaderModule, Count };
		// How many of `kind` are made and not destroyed. Any thread.
		size_t live(Kind kind) const;

		void init() override {}
		void cleanup() override {}
		GraphicsBackend backend() const override { return GraphicsBackend::None; }

		PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc&) override { return { made(Kind::Pipeline) }; }
		PipelineHandle createComputePipeline(const ComputePipelineDesc&) override { return { made(Kind::Pipeline) }; }
		void destroyGraphicsPipeline(PipelineHandle pipeline) override { gone(Kind::Pipeline, pipeline.id); }
		TextureHandle createTexture(const TextureDesc&, const void* = nullptr) override { return { made(Kind::Texture) }; }
		void destroyTexture(TextureHandle texture) override { gone(Kind::Texture, texture.id); }

		SamplerHandle createSampler(const SamplerDesc&) override { return { made(Kind::Sampler) }; }
		void destroySampler(SamplerHandle sampler) override { gone(Kind::Sampler, sampler.id); }

		ShaderModuleHandle createShaderModule(const ShaderModuleDesc&) override { return { made(Kind::ShaderModule) }; }
		void destroyShaderModule(ShaderModuleHandle handle) override { gone(Kind::ShaderModule, handle.id); }

		FramebufferHandle createFramebuffer(const FramebufferDesc&) override { return { made(Kind::Framebuffer) }; }
		void destroyFramebuffer(FramebufferHandle framebuffer) override { gone(Kind::Framebuffer, framebuffer.id); }
		FramebufferHandle getBackbufferFramebuffer() override { return mBackbuffer; }

		BufferHandle createBuffer(const BufferDesc& desc, const void* initialData = nullptr) override;
		void destroyBuffer(BufferHandle handle) override;
		void updateBuffer(BufferHandle, size_t = 0, size_t = 0, const void* = nullptr) override {}
		void* mapBuffer(BufferHandle handle, MapMode mode) override;
		void unmapBuffer(BufferHandle) override {}
		TransientAllocation allocTransient(size_t size) override;
		void beginFrame() override;

		RenderPassHandle createRenderPass(const RenderPassDesc&) override { return { made(Kind::RenderPass) }; }
		void destroyRenderPass(RenderPassHandle renderPass) override { gone(Kind::RenderPass, renderPass.id); }

		DescriptorSetLayoutHandle createDescriptorSetLayout(const DescriptorSetLayoutDesc&) override { return { made(Kind::SetLayout) }; }
		void destroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) override { gone(Kind::SetLayout, layout.id); }

		DescriptorSetHandle allocateDescriptorSet(DescriptorSetLayoutHandle) override { return { made(Kind::Set) }; }
		void freeDescriptorSet(DescriptorSetHandle set) override { gone(Kind::Set, set.id); }
		void updateDescriptorSets(const std::vector<WriteDescriptor>&) override {}

		CommandListHandle beginCommandList(CommandListType) override { return { nextId() }; }
		void endCommandList(CommandListHandle) override {}

		void cmdBeginRenderPass(CommandListHandle, const RenderPassBeginInfo&) override {}
		void cmdEndRenderPass(CommandListHandle) override {}
		void cmdBindPipeline(CommandListHandle, PipelineHandle) override {}
		void cmdBindVertexBuffer(CommandListHandle, uint32_t, BufferHandle, uint32_t = 0) override {}
		void cmdBindIndexBuffer(CommandListHandle, BufferHandle, IndexType, uint32_t = 0) override {}
		void cmdBindDescriptorSets(CommandListHandle, PipelineHandle, uint32_t, const DescriptorSetHandle*, uint32_t, const uint32_t* = nullptr, uint32_t = 0) override {}
		void cmdPushConstants(CommandListHandle, PipelineHandle, ShaderStage = ShaderStage::Vertex | ShaderStage::Fragment, const void* = nullptr, uint32_t = 0) override {}

		void cmdDraw(CommandListHandle, uint32_t, uint32_t = 1, uint32_t = 0, uint32_t = 0) override {}
		void cmdDrawIndexed(CommandListHandle, uint32_t, uint32_t = 1, uint32_t = 0, int32_t = 0, uint32_t = 0) override {}
		void cmdDrawIndirect(CommandListHandle, BufferHandle, size_t = 0, uint32_t = 1, uint32_t = 16) override {}
		void cmdDrawIndexedIndirect(CommandListHandle, BufferHandle, size_t = 0, uint32_t = 1, uint32_t = 20) override {}
		void cmdDrawIndexedIndirectCount(CommandListHandle, BufferHandle, size_t, BufferHandle, size_t, uint32_t, uint32_t = 20) override {}
		void cmdDispatch(CommandListHandle, uint32_t, uint32_t = 1, uint32_t = 1) override {}

		void cmdBlit(CommandListHandle, FramebufferHandle, FramebufferHandle, BlitMask = BlitMask::Color, BlitFilter = BlitFilter::Nearest) override {}
		void cmdPipelineBarrier(CommandListHandle, const PipelineBarrier&) override {}
		void cmdUpdateBuffer(CommandListHandle, BufferHandle, size_t, size_t, const void*) override {}
		void cmdCopyBuffer(CommandListHandle, BufferHandle, BufferHandle, size_t, size_t, size_t) override {}

		void submit(CommandListHandle) override {}
		void present() override {}

		void updateTexture(TextureHandle, uint32_t, uint32_t, uint32_t, uint32_t, TextureFormat, const void*, uint32_t, uint32_t) override {}
		void updateCubeFace(TextureHandle, uint32_t, uint32_t, const void*) override {}
		void updateCubeMip(TextureHandle, uint32_t, const void*, size_t) override {}
		uint64_t getNativeTextureId(TextureHandle tex) const override { return tex.id; }

	private:
		uint32_t nextId() { return mNextId.fetch_add(1, std::memory_order_relaxed); }
		// A new id, live as `kind` until gone() is told of it; a destroy of an id that is not live changes nothing.
		uint32_t made(Kind kind);
		void gone(Kind kind, uint32_t id);

		std::atomic<uint32_t> mNextId{ 0 };
		// Memory for allocTransient, dropped at the next frame: nothing reads it.
		std::vector<std::vector<uint8_t>> mTransient;
		BufferHandle mTransientBuffer{ nextId() };
		FramebufferHandle mBackbuffer{ nextId() };

		std::mutex mBuffersMutex;
		std::unordered_map<uint32_t, size_t> mBufferSizes;
		std::unordered_map<uint32_t, std::vector<uint8_t>> mMapped;

		mutable std::mutex mLiveMutex;
		std::array<std::unordered_set<uint32_t>, static_cast<size_t>(Kind::Count)> mLive;
	};
}
