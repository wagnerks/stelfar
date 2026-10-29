#include "SkyPass.h"

#include <algorithm>

#include "assetsModule/modelModule/Vertex.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

void SkyPass::init(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();
	mRenderPass = own(createFinalDepthRenderPass());

	DescriptorSetLayoutDesc layoutDesc;
	layoutDesc.bindings.emplace_back(15, DescriptorType::UniformBufferDynamic, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(20, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(21, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	mLayout = own(rhi->createDescriptorSetLayout(layoutDesc));
	mSets.resize((std::max)(1u, rhi->framesInFlight()));
	for (auto& set : mSets) {
		set = own(rhi->allocateDescriptorSet(mLayout));
	}

	GraphicsPipelineDesc desc;
	desc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(Vertex2D)), VertexInputRate::PerVertex);
	desc.layout.attributes.emplace_back(0, 0, VertexFormat::Float2, static_cast<uint32_t>(offsetof(Vertex2D, position)), false);
	desc.layout.attributes.emplace_back(1, 0, VertexFormat::Float2, static_cast<uint32_t>(offsetof(Vertex2D, texCoords)), false);
	desc.topology = PrimitiveTopology::Triangles;
	desc.cullMode = CullMode::None;
	desc.depthStencilState.depthTest = true;
	desc.depthStencilState.depthWrite = false;
	desc.depthStencilState.depthCompareOp = CompareOp::LessEqual;
	desc.renderPass = mRenderPass;
	desc.descriptorSetLayouts = { mLayout };
	desc.shaders.vertex = keep(mShaders, rhi->loadShader(ShaderStage::Vertex, "shaders/atmosphere.vs"));
	desc.shaders.fragment = keep(mShaders, rhi->loadShader(ShaderStage::Fragment, "shaders/atmosphere.fs"));
	mPipeline = own(rhi->createGraphicsPipeline(desc));
}

void SkyPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;
	Atmosphere::SkyParams params;
	if (!mPipeline || !Atmosphere::skyParams(renderDataHandle, params)) {
		return;
	}
	const auto fbo = mTargets.get(renderDataHandle, mRenderPass);
	if (!fbo) {
		return;
	}

	auto* rhi = Engine::instance()->rhi();
	const auto block = rhi->allocTransient(params);
	const DescriptorSetHandle set = mSets[rhi->frameInFlight() % mSets.size()];
	const auto skyLut = Atmosphere::skyTexture();
	const auto clouds = Atmosphere::cloudTexture();
	rhi->updateDescriptorSets({
		{ .set = set, .binding = 15, .buffers{ { block.buffer, 0, sizeof(params) } } },
		{ .set = set, .binding = 20, .images{ { skyLut.texture, skyLut.sampler } } },
		{ .set = set, .binding = 21, .images{ { clouds.texture ? clouds.texture : skyLut.texture, clouds.sampler ? clouds.sampler : skyLut.sampler } } },
	});

	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	RenderPassBeginInfo begin;
	begin.renderPass = mRenderPass;
	begin.framebuffer = fbo;
	begin.width = screen.renderW;
	begin.height = screen.renderH;
	rhi->cmdBeginRenderPass(cmd, begin);
	rhi->cmdBindPipeline(cmd, mPipeline);
	rhi->cmdBindDescriptorSets(cmd, mPipeline, 0, &set, 1, &block.offset, 1);
	Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);
}
