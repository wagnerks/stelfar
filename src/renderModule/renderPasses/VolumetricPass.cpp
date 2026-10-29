#include "VolumetricPass.h"

#include <algorithm>

#include "assetsModule/modelModule/Vertex.h"
#include "assetsModule/shaderModule/ShaderBase.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/RenderResources.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

namespace {
	// The march takes one ray for every 4x4 pixels: the light in the haze is soft, and the apply brings it back to each
	// pixel by depth (volumetric_apply.fs), so the edges of what stands in front of it stay clean.
	constexpr int kMarchDivisor = 4;

	// Its modules kept in `shaders`.
	SFE::Render::PipelineHandle makeFs(SFE::Render::RenderHardwareInterface* rhi, SFE::Render::RenderPassHandle renderPass,
		SFE::Render::DescriptorSetLayoutHandle layout, const char* vsPath, const char* fsPath,
		std::vector<SFE::Render::Held<SFE::Render::ShaderModuleHandle>>& shaders) {
		SFE::Render::GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(SFE::Vertex2D)), SFE::Render::VertexInputRate::PerVertex);
		pipelineDesc.layout.attributes.emplace_back(0, 0, SFE::Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SFE::Vertex2D, position)), false);
		pipelineDesc.layout.attributes.emplace_back(1, 0, SFE::Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SFE::Vertex2D, texCoords)), false);
		pipelineDesc.topology = SFE::Render::PrimitiveTopology::Triangles;
		pipelineDesc.cullMode = SFE::Render::CullMode::None;
		pipelineDesc.depthStencilState.depthTest = false;
		pipelineDesc.depthStencilState.depthWrite = false;
		pipelineDesc.renderPass = renderPass;
		pipelineDesc.descriptorSetLayouts = { layout };
		pipelineDesc.shaders.vertex = SFE::Render::keep(shaders, rhi->loadShader(SFE::Render::ShaderStage::Vertex, vsPath));
		pipelineDesc.shaders.fragment = SFE::Render::keep(shaders, rhi->loadShader(SFE::Render::ShaderStage::Fragment, fsPath));
		return rhi->createGraphicsPipeline(pipelineDesc);
	}
}

void VolumetricPass::init(SystemsModule::RenderData& renderDataHandle) {
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

	SFE::Render::DescriptorSetLayoutDesc marchLayout;
	marchLayout.bindings.emplace_back(0, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	marchLayout.bindings.emplace_back(8, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	marchLayout.bindings.emplace_back(21, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	marchLayout.bindings.emplace_back(5, SFE::Render::DescriptorType::UniformBuffer, SFE::Render::ShaderStage::Fragment);
	marchLayout.bindings.emplace_back(11, SFE::Render::DescriptorType::UniformBufferDynamic, SFE::Render::ShaderStage::Fragment);
	marchLayout.bindings.emplace_back(15, SFE::Render::DescriptorType::UniformBufferDynamic, SFE::Render::ShaderStage::Fragment);
	mMarchLayout = own(rhi->createDescriptorSetLayout(marchLayout));
	mMarchSet = own(rhi->allocateDescriptorSet(mMarchLayout));

	SFE::Render::DescriptorSetLayoutDesc applyLayout;
	applyLayout.bindings.emplace_back(0, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	applyLayout.bindings.emplace_back(1, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	applyLayout.bindings.emplace_back(2, SFE::Render::DescriptorType::CombinedImageSampler, SFE::Render::ShaderStage::Fragment);
	applyLayout.bindings.emplace_back(5, SFE::Render::DescriptorType::UniformBuffer, SFE::Render::ShaderStage::Fragment);
	applyLayout.bindings.emplace_back(15, SFE::Render::DescriptorType::UniformBufferDynamic, SFE::Render::ShaderStage::Fragment);
	mApplyLayout = own(rhi->createDescriptorSetLayout(applyLayout));
	mApplySet = own(rhi->allocateDescriptorSet(mApplyLayout));

	mMarchPipeline = own(makeFs(rhi, mRenderPass, mMarchLayout, "shaders/deferred_shading.vs", "shaders/volumetric.fs", mShaders));
	mApplyPipeline = own(makeFs(rhi, mRenderPass, mApplyLayout, "shaders/deferred_shading.vs", "shaders/volumetric_apply.fs", mShaders));
}

void VolumetricPass::resize(SystemsModule::RenderData& renderDataHandle) {
	auto& resources = *renderDataHandle.resources;
	SFE::Render::screenTarget(resources, name() + ".march", mMarchTex, mMarchFbo, mRenderPass, SFE::Render::TextureFormat::RGBA16F, kMarchDivisor);
	SFE::Render::screenTarget(resources, name() + ".color", mColor, mFbo, mRenderPass);
}

void VolumetricPass::render(SystemsModule::RenderData& renderDataHandle, SFE::Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;

	auto* rhi = Engine::instance()->rhi();
	auto& frame = renderDataHandle.frame;
	const auto src = frame.texture(SFE::Render::FrameId::Final);
	const auto pos = frame.texture(SFE::Render::FrameId::SceneDepth);
	const auto shadow = frame.texture(SFE::Render::FrameId::ShadowCsm);
	if (!src || !pos || !mMarchPipeline || !mApplyPipeline) {
		return;
	}

	const auto* csm = frame.data<SFE::Render::CsmFrame>(SFE::Render::FrameId::Csm);
	mParams.viewX = renderDataHandle.mCameraPos.x;
	mParams.viewY = renderDataHandle.mCameraPos.y;
	mParams.viewZ = renderDataHandle.mCameraPos.z;
	mParams.density = renderDataHandle.graphics.volumetricDensity;
	mParams.anisotropy = renderDataHandle.graphics.volumetricAnisotropy;
	mParams.intensity = renderDataHandle.graphics.volumetricIntensity;
	mParams.maxDistance = renderDataHandle.graphics.volumetricDistance;
	{
		SFE::Render::Atmosphere::SkyParams sky;
		mParams.cloudCoverage = SFE::Render::Atmosphere::skyParams(renderDataHandle, sky) ? sky.toSun[3] : 0.f;
	}
	{
		const auto& screen = Engine::instance()->getWindow()->getScreenData();
		mParams.resX = screen.renderW > 0 ? static_cast<float>(screen.renderW) : 1.f;
		mParams.resY = screen.renderH > 0 ? static_cast<float>(screen.renderH) : 1.f;
	}
	if (csm) {
		mParams.lightX = csm->lightDirection.x;
		mParams.lightY = csm->lightDirection.y;
		mParams.lightZ = csm->lightDirection.z;
		mParams.colorX = csm->lightColor.x;
		mParams.colorY = csm->lightColor.y;
		mParams.colorZ = csm->lightColor.z;
		mParams.cascadeCount = static_cast<int>(csm->shadowCascades.size());
	}
	const auto params = rhi->allocTransient(mParams);
	static const std::vector<SFE::Math::Mat4> noMatrices;
	const auto matrices = SFE::Render::Utils::allocMatrices(csm ? csm->lightSpaceMatrices : noMatrices, SFE::Render::CsmFrame::kMaxCascades);

	std::vector<SFE::Render::WriteDescriptor> marchWrites;
	{
		SFE::Render::WriteDescriptor w{};
		w.set = mMarchSet;
		w.binding = 0;
		w.images.push_back({ pos, mNearest });
		marchWrites.push_back(std::move(w));
	}
	if (shadow) {
		SFE::Render::WriteDescriptor w{};
		w.set = mMarchSet;
		w.binding = 8;
		w.images.push_back({ shadow, frame.sampler(SFE::Render::FrameId::ShadowCsm) });
		marchWrites.push_back(std::move(w));
	}
	if (const auto clouds = SFE::Render::Atmosphere::cloudTexture(); clouds.texture) {
		SFE::Render::WriteDescriptor w{};
		w.set = mMarchSet;
		w.binding = 21;
		w.images.push_back({ clouds.texture, clouds.sampler });
		marchWrites.push_back(std::move(w));
	}
	if (renderDataHandle.cameraMatricesUBO) {
		SFE::Render::WriteDescriptor w{};
		w.set = mMarchSet;
		w.binding = 5;
		w.buffers.push_back({ renderDataHandle.cameraMatricesUBO });
		marchWrites.push_back(std::move(w));
	}
	{
		SFE::Render::WriteDescriptor w{};
		w.set = mMarchSet;
		w.binding = 11;
		w.buffers.push_back({ matrices.buffer, 0, sizeof(SFE::Math::Mat4) * SFE::Render::CsmFrame::kMaxCascades });
		marchWrites.push_back(std::move(w));
	}
	{
		SFE::Render::WriteDescriptor w{};
		w.set = mMarchSet;
		w.binding = 15;
		w.buffers.push_back({ params.buffer, 0, sizeof(Params) });
		marchWrites.push_back(std::move(w));
	}
	rhi->updateDescriptorSets(marchWrites);

	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	{
		SFE::Render::RenderPassBeginInfo beginInfo;
		beginInfo.framebuffer = mMarchFbo;
		beginInfo.renderPass = mRenderPass;
		beginInfo.width = std::max(1, screen.renderW / kMarchDivisor);
		beginInfo.height = std::max(1, screen.renderH / kMarchDivisor);
		rhi->cmdBeginRenderPass(cmd, beginInfo);
		rhi->cmdBindPipeline(cmd, mMarchPipeline);
		const uint32_t offsets[] = { matrices.offset, params.offset }; // bindings 11, 15
		const SFE::Render::DescriptorSetHandle marchSet = mMarchSet;
		rhi->cmdBindDescriptorSets(cmd, mMarchPipeline, 0, &marchSet, 1, offsets, 2);
		SFE::Render::Utils::renderQuad(cmd);
		rhi->cmdEndRenderPass(cmd);
	}

	{
		std::vector<SFE::Render::WriteDescriptor> applyWrites;
		{
			SFE::Render::WriteDescriptor w{};
			w.set = mApplySet;
			w.binding = 0;
			w.images.push_back({ src, mLinear });
			applyWrites.push_back(std::move(w));
		}
		{
			// Gathered texel by texel, and weighed by the depth of each: nothing blended by the sampler.
			SFE::Render::WriteDescriptor w{};
			w.set = mApplySet;
			w.binding = 1;
			w.images.push_back({ mMarchTex, mNearest });
			applyWrites.push_back(std::move(w));
		}
		{
			SFE::Render::WriteDescriptor w{};
			w.set = mApplySet;
			w.binding = 2;
			w.images.push_back({ pos, mNearest });
			applyWrites.push_back(std::move(w));
		}
		if (renderDataHandle.cameraMatricesUBO) {
			SFE::Render::WriteDescriptor w{};
			w.set = mApplySet;
			w.binding = 5;
			w.buffers.push_back({ renderDataHandle.cameraMatricesUBO });
			applyWrites.push_back(std::move(w));
		}
		{
			SFE::Render::WriteDescriptor w{};
			w.set = mApplySet;
			w.binding = 15;
			w.buffers.push_back({ params.buffer, 0, sizeof(Params) });
			applyWrites.push_back(std::move(w));
		}
		rhi->updateDescriptorSets(applyWrites);

		SFE::Render::RenderPassBeginInfo beginInfo;
		beginInfo.framebuffer = mFbo;
		beginInfo.renderPass = mRenderPass;
		beginInfo.width = screen.renderW;
		beginInfo.height = screen.renderH;
		rhi->cmdBeginRenderPass(cmd, beginInfo);
		rhi->cmdBindPipeline(cmd, mApplyPipeline);
		const SFE::Render::DescriptorSetHandle applySet = mApplySet;
		rhi->cmdBindDescriptorSets(cmd, mApplyPipeline, 0, &applySet, 1, &params.offset, 1); // binding 15
		SFE::Render::Utils::renderQuad(cmd);
		rhi->cmdEndRenderPass(cmd);
	}

	frame.set(SFE::Render::FrameId::Final, { .texture = mColor, .sampler = mLinear, .framebuffer = mFbo });
}
