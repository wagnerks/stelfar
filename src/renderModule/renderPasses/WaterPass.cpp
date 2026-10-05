#include "WaterPass.h"

#include <cmath>

#include <algorithm>
#include <utility>

#include "assetsModule/modelModule/Vertex.h"
#include "componentsModule/TransformComponent.h"
#include "componentsModule/WaterComponent.h"
#include "core/ECSHandler.h"
#include "assetsModule/TextureHandler.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

namespace {
	// Mirrors WaterBlock in water.fs. std140.
	struct WaterParams {
		SFE::Render::Atmosphere::SkyParams sky;
		float surface[4]{}; // xz centre, y level, w half size
		float look[4]{};    // x clarity, yz 1 / render size, w 1 when the water looks for reflections on screen
		SFE::Math::Vec4 tintArea{}; // the area tint (AreaTintFrame); a zero area, no tint
		SFE::Math::Vec4 tintWater{};
		SFE::Math::Vec4 current{}; // xy the surface current (WaterComponent::current) on world x/z, metres a second
	};

	// The scene's first water surface under its sky, as the shaders take it: false with no sky or no water.
	bool waterParams(const SFE::SystemsModule::RenderData& renderData, WaterParams& params) {
		if (!SFE::Render::Atmosphere::skyParams(renderData, params.sky)) {
			return false;
		}
		for (const auto& [id, water, transform] : ECSHandler::registry().view<const SFE::ComponentsModule::WaterComponent, const SFE::ComponentsModule::TransformComponent>()) {
			if (!water || !transform) {
				continue;
			}
			const auto pos = transform->getPos(true);
			params.surface[0] = pos.x;
			params.surface[1] = pos.y;
			params.surface[2] = pos.z;
			params.surface[3] = water->size * 0.5f;
			params.look[0] = (std::max)(water->clarity, 0.1f);
			const auto& screen = SFE::Engine::instance()->getWindow()->getScreenData();
			params.look[1] = 1.f / static_cast<float>((std::max)(1, screen.renderW));
			params.look[2] = 1.f / static_cast<float>((std::max)(1, screen.renderH));
			// The same switch as the rest of the scene's screen-space reflections: without it the water shows only the sky.
			params.look[3] = renderData.graphics.ssr ? 1.f : 0.f;
			// The scene's own wind, not the settings' look of it: what floats is carried by this one too.
			if (const auto wind = SFE::Render::Atmosphere::sceneWind(false)) {
				const auto current = water->current(*wind);
				params.current = { current.x, current.y, 0.f, 0.f };
			}
			if (const auto* tint = renderData.frame.data<SFE::Render::AreaTintFrame>(SFE::Render::FrameId::AreaTint)) {
				params.tintArea = tint->area;
				params.tintWater = tint->water;
			}
			return true;
		}
		return false;
	}

	void makeSamplers(SFE::Render::SamplerHandle& nearest, SFE::Render::SamplerHandle& linear) {
		using namespace SFE::Render;
		auto* rhi = SFE::Engine::instance()->rhi();
		SamplerDesc desc;
		desc.minFilter = FilterMode::Nearest;
		desc.magFilter = FilterMode::Nearest;
		desc.addressU = AddressMode::ClampToEdge;
		desc.addressV = AddressMode::ClampToEdge;
		nearest = rhi->createSampler(desc);
		desc.minFilter = FilterMode::Linear;
		desc.magFilter = FilterMode::Linear;
		linear = rhi->createSampler(desc);
	}
}

void WaterPass::init(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();
	const auto& screen = Engine::instance()->getWindow()->getScreenData();

	makeSamplers(mNearest, mLinear);
	SamplerDesc ripple;
	ripple.minFilter = FilterMode::LinearMipmapLinear;
	ripple.magFilter = FilterMode::Linear;
	ripple.addressU = AddressMode::Repeat;
	ripple.addressV = AddressMode::Repeat;
	ripple.maxAnisotropy = 1.f;
	mRipple = rhi->createSampler(ripple);

	resize(renderDataHandle);

	mSurfacePass = createFinalDepthRenderPass();

	DescriptorSetLayoutDesc layoutDesc;
	layoutDesc.bindings.emplace_back(5, DescriptorType::UniformBuffer, ShaderStage::Vertex | ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(6, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(7, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(15, DescriptorType::UniformBufferDynamic, ShaderStage::Vertex | ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(20, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(21, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(22, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(23, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	mSurfaceLayout = rhi->createDescriptorSetLayout(layoutDesc);
	mSurfaceSets.resize((std::max)(1u, rhi->framesInFlight()));
	for (auto& set : mSurfaceSets) {
		set = rhi->allocateDescriptorSet(mSurfaceLayout);
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
	desc.renderPass = mSurfacePass;
	desc.descriptorSetLayouts = { mSurfaceLayout };
	desc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/water.vs");
	desc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/water.fs");
	mSurfacePipeline = rhi->createGraphicsPipeline(desc);
}

void WaterPass::resize(SystemsModule::RenderData&) {
	Utils::screenTarget(mSceneCopy, mSceneCopyFbo, {});
}

void WaterPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;
	WaterParams params;
	if (!mSurfacePipeline || !waterParams(renderDataHandle, params)) {
		return;
	}
	const auto finalFbo = renderDataHandle.frame.framebuffer(FrameId::Final);
	const auto surfaceFbo = mTargets.get(renderDataHandle, mSurfacePass);
	const auto sceneDepth = renderDataHandle.frame.texture(FrameId::SceneDepth);
	if (!finalFbo || !surfaceFbo || !sceneDepth) {
		return;
	}

	const auto& screen = Engine::instance()->getWindow()->getScreenData();

	auto* rhi = Engine::instance()->rhi();
	const auto set = mSurfaceSets[rhi->frameInFlight() % mSurfaceSets.size()];
	const auto block = rhi->allocTransient(params);
	const auto skyLut = Render::Atmosphere::skyTexture();
	const auto clouds = Render::Atmosphere::cloudTexture();
	// The fine ripple map; without it the water keeps the sum of waves alone, which is what it had before.
	const auto* ripple = AssetsModule::TextureHandler::instance()->loadTexture("textures/water/ripple.png");
	const auto tint = Utils::areaTintMap(renderDataHandle.frame);
	rhi->updateDescriptorSets({
		{ .set = set, .binding = 5, .buffers{ { renderDataHandle.cameraMatricesUBO } } },
		{ .set = set, .binding = 6, .images{ { mSceneCopy, mLinear } } },
		{ .set = set, .binding = 7, .images{ { sceneDepth, mNearest } } },
		{ .set = set, .binding = 15, .buffers{ { block.buffer, 0, sizeof(params) } } },
		{ .set = set, .binding = 20, .images{ { skyLut.texture, skyLut.sampler } } },
		{ .set = set, .binding = 21, .images{ { clouds.texture ? clouds.texture : skyLut.texture, clouds.sampler ? clouds.sampler : skyLut.sampler } } },
		{ .set = set, .binding = 22, .images{ { ripple ? ripple->texture : skyLut.texture, ripple ? mRipple : skyLut.sampler } } },
		{ .set = set, .binding = 23, .images{ { tint.first, tint.second } } },
	});
	// The lit scene under and around the water, for the refraction and for what the reflected rays run into.
	rhi->cmdBlit(cmd, finalFbo, mSceneCopyFbo, BlitMask::Color, BlitFilter::Nearest);

	RenderPassBeginInfo begin;
	begin.renderPass = mSurfacePass;
	begin.framebuffer = surfaceFbo;
	begin.width = static_cast<uint32_t>(screen.renderW);
	begin.height = static_cast<uint32_t>(screen.renderH);
	rhi->cmdBeginRenderPass(cmd, begin);
	rhi->cmdBindPipeline(cmd, mSurfacePipeline);
	rhi->cmdBindDescriptorSets(cmd, mSurfacePipeline, 0, &set, 1, &block.offset, 1);
	Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);
}

void UnderwaterPass::init(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();
	makeSamplers(mNearest, mLinear);
	resize(renderDataHandle);
	mPass = createFinalDepthRenderPass();

	DescriptorSetLayoutDesc layoutDesc;
	layoutDesc.bindings.emplace_back(5, DescriptorType::UniformBuffer, ShaderStage::Vertex | ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(6, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(7, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(15, DescriptorType::UniformBufferDynamic, ShaderStage::Vertex | ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(20, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(23, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
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
	desc.renderPass = mPass;
	desc.descriptorSetLayouts = { mLayout };
	desc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/ssao.vs");
	desc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/water_under.fs");
	mPipeline = rhi->createGraphicsPipeline(desc);
}

void UnderwaterPass::resize(SystemsModule::RenderData&) {
	Utils::screenTarget(mSceneCopy, mSceneCopyFbo, {});
}

void UnderwaterPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;
	WaterParams params;
	if (!mPipeline || !waterParams(renderDataHandle, params)) {
		return;
	}
	const float* eye = params.sky.cameraPos;
	if (eye[1] >= params.surface[1] || std::fabs(eye[0] - params.surface[0]) >= params.surface[3]
		|| std::fabs(eye[2] - params.surface[2]) >= params.surface[3]) {
		return;
	}
	const auto finalFbo = renderDataHandle.frame.framebuffer(FrameId::Final);
	const auto fbo = mTargets.get(renderDataHandle, mPass);
	const auto sceneDepth = renderDataHandle.frame.texture(FrameId::SceneDepth);
	if (!finalFbo || !fbo || !sceneDepth) {
		return;
	}
	auto* rhi = Engine::instance()->rhi();
	const auto set = mSets[rhi->frameInFlight() % mSets.size()];
	const auto block = rhi->allocTransient(params);
	const auto skyLut = Render::Atmosphere::skyTexture();
	const auto tint = Utils::areaTintMap(renderDataHandle.frame);
	rhi->updateDescriptorSets({
		{ .set = set, .binding = 5, .buffers{ { renderDataHandle.cameraMatricesUBO } } },
		{ .set = set, .binding = 6, .images{ { mSceneCopy, mLinear } } },
		{ .set = set, .binding = 7, .images{ { sceneDepth, mNearest } } },
		{ .set = set, .binding = 15, .buffers{ { block.buffer, 0, sizeof(params) } } },
		{ .set = set, .binding = 20, .images{ { skyLut.texture, skyLut.sampler } } },
		{ .set = set, .binding = 23, .images{ { tint.first, tint.second } } },
	});
	rhi->cmdBlit(cmd, finalFbo, mSceneCopyFbo, BlitMask::Color, BlitFilter::Nearest);

	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	RenderPassBeginInfo begin;
	begin.renderPass = mPass;
	begin.framebuffer = fbo;
	begin.width = static_cast<uint32_t>(screen.renderW);
	begin.height = static_cast<uint32_t>(screen.renderH);
	rhi->cmdBeginRenderPass(cmd, begin);
	rhi->cmdBindPipeline(cmd, mPipeline);
	rhi->cmdBindDescriptorSets(cmd, mPipeline, 0, &set, 1, &block.offset, 1);
	Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);
}
