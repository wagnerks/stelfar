#pragma once

#include "renderModule/renderPasses/FinalDepthTargets.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	// Water surfaces (WaterComponent) in a scene with a sky. The surface reflects what the screen already shows by
	// marching its reflected rays through the scene's depth, and the sky where they find nothing; under it the lit
	// image, refracted and dimmed by how much water the light crosses. With the eye under the water the surface is seen
	// from below (UnderwaterPass then shows all the picture through the water).
	class WaterPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		SamplerHandle mNearest{};
		SamplerHandle mLinear{};
		// The ripple map: repeating and trilinear, with no anisotropy. At a grazing angle an anisotropic read of it
		// costs more than every wave the shader sums.
		SamplerHandle mRipple{};

		TextureHandle mSceneCopy{};
		FramebufferHandle mSceneCopyFbo{};
		FinalDepthTargets mTargets;
		RenderPassHandle mSurfacePass{};
		PipelineHandle mSurfacePipeline{};
		DescriptorSetLayoutHandle mSurfaceLayout{};
		std::vector<DescriptorSetHandle> mSurfaceSets; // one per frame slot
	};

	// With the eye under a water surface: everything seen through the water the view crosses, the bottom dimmed and
	// netted with caustics, the picture wavering (water_under.fs). After the air's own mist and light shafts, which
	// the water then hides as it hides the rest.
	class UnderwaterPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		SamplerHandle mNearest{};
		SamplerHandle mLinear{};
		TextureHandle mSceneCopy{};
		FramebufferHandle mSceneCopyFbo{};
		FinalDepthTargets mTargets;
		RenderPassHandle mPass{};
		PipelineHandle mPipeline{};
		DescriptorSetLayoutHandle mLayout{};
		std::vector<DescriptorSetHandle> mSets; // one per frame slot
	};
}
