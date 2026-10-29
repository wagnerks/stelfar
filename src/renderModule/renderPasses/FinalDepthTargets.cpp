#include "FinalDepthTargets.h"

#include "core/Engine.h"
#include "renderModule/RenderResources.h"
#include "systemsModule/systems/RenderSystem.h"

namespace SFE::Render::RenderPasses {
	FramebufferHandle FinalDepthTargets::get(const SystemsModule::RenderData& renderData, const RenderPassHandle renderPass) {
		const auto color = renderData.frame.texture(FrameId::Final);
		const auto depth = renderData.frame.texture(FrameId::GBufferDepth);
		if (!color || !depth || !renderData.resources) {
			return {};
		}
		return renderData.resources->framebuffer(color, depth, renderPass);
	}

	RenderPassHandle createFinalDepthRenderPass() {
		RenderPassDesc desc;
		desc.colorAttachmentCount = 1;
		desc.colorAttachments[0].loadOp = AttachmentLoadOp::Load;
		desc.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
		desc.colorAttachments[0].format = TextureFormat::RGBA16F;
		desc.hasDepthStencil = true;
		desc.depthAttachment.loadOp = AttachmentLoadOp::Load;
		desc.depthAttachment.storeOp = AttachmentStoreOp::Store;
		desc.depthAttachment.format = TextureFormat::DEPTH_COMPONENT24;
		return Engine::instance()->rhi()->createRenderPass(desc);
	}
}
