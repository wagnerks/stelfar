#include "SSSPass.h"

#include "assetsModule/modelModule/Vertex.h"
#include "assetsModule/shaderModule/ShaderBase.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

void SSSPass::init(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();

	SamplerDesc lin;
	lin.minFilter = FilterMode::Linear;
	lin.magFilter = FilterMode::Linear;
	mLinear = rhi->createSampler(lin);
	SamplerDesc nearS;
	nearS.minFilter = FilterMode::Nearest;
	nearS.magFilter = FilterMode::Nearest;
	mNearest = rhi->createSampler(nearS);

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
	layoutDesc.bindings.emplace_back(5, DescriptorType::UniformBuffer, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(3, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(15, DescriptorType::UniformBufferDynamic, ShaderStage::Fragment);
	mLayout = rhi->createDescriptorSetLayout(layoutDesc);
	// One set per frame slot: what it binds alternates with the slot, and writing a set the previous
	// frame still binds costs a copy of it.
	for (uint32_t i = 0; i < (std::max)(1u, rhi->framesInFlight()); ++i) {
		mSets.push_back(rhi->allocateDescriptorSet(mLayout));
	}

	GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(Vertex2D)), VertexInputRate::PerVertex);
	pipelineDesc.layout.attributes.emplace_back(0, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, position)), false);
	pipelineDesc.layout.attributes.emplace_back(1, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, texCoords)), false);
	pipelineDesc.topology = PrimitiveTopology::Triangles;
	pipelineDesc.cullMode = CullMode::None;
	pipelineDesc.depthStencilState.depthTest = false;
	pipelineDesc.depthStencilState.depthWrite = false;
	pipelineDesc.renderPass = mRenderPass;
	pipelineDesc.descriptorSetLayouts = { mLayout };
	pipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/deferred_shading.vs");
	pipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/sss.fs");
	mPipeline = rhi->createGraphicsPipeline(pipelineDesc);
}

void SSSPass::resize(SystemsModule::RenderData&) {
	Utils::screenTarget(mColor, mFbo, mRenderPass);
}

void SSSPass::render(SystemsModule::RenderData& renderDataHandle, CommandListHandle cmd) {
	FUNCTION_BENCHMARK;

	auto* rhi = Engine::instance()->rhi();
	const auto mSet = mSets[rhi->frameInFlight() % mSets.size()];
	auto& frame = renderDataHandle.frame;
	const auto src = frame.texture(FrameId::Final);
	const auto pos = frame.texture(FrameId::SceneDepth);
	const auto material = frame.texture(FrameId::GBufferMaterial);
	const auto nrm = frame.texture(FrameId::GBufferNormal);
	if (!renderDataHandle.graphics.sss) {
		return;
	}
	if (!src || !pos || !material || !nrm || !mPipeline || !renderDataHandle.cameraMatricesUBO) {
		return;
	}

	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	mParams.strength = renderDataHandle.graphics.sssStrength;
	mParams.radius = renderDataHandle.graphics.sssRadius;
	mParams.resX = screen.renderW > 0 ? static_cast<float>(screen.renderW) : 1.f;
	mParams.resY = screen.renderH > 0 ? static_cast<float>(screen.renderH) : 1.f;
	const auto params = rhi->allocTransient(mParams);

	auto sampOr = [&](const char* id, SamplerHandle fallback) {
		const auto s = frame.sampler(id);
		return s ? s : fallback;
	};

	std::vector<WriteDescriptor> writes;
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 0;
		w.images.push_back({ src, mLinear });
		writes.push_back(std::move(w));
	}
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 1;
		w.images.push_back({ pos, sampOr(FrameId::SceneDepth, mNearest) });
		writes.push_back(std::move(w));
	}
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 2;
		w.images.push_back({ material, mNearest });
		writes.push_back(std::move(w));
	}
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 5;
		w.buffers.push_back({ renderDataHandle.cameraMatricesUBO });
		writes.push_back(std::move(w));
	}
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 3;
		w.images.push_back({ nrm, sampOr(FrameId::GBufferNormal, mNearest) });
		writes.push_back(std::move(w));
	}
	{
		WriteDescriptor w{};
		w.set = mSet;
		w.binding = 15;
		w.buffers.push_back({ params.buffer, 0, sizeof(Params) });
		writes.push_back(std::move(w));
	}
	rhi->updateDescriptorSets(writes);

	RenderPassBeginInfo beginInfo;
	beginInfo.framebuffer = mFbo;
	beginInfo.renderPass = mRenderPass;
	beginInfo.width = screen.renderW;
	beginInfo.height = screen.renderH;
	rhi->cmdBeginRenderPass(cmd, beginInfo);
	rhi->cmdBindPipeline(cmd, mPipeline);
	rhi->cmdBindDescriptorSets(cmd, mPipeline, 0, &mSet, 1, &params.offset, 1);
	Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);

	frame.set(FrameId::Final, { .texture = mColor, .sampler = mLinear, .framebuffer = mFbo });
}
