#include "LightingPass.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include "assetsModule/TextureHandler.h"
#include "core/Engine.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/IblBaker.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"
#include "imgui.h"
#include "componentsModule/LightSourceComponent.h"
#include "componentsModule/ModelComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "logsModule/logger.h"
#include "debugModule/Benchmark.h"
#include "ecss/Registry.h"
#include "systemsModule/SystemsPriority.h"

using namespace SFE::Render::RenderPasses;


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
	texLayoutDesc.bindings.emplace_back(12, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(13, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(14, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(21, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(22, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);

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
}

void LightingPass::bakeSkyLight(SystemsModule::RenderData& renderDataHandle) {
	mIbl = SFE::Render::bakeIbl(Engine::instance()->rhi(), renderDataHandle.graphics.sunIntensity);
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

std::array<float, 5> LightingPass::skyLightKey(const SystemsModule::RenderData& renderDataHandle) {
	const auto sky = SFE::Render::Atmosphere::sceneSky();
	if (!sky) {
		return {};
	}
	return { sky->sunElevation, sky->sunAzimuth, sky->cloudCoverage, renderDataHandle.graphics.sunIntensity, 1.f };
}

void LightingPass::updateSkyLight(SystemsModule::RenderData& renderDataHandle) {
	// Baked again when the sun or the sky changed: worked out on a worker, as it takes the best part of a second, and
	// written into the spare maps once it is done and they swapped in, the next started only then. A sun going along its
	// path never holds a frame up, and the sky's light follows it a bake behind. The maps written are never those a
	// frame in flight reads: made anew each time (and the old freed), or written over while still read, they held the
	// driver up for seconds at a time.
	if (mIblBaking.valid()) {
		if (mIblBaking.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
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

void LightingPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	assert(mTexSetInitialized);
	updateSkyLight(renderDataHandle);
	FUNCTION_BENCHMARK;

	auto* rhi = Engine::instance()->rhi();
	auto& frame = renderDataHandle.frame;
	const auto* point = frame.data<PointShadowFrame>(FrameId::PointShadow);
	const auto* csm = frame.data<CsmFrame>(FrameId::Csm);
	int offsetSum = 0;
	drawData.pointLightsSize = 0;
	if (point) {
		const size_t lightCount = (std::min)(point->shadowEntities.size(), static_cast<size_t>(12));
		for (size_t i = 0; i < lightCount; i++) {
			auto tc = ECSHandler::registry().pinComponent<const TransformComponent>(point->shadowEntities[i]);
			auto lightComp = ECSHandler::registry().pinComponent<const LightSourceComponent>(point->shadowEntities[i]);
			if (!tc || !lightComp) {
				continue;
			}

			auto& light = pointLight[i];

			const float t = renderDataHandle.current.wind.w;
			light.Position = tc->getPos(true);
			if (lightComp->mFlicker > 0.f) {
				light.Position = light.Position + lightComp->flickerShift(t);
			}
			light.texelSize = Math::Vec2{ lightComp->getTexelSize().x, lightComp->getTexelSize().y };
			light.bias = lightComp->getBias();
			light.samples = (std::clamp)(lightComp->getSamples(), 1, 64);
			light.radius = lightComp->mRadius;
			light.offset = offsetSum;

			light.Color = lightComp->getLightColor() * lightComp->getIntensity();
			if (lightComp->mFlicker > 0.f) {
				light.Color = light.Color * lightComp->flickerGain(t);
			}
			light.Linear = lightComp->mLinear;
			light.Quadratic = lightComp->mQuadratic;
			if (lightComp->getType() == ComponentsModule::eLightType::POINT) {
				light.Type = 0;
				light.Layers = 6;
			}
			else {
				light.Type = 1;
				light.Layers = 1;
			}

			offsetSum += lightComp->getTypeOffset(lightComp->getType());
		}

		drawData.pointLightsSize = static_cast<int>(lightCount);
	}

	if (csm && !csm->shadowCascadeLevels.empty()) {
		const auto& lightColor = csm->lightColor;
		drawData.ambientX = lightColor.x;
		drawData.ambientY = lightColor.y;
		drawData.ambientZ = lightColor.z;
		drawData.cascadeCount = static_cast<int>(csm->shadowCascades.size());
		drawData.shadowIntensity = csm->shadowsIntensity;

		cascadedShadowData.direction_intensity.xyz = csm->lightDirection;
		cascadedShadowData.direction_intensity.w = csm->shadowsIntensity;
		cascadedShadowData.color_and_misc.xyz = csm->lightColor;
		cascadedShadowData.color_and_misc.w = static_cast<float>(drawData.cascadeCount);
	
		for (size_t i = 0; i < csm->shadowCascades.size(); ++i) {
			auto& cascade = csm->shadowCascades[i];
			auto& cascadeRenderData = cascadedShadowData.data[i];

			const float invRes = 1.f / (std::max)(1.f, csm->resolution.x);
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
		}

		const auto& levels = csm->shadowCascadeLevels;
		auto splitAt = [&levels](size_t i) {
			return (i < levels.size()) ? levels[i] : 1.0e6f;
		};
		cascadedShadowData.cascadeSplits = {
			splitAt(1), splitAt(2), splitAt(3), splitAt(4)
		};
		cascadedShadowData.cascadeSplits2 = {
			splitAt(5), splitAt(6), 1.0e6f,
			ECSHandler::getSystem<SystemsModule::RenderSystem>()->isShadowsDebugData() ? 1.f : 0.f
		};

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
	drawData.debugTerm = static_cast<float>(Engine::instance()->lightingDebugTerm());
	drawData.sheenStrength = renderDataHandle.graphics.sheen ? renderDataHandle.graphics.sheenStrength : 0.f;
	drawData.rimStrength = renderDataHandle.graphics.rimStrength;
	drawData.taaFrame = renderDataHandle.graphics.taa ? static_cast<float>(renderDataHandle.taaFrame % 4096) : -1.f;
	{
		Atmosphere::SkyParams sky;
		drawData.cloudCoverage = Atmosphere::skyParams(renderDataHandle, sky) ? sky.toSun[3] : 0.f;
		drawData.stylise = renderDataHandle.graphics.stylise;
	}
	if (const auto* tint = renderDataHandle.frame.data<AreaTintFrame>(FrameId::AreaTint)) {
		drawData.tintArea = tint->area;
		drawData.tintFoliage = tint->foliage;
		drawData.tintAir = tint->air;
		drawData.tintGround = tint->ground;
	}
	else {
		drawData.tintArea = {};
	}

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

	{
		// The cloud layer, for the shadows the clouds lay over the ground.
		const auto clouds = Atmosphere::cloudTexture();
		addImage(21, clouds.texture, clouds.sampler, slotSets.clouds);
	}
	{
		const auto tint = Utils::areaTintMap(frame);
		addImage(22, tint.first, tint.second, slotSets.areaTint);
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
}
