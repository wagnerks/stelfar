#pragma once
#include <mutex>
#include <thread>
#include <vector>

#include "assetsModule/modelModule/Mesh.h"
#include "componentsModule/MeshComponent.h"
#include "renderModule/FrameBlackboard.h"
#include "renderModule/GpuScene.h"
#include "renderModule/MaterialTextures.h"
#include "rhi/RenderHardwareInterface.h"

#include "renderModule/renderPasses/RenderPass.h"


namespace SFE::Render::RenderPasses {

	class CascadedShadowPass : public RenderPass {
	public:

		using Data = Render::CsmFrame;

		CascadedShadowPass();
		~CascadedShadowPass() override;
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void initRender();

		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
	private:
		Math::Vec3 mSunRotation{ 1e9f, 0.f, 0.f }; // the sun last placed, degrees
		void updateRenderData(SystemsModule::RenderData& renderDataHandle);
		void debug(SystemsModule::RenderData& renderDataHandle);

		// One framebuffer per cascade layer. The layer used to come from gl_Layer in a geometry
		// shader whose only other job was to copy the triangle through.
		std::vector<FramebufferHandle> lightFbos;
		TextureHandle  lightDepthMa;
		SamplerHandle  lightSampler;
		SamplerHandle  lightDepthSampler;

		// Opaque casters render depth only; anything with a cutout or dithered opacity needs a
		// fragment shader with discard, which costs early-Z, so it gets its own pipeline.
		PipelineHandle pipelineOpaque;
		PipelineHandle pipelineCutout;
		PipelineHandle pipelineCutoutSolid; // a cut-out caster drawn solid in the far cascades
		RenderPassHandle renderPass;
		RenderPassHandle loadRenderPass; // the second occlusion phase draws over the first one's depth

		// Set 0 holds the buffers and differs per cascade (each has its own visible list); set 1 holds the
		// caster's mask texture and is shared by every cascade.
		struct Cascade {
			GpuScene::View view;
			DescriptorSetHandle set;
			uint32_t boundIds[5]{};
			uint32_t matricesId = UINT32_MAX; // transient buffer binding 11 was last written with
			// What the map was last drawn with; a cascade that skips a frame is lit with these.
			bool drawn = false;
			uint32_t framesSinceDrawn = 0;
			float age = 0.f;      // seconds since it was drawn
			Math::Vec3 drawnFrom; // where the camera was then
			Math::Mat4 lightSpace;
			ComponentsModule::ShadowCascade settings;
		};
		// Cascades from this one out are the far ones: drawn less often, kept while their volume stays put, and their
		// casters not culled to what the camera sees.
		static constexpr size_t kFirstFar = 2;
		// Metres up-sun past a cascade's near plane that its casters are still drawn from.
		static constexpr float kCasterReachTowardsSun = 400.f;
		// Frames a far cascade whose volume has not moved keeps its map at most, so what moved inside it (a fallen
		// log spawned, a tree swapped for its picture) still shows up.
		static constexpr uint32_t kStaticRefreshFrames = 30;
		// Cascades that may yet be drawn, carried on from frame to frame (shadowRefreshRate).
		float mDrawBudget = 0.f;
		Render::DescriptorSetLayoutHandle mBufferLayout;
		Render::DescriptorSetLayoutHandle mTextureLayout;
		std::vector<Cascade> mCascades;
		SingleTextureSets mMaskSets;
		bool mProxyRegistered = false;

		ecss::EntityId mShadowSource;
		bool mInited = false;
		// The drawn look, read once when the maps are made: a smaller map, coarser casters, fewer of them.
		bool mStylised = false;
		Data mData;

		SFE::MeshObject3D mGlassShadowMesh;
		SFE::RenderMeshData mGlassShadowMeshData;
	};
}

