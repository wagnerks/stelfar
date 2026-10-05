#include "PointLightPass.h"

#include <algorithm>

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

	// Caster opacity and cutoff come from the material table.
	struct PointShadowPush {
		int light = 0;
		float pad[3]{};
	};
}

namespace SFE::Render::RenderPasses {

PointLightPass::PointLightPass() {
}

void PointLightPass::collectLights(const FrustumModule::Frustum& camFrustum, const Math::Vec3& cameraPos, float time, std::vector<Math::Mat4>& matrices, std::vector<Math::Vec4>& spheres, std::vector<ecss::SectorId>& entities) {
	matrices.clear();
	spheres.clear();
	entities.clear();

	struct Candidate {
		ecss::SectorId id;
		Math::Vec3 pos;
		float nearP = 0.1f;
		float radius = 1.f;
		float dist2 = 0.f;
	};
	std::vector<Candidate> candidates;
	candidates.reserve(32);

	for (const auto& [entity, lightSource, transform] : ECSHandler::registry().view<LightSourceComponent, TransformComponent>()) {
		if (!lightSource || !transform || lightSource->getType() != ComponentsModule::eLightType::POINT) {
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
		candidates.push_back({ entity, pos, lightSource->mNear, lightSource->mRadius, Math::dot(d, d) });
	}

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		return a.dist2 < b.dist2;
	});
	if (candidates.size() > static_cast<size_t>(maxLights)) {
		candidates.resize(static_cast<size_t>(maxLights));
	}

	for (const auto& c : candidates) {
		entities.push_back(c.id);
		appendFaceMatrices(matrices, c.pos, c.nearP, c.radius);
		// The six faces reach the corners of a cube round the light, but its light ends at its radius
		// (deferred_shading.fs), and anything between it and what it lights is nearer than that: the rest is not drawn.
		spheres.push_back({ c.pos.x, c.pos.y, c.pos.z, c.radius });
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
	layoutDesc.bindings.emplace_back(11, Render::DescriptorType::UniformBufferDynamic, Render::ShaderStage::Geometry);
	layoutDesc.bindings.emplace_back(1, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(2, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(3, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(7, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Fragment);
	mBufferLayout = rhi->createDescriptorSetLayout(layoutDesc);
	mSet = rhi->allocateDescriptorSet(mBufferLayout);

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
	pipelineDesc.pushConstantStages = ShaderStage::Geometry | ShaderStage::Fragment;
	pipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/cascadeShadowMap.vs");
	pipelineDesc.shaders.geometry = rhi->loadShader(ShaderStage::Geometry, "shaders/pointLightMap.gs");
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
	auto& spheres = mSpheres;
	collectLights(renderDataHandle.mCamFrustum, renderDataHandle.mCameraPos, renderDataHandle.current.wind.w, matrices, spheres, data.shadowEntities);
	if (matrices.empty()) {
		publish(renderDataHandle);
		return;
	}

	ensureGpu(renderDataHandle);
	publish(renderDataHandle);

	auto* rhi = Engine::instance()->rhi();
	const auto matricesBlock = Utils::allocMatrices(matrices, maxShadowFaces);
	if (matricesBlock.buffer.id != mMatricesId) {
		mMatricesId = matricesBlock.buffer.id;
		rhi->updateDescriptorSets({ { .set = mSet, .binding = 11, .buffers{ { matricesBlock.buffer, 0, sizeof(Math::Mat4) * maxShadowFaces } } } });
	}

	auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
	auto& scene = renderSystem->scene();
	const auto materials = renderSystem->materials().upload();
	const bool visible = scene.cullSpheres(cmd, GpuScene::List::Shadow, mView, spheres);
	if (visible) {
		const uint32_t ids[4] = { renderDataHandle.transformsSSBO.id, renderDataHandle.bonesSSBO.id, mView.visible.id, materials.id };
		if (!std::equal(std::begin(ids), std::end(ids), std::begin(mBoundIds))) {
			std::copy(std::begin(ids), std::end(ids), std::begin(mBoundIds));
			rhi->updateDescriptorSets({
				{ .set = mSet, .binding = 1, .buffers{ { renderDataHandle.transformsSSBO } } },
				{ .set = mSet, .binding = 2, .buffers{ { renderDataHandle.bonesSSBO } } },
				{ .set = mSet, .binding = 3, .buffers{ { mView.visible } } },
				{ .set = mSet, .binding = 7, .buffers{ { materials } } },
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
	if (visible) {
		const auto* meshes = SFE::MeshVaoRegistry::instance();
		rhi->cmdBindPipeline(cmd, pipeline);
		rhi->cmdBindVertexBuffer(cmd, 0, meshes->vertexBuffer(), 0);
		rhi->cmdBindIndexBuffer(cmd, meshes->indexBuffer(), IndexType::Uint32, 0);
		// The casters drawn again for each light, its six faces at once (pointLightMap.gs): a geometry shader that may
		// write the vertices of every light's faces is slow however few it writes, the lights' faces in one draw took
		// more than twice as long for a single light.
		PointShadowPush push;
		for (size_t light = 0; light < data.shadowEntities.size(); ++light) {
			push.light = static_cast<int>(light);
			rhi->cmdPushConstants(cmd, pipeline, ShaderStage::Geometry | ShaderStage::Fragment, &push, sizeof(push));
			for (const auto& group : scene.groups(GpuScene::List::Shadow)) {
				// Transparent casters only reach the cascades, through the glass proxy.
				if (group.transparent || group.batchCount == 0) {
					continue;
				}
				const DescriptorSetHandle sets[] = { mSet, mMaskSets.get(mTextureLayout, 4, group.maskTexture) };
				rhi->cmdBindDescriptorSets(cmd, pipeline, 0, sets, 2, &matricesBlock.offset, 1);
				scene.draw(cmd, mView, group);
			}
		}
	}
	rhi->cmdEndRenderPass(cmd);
}

}
