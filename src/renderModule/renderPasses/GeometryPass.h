#pragma once
#include <future>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#include "assetsModule/TextureHandler.h"
#include "logsModule/logger.h"
#include "renderModule/GpuScene.h"
#include "rhi/RenderHardwareInterface.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {


	class GeometryPass : public RenderPass {
	public:
		struct Data {
			FramebufferHandle gFramebuffer;

			TextureHandle materialBuffer;
			TextureHandle viewPositionBuffer;
			TextureHandle normalBuffer;
			TextureHandle albedoBuffer;
			TextureHandle velocityBuffer;
			TextureHandle emissiveBuffer;
			TextureHandle depthsBuffer;
		};

		void publish(SystemsModule::RenderData& renderDataHandle);

		RenderPassHandle geometryPass;
		RenderPassHandle mGeometryLoadPass;
		PipelineHandle geometryPipeline;

		PipelineHandle gBufferOutlinePipeline;
		RenderPassHandle outlineBufferRenderPass;
		PipelineHandle outlinePipeline;
		RenderPassHandle outlineRenderPass;

		Render::DescriptorSetLayoutHandle mTexSetLayoutOutlines{};
		// One per frame slot: what they bind alternates with the slot.
		struct OutlineTextures {
			Render::DescriptorSetHandle set{};
			TextureHandle lights{};
			TextureHandle normal{};
			TextureHandle mask{};
		};
		std::vector<OutlineTextures>      mTexSetsOutlines;
		std::vector<Render::DescriptorSetHandle> mOutlineFrameSets;

		Render::DescriptorSetLayoutHandle gBuffetLayout{};
		Render::DescriptorSetHandle       gBuffetLayoutSet{};

		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

		// The opaque scene culled into `view`, drawn with each material's G-buffer pipeline into whatever render pass
		// is open, as long as it has this pass's attachments. `frameSet` is laid out like the renderer's frame set and
		// carries the camera to draw from; the visible list and the materials are written into it here.
		void drawGBuffer(CommandListHandle cmd, const GpuScene::View& view, DescriptorSetHandle frameSet, BufferHandle materials,
			PolygonFillMode fillMode = PolygonFillMode::Fill, bool secondPhase = false);
	private:
		// The fill mode is part of the key: wireframe is a second pipeline, not a state change. The
		// RHI has no dynamic polygon mode -- Vulkan bakes it into the PSO and cannot override it. So is whether back faces
		// are culled (DrawGroup::oneSided).
		Render::PipelineHandle getGBufferPipeline(const std::string& vertexShader, const std::string& fragmentShader,
			Render::PolygonFillMode fillMode = Render::PolygonFillMode::Fill, bool oneSided = false);
		// The ground drawn dense near the camera (GroundMarksFrame::patch): g_buffer_ground.vs with the terrain's own
		// fragment shader, no vertex input. None if its shaders are missing.
		Render::PipelineHandle groundPatchPipeline(Render::PolygonFillMode fillMode);
		void initGrid(Render::DescriptorSetLayoutHandle frameLayout);
		void drawGrid(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd);


		bool mInited = false;
		std::vector<Data> mFrames; // one per frame the RHI keeps in flight
		uint32_t mGpuFrames = 1;
		SamplerHandle mSampler{};
		TextureHandle outlinesMask{};
		TextureHandle outlinesBuffer{};
		TextureHandle lightsBuffer{};
		FramebufferHandle outlineFramebuffer{};
		FramebufferHandle outlineResultFbo{};
		bool needClearOutlines = true;


		GpuScene::View mCameraView;
		GpuScene::View mOutlineView;
		std::unordered_map<uint64_t, DescriptorSetHandle> mTextureSets;
		// Per frame set: the visible-instance and material buffers last written, so a set is rewritten
		// only when one of them was replaced.
		std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> mFrameSetBuffers;

		GraphicsPipelineDesc mGBufferPipelineDesc;
		std::unordered_map<std::string, PipelineHandle> mGBufferPipelines;
		PipelineHandle mGroundPatchPipelines[2]{}; // filled, wireframe

		PipelineHandle mGridPipeline;
		BufferHandle mGridVBO;
		BufferHandle mGridEBO;
		struct GridDrawData {
			Math::Vec4 viewPos;
			float far = 0.f;
			float near = 0.f;
		};
		GridDrawData mGridDrawData;
	};
}
