#include "DoFPass.h"

#include "assetsModule/modelModule/Vertex.h"
#include "assetsModule/shaderModule/ShaderBase.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "renderModule/RenderResources.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

void DoFPass::init(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();

	SFE::Render::SamplerDesc lin;
	lin.minFilter = SFE::Render::FilterMode::Linear;
	lin.magFilter = SFE::Render::FilterMode::Linear;
	mLinear = own(rhi->createSampler(lin));
	SFE::Render::SamplerDesc nearS;
	nearS.minFilter = SFE::Render::FilterMode::Nearest;
	nearS.magFilter = SFE::Render::FilterMode::Nearest;
	mNearest = own(rhi->createSampler(nearS));

	SFE::Render::RenderPassDesc rpDesc;
	rpDesc.colorAttachmentCount = 1;
	rpDesc.colorAttachments[0].loadOp = SFE::Render::AttachmentLoadOp::Clear;
	rpDesc.colorAttachments[0].storeOp = SFE::Render::AttachmentStoreOp::Store;
	rpDesc.colorAttachments[0].format = SFE::Render::TextureFormat::RGBA16F;
	mRenderPass = own(rhi->createRenderPass(rpDesc));
	resize(renderDataHandle);

	SFE::Render::DescriptorSetLayoutDesc layoutDesc;
	layoutDesc.bindings.emplace_back(0, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(1, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(5, SFE::Render::DescriptorType::UniformBuffer, SFE::Render::ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(15, SFE::Render::DescriptorType::UniformBufferDynamic, SFE::Render::ShaderStage::Fragment);
	mLayout = own(rhi->createDescriptorSetLayout(layoutDesc));
	// One set per frame slot: what it binds alternates with the slot, and writing a set the previous
	// frame still binds costs a copy of it.
	for (uint32_t i = 0; i < (std::max)(1u, rhi->framesInFlight()); ++i) {
		mSets.push_back(own(rhi->allocateDescriptorSet(mLayout)));
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
	pipelineDesc.shaders.vertex = keep(mShaders, rhi->loadShader(SFE::Render::ShaderStage::Vertex, "shaders/deferred_shading.vs"));
	pipelineDesc.shaders.fragment = keep(mShaders, rhi->loadShader(SFE::Render::ShaderStage::Fragment, "shaders/dof.fs"));
	mPipeline = own(rhi->createGraphicsPipeline(pipelineDesc));
}

void DoFPass::render(SystemsModule::RenderData& renderDataHandle, SFE::Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;

	auto* rhi = Engine::instance()->rhi();
	const DescriptorSetHandle mSet = mSets[rhi->frameInFlight() % mSets.size()];
	auto& frame = renderDataHandle.frame;
	const auto src = frame.texture(SFE::Render::FrameId::Final);
	const auto pos = frame.texture(SFE::Render::FrameId::SceneDepth);
	if (!src || !pos || !mPipeline || !renderDataHandle.cameraMatricesUBO) {
		return;
	}

	mParams.viewX = renderDataHandle.mCameraPos.x;
	mParams.viewY = renderDataHandle.mCameraPos.y;
	mParams.viewZ = renderDataHandle.mCameraPos.z;
	mParams.focusDistance = renderDataHandle.graphics.dofFocus;
	mParams.focusRange = renderDataHandle.graphics.dofRange;
	mParams.maxBlur = renderDataHandle.graphics.dofBlur;
	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	mParams.resX = screen.renderW > 0 ? static_cast<float>(screen.renderW) : 1.f;
	mParams.resY = screen.renderH > 0 ? static_cast<float>(screen.renderH) : 1.f;
	const auto params = rhi->allocTransient(mParams);

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
		w.images.push_back({ pos, mNearest });
		writes.push_back(std::move(w));
	}
	{
		SFE::Render::WriteDescriptor w{};
		w.set = mSet;
		w.binding = 5;
		w.buffers.push_back({ renderDataHandle.cameraMatricesUBO });
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

	SFE::Render::RenderPassBeginInfo beginInfo;
	beginInfo.framebuffer = mFbo;
	beginInfo.renderPass = mRenderPass;
	beginInfo.width = screen.renderW;
	beginInfo.height = screen.renderH;
	rhi->cmdBeginRenderPass(cmd, beginInfo);
	rhi->cmdBindPipeline(cmd, mPipeline);
	rhi->cmdBindDescriptorSets(cmd, mPipeline, 0, &mSet, 1, &params.offset, 1);
	SFE::Render::Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);

	frame.set(SFE::Render::FrameId::Final, { .texture = mColor, .sampler = mLinear, .framebuffer = mFbo });
}

void DoFPass::resize(SystemsModule::RenderData& renderDataHandle) {
	SFE::Render::screenTarget(*renderDataHandle.resources, name() + ".color", mColor, mFbo, mRenderPass);
}
