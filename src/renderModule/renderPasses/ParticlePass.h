#pragma once

#include <array>
#include <vector>

#include "renderModule/Held.h"
#include "renderModule/Particles.h"
#include "renderModule/renderPasses/FinalDepthTargets.h"
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render::FrameId {
	// The particles (Particles::GpuParticle, kCapacity of them), ParticlePass's once it has let one go: what the drops
	// and the motes are drawn from.
	constexpr const char* ParticleBuffer = "particles.buffer";
	// The drops' liquid's field as ParticleLiquidPass laid it this frame, for ParticleClearLiquidPass: none where it laid
	// none.
	constexpr const char* LiquidHow = "particles.liquid.how";
	constexpr const char* LiquidWhere = "particles.liquid.where";
	constexpr const char* LiquidFacing = "particles.liquid.facing";
}

namespace SFE::Render::RenderPasses {
	// The particles (renderModule/Particles.h) a step on and their chips drawn: what was let go since the last frame put
	// in, everything moved on the GPU (particles.cs) against this frame's G-buffer depth and normals, then the chips
	// drawn into the G-buffer to be lit with the rest. After every pass that draws into the G-buffer, before its depth
	// is copied (SceneDepthPass). Nothing at all while nothing let go is still alive. What the catchers caught (the
	// drops that stuck to them) is read back when its frame slot comes round again and told (Particles::caught). Gives
	// the particles to the passes that draw them (FrameId::ParticleBuffer).
	class ParticlePass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;

	private:
		Held<PipelineHandle> mStep;
		Held<PipelineHandle> mChips;
		Held<DescriptorSetLayoutHandle> mStepLayout;
		Held<DescriptorSetLayoutHandle> mChipLayout;
		std::vector<Held<DescriptorSetHandle>> mStepSets; // one per frame slot
		std::vector<Held<DescriptorSetHandle>> mChipSets;
		Held<RenderPassHandle> mGBufferPass;
		Held<BufferHandle> mQuad;
		// The particles, made the first frame one lives.
		Held<BufferHandle> mParticles;
		uint32_t mHead = 0; // where the next let go goes
		// The catchers' shapes, kShapeWords each, as last given (their versions); the drops caught, one ring a frame slot,
		// and how far into each one's count it was read.
		Held<BufferHandle> mShapes;
		std::array<uint64_t, Particles::kCatchers> mShapeVersions{};
		std::vector<Held<BufferHandle>> mCaught;
		std::vector<uint32_t> mCaughtRead;
		std::vector<Held<ShaderModuleHandle>> mShaders;
	};

	// The drops, in flight and at rest, drawn as the liquid they are (Particles::Look::Drop). Each drop lays a soft ball
	// of its liquid into a field the picture's size (particle_liquid.vs, .fs), against the G-buffer's depth: how much of
	// it there is, more the nearer the eye's ray comes to the drop's middle, with its colour, how far off and which way
	// its face is, how much of the light it stops and how it moves. Where the field holds enough, the liquid's surface
	// is, so drops near each other run together into one body of it (a stream, a puddle) and one on its own stays a
	// drop; the further its liquid gathers from, the smoother. Where it stops most of the light (mud), that surface into
	// the G-buffer (particle_liquid_gbuffer.fs), lit with the rest of the scene; the clear of it (water) is drawn over the
	// picture after TAA from the same field (ParticleClearLiquidPass). After the chips, before the G-buffer's depth is
	// copied (SceneDepthPass). Nothing while no drop lives.
	class ParticleLiquidPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;

	private:
		Held<RenderPassHandle> mFieldPass;
		Held<RenderPassHandle> mGBufferPass;
		Held<PipelineHandle> mField;
		Held<PipelineHandle> mOpaque;
		Held<DescriptorSetLayoutHandle> mFieldLayout;
		Held<DescriptorSetLayoutHandle> mOpaqueLayout;
		std::vector<Held<DescriptorSetHandle>> mFieldSets; // one per frame slot
		std::vector<Held<DescriptorSetHandle>> mOpaqueSets;
		Held<BufferHandle> mQuad;
		// The field: its liquid's colour and how much of it (RGBA16F); how far off and how much light it stops (RG32F);
		// which way it faces (RGBA16F); how it moves (RG16F).
		Held<TextureHandle> mHow;
		Held<TextureHandle> mWhere;
		Held<TextureHandle> mFacing;
		Held<TextureHandle> mMoving;
		Held<FramebufferHandle> mFieldFbo;
		Held<SamplerHandle> mLinear;
		std::vector<Held<ShaderModuleHandle>> mShaders;
	};

	// The clear of the drops' liquid (water), over the resolved picture after TAA, as the motes are, from the field
	// ParticleLiquidPass laid this frame (particle_liquid_surface.fs): the picture behind seen through it bent as far as
	// the liquid is clear, lit by the sun where the shadows let it and by the sky as the scene is, mirroring the scene
	// more at a glance.
	class ParticleClearLiquidPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;

	private:
		FinalDepthTargets mTargets;
		Held<RenderPassHandle> mSurfacePass;
		Held<PipelineHandle> mSurface;
		Held<DescriptorSetLayoutHandle> mSurfaceLayout;
		std::vector<Held<DescriptorSetHandle>> mSurfaceSets; // one per frame slot
		// The picture behind, copied (Render::kSceneCopy, shared with every pass that copies it before it reads it).
		Held<TextureHandle> mCopy;
		Held<FramebufferHandle> mCopyFbo;
		Held<SamplerHandle> mLinear;
		std::vector<Held<ShaderModuleHandle>> mShaders;
	};

	// The motes and the sparks drawn over the resolved picture after TAA, with its depth: TAA would smear what moves this
	// fast across a pixel to nothing.
	class ParticleSpritePass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;

	private:
		FinalDepthTargets mTargets;
		Held<RenderPassHandle> mRenderPass;
		Held<PipelineHandle> mPipeline;
		Held<DescriptorSetLayoutHandle> mLayout;
		std::vector<Held<DescriptorSetHandle>> mSets; // one per frame slot
		Held<BufferHandle> mQuad;
		std::vector<Held<ShaderModuleHandle>> mShaders;
	};
}
