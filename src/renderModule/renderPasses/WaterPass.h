#pragma once

#include <vector>

#include "renderModule/Held.h"
#include "renderModule/renderPasses/FinalDepthTargets.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	// Whether the scene has a water the water's passes draw: a WaterComponent with a place, in a scene with a sky. Their
	// PassEntry::on, by the rule their draw finds the water by.
	bool waterUnderSky(const SystemsModule::RenderData& renderData);

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
		Held<SamplerHandle> mNearest;
		Held<SamplerHandle> mLinear;
		// The ripple map: repeating and trilinear, with no anisotropy. At a grazing angle an anisotropic read of it
		// costs more than every wave the shader sums.
		Held<SamplerHandle> mRipple;

		// The picture's copy (Render::kSceneCopy), shared with every pass that copies the picture before it reads it.
		Held<TextureHandle> mSceneCopy;
		Held<FramebufferHandle> mSceneCopyFbo;
		FinalDepthTargets mTargets;
		Held<RenderPassHandle> mSurfacePass;
		Held<PipelineHandle> mSurfacePipeline;
		Held<DescriptorSetLayoutHandle> mSurfaceLayout;
		std::vector<Held<DescriptorSetHandle>> mSurfaceSets; // one per frame slot
		std::vector<Held<ShaderModuleHandle>> mShaders;
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
		Held<SamplerHandle> mNearest;
		Held<SamplerHandle> mLinear;
		Held<TextureHandle> mSceneCopy;
		Held<FramebufferHandle> mSceneCopyFbo;
		FinalDepthTargets mTargets;
		Held<RenderPassHandle> mPass;
		Held<PipelineHandle> mPipeline;
		Held<DescriptorSetLayoutHandle> mLayout;
		std::vector<Held<DescriptorSetHandle>> mSets; // one per frame slot
		std::vector<Held<ShaderModuleHandle>> mShaders;
	};
}
