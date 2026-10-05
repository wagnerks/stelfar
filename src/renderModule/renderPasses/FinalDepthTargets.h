#pragma once

#include <vector>

#include "rhi/RenderHardwareInterface.h"

namespace SFE::SystemsModule {
	struct RenderData;
}

namespace SFE::Render::RenderPasses {
	// Framebuffers over the lit image with the scene's depth, for a pass that draws into the picture after lighting
	// and tests against what is already there. Both textures alternate between frames, so one framebuffer is kept
	// per pair they come in -- all of them let go once the picture is drawn at another size, the textures under them
	// being gone.
	class FinalDepthTargets {
	public:
		// Empty until the frame has both.
		FramebufferHandle get(const SystemsModule::RenderData& renderData, RenderPassHandle renderPass);

	private:
		struct Entry {
			TextureHandle color;
			TextureHandle depth;
			FramebufferHandle fbo;
		};
		std::vector<Entry> mEntries;
		int mWidth = 0;
		int mHeight = 0;
	};

	// Loads and keeps an RGBA16F colour and a 24-bit depth, the attachments FinalDepthTargets holds.
	RenderPassHandle createFinalDepthRenderPass();
}
