#include "SunShadowPass.h"

#include <algorithm>
#include <cstddef>

#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "renderModule/Utils.h"
#include "renderModule/renderPasses/LightingPass.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

// 8 a pixel is 128 a block, against the 12 the scene's cascades ask for at each pixel alone; the blur's tolerance is
// the AO's (ssao_blur.fs).
SunShadowPass::Settings SunShadowPass::settings{ 8.f, 0.01f };

namespace SFE::Render::RenderPasses {
namespace {
	PipelineHandle fullScreenPipeline(RenderPassHandle pass, DescriptorSetLayoutHandle setLayout, const char* fragment) {
		auto* rhi = Engine::instance()->rhi();
		GraphicsPipelineDesc desc;
		desc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(Vertex2D)), VertexInputRate::PerVertex);
		desc.layout.attributes.emplace_back(0, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, position)), false);
		desc.layout.attributes.emplace_back(1, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, texCoords)), false);
		desc.topology = PrimitiveTopology::Triangles;
		desc.cullMode = CullMode::None;
		desc.renderPass = pass;
		desc.descriptorSetLayouts = { setLayout };
		desc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/ssao.vs");
		desc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, fragment);
		return rhi->createGraphicsPipeline(desc);
	}

	// An R8 target cleared to lit.
	RenderPassHandle litTarget() {
		RenderPassDesc desc;
		desc.colorAttachmentCount = 1;
		desc.colorAttachments[0].loadOp = AttachmentLoadOp::Clear;
		desc.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
		desc.colorAttachments[0].clearColor = { 0.f, 0.f, 0.f, 0.f };
		desc.colorAttachments[0].format = TextureFormat::R8;
		return Engine::instance()->rhi()->createRenderPass(desc);
	}
}
}

void SunShadowPass::init(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();
	mMaskPass = litTarget();
	mBlurPass = litTarget();

	SamplerDesc samplerDesc;
	samplerDesc.minFilter = FilterMode::Nearest;
	samplerDesc.magFilter = FilterMode::Nearest;
	mSampler = rhi->createSampler(samplerDesc);

	// sun_shadow.fs: the depth, the normals, the material, the camera; the cascades' depth, their block and matrices.
	DescriptorSetLayoutDesc maskDesc;
	maskDesc.bindings.emplace_back(0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	maskDesc.bindings.emplace_back(1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	maskDesc.bindings.emplace_back(2, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	maskDesc.bindings.emplace_back(3, DescriptorType::UniformBuffer, ShaderStage::Fragment);
	maskDesc.bindings.emplace_back(4, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	maskDesc.bindings.emplace_back(5, DescriptorType::UniformBufferDynamic, ShaderStage::Fragment);
	maskDesc.bindings.emplace_back(6, DescriptorType::UniformBufferDynamic, ShaderStage::Fragment);
	mMaskSetLayout = rhi->createDescriptorSetLayout(maskDesc);

	// sun_shadow_blur.fs: the shadow, the depth, the material, the camera, its block.
	DescriptorSetLayoutDesc blurDesc;
	blurDesc.bindings.emplace_back(0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	blurDesc.bindings.emplace_back(1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	blurDesc.bindings.emplace_back(2, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	blurDesc.bindings.emplace_back(3, DescriptorType::UniformBuffer, ShaderStage::Fragment);
	blurDesc.bindings.emplace_back(4, DescriptorType::UniformBufferDynamic, ShaderStage::Fragment);
	mBlurSetLayout = rhi->createDescriptorSetLayout(blurDesc);

	mSlots.resize((std::max)(1u, rhi->framesInFlight()));
	for (auto& slot : mSlots) {
		slot.maskSet = rhi->allocateDescriptorSet(mMaskSetLayout);
		slot.blurSet = rhi->allocateDescriptorSet(mBlurSetLayout);
	}

	mMaskPipeline = fullScreenPipeline(mMaskPass, mMaskSetLayout, "shaders/sun_shadow.fs");
	mBlurPipeline = fullScreenPipeline(mBlurPass, mBlurSetLayout, "shaders/sun_shadow_blur.fs");

	resize(renderDataHandle);
}

void SunShadowPass::resize(SystemsModule::RenderData& renderDataHandle) {
	Utils::screenTarget(mMask, mMaskFbo, mMaskPass, TextureFormat::R8);
	Utils::screenTarget(mBlurred, mBlurredFbo, mBlurPass, TextureFormat::R8);
	// The blur set reads the first target: written again along with the G-buffer's, new as well.
	for (auto& slot : mSlots) {
		slot.depth = {};
	}
	renderDataHandle.frame.set(kMask, { .texture = mBlurred, .sampler = mSampler });
}

void SunShadowPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK

	auto* rhi = Engine::instance()->rhi();
	auto& frame = renderDataHandle.frame;
	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	RenderPassBeginInfo begin;
	begin.width = screen.renderW;
	begin.height = screen.renderH;
	begin.renderPass = mBlurPass;
	begin.framebuffer = mBlurredFbo;

	const auto* csm = frame.data<CsmFrame>(FrameId::Csm);
	const auto csmDepth = frame.texture(FrameId::ShadowCsmDepth);
	if (!csm || csm->shadowCascadeLevels.empty() || !csmDepth || !renderDataHandle.cameraMatricesUBO) {
		// No sun to cast one: lit all over, as the target is cleared.
		rhi->cmdBeginRenderPass(cmd, begin);
		rhi->cmdEndRenderPass(cmd);
		return;
	}

	auto& slot = mSlots[rhi->frameInFlight() % mSlots.size()];
	const auto depth = frame.texture(FrameId::SceneDepth);
	const auto normal = frame.texture(FrameId::GBufferNormal);
	const auto material = frame.texture(FrameId::GBufferMaterial);
	std::vector<WriteDescriptor> writes;
	if (slot.depth.id != depth.id || slot.normal.id != normal.id || slot.material.id != material.id || slot.csmDepth.id != csmDepth.id) {
		slot.depth = depth;
		slot.normal = normal;
		slot.material = material;
		slot.csmDepth = csmDepth;
		const auto depthSampler = frame.sampler(FrameId::SceneDepth);
		const auto materialSampler = frame.sampler(FrameId::GBufferMaterial);
		writes.push_back({ .set = slot.maskSet, .binding = 0, .images{ { depth, depthSampler } } });
		writes.push_back({ .set = slot.maskSet, .binding = 1, .images{ { normal, frame.sampler(FrameId::GBufferNormal) } } });
		writes.push_back({ .set = slot.maskSet, .binding = 2, .images{ { material, materialSampler } } });
		writes.push_back({ .set = slot.maskSet, .binding = 4, .images{ { csmDepth, frame.sampler(FrameId::ShadowCsmDepth) } } });
		writes.push_back({ .set = slot.blurSet, .binding = 0, .images{ { mMask, mSampler } } });
		writes.push_back({ .set = slot.blurSet, .binding = 1, .images{ { depth, depthSampler } } });
		writes.push_back({ .set = slot.blurSet, .binding = 2, .images{ { material, materialSampler } } });
	}
	if (slot.camera.id != renderDataHandle.cameraMatricesUBO.id) {
		slot.camera = renderDataHandle.cameraMatricesUBO;
		writes.push_back({ .set = slot.maskSet, .binding = 3, .buffers{ { slot.camera } } });
		writes.push_back({ .set = slot.blurSet, .binding = 3, .buffers{ { slot.camera } } });
	}

	// The lighting's own block (it fills it again for itself), but for the samples.
	LightingPass::CascadedShadowBlock block{};
	LightingPass::fillCascadedShadows(renderDataHandle, *csm, block);
	for (auto& cascade : block.data) {
		cascade.cascade.w = settings.samples;
	}
	const auto shadows = rhi->allocTransient(block);
	const auto matrices = Utils::allocMatrices(csm->lightSpaceMatrices, CsmFrame::kMaxCascades);
	const auto blurParams = rhi->allocTransient(BlurParams{ .tolerance = (std::max)(settings.tolerance, 1e-4f) });
	if (slot.shadows.id != shadows.buffer.id) {
		slot.shadows = shadows.buffer;
		writes.push_back({ .set = slot.maskSet, .binding = 5, .buffers{ { shadows.buffer, 0, sizeof(block) } } });
	}
	if (slot.matrices.id != matrices.buffer.id) {
		slot.matrices = matrices.buffer;
		writes.push_back({ .set = slot.maskSet, .binding = 6, .buffers{ { matrices.buffer, 0, sizeof(Math::Mat4) * CsmFrame::kMaxCascades } } });
	}
	if (slot.blurParams.id != blurParams.buffer.id) {
		slot.blurParams = blurParams.buffer;
		writes.push_back({ .set = slot.blurSet, .binding = 4, .buffers{ { blurParams.buffer, 0, sizeof(BlurParams) } } });
	}
	if (!writes.empty()) {
		rhi->updateDescriptorSets(writes);
	}

	RenderPassBeginInfo maskBegin = begin;
	maskBegin.renderPass = mMaskPass;
	maskBegin.framebuffer = mMaskFbo;
	rhi->cmdBeginRenderPass(cmd, maskBegin);
	rhi->cmdBindPipeline(cmd, mMaskPipeline);
	// The dynamic offsets in binding order: 5, 6.
	const uint32_t maskOffsets[] = { shadows.offset, matrices.offset };
	rhi->cmdBindDescriptorSets(cmd, mMaskPipeline, 0, &slot.maskSet, 1, maskOffsets, 2);
	Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);

	rhi->cmdBeginRenderPass(cmd, begin);
	rhi->cmdBindPipeline(cmd, mBlurPipeline);
	rhi->cmdBindDescriptorSets(cmd, mBlurPipeline, 0, &slot.blurSet, 1, &blurParams.offset, 1);
	Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);
}
