#include "MistPass.h"

#include <algorithm>

#include "assetsModule/modelModule/Vertex.h"
#include "componentsModule/TransformComponent.h"
#include "componentsModule/WaterComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/UserFrameSet.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

namespace {
	// Mirrors MistBlock in mist.fs. std140.
	struct MistParams {
		SFE::Render::Atmosphere::SkyParams sky;
		float mist[4]{}; // x density at the water line per metre, y metres up over which it thins to 1/e, z haze per metre, w water level
		float view[4]{}; // x the view distance
	};
}

void MistPass::init(SystemsModule::RenderData&) {
	auto* rhi = Engine::instance()->rhi();
	mRenderPass = createFinalDepthRenderPass();

	SamplerDesc nearest;
	nearest.minFilter = FilterMode::Nearest;
	nearest.magFilter = FilterMode::Nearest;
	mNearest = rhi->createSampler(nearest);

	DescriptorSetLayoutDesc layoutDesc;
	layoutDesc.bindings.emplace_back(5, DescriptorType::UniformBuffer, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(7, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(15, DescriptorType::UniformBufferDynamic, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(20, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	mLayout = rhi->createDescriptorSetLayout(layoutDesc);
	mSets.resize((std::max)(1u, rhi->framesInFlight()));
	for (auto& set : mSets) {
		set = rhi->allocateDescriptorSet(mLayout);
	}

	GraphicsPipelineDesc desc;
	desc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(Vertex2D)), VertexInputRate::PerVertex);
	desc.layout.attributes.emplace_back(0, 0, VertexFormat::Float2, static_cast<uint32_t>(offsetof(Vertex2D, position)), false);
	desc.layout.attributes.emplace_back(1, 0, VertexFormat::Float2, static_cast<uint32_t>(offsetof(Vertex2D, texCoords)), false);
	desc.topology = PrimitiveTopology::Triangles;
	desc.cullMode = CullMode::None;
	desc.depthStencilState.depthTest = false;
	desc.depthStencilState.depthWrite = false;
	// The shader gives the light the fog adds and, in alpha, how much of what is behind it gets through.
	desc.blend.enabled = true;
	desc.blend.srcColor = BlendFactor::One;
	desc.blend.dstColor = BlendFactor::SrcAlpha;
	desc.renderPass = mRenderPass;
	desc.descriptorSetLayouts = { mLayout };
	desc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/atmosphere.vs");
	desc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/mist.fs");
	// What the application's mist hook (sfe_user_mist.glsl) reads of its own is in the user frame set, set 3.
	UserFrameSet::instance().extend(desc);
	mPipeline = rhi->createGraphicsPipeline(desc);
}

void MistPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;
	MistParams params;
	const auto sky = Atmosphere::sceneSky();
	// Always with a sky: it is also what hides where the view distance cuts the scene off.
	if (!mPipeline || !sky || !Atmosphere::skyParams(renderDataHandle, params.sky)) {
		return;
	}
	const auto fbo = mTargets.get(renderDataHandle, mRenderPass);
	const auto position = renderDataHandle.frame.texture(FrameId::SceneDepth);
	if (!fbo || !position) {
		return;
	}
	params.mist[0] = sky->mist;
	params.mist[1] = (std::max)(sky->mistHeight, 0.1f);
	params.mist[2] = sky->haze;
	params.view[0] = renderDataHandle.cameraProjection.getFar();
	for (const auto& [id, water, transform] : ECSHandler::registry().view<const ComponentsModule::WaterComponent, const ComponentsModule::TransformComponent>()) {
		if (water && transform) {
			params.mist[3] = transform->getPos(true).y;
			break;
		}
	}

	auto* rhi = Engine::instance()->rhi();
	const auto block = rhi->allocTransient(params);
	const auto set = mSets[rhi->frameInFlight() % mSets.size()];
	const auto skyLut = Render::Atmosphere::skyTexture();
	rhi->updateDescriptorSets({
		{ .set = set, .binding = 5, .buffers{ { renderDataHandle.cameraMatricesUBO } } },
		{ .set = set, .binding = 7, .images{ { position, mNearest } } },
		{ .set = set, .binding = 15, .buffers{ { block.buffer, 0, sizeof(params) } } },
		{ .set = set, .binding = 20, .images{ { skyLut.texture, skyLut.sampler } } },
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
	// The application's frame data, after the pass's own set (UserFrameSet::bind).
	UserFrameSet::instance().bind(cmd, mPipeline);
	Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);
}
