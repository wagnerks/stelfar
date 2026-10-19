#include "PointLightPass.h"

#include <algorithm>
#include <cmath>

#include "assetsModule/TextureHandler.h"
#include "assetsModule/modelModule/MeshVaoRegistry.h"
#include "assetsModule/modelModule/Vertex.h"
#include "mathModule/Projection.h"
#include "mathModule/Utils.h"
#include "assetsModule/shaderModule/ShaderBase.h"
#include "componentsModule/LightSourceComponent.h"
#include "componentsModule/MaterialComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "ecss/Registry.h"
#include "multithreading/ThreadPool.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/OcTreeSystem.h"
#include "systemsModule/systems/RenderSystem.h"
#include "systemsModule/SystemManager.h"

namespace {
	void appendFaceMatrices(std::vector<SFE::Math::Mat4>& out, SFE::Math::Vec3 pos, float nearP, float radius) {
		SFE::MathModule::PerspectiveProjection proj(90.f, 1.f, nearP, radius);
		const auto transform = SFE::Math::translate(SFE::Math::Mat4(1.0f), pos);
		for (auto angle : { 0.f, 90.f, -90.f, 180.f }) {
			out.push_back(proj.getProjectionsMatrix() * SFE::Math::inverseAffine(SFE::Math::rotate(transform, SFE::Math::radians(angle), { 0.f, 1.f, 0.f })));
		}
		for (auto angle : { 90.f, -90.f }) {
			const auto rotated = SFE::Math::rotate(transform, SFE::Math::radians(angle), { 1.f, 0.f, 0.f });
			out.push_back(proj.getProjectionsMatrix() * SFE::Math::inverseAffine(rotated));
		}
	}

	// A spot's one face: from `pos` down the entity's -Z, its cone wide, the entity's scale left out.
	void appendSpotMatrix(std::vector<SFE::Math::Mat4>& out, const SFE::Math::Mat4& world, SFE::Math::Vec3 pos, float outer, float nearP, float radius) {
		SFE::MathModule::PerspectiveProjection proj(2.f * outer, 1.f, nearP, radius);
		SFE::Math::Mat4 eye(1.f);
		for (int axis = 0; axis < 3; ++axis) {
			eye[axis] = SFE::Math::Vec4(SFE::Math::normalize(SFE::Math::Vec3(world[axis])), 0.f);
		}
		eye[3] = SFE::Math::Vec4(pos, 1.f);
		out.push_back(proj.getProjectionsMatrix() * SFE::Math::inverseAffine(eye));
	}

	// The widest a spot's cone goes, half its angle in degrees: one face's projection runs out towards 90.
	constexpr float kMaxSpotOuter = 80.f;

	// The face drawn (pointLightMap.vs). Caster opacity and cutoff come from the material table.
	struct PointShadowPush {
		int face = 0;
		float pad[3]{};
	};
}

namespace SFE::Render::RenderPasses {

PointLightPass::PointLightPass() {
}

void PointLightPass::collectLights(const FrustumModule::Frustum& camFrustum, const Math::Vec3& cameraPos, float time, std::vector<Math::Mat4>& matrices, std::vector<Data::Light>& lights) {
	matrices.clear();
	lights.clear();

	struct Candidate {
		Data::Light light;
		Math::Mat4 world;
		float nearP = 0.1f;
		float radius = 1.f;
		float outer = 0.f; // a spot's cone, half its angle in degrees
		bool shadows = true;
		float dist2 = 0.f;
	};
	std::vector<Candidate> candidates;
	candidates.reserve(32);

	for (const auto& [entity, lightSource, transform] : ECSHandler::registry().view<LightSourceComponent, TransformComponent>()) {
		if (!lightSource || !transform) {
			continue;
		}
		const bool spot = lightSource->getType() == ComponentsModule::eLightType::SPOT;
		if (!spot && lightSource->getType() != ComponentsModule::eLightType::POINT) {
			continue;
		}
		auto pos = transform->getPos(true);
		if (lightSource->mFlicker > 0.f) {
			pos = pos + lightSource->flickerShift(time);
		}
		if (!FrustumModule::SquareAABB::isOnFrustum(camFrustum, pos, lightSource->mRadius)) {
			continue;
		}
		const auto d = pos - cameraPos;
		Candidate c;
		c.light.entity = entity;
		c.light.position = pos;
		c.light.spot = spot;
		c.world = transform->copyTransform();
		c.nearP = lightSource->mNear;
		c.radius = lightSource->mRadius;
		c.shadows = lightSource->mWithShadows;
		c.dist2 = Math::dot(d, d);
		if (spot) {
			c.outer = (std::clamp)(lightSource->mSpotOuter, 1.f, kMaxSpotOuter);
			c.light.direction = Math::normalize(-Math::Vec3(c.world[2]));
			c.light.cosOuter = std::cos(Math::radians(c.outer));
			c.light.cosInner = (std::max)(std::cos(Math::radians((std::clamp)(lightSource->mSpotInner, 0.f, c.outer))), c.light.cosOuter + 1e-4f);
		}
		candidates.push_back(c);
	}

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		return a.dist2 < b.dist2;
	});
	if (candidates.size() > static_cast<size_t>(maxLights)) {
		candidates.resize(static_cast<size_t>(maxLights));
	}

	for (auto& c : candidates) {
		c.light.firstFace = static_cast<int>(matrices.size());
		if (c.shadows) {
			if (c.light.spot) {
				appendSpotMatrix(matrices, c.world, c.light.position, c.outer, c.nearP, c.radius);
			}
			else {
				appendFaceMatrices(matrices, c.light.position, c.nearP, c.radius);
			}
		}
		c.light.faces = static_cast<int>(matrices.size()) - c.light.firstFace;
		lights.push_back(c.light);
	}
}

void PointLightPass::init(SystemsModule::RenderData&) {
	mInited = true;
}

void PointLightPass::ensureGpu(SystemsModule::RenderData&) {
	if (mGpuReady) {
		return;
	}
	mGpuReady = true;

	auto* rhi = Engine::instance()->rhi();

	TextureDesc texDesc;
	texDesc.width = shadowResolution;
	texDesc.height = shadowResolution;
	texDesc.depth = maxShadowFaces;
	texDesc.dimension = TextureDimension::Array;
	texDesc.format = TextureFormat::DEPTH_COMPONENT32;
	texDesc.usage = TextureUsage::DepthStencilAttachment | TextureUsage::Sampled;
	mLightDepthMaps = rhi->createTexture(texDesc);

	SamplerDesc samplerDesc;
	samplerDesc.minFilter = FilterMode::Linear;
	samplerDesc.magFilter = FilterMode::Linear;
	samplerDesc.addressU = AddressMode::ClampToEdge;
	samplerDesc.addressV = AddressMode::ClampToEdge;
	samplerDesc.addressW = AddressMode::ClampToEdge;
	samplerDesc.compareOp = CompareOp::LessEqual;
	mLightDepthMapsSampler = rhi->createSampler(samplerDesc);

	RenderPassDesc rpDesc;
	rpDesc.hasDepthStencil = true;
	rpDesc.depthAttachment.loadOp = AttachmentLoadOp::Clear;
	rpDesc.depthAttachment.storeOp = AttachmentStoreOp::Store;
	rpDesc.depthAttachment.clearDepth = 1.f;
	rpDesc.depthAttachment.format = TextureFormat::DEPTH_COMPONENT32;
	renderPass = rhi->createRenderPass(rpDesc);

	FramebufferDesc fbDesc;
	fbDesc.width = shadowResolution;
	fbDesc.height = shadowResolution;
	fbDesc.renderPass = renderPass;
	FramebufferAttachmentDesc depthAtt;
	depthAtt.texture = mLightDepthMaps;
	depthAtt.mipLevel = 0;
	depthAtt.layer = UINT32_MAX;
	fbDesc.depthStencilAttachment = depthAtt;
	lightFramebuffer = rhi->createFramebuffer(fbDesc);

	// Set 0: buffers. Set 1: the caster's mask texture.
	Render::DescriptorSetLayoutDesc layoutDesc;
	layoutDesc.bindings.emplace_back(11, Render::DescriptorType::UniformBufferDynamic, Render::ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(1, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(2, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(3, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(7, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Fragment);
	mBufferLayout = rhi->createDescriptorSetLayout(layoutDesc);

	Render::DescriptorSetLayoutDesc textureLayoutDesc;
	textureLayoutDesc.bindings.emplace_back(4, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	mTextureLayout = rhi->createDescriptorSetLayout(textureLayoutDesc);

	GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.depthStencilState.depthTest = true;
	pipelineDesc.depthStencilState.depthWrite = true;
	pipelineDesc.depthStencilState.depthCompareOp = CompareOp::Less;
	// Front-face culling stores the far side of thin casters (paws, tail) and
	// lifts the contact shadow off the ground. Sample both faces; closest depth wins.
	pipelineDesc.cullMode = CullMode::None;
	pipelineDesc.frontFace = FrontFace::CounterClockWise;
	pipelineDesc.renderPass = renderPass;
	pipelineDesc.descriptorSetLayouts = { mBufferLayout, mTextureLayout };
	pipelineDesc.pushConstantSize = sizeof(PointShadowPush);
	pipelineDesc.pushConstantStages = ShaderStage::Vertex;
	pipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/pointLightMap.vs");
	pipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/pointLightMap.fs");

	Render::VertexLayoutDesc vaoDesc;
	vaoDesc.bindings.push_back({ 0, sizeof(SFE::Vertex3D), Render::VertexInputRate::PerVertex });
	vaoDesc.attributes.push_back({ 0, 0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, position)), true });
	vaoDesc.attributes.push_back({ 1, 0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, normal)), true });
	vaoDesc.attributes.push_back({ 2, 0, Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, texCoords)), true });
	vaoDesc.attributes.push_back({ 3, 0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, tangent)), true });
	vaoDesc.attributes.push_back({ 4, 0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, biTangent)), true });
	vaoDesc.attributes.push_back({ 5, 0, Render::VertexFormat::Int4, static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, boneIDs)), false });
	vaoDesc.attributes.push_back({ 6, 0, Render::VertexFormat::Float4, static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, weights)), false });
	vaoDesc.attributes.push_back({ 7, 0, Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, texCoords1)), true });
	pipelineDesc.layout = vaoDesc;
	pipeline = rhi->createGraphicsPipeline(pipelineDesc);
	pipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/cascadeShadowMap_opaque.fs");
	pipelineOpaque = rhi->createGraphicsPipeline(pipelineDesc);
}

void PointLightPass::publish(SystemsModule::RenderData& renderDataHandle) {
	renderDataHandle.frame.setData(FrameId::PointShadow, &data);
	renderDataHandle.frame.set(FrameId::ShadowPoint, { .texture = mLightDepthMaps, .sampler = mLightDepthMapsSampler });
}

void PointLightPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	if (!mInited) {
		return;
	}

	FUNCTION_BENCHMARK;

	auto& matrices = data.matrices;
	collectLights(renderDataHandle.mCamFrustum, renderDataHandle.mCameraPos, renderDataHandle.current.wind.w, matrices, data.lights);
	if (matrices.empty()) {
		publish(renderDataHandle);
		return;
	}

	ensureGpu(renderDataHandle);
	publish(renderDataHandle);

	auto* rhi = Engine::instance()->rhi();
	const auto matricesBlock = Utils::allocMatrices(matrices, maxShadowFaces);

	auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
	auto& scene = renderSystem->scene();
	const auto materials = renderSystem->materials().upload();
	// Each face's casters culled against its own frustum, the light's radius its far end: a caster beside a light goes
	// only to the one or two faces it shows in, not to all six.
	const size_t faces = (std::min)(matrices.size(), mViews.size());
	mFaceViews.clear();
	mFaceFrusta.clear();
	for (size_t face = 0; face < faces; ++face) {
		mFaceViews.push_back(&mViews[face].view);
		mFaceFrusta.push_back(FrustumModule::createFrustum(matrices[face]));
	}
	rhi->cmdBeginRegion(cmd, "PointCull");
	const bool culled = scene.cullFrusta(cmd, GpuScene::List::Shadow, mFaceViews, mFaceFrusta);
	rhi->cmdEndRegion(cmd);
	// Nothing culled leaves nothing to draw, but the pass still runs: the maps are cleared, and on Vulkan they reach
	// SHADER_READ_ONLY only through it.
	const size_t drawn = culled ? faces : 0;
	for (size_t face = 0; face < drawn; ++face) {
		auto& faceView = mViews[face];
		if (!faceView.set) {
			faceView.set = rhi->allocateDescriptorSet(mBufferLayout);
		}
		const uint32_t ids[5] = { renderDataHandle.transformsSSBO.id, renderDataHandle.bonesSSBO.id, faceView.view.visible.id, materials.id,
			matricesBlock.buffer.id };
		if (!std::equal(std::begin(ids), std::end(ids), std::begin(faceView.boundIds))) {
			std::copy(std::begin(ids), std::end(ids), std::begin(faceView.boundIds));
			rhi->updateDescriptorSets({
				{ .set = faceView.set, .binding = 1, .buffers{ { renderDataHandle.transformsSSBO } } },
				{ .set = faceView.set, .binding = 2, .buffers{ { renderDataHandle.bonesSSBO } } },
				{ .set = faceView.set, .binding = 3, .buffers{ { faceView.view.visible } } },
				{ .set = faceView.set, .binding = 7, .buffers{ { materials } } },
				{ .set = faceView.set, .binding = 11, .buffers{ { matricesBlock.buffer, 0, sizeof(Math::Mat4) * maxShadowFaces } } },
			});
		}
	}

	RenderPassBeginInfo begin;
	begin.renderPass = renderPass;
	begin.framebuffer = lightFramebuffer;
	begin.x = 0;
	begin.y = 0;
	begin.width = shadowResolution;
	begin.height = shadowResolution;

	rhi->cmdBeginRenderPass(cmd, begin);
	{
		const auto* meshes = SFE::MeshVaoRegistry::instance();
		// The casters drawn again for each face, its layer picked in the vertex shader. The opaque ones first, depth only,
		// so their depth is there to reject the cut-out ones behind them.
		PointShadowPush push;
		for (size_t face = 0; face < drawn; ++face) {
			push.face = static_cast<int>(face);
			PipelineHandle bound{};
			for (const bool cutoutPass : { false, true })
			for (const auto& group : scene.groups(GpuScene::List::Shadow)) {
				// Transparent casters only reach the cascades, through the glass proxy.
				if (group.transparent || group.batchCount == 0 || group.cutout != cutoutPass) {
					continue;
				}
				const auto drawn = group.cutout ? pipeline : pipelineOpaque;
				if (drawn.id != bound.id) {
					bound = drawn;
					rhi->cmdBindPipeline(cmd, drawn);
					// After the pipeline: OpenGL keeps the vertex and index buffers on the pipeline's VAO.
					rhi->cmdBindVertexBuffer(cmd, 0, meshes->vertexBuffer(), 0);
					rhi->cmdBindIndexBuffer(cmd, meshes->indexBuffer(), IndexType::Uint32, 0);
					rhi->cmdPushConstants(cmd, drawn, ShaderStage::Vertex, &push, sizeof(push));
				}
				const DescriptorSetHandle sets[] = { mViews[face].set, mMaskSets.get(mTextureLayout, 4, group.maskTexture) };
				rhi->cmdBindDescriptorSets(cmd, drawn, 0, sets, 2, &matricesBlock.offset, 1);
				scene.draw(cmd, mViews[face].view, group);
			}
		}
	}
	rhi->cmdEndRenderPass(cmd);
}

}
