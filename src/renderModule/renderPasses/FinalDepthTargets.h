#pragma once

#include "rhi/RenderHardwareInterface.h"

namespace SFE::SystemsModule {
	struct RenderData;
}

namespace SFE::Render::RenderPasses {
	// Framebuffers over the lit image with the scene's depth, for a pass that draws into the picture after lighting
	// and tests against what is already there. Both textures alternate between frames, so there is one framebuffer per
	// pair they come in (and render pass), kept by the frame's Render::Resources and let go of, all of them at once,
	// whenever the frame's targets change (a new size, a pass made or let go): none outlives a target it is over, drawn
	// with again or not.
	class FinalDepthTargets {
	public:
		// Empty until the frame has both.
		FramebufferHandle get(const SystemsModule::RenderData& renderData, RenderPassHandle renderPass);
	};

	// Loads and keeps an RGBA16F colour and a 24-bit depth, the attachments FinalDepthTargets holds.
	RenderPassHandle createFinalDepthRenderPass();
}
