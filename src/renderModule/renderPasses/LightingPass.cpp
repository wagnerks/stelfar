#include "LightingPass.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include "assetsModule/TextureHandler.h"
#include "core/Clocks.h"
#include "core/Engine.h"
#include "core/Params.h"
#include "propertiesModule/SceneEvents.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/DebugView.h"
#include "renderModule/IblBaker.h"
#include "renderModule/UserFrameSet.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"
#include "imgui.h"
#include "componentsModule/LightSourceComponent.h"
#include "componentsModule/ModelComponent.h"
#include "componentsModule/ShelterComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "logsModule/logger.h"
#include "debugModule/Benchmark.h"
#include "ecss/Registry.h"
#include "renderModule/renderPasses/SunShadowPass.h"

using namespace SFE::Render::RenderPasses;

// The lighting's debug view, a number of the registry: --debug-term, the debugTerm command and the Graphics panel set it.
int& SFE::Render::debugView() {
	static int& view = SFE::Params::value(kDebugViewGroup, kDebugViewName, 0, { .help = "What the lighting draws instead of the "
		"lit picture (deferred_shading.fs's debugTerm): 0 the picture, 1 shadow, 2 ambient, 3 direct light, 4 albedo, "
		"5 normals, 6 AO, 7 irradiance, 8 reflection, 9 BRDF, 10-14 the shadow's cascades, 15 the material's AO.",
		.min = 0.f, .max = 15.f, .transient = true });
	return view;
}


LightingPass::LightingPass() {

}

void LightingPass::init(SystemsModule::RenderData& renderDataHandle)
{
	RenderPass::init(renderDataHandle);
	
	auto* rhi = Engine::instance()->rhi();

	Render::DescriptorSetLayoutDesc texLayoutDesc;
	texLayoutDesc.bindings.emplace_back(0, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(1, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(2, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(3, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(4, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(5, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(6, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(7, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(8, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(9, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(10, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(12, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(13, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(14, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(21, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(23, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);

	mTexSetLayout = rhi->createDescriptorSetLayout(texLayoutDesc);

	Render::DescriptorSetLayoutDesc uboLayoutDesc;
	uboLayoutDesc.bindings.emplace_back(3, Render::DescriptorType::UniformBufferDynamic, Render::ShaderStage::Fragment);
	uboLayoutDesc.bindings.emplace_back(5, Render::DescriptorType::UniformBuffer, Render::ShaderStage::Fragment);
	uboLayoutDesc.bindings.emplace_back(9, Render::DescriptorType::UniformBufferDynamic, Render::ShaderStage::Fragment);
	uboLayoutDesc.bindings.emplace_back(10, Render::DescriptorType::UniformBufferDynamic, Render::ShaderStage::Fragment);
	uboLayoutDesc.bindings.emplace_back(11, Render::DescriptorType::UniformBufferDynamic, Render::ShaderStage::Fragment);
	uboLayoutDesc.bindings.emplace_back(15, Render::DescriptorType::UniformBufferDynamic, Render::ShaderStage::Fragment);
	mUboSetLayout = rhi->createDescriptorSetLayout(uboLayoutDesc);

	mSlots.resize((std::max)(1u, rhi->framesInFlight()));
	for (auto& slot : mSlots) {
		slot.texSet = rhi->allocateDescriptorSet(mTexSetLayout);
		slot.uboSet = rhi->allocateDescriptorSet(mUboSetLayout);
	}

	mTexSetInitialized = true;

	RenderPassDesc rpDesc;
	rpDesc.colorAttachmentCount = 1;
	rpDesc.colorAttachments[0] = {
		.loadOp = AttachmentLoadOp::Clear,
		.storeOp = AttachmentStoreOp::Store,
		.clearColor = Math::Vec4{0.f, 0.f, 0.f, 1.f},
		.format = TextureFormat::RGBA16F,
	};

	renderPass = rhi->createRenderPass(rpDesc);

	{
		SamplerDesc presentSamp;
		presentSamp.minFilter = FilterMode::Linear;
		presentSamp.magFilter = FilterMode::Linear;
		mPresentSampler = rhi->createSampler(presentSamp);
	}
	resize(renderDataHandle);

	GraphicsPipelineDesc pipelineDesc{};
	Render::VertexLayoutDesc& layout = pipelineDesc.layout;
	layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(Vertex2D)), Render::VertexInputRate::PerVertex);
	layout.attributes.emplace_back(0, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, position)), false);
	layout.attributes.emplace_back(1, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, texCoords)), false);

	pipelineDesc.topology = PrimitiveTopology::Triangles;
	pipelineDesc.cullMode = CullMode::None;
	pipelineDesc.depthStencilState.depthTest = false;
	pipelineDesc.depthStencilState.depthWrite = false;
	pipelineDesc.renderPass = renderPass;
	pipelineDesc.descriptorSetLayouts = { mTexSetLayout, mUboSetLayout };
	pipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/deferred_shading.vs");
	pipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/deferred_shading.fs");
	// deferred_shading.fs's PushConstants: the shelters right after shelterSoft.
	static_assert(offsetof(DrawData, shelters) == 112);
	// What the application's lighting hook (sfe_user_lighting.glsl) reads of its own is in the user frame set, set 3.
	UserFrameSet::instance().extend(pipelineDesc);
	pipeline = rhi->createGraphicsPipeline(pipelineDesc);

	{
		TextureDesc dummyShadow;
		dummyShadow.width = 1;
		dummyShadow.height = 1;
		dummyShadow.depth = 6;
		dummyShadow.dimension = TextureDimension::Array;
		dummyShadow.format = TextureFormat::DEPTH_COMPONENT32;
		dummyShadow.usage = TextureUsage::Sampled;
		mDummyPointShadow = rhi->createTexture(dummyShadow);
		SamplerDesc dummySamp;
		dummySamp.minFilter = FilterMode::Nearest;
		dummySamp.magFilter = FilterMode::Nearest;
		dummySamp.addressU = AddressMode::ClampToEdge;
		dummySamp.addressV = AddressMode::ClampToEdge;
		dummySamp.addressW = AddressMode::ClampToEdge;
		dummySamp.compareOp = CompareOp::LessEqual;
		mDummyPointShadowSampler = rhi->createSampler(dummySamp);
		for (auto& slot : mSlots) {
			rhi->updateDescriptorSets({
				{ slot.texSet, 7, {}, {{ mDummyPointShadow, mDummyPointShadowSampler }} },
			});
			slot.pointShadow = { mDummyPointShadow, mDummyPointShadowSampler };
		}
	}

	mIbl = {};
	bakeSkyLight(renderDataHandle);
	// The sky light by the rule of whatever takes something of the world (SceneEvents.h): the sky there now (above), and
	// the sky of each scene loaded after, taken as it is told of the way init takes it -- baked at once, not a bake
	// behind, so a scene told of before the first frame lights that frame. The RenderData is RenderSystem's and outlives
	// the pass, whose listeners RenderSystem forgets before it goes.
	PropertiesModule::onSceneLoaded(this, [this, data = &renderDataHandle](const PropertiesModule::SceneLoaded&) {
		if (skyLightKey(*data) == mIblKey) {
			// The sky taken already, or still none.
			return;
		}
		if (mIblBaking.valid()) {
			// A bake of the sky before it: stale.
			mIblBaking.wait();
			mIblBaking = {};
		}
		bakeSkyLight(*data);
	});
}

void LightingPass::bakeSkyLight(SystemsModule::RenderData& renderDataHandle) {
	// Into the maps there are, made the first time (uploadIbl): the sky of a scene told of later is written over the maps
	// the frames bind rather than into new ones.
	if (auto* rhi = Engine::instance()->rhi()) {
		SFE::Render::uploadIbl(rhi, SFE::Render::computeIbl(SFE::Render::Atmosphere::sceneSky(), renderDataHandle.graphics.sunIntensity), mIbl);
	}
	mIblKey = skyLightKey(renderDataHandle);
	useSkyLight(renderDataHandle);
}

void LightingPass::useSkyLight(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();
	if (mIbl.irradiance && mIbl.prefilter && mIbl.brdfLut) {
		for (auto& slot : mSlots) {
			rhi->updateDescriptorSets({
				{ .set = slot.texSet, .binding = 12, .images{ { mIbl.irradiance, mIbl.irradianceSampler } } },
				{ .set = slot.texSet, .binding = 13, .images{ { mIbl.prefilter, mIbl.prefilterSampler } } },
				{ .set = slot.texSet, .binding = 14, .images{ { mIbl.brdfLut, mIbl.lutSampler } } },
			});
		}
		renderDataHandle.frame.set(FrameId::IblIrradiance, { .texture = mIbl.irradiance, .sampler = mIbl.irradianceSampler });
		renderDataHandle.frame.set(FrameId::IblPrefilter, { .texture = mIbl.prefilter, .sampler = mIbl.prefilterSampler });
		renderDataHandle.frame.set(FrameId::IblBrdf, { .texture = mIbl.brdfLut, .sampler = mIbl.lutSampler });
	}
}

std::array<float, 8> LightingPass::skyLightKey(const SystemsModule::RenderData& renderDataHandle) {
	const auto sky = SFE::Render::Atmosphere::sceneSky();
	if (!sky) {
		return {};
	}
	return { sky->sunElevation, sky->sunAzimuth, sky->cloudCoverage, renderDataHandle.graphics.sunIntensity, 1.f,
		sky->groundAlbedo.x, sky->groundAlbedo.y, sky->groundAlbedo.z };
}

void LightingPass::updateSkyLight(SystemsModule::RenderData& renderDataHandle) {
	// Baked again when the sun or the sky changed: worked out on a worker, as it takes the best part of a second, and
	// written into the spare maps once it is done and they swapped in, the next started only then. A sun going along its
	// path never holds a frame up, and the sky's light follows it a bake behind (when frames step the world, exactly one
	// frame behind: Clocks::ready). The maps written are never those a frame in flight reads: made anew each time (and
	// the old freed), or written over while still read, they held the driver up for seconds at a time.
	if (mIblBaking.valid()) {
		if (SFE::Clocks::ready(mIblBaking)) {
			SFE::Render::uploadIbl(Engine::instance()->rhi(), mIblBaking.get(), mIblSpare);
			std::swap(mIbl, mIblSpare);
			useSkyLight(renderDataHandle);
		}
	}
	else if (const auto key = skyLightKey(renderDataHandle); key != mIblKey) {
		mIblKey = key;
		mIblBaking = std::async(std::launch::async, SFE::Render::computeIbl, SFE::Render::Atmosphere::sceneSky(), renderDataHandle.graphics.sunIntensity);
	}
}

void LightingPass::resize(SystemsModule::RenderData& renderDataHandle) {
	for (int i = 0; i < 2; ++i) {
		Utils::screenTarget(mPresentTex[i], mPresentFbo[i], renderPass);
	}
	renderDataHandle.frame.set(FrameId::Lit, { .texture = mPresentTex[0], .sampler = mPresentSampler });
	renderDataHandle.frame.set(FrameId::Final, { .texture = mPresentTex[0], .sampler = mPresentSampler, .framebuffer = mPresentFbo[0] });
}

void LightingPass::fillCascadedShadows(const SystemsModule::RenderData& renderDataHandle, const CsmFrame& csm,
	CascadedShadowBlock& block) {
	block.direction_intensity.xyz = csm.lightDirection;
	block.direction_intensity.w = csm.shadowsIntensity;
	block.color_and_misc.xyz = csm.lightColor;
	block.color_and_misc.w = static_cast<float>(csm.shadowCascades.size());

	for (size_t i = 0; i < csm.shadowCascades.size(); ++i) {
		auto& cascade = csm.shadowCascades[i];
		auto& cascadeRenderData = block.data[i];

		const float invRes = 1.f / (std::max)(1.f, csm.resolution.x);
		cascadeRenderData.cascade.x = invRes;
		// y carried max(texelSize.x, invRes), but texelSize is never filled for cascades and no
		// shader read the slot -- it held a duplicate of x. It now carries the world size of one
		// shadow-map texel, which is what the normal-offset bias needs.
		cascadeRenderData.cascade.y = cascade.worldPerTexel;
		// bias is authored in depth units. That means very different world distances per
		// cascade (worldPerTexel and depthRange differ by ~40x between the near and far one),
		// which is worth revisiting -- but converting it to texel-relative units changes the
		// picture on both backends, so it is not a standalone fix.
		cascadeRenderData.cascade.z = std::isfinite(cascade.bias) ? cascade.bias : 0.f;
		cascadeRenderData.cascade.w = static_cast<float>(cascade.samples);
		cascadeRenderData.cascadeExtra.x = cascade.depthRange;
		cascadeRenderData.cascadeExtra.y = renderDataHandle.graphics.shadowSunWidth;
		cascadeRenderData.cascadeExtra.z = renderDataHandle.graphics.shadowPenumbraMost;
		// How far the light spreads under the skin, metres: sss.fs's three taps.
		cascadeRenderData.cascadeExtra.w = 3.f * renderDataHandle.graphics.sssRadius;
	}

	const auto& levels = csm.shadowCascadeLevels;
	auto splitAt = [&levels](size_t i) {
		return (i < levels.size()) ? levels[i] : 1.0e6f;
	};
	block.cascadeSplits = {
		splitAt(1), splitAt(2), splitAt(3), splitAt(4)
	};
	block.cascadeSplits2 = {
		splitAt(5), splitAt(6), 1.0e6f,
		ECSHandler::getSystem<SystemsModule::RenderSystem>()->isShadowsDebugData() ? 1.f : 0.f
	};
}

void LightingPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	assert(mTexSetInitialized);
	updateSkyLight(renderDataHandle);
	FUNCTION_BENCHMARK;

	auto* rhi = Engine::instance()->rhi();
	auto& frame = renderDataHandle.frame;
	const auto* point = frame.data<PointShadowFrame>(FrameId::PointShadow);
	const auto* csm = frame.data<CsmFrame>(FrameId::Csm);
	drawData.pointLightsSize = 0;
	if (point) {
		const float t = renderDataHandle.current.wind.w;
		for (const auto& lit : point->lights) {
			if (drawData.pointLightsSize >= static_cast<int>(std::size(pointLight))) {
				break;
			}
			auto lightComp = ECSHandler::registry().pinComponent<const LightSourceComponent>(lit.entity);
			if (!lightComp) {
				continue;
			}

			auto& light = pointLight[drawData.pointLightsSize++];
			light.Position = lit.position;
			light.texelSize = Math::Vec2{ lightComp->getTexelSize().x, lightComp->getTexelSize().y };
			light.bias = lightComp->getBias();
			light.samples = (std::clamp)(lightComp->getSamples(), 1, 64);
			light.radius = lightComp->mRadius;
			light.offset = lit.firstFace;
			light.Layers = lit.faces;
			light.Type = lit.spot ? 1 : 0;
			light.Direction = lit.direction;
			light.cosInner = lit.cosInner;
			light.cosOuter = lit.cosOuter;

			light.Color = lightComp->getLightColor() * lightComp->getIntensity();
			if (lightComp->mFlicker > 0.f) {
				light.Color = light.Color * lightComp->flickerGain(t);
			}
			light.Linear = lightComp->mLinear;
			light.Quadratic = lightComp->mQuadratic;
		}
	}

	if (csm && !csm->shadowCascadeLevels.empty()) {
		const auto& lightColor = csm->lightColor;
		drawData.ambientX = lightColor.x;
		drawData.ambientY = lightColor.y;
		drawData.ambientZ = lightColor.z;
		drawData.cascadeCount = static_cast<int>(csm->shadowCascades.size());
		drawData.shadowIntensity = csm->shadowsIntensity;

		fillCascadedShadows(renderDataHandle, *csm, cascadedShadowData);
	}

	drawData.fogEnd = Engine::instance()->getWindow()->getScreenData().far;
	drawData.fogStart = Engine::instance()->getWindow()->getScreenData().far * renderDataHandle.graphics.fogStart;
	drawData.ssaoAmount = renderDataHandle.graphics.ssao ? 1.f : 0.f;
	drawData.ssaoSun = renderDataHandle.graphics.ssaoSun;
	drawData.viewPosX = renderDataHandle.mCameraPos.x;
	drawData.viewPosY = renderDataHandle.mCameraPos.y;
	drawData.viewPosZ = renderDataHandle.mCameraPos.z;
	drawData.viewDirX = renderDataHandle.mViewDir.x;
	drawData.viewDirY = renderDataHandle.mViewDir.y;
	drawData.viewDirZ = renderDataHandle.mViewDir.z;
	drawData.iblStrength = renderDataHandle.graphics.iblStrength;
	drawData.shadowAmbient = renderDataHandle.graphics.shadowAmbient;
	drawData.padLook0 = renderDataHandle.graphics.sssWrap;
	drawData.padLook1 = renderDataHandle.graphics.sssStrength;
	drawData.debugTerm = static_cast<float>(Render::debugView());
	drawData.sheenStrength = renderDataHandle.graphics.sheen ? renderDataHandle.graphics.sheenStrength : 0.f;
	drawData.rimStrength = renderDataHandle.graphics.rimStrength;
	drawData.taaFrame = renderDataHandle.graphics.taa ? static_cast<float>(renderDataHandle.taaFrame % 4096) : -1.f;
	{
		Atmosphere::SkyParams sky;
		drawData.cloudCoverage = Atmosphere::skyParams(renderDataHandle, sky) ? sky.toSun[3] : 0.f;
		drawData.stylise = renderDataHandle.graphics.stylise;
	}
	int shelters = 0;
	for (const auto& [id, shelter, transform] : ECSHandler::registry().view<const ShelterComponent, const TransformComponent>()) {
		if (shelter && transform && shelters < DrawData::kMostShelters) {
			auto& to = drawData.shelters[shelters++];
			to.toLocal = Math::inverseAffine(transform->getTransform());
			to.box = { shelter->size.x, shelter->size.y, shelter->size.z, shelter->eaves };
			to.sky = { shelter->sky, 0.f, 0.f, 0.f };
		}
	}
	drawData.shelterCount = static_cast<float>(shelters);
	drawData.shelterSoft = ShelterComponent::kSoft;

	// Every block every frame: a dynamic binding needs this frame's offset even when its data did not change.
	static const std::vector<Math::Mat4> noMatrices;
	const UboBlocks blocks{
		.pointMatrices = Utils::allocMatrices(point ? point->matrices : noMatrices, PointShadowFrame::kMaxShadowFaces),
		.cascadedShadows = rhi->allocTransient(cascadedShadowData),
		.pointLights = rhi->allocTransient(pointLight),
		.lightMatrices = Utils::allocMatrices(csm ? csm->lightSpaceMatrices : noMatrices, CsmFrame::kMaxCascades),
		.drawData = rhi->allocTransient(drawData),
	};

	const int write = 1 - mPresentShow;

	RenderPassBeginInfo beginInfo;
	beginInfo.framebuffer = mPresentFbo[write];
	beginInfo.renderPass = renderPass;
	beginInfo.width = Engine::instance()->getWindow()->getScreenData().renderW;
	beginInfo.height = Engine::instance()->getWindow()->getScreenData().renderH;

	rhi->cmdBeginRenderPass(cmd, beginInfo);
	rhi->cmdBindPipeline(cmd, pipeline);
	updateTextures(renderDataHandle, cmd, blocks);
	Utils::renderQuad(cmd);
	rhi->cmdEndRenderPass(cmd);

	mPresentShow = write;
	renderDataHandle.frame.set(FrameId::Lit, { .texture = mPresentTex[write], .sampler = mPresentSampler });
	renderDataHandle.frame.set(FrameId::Final, { .texture = mPresentTex[write], .sampler = mPresentSampler, .framebuffer = mPresentFbo[write] });
}

void LightingPass::updateTextures(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd, const UboBlocks& blocks)
{
	auto rhi = Engine::instance()->rhi();
	auto& slotSets = mSlots[rhi->frameInFlight() % mSlots.size()];
	const auto texSet = slotSets.texSet;
	const auto uboSet = slotSets.uboSet;

	std::vector<Render::WriteDescriptor> writes;
	auto& frame = renderDataHandle.frame;
	const auto* csm = frame.data<CsmFrame>(FrameId::Csm);

	auto addImage = [&](uint32_t binding, TextureHandle tex, SamplerHandle samp, BoundImage& prev) {
		if (tex.id == prev.texture.id && samp.id == prev.sampler.id) {
			return;
		}
		prev.texture = tex;
		prev.sampler = samp;
		if (!tex) {
			return;
		}
		writes.push_back({
			.set = texSet,
			.binding = binding,
			
			.images{ {tex, samp} }
		});
	};
	auto addBuffer = [&](uint32_t binding, BufferHandle buf, BufferHandle& prev, size_t range = 0) {
		if (buf.id == prev.id) {
			return;
		}
		prev = buf;
		if (!buf) {
			return;
		}
		writes.push_back({
			.set = uboSet,
			.binding = binding,
			
			.buffers{ {buf, 0, range} }
		});
	};

	addImage(0, frame.texture(FrameId::GBufferMaterial), frame.sampler(FrameId::GBufferMaterial), slotSets.gMaterial);
	addImage(4, frame.texture(FrameId::SceneDepth), frame.sampler(FrameId::SceneDepth), slotSets.sceneDepth);
	addBuffer(11, blocks.lightMatrices.buffer, slotSets.lightMat, sizeof(Math::Mat4) * CsmFrame::kMaxCascades);
	addImage(1, frame.texture(FrameId::GBufferNormal), frame.sampler(FrameId::GBufferNormal), slotSets.gNormal);
	addImage(2, frame.texture(FrameId::GBufferAlbedo), frame.sampler(FrameId::GBufferAlbedo), slotSets.gAlbedo);
	addImage(9, frame.texture(FrameId::GBufferVelocity), frame.sampler(FrameId::GBufferVelocity), slotSets.gVelocity);
	addImage(23, frame.texture(FrameId::GBufferEmissive), frame.sampler(FrameId::GBufferEmissive), slotSets.gEmissive);

	{
		Render::TextureHandle ssaoTex = frame.texture(FrameId::SsaoBlur);
		Render::SamplerHandle ssaoSampler = frame.sampler(FrameId::SsaoBlur);
		if (!ssaoTex) {
			if (const auto* white = AssetsModule::TextureHandler::instance()->loadTexture("white.png")) {
				ssaoTex = white->texture;
				ssaoSampler = white->sampler;
			}
		}
		addImage(3, ssaoTex, ssaoSampler, slotSets.ssao);
	}
	addImage(10, frame.texture(SunShadowPass::kMask), frame.sampler(SunShadowPass::kMask), slotSets.sunShadow);

	{
		// The cloud layer, for the shadows the clouds lay over the ground.
		const auto clouds = Atmosphere::cloudTexture();
		addImage(21, clouds.texture, clouds.sampler, slotSets.clouds);
	}
	addBuffer(3, blocks.pointMatrices.buffer, slotSets.pointMat, sizeof(Math::Mat4) * PointShadowFrame::kMaxShadowFaces);
	addImage(7, frame.texture(FrameId::ShadowPoint), frame.sampler(FrameId::ShadowPoint), slotSets.pointShadow);

	{
		Render::TextureHandle outlinesTex = frame.texture(FrameId::GBufferOutlines);
		Render::SamplerHandle outlinesSampler = frame.sampler(FrameId::GBufferOutlines);
		if (!outlinesTex) {
			if (const auto* white = AssetsModule::TextureHandler::instance()->loadTexture("white.png")) {
				outlinesTex = white->texture;
				outlinesSampler = white->sampler;
			}
		}
		addImage(5, outlinesTex, outlinesSampler, slotSets.outlines);
	}

	if (csm) {
		addImage(8, frame.texture(FrameId::ShadowCsm), frame.sampler(FrameId::ShadowCsm), slotSets.csm);
		addImage(6, frame.texture(FrameId::ShadowCsmDepth), frame.sampler(FrameId::ShadowCsmDepth), slotSets.csmDepth);
	}

	addBuffer(15, blocks.drawData.buffer, slotSets.pc, sizeof(DrawData));
	addBuffer(9, blocks.cascadedShadows.buffer, slotSets.shadows, sizeof(CascadedShadowBlock));
	addBuffer(10, blocks.pointLights.buffer, slotSets.pointLights, sizeof(pointLight));
	addBuffer(5, renderDataHandle.cameraMatricesUBO, slotSets.cam);

	if (mIbl.irradiance && mIbl.prefilter && mIbl.brdfLut) {
		addImage(12, mIbl.irradiance, mIbl.irradianceSampler, slotSets.iblIrr);
		addImage(13, mIbl.prefilter, mIbl.prefilterSampler, slotSets.iblPref);
		addImage(14, mIbl.brdfLut, mIbl.lutSampler, slotSets.iblLut);
	}

	if (!writes.empty()) {
		rhi->updateDescriptorSets(writes);
	}
	Render::DescriptorSetHandle sets[2] = { texSet, uboSet };
	// Set 0 has no dynamic bindings; set 1's in binding order.
	const uint32_t offsets[] = { blocks.pointMatrices.offset, blocks.cascadedShadows.offset, blocks.pointLights.offset,
		blocks.lightMatrices.offset, blocks.drawData.offset };
	rhi->cmdBindDescriptorSets(cmd, pipeline, 0, sets, 2, offsets, static_cast<uint32_t>(std::size(offsets)));
	// The application's frame data, after the pass's own sets (UserFrameSet::bind).
	UserFrameSet::instance().bind(cmd, pipeline);
}
