#include "SSAOPass.h"

#include <algorithm>

#include "assetsModule/shaderModule/ShaderBase.h"
#include "core/Engine.h"
#include "renderModule/Utils.h"
#include "debugModule/Benchmark.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

void SSAOPass::init(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();

	RenderPassDesc mainRPD;
	mainRPD.colorAttachmentCount = 1;
	mainRPD.colorAttachments[0].loadOp = AttachmentLoadOp::Clear;
	mainRPD.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
	mainRPD.colorAttachments[0].clearColor = {1.f, 1.f, 1.f, 1.f};
	mainRPD.colorAttachments[0].format = TextureFormat::R8;
	main = rhi->createRenderPass(mainRPD);

	RenderPassDesc blurRPD;
	blurRPD.colorAttachmentCount = 1;
	blurRPD.colorAttachments[0].loadOp = AttachmentLoadOp::Clear;
	blurRPD.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
	blurRPD.colorAttachments[0].clearColor = {1.f, 1.f, 1.f, 1.f};
	blurRPD.colorAttachments[0].format = TextureFormat::R8;
	blur = rhi->createRenderPass(blurRPD);

	SamplerDesc samplerDesc;
	samplerDesc.minFilter = FilterMode::Linear;
	samplerDesc.magFilter = FilterMode::Linear;
	mData.sampler = rhi->createSampler(samplerDesc);

	Render::DescriptorSetLayoutDesc texLayoutDesc;
	texLayoutDesc.bindings.emplace_back(0, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(1, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(2, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(3, Render::DescriptorType::UniformBufferDynamic, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(5, Render::DescriptorType::UniformBuffer, Render::ShaderStage::Fragment);
	mainTexSetLayout = rhi->createDescriptorSetLayout(texLayoutDesc);

	Render::DescriptorSetLayoutDesc texLayoutBlurDesc;
	texLayoutBlurDesc.bindings.emplace_back(0, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutBlurDesc.bindings.emplace_back(1, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutBlurDesc.bindings.emplace_back(2, Render::DescriptorType::UniformBufferDynamic, Render::ShaderStage::Fragment);
	texLayoutBlurDesc.bindings.emplace_back(3, Render::DescriptorType::UniformBuffer, Render::ShaderStage::Fragment);
	texLayoutBlurDesc.bindings.emplace_back(4, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	blurTexSetLayout = rhi->createDescriptorSetLayout(texLayoutBlurDesc);
	mSlots.resize((std::max)(1u, rhi->framesInFlight()));
	for (auto& slot : mSlots) {
		slot.mainSet = rhi->allocateDescriptorSet(mainTexSetLayout);
		slot.blurSet = rhi->allocateDescriptorSet(blurTexSetLayout);
	}

	GraphicsPipelineDesc mainPipelineDesc;
	Render::VertexLayoutDesc& layout = mainPipelineDesc.layout;
	layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(Vertex2D)), Render::VertexInputRate::PerVertex);
	layout.attributes.emplace_back(0, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, position)), false);
	layout.attributes.emplace_back(1, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, texCoords)), false);
	mainPipelineDesc.topology = PrimitiveTopology::Triangles;
	mainPipelineDesc.cullMode = CullMode::None;
	mainPipelineDesc.renderPass = main;
	mainPipelineDesc.descriptorSetLayouts = { mainTexSetLayout };
	mainPipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/ssao.vs");
	mainPipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/ssao.fs");
	mainPipeline = rhi->createGraphicsPipeline(mainPipelineDesc);

	GraphicsPipelineDesc blurPipelineDesc;
	blurPipelineDesc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(Vertex2D)), Render::VertexInputRate::PerVertex);
	blurPipelineDesc.layout.attributes.emplace_back(0, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, position)), false);
	blurPipelineDesc.layout.attributes.emplace_back(1, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, texCoords)), false);
	blurPipelineDesc.topology = PrimitiveTopology::Triangles;
	blurPipelineDesc.cullMode = CullMode::None;
	blurPipelineDesc.renderPass = blur;
	blurPipelineDesc.descriptorSetLayouts = { blurTexSetLayout };
	blurPipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/ssao.vs");
	blurPipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/ssao_blur.fs");
	blurPipeline = rhi->createGraphicsPipeline(blurPipelineDesc);

	blurParams.sigmaS = mData.sigmaS;
	blurParams.sigmaL = mData.sigmaL;
	blurParams.updateFac();


	resize(renderDataHandle);
}

void SSAOPass::resize(SystemsModule::RenderData& renderDataHandle) {
	Utils::screenTarget(mData.mSsaoColorBuffer, mData.mSsaoFbo, main, TextureFormat::R8, 2);
	Utils::screenTarget(mData.mSsaoColorBufferBlur, mData.mSsaoBlurFbo, blur, TextureFormat::R8, 2);
	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	mSsaoW = (std::max)(1, screen.renderW / 2);
	mSsaoH = (std::max)(1, screen.renderH / 2);
	// The blur set reads the first target: written again along with the G-buffer's, new as well.
	for (auto& slot : mSlots) {
		slot.position = {};
		slot.normal = {};
	}
	renderDataHandle.frame.set(FrameId::SsaoColor, { .texture = mData.mSsaoColorBuffer, .sampler = mData.sampler });
	renderDataHandle.frame.set(FrameId::SsaoBlur, { .texture = mData.mSsaoColorBufferBlur, .sampler = mData.sampler });
}

void SSAOPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	if (!renderDataHandle.graphics.ssao) {
		return;
	}
	FUNCTION_BENCHMARK

	auto* rhi = Engine::instance()->rhi();
	auto& slot = mSlots[rhi->frameInFlight() % mSlots.size()];
	const auto gPos = renderDataHandle.frame.texture(FrameId::SceneDepth);
	const auto gPosSamp = renderDataHandle.frame.sampler(FrameId::SceneDepth);
	const auto gNrm = renderDataHandle.frame.texture(FrameId::GBufferNormal);
	const auto gNrmSamp = renderDataHandle.frame.sampler(FrameId::GBufferNormal);
	// The material: which pixels are leaves and blades (sfeGBufferMaterial), what the ground round a tuft is not shaded by.
	const auto gMat = renderDataHandle.frame.texture(FrameId::GBufferMaterial);
	const auto gMatSamp = renderDataHandle.frame.sampler(FrameId::GBufferMaterial);
	if (slot.position.id != gPos.id || slot.normal.id != gNrm.id || slot.material.id != gMat.id) {
		slot.position = gPos;
		slot.normal = gNrm;
		slot.material = gMat;
		rhi->updateDescriptorSets({
			{ .set = slot.mainSet, .binding = 0, .images{ { gPos, gPosSamp } } },
			{ .set = slot.mainSet, .binding = 1, .images{ { gNrm, gNrmSamp } } },
			{ .set = slot.mainSet, .binding = 2, .images{ { gMat, gMatSamp } } },
			{ .set = slot.blurSet, .binding = 0, .images{ { mData.mSsaoColorBuffer, mData.sampler } } },
			{ .set = slot.blurSet, .binding = 1, .images{ { gPos, gPosSamp } } },
			{ .set = slot.blurSet, .binding = 4, .images{ { gMat, gMatSamp } } },
		});
	}
	// Without TAA there is nothing to average the rotation, so a moving pattern would only
	// read as flicker -- hold it still in that case.
	const auto& graphics = renderDataHandle.graphics;
	params.intensity = graphics.ssaoIntensity;
	params.foliageFloor = graphics.ssaoFoliageFloor;
	params.radius = graphics.ssaoRadius;
	params.thickness = graphics.ssaoThickness;
	params.frameIndex = renderDataHandle.graphics.taa ? static_cast<float>(renderDataHandle.taaFrame % 8) : 0.f;
	const auto ssaoParams = rhi->allocTransient(params);
	const auto ssaoBlurParams = rhi->allocTransient(blurParams);
	if (ssaoParams.buffer.id != slot.mainParamsBufferId) {
		slot.mainParamsBufferId = ssaoParams.buffer.id;
		rhi->updateDescriptorSets({ { .set = slot.mainSet, .binding = 3, .buffers{ { ssaoParams.buffer, 0, sizeof(SSAOParams) } } } });
	}
	if (ssaoBlurParams.buffer.id != slot.blurParamsBufferId) {
		slot.blurParamsBufferId = ssaoBlurParams.buffer.id;
		rhi->updateDescriptorSets({ { .set = slot.blurSet, .binding = 2, .buffers{ { ssaoBlurParams.buffer, 0, sizeof(SSAOBlurParams) } } } });
	}

	if (renderDataHandle.cameraMatricesUBO && slot.camera.id != renderDataHandle.cameraMatricesUBO.id) {
		slot.camera = renderDataHandle.cameraMatricesUBO;
		rhi->updateDescriptorSets({
			{ .set = slot.mainSet, .binding = 5, .buffers{ { renderDataHandle.cameraMatricesUBO } } },
			{ .set = slot.blurSet, .binding = 3, .buffers{ { renderDataHandle.cameraMatricesUBO } } },
		});
	}

	const int w = mSsaoW > 0 ? mSsaoW : (std::max)(1, Engine::instance()->getWindow()->getScreenData().renderW / 2);
	const int h = mSsaoH > 0 ? mSsaoH : (std::max)(1, Engine::instance()->getWindow()->getScreenData().renderH / 2);

	RenderPassBeginInfo beginInfo;
	beginInfo.width = w;
	beginInfo.height = h;
	beginInfo.framebuffer = mData.mSsaoFbo;
	beginInfo.renderPass = main;
	rhi->cmdBeginRenderPass(cmd, beginInfo);
	rhi->cmdBindPipeline(cmd, mainPipeline);
	rhi->cmdBindDescriptorSets(cmd, mainPipeline, 0, &slot.mainSet, 1, &ssaoParams.offset, 1);
	Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);

	beginInfo.framebuffer = mData.mSsaoBlurFbo;
	beginInfo.renderPass = blur;
	rhi->cmdBeginRenderPass(cmd, beginInfo);
	rhi->cmdBindPipeline(cmd, blurPipeline);
	rhi->cmdBindDescriptorSets(cmd, blurPipeline, 0, &slot.blurSet, 1, &ssaoBlurParams.offset, 1);
	Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);
}
