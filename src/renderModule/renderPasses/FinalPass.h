#pragma once
#include <vector>
#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render {
	struct Font;
}

namespace SFE::Render::RenderPasses {
	class FinalPass : public RenderPass {
	public:
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;

	private:
		PipelineHandle makeFullscreen(RenderHardwareInterface* rhi, RenderPassHandle renderPass, const char* vsPath, const char* fsPath);

		// The debug overlay: the text, and behind it a graph of the last few seconds of frame and GPU times.
		// Pixels, with the origin at the top left of the render target.
		struct Rect {
			float x = 0.f;
			float y = 0.f;
			float width = 0.f;
			float height = 0.f;
		};
		void drawOverlay(Render::CommandListHandle cmd, int screenWidth, int screenHeight);
		void addRect(const Rect& rect, std::vector<Math::Vec2>& out) const;
		// `samples` is a ring starting at `cursor`, drawn left to right.
		void addGraph(const Rect& rect, const float* samples, int count, int cursor, float scale, std::vector<Math::Vec2>& out) const;
		void drawShapes(Render::CommandListHandle cmd, PipelineHandle pipeline, const std::vector<Math::Vec2>& vertices, const Math::Vec4& color);

		PipelineHandle mOverlayFillPipeline{}; // triangles: the panel behind the graphs
		PipelineHandle mOverlayLinePipeline{}; // line segments: the graphs themselves
		std::vector<Math::Vec2> mOverlayVertices; // kept between frames so building the graph does not allocate
		float mScreenWidth = 0.f;
		float mScreenHeight = 0.f;

		PipelineHandle mBloomPipeline{};
		PipelineHandle mPostPipeline{};
		// The frame through the tonemap alone, for the sharpening to read its neighbours from.
		PipelineHandle mTonePipeline{};
		std::vector<DescriptorSetHandle> mToneSets; // one per frame slot
		TextureHandle mToneTex{};
		FramebufferHandle mToneFbo{};
		RenderPassHandle mBloomPass{};
		RenderPassHandle mDisplayPass{};
		DescriptorSetLayoutHandle mLayout{};
		std::vector<DescriptorSetHandle> mBloomSets; // one per frame slot
		std::vector<DescriptorSetHandle> mPostSets; // one per frame slot
		SamplerHandle mSampler{};

		// Mirrors PostBlock in sfe_post.glsl. std140.
		struct PostParams {
			float bloomStrength = 0.62f;
			float exposure = 0.92f;
			float sunHeight = 1.f; // see sfe_post.glsl
			float ca = 0.0028f;
			float sharpness = 0.5f;
			float autoExposure = 1.f;
			float flareStrength = 0.f;
			float warmth = 0.f;
			float saturation = 1.f;
			float contrast = 1.f;
			float sunU = 0.f;
			float sunV = 0.f;
			float sunInFront = 0.f;
			float aspect = 1.f;
			float adaptSeconds = 0.f;
			float adaptSpeed = 1.f;
			float sunColor[4]{};
			float extra[4]{}; // see sfe_post.glsl
		} mPostParams;

		// Auto exposure and the sun's visibility for the flare: the frame's log luminance from 64x64 down to 1x1, and
		// what the eye has adapted to, carried from frame to frame in two 1x1 targets that take turns.
		struct Target {
			TextureHandle texture{};
			FramebufferHandle fbo{};
			uint32_t size = 0;
		};
		void adapt(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd, TextureHandle src, const TransientAllocation& post);
		std::vector<Target> mLuminance;
		Target mAdapted[2];
		uint32_t mAdaptedCurrent = 0;
		bool mAdaptedValid = false;
		double mLastFrameAt = 0.0;
		PipelineHandle mLuminanceLogPipeline{};
		PipelineHandle mLuminanceDownPipeline{};
		PipelineHandle mAdaptPipeline{};
		std::vector<std::vector<DescriptorSetHandle>> mAdaptSets; // per frame slot, one per step

		TextureHandle mBloomTex{};
		FramebufferHandle mBloomFbo{};
		TextureHandle mDisplayTex{};
		FramebufferHandle mDisplayFbo{};
		Font* mOverlayFont = nullptr;
	};
}
