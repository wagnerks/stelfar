#pragma once

#include <array>
#include <vector>

#include "renderModule/Particles.h"
#include "renderModule/renderPasses/FinalDepthTargets.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::RenderPasses {
	// The particles (renderModule/Particles.h) a step on and their chips drawn: what was let go since the last frame put
	// in, everything moved on the GPU (particles.cs) against this frame's G-buffer depth and normals, then the chips
	// drawn into the G-buffer to be lit with the rest. After every pass that draws into the G-buffer, before its depth
	// is copied (SceneDepthPass). Nothing at all while nothing let go is still alive. What the catchers caught (the
	// drops that stuck to them) is read back when its frame slot comes round again and told (Particles::caught).
	class ParticlePass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;

	private:
		PipelineHandle mStep{};
		PipelineHandle mChips{};
		DescriptorSetLayoutHandle mStepLayout{};
		DescriptorSetLayoutHandle mChipLayout{};
		std::vector<DescriptorSetHandle> mStepSets; // one per frame slot
		std::vector<DescriptorSetHandle> mChipSets;
		RenderPassHandle mGBufferPass{};
		BufferHandle mQuad{};
		uint32_t mHead = 0; // where the next let go goes
		// The catchers' shapes, kShapeWords each, as last given (their versions); the drops caught, one ring a frame slot,
		// and how far into each one's count it was read.
		BufferHandle mShapes{};
		std::array<uint64_t, Particles::kCatchers> mShapeVersions{};
		std::vector<BufferHandle> mCaught;
		std::vector<uint32_t> mCaughtRead;
		double mLast = -1.0;
	};

	// The motes and the sparks drawn over the resolved picture after TAA, with its depth, as AirLifePass's specks are:
	// TAA would smear what moves this fast across a pixel to nothing.
	class ParticleSpritePass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;

	private:
		FinalDepthTargets mTargets;
		RenderPassHandle mRenderPass{};
		PipelineHandle mPipeline{};
		DescriptorSetLayoutHandle mLayout{};
		std::vector<DescriptorSetHandle> mSets; // one per frame slot
		BufferHandle mQuad{};
	};
}
