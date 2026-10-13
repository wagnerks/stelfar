#include "CascadedShadowPass.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "imgui.h"
#include "componentsModule/FrustumComponent.h"
#include "componentsModule/ModelComponent.h"
#include "core/Engine.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"
#include "assetsModule/modelModule/BoundingVolume.h"
#include "systemsModule/systems/CameraSystem.h"

#include <thread>

#include "assetsModule/TextureHandler.h"
#include "assetsModule/modelModule/MeshVaoRegistry.h"
#include "assetsModule/modelModule/ModelLoader.h"
#include "assetsModule/shaderModule/ShaderBase.h"
#include "componentsModule/ArmatureComponent.h"
#include "componentsModule/CameraComponent.h"
#include "componentsModule/CascadeShadowComponent.h"
#include "componentsModule/CascadeShadowComponent.h"
#include "componentsModule/DebugDataComponent.h"
#include "componentsModule/IsDrawableComponent.h"
#include "componentsModule/LightSourceComponent.h"
#include "componentsModule/MaterialComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/FileSystem.h"
#include "multithreading/ThreadPool.h"
#include "debugModule/Benchmark.h"
#include "ecss/Registry.h"
#include "logsModule/logger.h"
#include "renderModule/Atmosphere.h"
#include "mathModule/Forward.h"
#include "propertiesModule/PropertiesSystem.h"
#include "systemsModule/systems/OcTreeSystem.h"
#include "systemsModule/SystemManager.h"
#include "systemsModule/SystemsPriority.h"
#include "logsModule/logger.h"

using namespace SFE::Render::RenderPasses;

namespace {
	// How far the camera goes from where a cascade was drawn, as a share of its half width, before it is drawn anew
	// whatever the refresh rate: a teleport, a fall. Walking never goes that far between two of its frames.
	constexpr float kGoneShare = 0.25f;
	// Texels a metre a cascade has at least for a caster's detail to read in its shadow -- a leaf card's cut-out outline,
	// the finer of its levels of detail: a centimetre a texel, the cascades about the eye and hands.
	constexpr float kFineTexelsPerMetre = 100.f;
}

CascadedShadowPass::CascadedShadowPass() {
}

CascadedShadowPass::~CascadedShadowPass() {

}

void CascadedShadowPass::init(SystemsModule::RenderData& renderDataHandle) {
	if (mInited) {
		return;
	}
	mInited = true;

	mShadowSource = ECSHandler::registry().takeEntity();
	
	auto transform = ECSHandler::registry().addComponent<TransformComponent>(mShadowSource, mShadowSource);
	auto worldLight = ECSHandler::registry().addComponent<LightSourceComponent>(mShadowSource, mShadowSource, SFE::ComponentsModule::eLightType::WORLD);
	worldLight->setLightColor({ 1.0f, 0.96f, 0.88f });
	worldLight->setIntensity(3.2f);

	auto cmp = ECSHandler::registry().addComponent<CascadeShadowComponent>(mShadowSource, mShadowSource);
	

	auto debugData = ECSHandler::registry().addComponent<DebugDataComponent>(mShadowSource);
	debugData->stringId = "cascadeShadows";

	auto shadow = FileSystem::readJson("cascadedShadows.json");

	PropertiesModule::PropertiesSystem::deserializeProperty<CascadeShadowComponent>(mShadowSource, shadow["Properties"]);
	// The drawn look keeps the authored map: a quarter or a half of it a side, to save filling texels, blurred every
	// shadow near the eye and stood it off its caster by the offsets a texel that size needs. Read once: the maps are
	// made here and never resized.
	mStylised = renderDataHandle.graphics.stylise > 0.5f;
	if (mStylised) {
		// And half the distance. The last cascade is fitted to the slice from 120 m out, so every tree in the forest
		// stands in it; drawing it to 200 m instead of 400 is most of what this pass costs in a wood, and past that
		// the air has taken the contrast out of the trees anyway.
		if (!cmp->shadowCascadeLevels.empty()) {
			cmp->shadowCascadeLevels.back() *= 0.5f;
		}
	}
	//cmp->resolution = Math::Vec2{ 256.f, 256.f };
	//cmp->shadowCascadeLevels = { cameraProjection.getNear(), 50.f, 150.f, /*500.f, 5000.f*/ };
	//cmp->updateCascades(cameraProjection);
	transform->setRotate({ -32.f, 28.f, 8.f });
	// An outdoor scene places the sun itself, and the air it shines through colours it.
	if (const auto sky = Render::Atmosphere::sceneSky()) {
		transform->setRotate(sky->sunRotation());
		worldLight->setLightColor(Render::Atmosphere::sunTransmittance(sky->toSun()));
	}

	initRender();
	renderDataHandle.frame.setData(FrameId::Csm, &mData);
	renderDataHandle.frame.set(FrameId::ShadowCsm, { .texture = lightDepthMa, .sampler = lightSampler });
	renderDataHandle.frame.set(FrameId::ShadowCsmDepth, { .texture = lightDepthMa, .sampler = lightDepthSampler });
}

void CascadedShadowPass::initRender() {
	auto cmp = ECSHandler::registry().pinComponent<CascadeShadowComponent>(mShadowSource);
	auto& cameraProjection = ECSHandler::registry().pinComponent<const CameraComponent>(ECSHandler::getSystem<SFE::SystemsModule::CameraSystem>()->getCurrentCamera())->getProjection();

	cmp->updateCascades(cameraProjection);

	{
		// ~6x8 UV sphere for glass shadow casters (render sphere is 32x48).
		constexpr int stacks = 6;
		constexpr int slices = 8;
		constexpr float pi = 3.14159265f;
		auto& verts = mGlassShadowMesh.mesh.vertices;
		auto& inds = mGlassShadowMesh.mesh.indices;
		verts.clear();
		inds.clear();
		verts.push_back({});
		verts.back().position = { 0.f, 1.f, 0.f };
		verts.back().normal = verts.back().position;
		for (int i = 1; i < stacks; ++i) {
			const float v = static_cast<float>(i) / static_cast<float>(stacks);
			const float phi = v * pi;
			const float sp = std::sin(phi);
			const float cp = std::cos(phi);
			for (int j = 0; j <= slices; ++j) {
				const float u = static_cast<float>(j) / static_cast<float>(slices);
				const float theta = u * 2.f * pi;
				SFE::Vertex3D vert{};
				vert.position = { sp * std::cos(theta), cp, sp * std::sin(theta) };
				vert.normal = vert.position;
				verts.push_back(vert);
			}
		}
		verts.push_back({});
		verts.back().position = { 0.f, -1.f, 0.f };
		verts.back().normal = verts.back().position;
		const unsigned south = static_cast<unsigned>(verts.size() - 1);
		for (int j = 0; j < slices; ++j) {
			inds.push_back(0);
			inds.push_back(1 + j);
			inds.push_back(1 + j + 1);
		}
		for (int i = 0; i < stacks - 2; ++i) {
			const int row = 1 + i * (slices + 1);
			const int next = row + (slices + 1);
			for (int j = 0; j < slices; ++j) {
				inds.push_back(row + j);
				inds.push_back(next + j);
				inds.push_back(row + j + 1);
				inds.push_back(row + j + 1);
				inds.push_back(next + j);
				inds.push_back(next + j + 1);
			}
		}
		const int lastRow = 1 + (stacks - 2) * (slices + 1);
		for (int j = 0; j < slices; ++j) {
			inds.push_back(lastRow + j);
			inds.push_back(south);
			inds.push_back(lastRow + j + 1);
		}
		mGlassShadowMeshData = SFE::MeshVaoRegistry::instance()->get(&mGlassShadowMesh.mesh);
	}

	TextureDesc depthTex;
	depthTex.width = static_cast<int>(cmp->resolution.x);
	depthTex.height = static_cast<int>(cmp->resolution.y);
	const uint32_t cascadeLayers = static_cast<uint32_t>((std::max)(size_t{1}, cmp->cascades.size()));
	depthTex.depth = cascadeLayers;
	depthTex.arrayLayers = cascadeLayers;
	depthTex.dimension = TextureDimension::Array;
	depthTex.format = TextureFormat::DEPTH_COMPONENT32;
	depthTex.usage = TextureUsage::DepthStencilAttachment | TextureUsage::Sampled;

	SamplerDesc shadowSamp;
	shadowSamp.minFilter = FilterMode::Linear;
	shadowSamp.magFilter = FilterMode::Linear;
	shadowSamp.addressU = AddressMode::ClampToEdge;
	shadowSamp.addressV = AddressMode::ClampToEdge;
	shadowSamp.addressW = AddressMode::ClampToEdge;
	shadowSamp.compareOp = CompareOp::LessEqual;

	SamplerDesc depthSamp;
	depthSamp.minFilter = FilterMode::Nearest;
	depthSamp.magFilter = FilterMode::Nearest;
	depthSamp.addressU = AddressMode::ClampToEdge;
	depthSamp.addressV = AddressMode::ClampToEdge;
	depthSamp.addressW = AddressMode::ClampToEdge;
	depthSamp.compareOp = CompareOp::None;

	lightDepthMa = Engine::instance()->rhi()->createTexture(depthTex);
	lightSampler = Engine::instance()->rhi()->createSampler(shadowSamp);
	lightDepthSampler = Engine::instance()->rhi()->createSampler(depthSamp);

	auto* rhi = Engine::instance()->rhi();

	RenderPassDesc rpDesc;
	rpDesc.hasDepthStencil = true;
	rpDesc.depthAttachment.loadOp = AttachmentLoadOp::Clear;
	rpDesc.depthAttachment.storeOp = AttachmentStoreOp::Store;
	rpDesc.depthAttachment.format = TextureFormat::DEPTH_COMPONENT32;
	renderPass = rhi->createRenderPass(rpDesc);
	rpDesc.depthAttachment.loadOp = AttachmentLoadOp::Load;
	loadRenderPass = rhi->createRenderPass(rpDesc);

	// One framebuffer per layer, not one layered framebuffer: with the geometry shader gone the
	// layer comes from the attachment, and every cascade is its own render pass anyway now that
	// each draws its own culled list.
	lightFbos.clear();
	for (uint32_t layer = 0; layer < cascadeLayers; ++layer) {
		FramebufferDesc fb{};
		fb.width = static_cast<uint32_t>(cmp->resolution.x);
		fb.height = static_cast<uint32_t>(cmp->resolution.y);
		fb.renderPass = renderPass;
		fb.layers = 1;
		FramebufferAttachmentDesc depthAtt;
		depthAtt.texture = lightDepthMa;
		depthAtt.mipLevel = 0;
		depthAtt.layer = layer;
		fb.depthStencilAttachment = depthAtt;
		lightFbos.push_back(rhi->createFramebuffer(fb));
	}

	// Set 0: buffers, one per cascade. Set 1: the caster's mask texture, shared.
	Render::DescriptorSetLayoutDesc bufferLayoutDesc;
	bufferLayoutDesc.bindings.emplace_back(11, Render::DescriptorType::UniformBufferDynamic, Render::ShaderStage::Vertex);
	bufferLayoutDesc.bindings.emplace_back(1, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex);
	bufferLayoutDesc.bindings.emplace_back(2, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex);
	bufferLayoutDesc.bindings.emplace_back(3, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex);
	bufferLayoutDesc.bindings.emplace_back(7, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex | Render::ShaderStage::Fragment);
	// The wind, for casters that sway in it.
	bufferLayoutDesc.bindings.emplace_back(5, Render::DescriptorType::UniformBuffer, Render::ShaderStage::Vertex);
	mBufferLayout = rhi->createDescriptorSetLayout(bufferLayoutDesc);

	Render::DescriptorSetLayoutDesc textureLayoutDesc;
	textureLayoutDesc.bindings.emplace_back(4, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	mTextureLayout = rhi->createDescriptorSetLayout(textureLayoutDesc);

	GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.depthStencilState.depthTest = true;
	pipelineDesc.depthStencilState.depthWrite = true;
	pipelineDesc.depthStencilState.depthCompareOp = CompareOp::Less;
	pipelineDesc.cullMode = CullMode::Front;
	// Pushed off the sun by their slope (GraphicsSettings::shadowSlopeBias), set as each is bound.
	pipelineDesc.depthBias = true;
	// Plain CCW, like every other pass. toVkFrontFace already inverts the winding to absorb the
	// un-flipped Vulkan viewport, so branching on the backend here compensated a second time and
	// left the two backends storing opposite faces: OpenGL the far surface, Vulkan the near one.
	pipelineDesc.frontFace = FrontFace::CounterClockWise;
	pipelineDesc.renderPass = renderPass;
	pipelineDesc.descriptorSetLayouts = { mBufferLayout, mTextureLayout };
	pipelineDesc.pushConstantSize = 16u;
	// The vertex shader applies the cascade matrix now, so it needs the push block too.
	pipelineDesc.pushConstantStages = ShaderStage::Vertex | ShaderStage::Fragment;
	// Not cascadeShadowMap.vs: that one stays world-space because PointLightPass shares it.
	pipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/cascadeShadowMap_csm.vs");
	
	Render::VertexLayoutDesc vaoDesc;
	vaoDesc.bindings.push_back({ 0,sizeof(Vertex3D), Render::VertexInputRate::PerVertex });
	vaoDesc.attributes.push_back({ 0,0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, position)), true });
	vaoDesc.attributes.push_back({ 1,0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, normal)),  true });
	vaoDesc.attributes.push_back({ 2,0, Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex3D, texCoords)), true });
	vaoDesc.attributes.push_back({ 3,0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, tangent)), true });
	vaoDesc.attributes.push_back({ 4,0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, biTangent)), true });
	vaoDesc.attributes.push_back({ 5,0, Render::VertexFormat::Int4, static_cast<std::uint32_t>(offsetof(Vertex3D, boneIDs)), false });
	vaoDesc.attributes.push_back({ 6,0, Render::VertexFormat::Float4, static_cast<std::uint32_t>(offsetof(Vertex3D, weights)), false });
	vaoDesc.attributes.push_back({ 7,0, Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex3D, texCoords1)), true });

	pipelineDesc.layout = vaoDesc;

	pipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/cascadeShadowMap_opaque.fs");
	pipelineOpaque = rhi->createGraphicsPipeline(pipelineDesc);

	// Cut-out casters are cards more often than not -- leaves, grass -- one polygon thick: culling either side leaves
	// half of them out of the shadow, depending on which way each faces the sun.
	pipelineDesc.cullMode = CullMode::None;
	// Its own shader for the holes a tree's or a plant's leaves let the sun through (leafLight), the same as the cut-out's.
	pipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/cascadeShadowMap_solid.fs");
	pipelineCutoutSolid = rhi->createGraphicsPipeline(pipelineDesc);

	pipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/cascadeShadowMap.fs");
	pipelineCutout = rhi->createGraphicsPipeline(pipelineDesc);

}

void CascadedShadowPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	if (!mInited) {
		return;
	}
	
	debug(renderDataHandle);

	FUNCTION_BENCHMARK;

	auto rhi = Engine::instance()->rhi();

	updateRenderData(renderDataHandle);

	// The sun moves with the time of day, and once it is down the moon takes its place: the same light from the other
	// side of the sky, faint and cold, so the night has a direction to it and the trees still lay shadows.
	if (const auto sky = Render::Atmosphere::sceneSky()) {
		const float night = std::clamp((0.04f - sky->toSun().y) / 0.1f, 0.f, 1.f);
		const auto rotation = night > 0.5f ? sky->moonRotation() : sky->sunRotation();
		if (rotation != mSunRotation) {
			mSunRotation = rotation;
			if (auto transform = ECSHandler::registry().pinComponent<TransformComponent>(mShadowSource)) {
				transform->setRotate(rotation);
			}
		}
		if (auto light = ECSHandler::registry().pinComponent<LightSourceComponent>(mShadowSource)) {
			const auto sun = Render::Atmosphere::sunTransmittance(sky->toSun());
			// Moonlight is sunlight bounced off a grey rock: a thousandth of it, and blue to the eye at that level.
			const Math::Vec3 moon{ 0.55f * 0.02f, 0.65f * 0.02f, 1.0f * 0.02f };
			light->setLightColor(night > 0.5f ? moon * night : sun);
		}
	}

	auto shadowsComp = ECSHandler::registry().pinComponent<CascadeShadowComponent>(mShadowSource);
	if (!shadowsComp) {
		return;
	}

	shadowsComp->calculateLightSpaceMatrices(renderDataHandle.cameraProjection, renderDataHandle.current.view, renderDataHandle.shadowFocus);
	const auto lightMatrices = shadowsComp->getLightSpaceMatrices();

	updateRenderData(renderDataHandle);

	if (!lightMatrices.empty()) {
		mData.lightSpaceMatrices = lightMatrices;
	}
	// The near ones (reaching no further than kNearReach) are drawn every frame: the character's own shadow and the leaves
	// swaying over the ground about it, kept a frame or two, trailed the running feet and stepped while the leaves moved
	// smoothly. However many there are: one fitted round the hands alone, for their own shadows, is near as well.
	// A far one is drawn no more often than graphics.shadowRefreshRate times a second (0: every frame), and what moves in
	// it moves in its shadow that much later; kShare of that, from the first far one on, a half and a third: each costs
	// about as much to draw as a near one, and what moves past 12 m moves little on the screen. While the camera and the
	// sun stay put a far cascade's volume does not move (it is fitted to a circle round its slice and snapped to texels,
	// so turning on the spot keeps it too), and then its map is kept for up to kStaticRefreshFrames: past 60 m nothing
	// sways enough to see. One that skips keeps the matrix it was last drawn with, so lighting reads its map where it was
	// drawn. Those due are drawn the most overdue first, and no more of them a frame than keep them all to their rates at
	// the rate the frames come, the fractions carried on rather than rounded up: the cost spread over the frames, no more
	// than kMostBanked saved up for one. One the camera has gone kGoneShare of its half width from is drawn at once,
	// whatever the rate.
	constexpr float kShare[] = { 0.5f, 1.f / 3.f };
	const auto& levels = shadowsComp->shadowCascadeLevels;
	size_t firstFar = 0;
	while (firstFar + 1 < levels.size() && levels[firstFar + 1] <= kNearReach) {
		++firstFar;
	}
	constexpr float kMostBanked = 2.f;
	mCascades.resize(lightFbos.size());
	std::vector<bool> skip(mCascades.size(), false);
	const size_t count = (std::min)({ mCascades.size(), mData.lightSpaceMatrices.size(), mData.shadowCascades.size() });
	const float rate = renderDataHandle.graphics.shadowRefreshRate;
	const float step = Engine::instance()->getDeltaTime();
	constexpr float kMust = std::numeric_limits<float>::max();
	std::vector<std::pair<float, size_t>> due; // how overdue each is, and which
	float shares = 0.f;
	for (size_t layer = 0; layer < count; ++layer) {
		auto& cascade = mCascades[layer];
		const bool unmoved = cascade.lightSpace == mData.lightSpaceMatrices[layer] && cascade.framesSinceDrawn < kStaticRefreshFrames;
		const bool everyFrame = layer < firstFar;
		const float share = everyFrame ? 1.f : kShare[(std::min)(layer - firstFar, std::size(kShare) - 1)];
		shares += everyFrame ? 0.f : share;
		++cascade.framesSinceDrawn;
		cascade.age += step;
		skip[layer] = true;
		const auto& kept = cascade.lightSpace;
		const float halfWidth = 1.f / (std::max)(Math::length(Math::Vec3{ kept[0][0], kept[1][0], kept[2][0] }), 1e-6f);
		if (everyFrame || !cascade.drawn || Math::length(renderDataHandle.mCameraPos - cascade.drawnFrom) > halfWidth * kGoneShare) {
			due.emplace_back(kMust, layer);
		}
		else if (!(layer >= firstFar && unmoved) && (rate <= 0.f || cascade.age * rate * share >= 1.f)) {
			due.emplace_back(cascade.age * rate * share, layer);
		}
	}
	size_t perFrame = count;
	if (rate > 0.f) {
		const float earned = shares * rate * step;
		mDrawBudget = (std::min)(mDrawBudget + earned, (std::max)(earned, kMostBanked));
		perFrame = static_cast<size_t>((std::max)(mDrawBudget, 0.f));
	}
	std::stable_sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	float drawnNow = 0.f;
	for (size_t i = 0; i < due.size(); ++i) {
		skip[due[i].second] = i >= perFrame && due[i].first != kMust;
		drawnNow += skip[due[i].second] || due[i].second < firstFar ? 0.f : 1.f;
	}
	// One drawn at once over the budget is paid back over the next frames, no more than one cascade's worth; the near
	// ones are outside the budget.
	mDrawBudget = (std::max)(mDrawBudget - drawnNow, -1.f);
	for (size_t layer = 0; layer < count; ++layer) {
		auto& cascade = mCascades[layer];
		if (skip[layer]) {
			mData.lightSpaceMatrices[layer] = cascade.lightSpace;
			mData.shadowCascades[layer] = cascade.settings;
		}
		else {
			cascade.lightSpace = mData.lightSpaceMatrices[layer];
			cascade.settings = mData.shadowCascades[layer];
			cascade.drawn = true;
			cascade.framesSinceDrawn = 0;
			cascade.age = 0.f;
			cascade.drawnFrom = renderDataHandle.mCameraPos;
		}
	}
	const auto matrices = Utils::allocMatrices(mData.lightSpaceMatrices, CsmFrame::kMaxCascades);

	auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
	auto& scene = renderSystem->scene();
	if (!mProxyRegistered) {
		scene.setGlassShadowProxy(mGlassShadowMeshData);
		mProxyRegistered = true;
	}
	const auto materials = renderSystem->materials().upload();
	const auto* meshes = SFE::MeshVaoRegistry::instance();
	// Whatever else draws casters of its own -- grass, impostors, a game's own -- says so by implementing
	// ShadowCasterPass; this pass does not know what they are.
	std::vector<ShadowCasterPass*> casters;
	for (const auto& pass : renderSystem->renderPasses()) {
		if (auto* caster = dynamic_cast<ShadowCasterPass*>(pass.get())) {
			caster->prepareShadows(renderDataHandle, cmd);
			casters.push_back(caster);
		}
	}

	// A caster only needs the fragment shader when something can make it transparent: an alpha cutout, a
	// mask or a dithered opacity. Everything else writes depth on a pipeline with no fragment shader work,
	// which keeps early-Z for the bulk of the scene.
	const auto& groups = scene.groups(GpuScene::List::Shadow);

	// Every layer, not just the ones with casters. A cascade that renders no pass keeps last frame's depth
	// and, on Vulkan, never reaches SHADER_READ_ONLY, so sampling it is undefined.
	for (size_t layer = 0; layer < lightFbos.size(); ++layer) {
		if (!lightFbos[layer]) {
			continue;
		}
		auto& cascade = mCascades[layer];

		// The cascade's own volume, widened a little so casters just outside it still reach in: casters are culled by
		// their bounds, so a margin is only for those that move past their bounds between the cull and the draw.
		auto frustum = layer < shadowsComp->cascades.size() ? shadowsComp->cascades[layer].frustum : FrustumModule::Frustum{};
		for (auto* plane : { &frustum.leftFace, &frustum.rightFace, &frustum.topFace, &frustum.bottomFace, &frustum.nearFace, &frustum.farFace }) {
			plane->distance += 2.f;
		}
		// Towards the sun without a limit that matters: the casters shaders press onto the near plane (pancaking) still
		// have to be drawn, and at a low sun they stand far up-sun of what they shade.
		frustum.nearFace.distance += kCasterReachTowardsSun;
		// Texels a metre covers: the light's projection is orthographic, so its x row's length in NDC per metre.
		float texelsPerMetre = 0.f;
		if (layer < mData.lightSpaceMatrices.size()) {
			const auto& lightSpace = mData.lightSpaceMatrices[layer];
			texelsPerMetre = Math::length(Math::Vec3{ lightSpace[0][0], lightSpace[1][0], lightSpace[2][0] }) * shadowsComp->resolution.x * 0.5f;
		}
		// No occlusion culling: building a depth pyramid of each cascade every frame cost more than the casters it
		// saved. The cascades redrawn every frame also leave out casters whose shadow falls outside the camera's view;
		// a kept map may be read after the camera has turned, so those draw everything in their volume.
		// In a coarse one a caster's own shading is too far off to show the coarser silhouette.
		const bool fine = texelsPerMetre >= kFineTexelsPerMetre;
		cascade.view.lodBias = fine ? 0u : 1u;
		const bool visible = !skip[layer] && layer < shadowsComp->cascades.size()
			&& scene.cullShadowCasters(cmd, cascade.view, frustum, layer < firstFar ? &renderDataHandle.mCamFrustum : nullptr,
				mData.lightDirection, texelsPerMetre);

		if (visible) {
			if (!cascade.set) {
				cascade.set = rhi->allocateDescriptorSet(mBufferLayout);
			}
			if (matrices.buffer.id != cascade.matricesId) {
				cascade.matricesId = matrices.buffer.id;
				rhi->updateDescriptorSets({ { .set = cascade.set, .binding = 11, .buffers{ { matrices.buffer, 0, sizeof(Math::Mat4) * CsmFrame::kMaxCascades } } } });
			}
			const uint32_t ids[5] = { renderDataHandle.transformsSSBO.id, renderDataHandle.bonesSSBO.id, cascade.view.visible.id, materials.id,
				renderDataHandle.cameraMatricesUBO.id };
			if (!std::equal(std::begin(ids), std::end(ids), std::begin(cascade.boundIds))) {
				std::copy(std::begin(ids), std::end(ids), std::begin(cascade.boundIds));
				rhi->updateDescriptorSets({
					{ .set = cascade.set, .binding = 1, .buffers{ { renderDataHandle.transformsSSBO } } },
					{ .set = cascade.set, .binding = 2, .buffers{ { renderDataHandle.bonesSSBO } } },
					{ .set = cascade.set, .binding = 3, .buffers{ { cascade.view.visible } } },
					{ .set = cascade.set, .binding = 7, .buffers{ { materials } } },
					{ .set = cascade.set, .binding = 5, .buffers{ { renderDataHandle.cameraMatricesUBO } } },
				});
			}
		}

		RenderPassBeginInfo begin;
		// A skipped cascade still gets its (empty) pass, keeping last frame's depth.
		begin.renderPass = skip[layer] ? loadRenderPass : renderPass;
		begin.framebuffer = lightFbos[layer];
		begin.x = 0;
		begin.y = 0;
		begin.width = static_cast<int>(shadowsComp->resolution.x);
		begin.height = static_cast<int>(shadowsComp->resolution.y);
		rhi->cmdBeginRenderPass(cmd, begin);

		const auto drawCasters = [&]() {
			struct ShadowPush {
				int cascadeLayer = 0;
				float leafLight = 0.f;
				float pad[2]{};
			} push;
			push.cascadeLayer = static_cast<int>(layer);
			push.leafLight = renderDataHandle.graphics.shadowLeafLight;

			// In a coarse cascade a leaf card's cut-out outline hardly reads in the shadow, only its coverage: drawn solid
			// there, without the texture fetch the cut-out takes (cut out to 50 m the floor of a forest looked much the same,
			// and the pass took 9 ms instead of 4).
			const bool solidCutouts = !fine;
			PipelineHandle bound{};
			// Solid casters first, so their depth is there to reject the cut-out ones behind them.
			for (const bool cutoutPass : { false, true })
			for (const auto& group : groups) {
				if (group.batchCount == 0 || (group.cutout && !solidCutouts) != cutoutPass) {
					continue;
				}
				const auto pipeline = !group.cutout ? pipelineOpaque : solidCutouts ? pipelineCutoutSolid : pipelineCutout;
				if (pipeline.id != bound.id) {
					bound = pipeline;
					rhi->cmdBindPipeline(cmd, pipeline);
					// After the pipeline: OpenGL keeps the vertex and index buffers on the pipeline's VAO.
					rhi->cmdBindVertexBuffer(cmd, 0, meshes->vertexBuffer(), 0);
					rhi->cmdBindIndexBuffer(cmd, meshes->indexBuffer(), IndexType::Uint32, 0);
					rhi->cmdPushConstants(cmd, pipeline, ShaderStage::Vertex | ShaderStage::Fragment, &push, sizeof(push));
					rhi->cmdSetDepthBias(cmd, 0.f, renderDataHandle.graphics.shadowSlopeBias,
						renderDataHandle.graphics.shadowSlopeBiasMost / (std::max)(shadowsComp->cascades[layer].depthRange, 1e-4f));
				}
				const DescriptorSetHandle sets[] = { cascade.set, mMaskSets.get(mTextureLayout, 4, group.maskTexture) };
				rhi->cmdBindDescriptorSets(cmd, pipeline, 0, sets, 2, &matrices.offset, 1);
				scene.draw(cmd, cascade.view, group);
			}
		};

		if (visible) {
			drawCasters();
		}
		if (!skip[layer] && layer < mData.lightSpaceMatrices.size()) {
			const ShadowCascade shadow{ renderDataHandle, cmd, mData.lightSpaceMatrices[layer], frustum,
				mData.lightDirection, texelsPerMetre, static_cast<uint32_t>(layer) };
			for (auto* caster : casters) {
				caster->drawShadowCasters(shadow);
			}
		}

		rhi->cmdEndRenderPass(cmd);
	}

	{
		auto* info = SFE::SystemsModule::RenderDebugInfo::instance();
		info->shadowMeshes = groups.size();
		info->shadowFbosTotal = static_cast<uint32_t>(lightFbos.size());
		info->shadowFbos = static_cast<uint32_t>(lightFbos.size());
		info->shadowPipeline = pipelineOpaque.id;
		info->shadowIntensity = mData.shadowsIntensity;
	}
}

void CascadedShadowPass::updateRenderData(SystemsModule::RenderData& renderDataHandle) {
	auto shadowsComp = ECSHandler::registry().pinComponent<const CascadeShadowComponent>(mShadowSource);

	mData.shadowMapTexture = lightDepthMa;
	mData.shadowMapSampler = lightSampler;
	mData.lightDirection = ECSHandler::registry().pinComponent<const TransformComponent>(mShadowSource)->getForward();
	mData.lightColor = ECSHandler::registry().pinComponent<const LightSourceComponent>(mShadowSource)->getLightColor()
		* renderDataHandle.graphics.sunIntensity;
	mData.resolution = shadowsComp->resolution;
	mData.cameraFarPlane = shadowsComp->cascades.back().viewProjection.getFar();
	mData.shadowCascadeLevels = shadowsComp->shadowCascadeLevels;
	mData.shadows = mShadowSource;
	mData.shadowCascades = shadowsComp->cascades;
	mData.shadowsIntensity = shadowsComp->shadowIntensity;

	renderDataHandle.frame.setData(FrameId::Csm, &mData);
	renderDataHandle.frame.set(FrameId::ShadowCsm, { .texture = lightDepthMa, .sampler = lightSampler });
	renderDataHandle.frame.set(FrameId::ShadowCsmDepth, { .texture = lightDepthMa, .sampler = lightDepthSampler });
}

void CascadedShadowPass::debug(SystemsModule::RenderData& renderDataHandle) {
	return;
	if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("Debug")) {
			if (ImGui::BeginMenu("Shadows debug")) {

				if (mShadowSource) {
					if (ImGui::Button("cache")) {
						ECSHandler::registry().pinComponent<CascadeShadowComponent>(mShadowSource)->cacheMatrices();
					}

					if (ImGui::Button("clear")) {
						ECSHandler::registry().pinComponent<CascadeShadowComponent>(mShadowSource)->clearCacheMatrices();
					}
					static float sunProgress = 0.4f;
					if (ImGui::DragFloat("sun pos", &sunProgress, 0.001f, 0.f)) {
						/*auto x = cosf(Math::radians(-sunProgress * 180.f));
						auto y = sinf(Math::radians(sunProgress * 180.f));
						auto z = sinf(Math::radians(sunProgress * 180.f));*/
						ECSHandler::registry().pinComponent<TransformComponent>(mShadowSource)->setRotate({ -sunProgress * 180.f,0.f, sunProgress * 5.f });
					}
				}
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}
	}
	ImGui::EndMainMenuBar();
}
