#include "SSRPass.h"

#include "assetsModule/shaderModule/ShaderBase.h"
#include "core/Engine.h"
#include "renderModule/Utils.h"
#include "debugModule/Benchmark.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

void SSRPass::init(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();

	SamplerDesc samplerDesc;
	samplerDesc.minFilter = FilterMode::Linear;
	samplerDesc.magFilter = FilterMode::Linear;
	mSampler = rhi->createSampler(samplerDesc);

	RenderPassDesc rpDesc;
	rpDesc.colorAttachmentCount = 1;
	rpDesc.colorAttachments[0].loadOp = AttachmentLoadOp::Clear;
	rpDesc.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
	rpDesc.colorAttachments[0].format = TextureFormat::RGBA16F;
	mRenderPass = rhi->createRenderPass(rpDesc);
	resize(renderDataHandle);

	DescriptorSetLayoutDesc layoutDesc;
	layoutDesc.bindings.emplace_back(0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(2, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(4, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(3, DescriptorType::UniformBufferDynamic, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(5, DescriptorType::UniformBuffer, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(6, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	mLayout = rhi->createDescriptorSetLayout(layoutDesc);
	// One set per frame slot: what it binds alternates with the slot, and writing a set the previous
	// frame still binds costs a copy of it.
	for (uint32_t i = 0; i < (std::max)(1u, rhi->framesInFlight()); ++i) {
		mSets.push_back(rhi->allocateDescriptorSet(mLayout));
	}

	GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(Vertex2D)), Render::VertexInputRate::PerVertex);
	pipelineDesc.layout.attributes.emplace_back(0, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, position)), false);
	pipelineDesc.layout.attributes.emplace_back(1, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, texCoords)), false);
	pipelineDesc.topology = PrimitiveTopology::Triangles;
	pipelineDesc.cullMode = CullMode::None;
	pipelineDesc.depthStencilState.depthTest = false;
	pipelineDesc.depthStencilState.depthWrite = false;
	pipelineDesc.renderPass = mRenderPass;
	pipelineDesc.descriptorSetLayouts = { mLayout };
	pipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/deferred_shading.vs");
	pipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/ssr.fs");
	mPipeline = rhi->createGraphicsPipeline(pipelineDesc);
}

void SSRPass::resize(SystemsModule::RenderData&) {
	Utils::screenTarget(mColor, mFbo, mRenderPass);
}

void SSRPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;

	auto* rhi = Engine::instance()->rhi();
	const auto mSet = mSets[rhi->frameInFlight() % mSets.size()];
	auto& frame = renderDataHandle.frame;
	auto src = frame.texture(FrameId::Final);
	if (!src) {
		src = frame.texture(FrameId::Lit);
	}
	if (!renderDataHandle.graphics.ssr) {
		return;
	}
	if (!src || !mPipeline || !mFbo) {
		return;
	}

	mParams.viewPosX = renderDataHandle.mCameraPos.x;
	mParams.viewPosY = renderDataHandle.mCameraPos.y;
	mParams.viewPosZ = renderDataHandle.mCameraPos.z;
	mParams.maxDistance = renderDataHandle.graphics.ssrDistance;
	{
		const auto& screen = Engine::instance()->getWindow()->getScreenData();
		mParams.resolutionX = screen.renderW > 0 ? static_cast<float>(screen.renderW) : 1.f;
		mParams.resolutionY = screen.renderH > 0 ? static_cast<float>(screen.renderH) : 1.f;
	}
	const auto params = rhi->allocTransient(mParams);

	std::vector<WriteDescriptor> writes;
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 0;
		w.images.push_back({ frame.texture(FrameId::SceneDepth), frame.sampler(FrameId::SceneDepth) });
		writes.push_back(std::move(w));
	}
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 6;
		w.images.push_back({ frame.texture(FrameId::GBufferMaterial), frame.sampler(FrameId::GBufferMaterial) });
		writes.push_back(std::move(w));
	}
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 1;
		w.images.push_back({ frame.texture(FrameId::GBufferNormal), frame.sampler(FrameId::GBufferNormal) });
		writes.push_back(std::move(w));
	}
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 2;
		w.images.push_back({ frame.texture(FrameId::GBufferAlbedo), frame.sampler(FrameId::GBufferAlbedo) });
		writes.push_back(std::move(w));
	}
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 4;
		w.images.push_back({ src, mSampler });
		writes.push_back(std::move(w));
	}
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 3;
		w.buffers.push_back({ params.buffer, 0, sizeof(Params) });
		writes.push_back(std::move(w));
	}
	if (renderDataHandle.cameraMatricesUBO) {
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 5;
		w.buffers.push_back({ renderDataHandle.cameraMatricesUBO });
		writes.push_back(std::move(w));
	}
	rhi->updateDescriptorSets(writes);

	const int w = Engine::instance()->getWindow()->getScreenData().renderW;
	const int h = Engine::instance()->getWindow()->getScreenData().renderH;
	RenderPassBeginInfo beginInfo;
	beginInfo.framebuffer = mFbo;
	beginInfo.renderPass = mRenderPass;
	beginInfo.width = w;
	beginInfo.height = h;

	rhi->cmdBeginRenderPass(cmd, beginInfo);
	rhi->cmdBindPipeline(cmd, mPipeline);
	rhi->cmdBindDescriptorSets(cmd, mPipeline, 0, &mSet, 1, &params.offset, 1);
	Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);

	frame.set(FrameId::Final, { .texture = mColor, .sampler = mSampler, .framebuffer = mFbo });
}
