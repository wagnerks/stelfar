#include "TAAPass.h"

#include "assetsModule/modelModule/Vertex.h"
#include "assetsModule/shaderModule/ShaderBase.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

void TAAPass::init(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();

	SFE::Render::SamplerDesc lin;
	lin.minFilter = SFE::Render::FilterMode::Linear;
	lin.magFilter = SFE::Render::FilterMode::Linear;
	mLinear = rhi->createSampler(lin);
	SFE::Render::SamplerDesc nearS;
	nearS.minFilter = SFE::Render::FilterMode::Nearest;
	nearS.magFilter = SFE::Render::FilterMode::Nearest;
	mNearest = rhi->createSampler(nearS);

	SFE::Render::RenderPassDesc rpDesc;
	rpDesc.colorAttachmentCount = 1;
	rpDesc.colorAttachments[0].loadOp = SFE::Render::AttachmentLoadOp::Clear;
	rpDesc.colorAttachments[0].storeOp = SFE::Render::AttachmentStoreOp::Store;
	rpDesc.colorAttachments[0].format = SFE::Render::TextureFormat::RGBA16F;
	mRenderPass = rhi->createRenderPass(rpDesc);
	resize(renderDataHandle);

	SFE::Render::DescriptorSetLayoutDesc layoutDesc;
	layoutDesc.bindings.emplace_back(0, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(1, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(2, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(3, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(15, SFE::Render::DescriptorType::UniformBufferDynamic, SFE::Render::ShaderStage::Fragment);
	mLayout = rhi->createDescriptorSetLayout(layoutDesc);
	// One set per frame slot: what it binds alternates with the slot, and writing a set the previous
	// frame still binds costs a copy of it.
	for (uint32_t i = 0; i < (std::max)(1u, rhi->framesInFlight()); ++i) {
		mSets.push_back(rhi->allocateDescriptorSet(mLayout));
	}

	SFE::Render::GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(SFE::Vertex2D)), SFE::Render::VertexInputRate::PerVertex);
	pipelineDesc.layout.attributes.emplace_back(0, 0, SFE::Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SFE::Vertex2D, position)), false);
	pipelineDesc.layout.attributes.emplace_back(1, 0, SFE::Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SFE::Vertex2D, texCoords)), false);
	pipelineDesc.topology = SFE::Render::PrimitiveTopology::Triangles;
	pipelineDesc.cullMode = SFE::Render::CullMode::None;
	pipelineDesc.depthStencilState.depthTest = false;
	pipelineDesc.depthStencilState.depthWrite = false;
	pipelineDesc.renderPass = mRenderPass;
	pipelineDesc.descriptorSetLayouts = { mLayout };
	pipelineDesc.shaders.vertex = rhi->loadShader(SFE::Render::ShaderStage::Vertex, "shaders/deferred_shading.vs");
	pipelineDesc.shaders.fragment = rhi->loadShader(SFE::Render::ShaderStage::Fragment, "shaders/taa.fs");
	mPipeline = rhi->createGraphicsPipeline(pipelineDesc);
}

void TAAPass::resize(SystemsModule::RenderData&) {
	for (int i = 0; i < 2; ++i) {
		SFE::Render::Utils::screenTarget(mHistory[i], mFbo[i], mRenderPass);
	}
	// What it held was a picture of another size.
	mHasHistory = false;
}

void TAAPass::render(SystemsModule::RenderData& renderDataHandle, SFE::Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;

	auto* rhi = Engine::instance()->rhi();
	const auto mSet = mSets[rhi->frameInFlight() % mSets.size()];
	auto& frame = renderDataHandle.frame;
	const auto src = frame.texture(SFE::Render::FrameId::Final);
	const auto pos = frame.texture(SFE::Render::FrameId::SceneDepth);
	const auto velocity = frame.texture(SFE::Render::FrameId::GBufferVelocity);
	if (!renderDataHandle.graphics.taa) {
		mHasHistory = false;
		return;
	}
	if (!src || !pos || !velocity || !mPipeline) {
		return;
	}

	// Both unjittered. The shader needs the pair, not just the previous one: differencing them
	// gives a true motion vector, so a still camera samples the history at the pixel centre
	// instead of at this frame's jitter offset, which would feed the jitter back into it.
	mParams.prevPV = renderDataHandle.prevPV;
	mParams.curPV = renderDataHandle.current.PV;
	mParams.blend = renderDataHandle.graphics.taaBlend;
	mParams.reset = mHasHistory ? 0 : 1;
	{
		const auto& screen = Engine::instance()->getWindow()->getScreenData();
		mParams.resX = screen.renderW > 0 ? static_cast<float>(screen.renderW) : 1.f;
		mParams.resY = screen.renderH > 0 ? static_cast<float>(screen.renderH) : 1.f;
	}
	const auto params = rhi->allocTransient(mParams);

	const int hist = 1 - mWrite;
	std::vector<SFE::Render::WriteDescriptor> writes;
	{
		SFE::Render::WriteDescriptor w{};
		w.set = mSet;
		w.binding = 0;
		w.images.push_back({ src, mLinear });
		writes.push_back(std::move(w));
	}
	{
		SFE::Render::WriteDescriptor w{};
		w.set = mSet;
		w.binding = 1;
		w.images.push_back({ mHistory[hist], mLinear });
		writes.push_back(std::move(w));
	}
	{
		SFE::Render::WriteDescriptor w{};
		w.set = mSet;
		w.binding = 2;
		// Always nearest: world position must never be filtered. Across a depth discontinuity a
		// linear tap blends foreground and background into a point that exists nowhere, which
		// reprojects far off and pins the current weight high on exactly the silhouette pixels
		// that need the history most.
		w.images.push_back({ pos, mNearest });
		writes.push_back(std::move(w));
	}
	{
		SFE::Render::WriteDescriptor w{};
		w.set = mSet;
		w.binding = 3;
		// Nearest for the same reason as the position: a filtered velocity across a silhouette points nowhere.
		w.images.push_back({ velocity, mNearest });
		writes.push_back(std::move(w));
	}
	{
		SFE::Render::WriteDescriptor w{};
		w.set = mSet;
		w.binding = 15;
		w.buffers.push_back({ params.buffer, 0, sizeof(Params) });
		writes.push_back(std::move(w));
	}
	rhi->updateDescriptorSets(writes);

	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	SFE::Render::RenderPassBeginInfo beginInfo;
	beginInfo.framebuffer = mFbo[mWrite];
	beginInfo.renderPass = mRenderPass;
	beginInfo.width = screen.renderW;
	beginInfo.height = screen.renderH;
	rhi->cmdBeginRenderPass(cmd, beginInfo);
	rhi->cmdBindPipeline(cmd, mPipeline);
	rhi->cmdBindDescriptorSets(cmd, mPipeline, 0, &mSet, 1, &params.offset, 1);
	SFE::Render::Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);

	frame.set(SFE::Render::FrameId::Final, { .texture = mHistory[mWrite], .sampler = mLinear, .framebuffer = mFbo[mWrite] });
	mWrite = hist;
	mHasHistory = true;
}
