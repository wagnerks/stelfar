#include "TextOverlayPass.h"

#include "core/Engine.h"
#include "renderModule/FrameBlackboard.h"
#include "rhi/GraphicsBackend.h"
#include "renderModule/TextRenderer.h"
#include "systemsModule/systems/RenderSystem.h"
#include "debugModule/Profiler.h"

#include <cstdio>

namespace SFE::Render::RenderPasses {

	void TextOverlayPass::init(SystemsModule::RenderData&)
	{
		auto* rhi = Engine::instance()->rhi();
		mFont = FontsRegistry::instance()->getFont("fonts/DroidSans.ttf", 22);

		RenderPassDesc rpd;
		rpd.colorAttachmentCount = 1;
		rpd.colorAttachments[0].loadOp = AttachmentLoadOp::Load;
		rpd.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
		rpd.colorAttachments[0].format = TextureFormat::RGBA8;
		mRenderPass = rhi->createRenderPass(rpd);
		TextRenderer::instance()->bindRenderPass(mRenderPass);
	}

	void TextOverlayPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd)
	{
		auto* rhi = Engine::instance()->rhi();
		if (!mFont || !mRenderPass) {
			return;
		}

		// The GPU's work and the main thread's own (its waits for the GPU and the screen left out), and the share of the
		// frame each takes: the one near 100% holds the frame back.
		const auto load = SFE::Debug::Profiler::load();
		std::snprintf(
			mOverlayText,
			sizeof(mOverlayText),
			"%s | %s\nFPS: %d\nGPU: %.1f ms %.0f%%  CPU: %.1f ms %.0f%%",
			graphicsBackendName(activeGraphicsBackend()),
			rhi->deviceName(),
			Engine::instance()->getFPS(),
			static_cast<double>(load.gpuMs),
			static_cast<double>(load.gpu) * 100.0,
			static_cast<double>(load.cpuMs),
			static_cast<double>(load.cpu) * 100.0
		);

		const auto fb = renderDataHandle.frame.framebuffer(FrameId::Final);
		if (!fb) {
			return;
		}
		const auto& screen = Engine::instance()->getWindow()->getScreenData();

		RenderPassBeginInfo rpBeginInfo;
		rpBeginInfo.renderPass = mRenderPass;
		rpBeginInfo.framebuffer = fb;
		rpBeginInfo.width = screen.renderW;
		rpBeginInfo.height = screen.renderH;
		rhi->cmdBeginRenderPass(cmd, rpBeginInfo);

		TextRenderer::instance()->renderText(
			cmd,
			mOverlayText,
			14.f,
			34.f,
			1.f,
			Math::Vec3{ 0.95f, 0.98f, 1.f },
			mFont
		);

		rhi->cmdEndRenderPass(cmd);
	}

} // namespace SFE::Render::RenderPasses
