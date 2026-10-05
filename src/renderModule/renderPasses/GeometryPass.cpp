#include "GeometryPass.h"

#include <algorithm>
#include "imgui.h"
#include "componentsModule/ModelComponent.h"
#include "assetsModule/shaderModule/ShaderBase.h"
#include "assetsModule/TextureHandler.h"
#include "assetsModule/modelModule/MeshVaoRegistry.h"
#include "assetsModule/modelModule/ModelLoader.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/MaterialTextures.h"
#include "renderModule/Utils.h"
#include "componentsModule/ArmatureComponent.h"
#include "componentsModule/CameraComponent.h"
#include "componentsModule/IsDrawableComponent.h"
#include "componentsModule/LightSourceComponent.h"
#include "componentsModule/MaterialComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/TransformComponent.h"
#include "componentsModule/OutlineComponent.h"
#include "containersModule/Vector.h"
#include "core/ECSHandler.h"
#include "multithreading/ThreadPool.h"
#include "debugModule/Benchmark.h"
#include "ecss/Registry.h"
#include "logsModule/logger.h"
#include "systemsModule/systems/CameraSystem.h"
#include "systemsModule/systems/OcTreeSystem.h"
#include "systemsModule/systems/RenderSystem.h"
#include "systemsModule/SystemManager.h"
#include "systemsModule/SystemsPriority.h"

using namespace SFE::Render::RenderPasses;

void GeometryPass::init(SystemsModule::RenderData& renderDataHandle) {
	if (mInited) {
		return;
	}
	mInited = true;

	auto rhi = Engine::instance()->rhi();
	mGpuFrames = (std::max)(1u, rhi->framesInFlight());
	mFrames.resize(mGpuFrames);
	auto* renderSystem = ECSHandler::getSystem<SFE::SystemsModule::RenderSystem>();
	const auto frameLayout = renderDataHandle.frameSetLayout
		? renderDataHandle.frameSetLayout
		: (renderSystem ? renderSystem->frameSetLayoutHandle() : Render::DescriptorSetLayoutHandle{});

	SamplerDesc gbufferSamplerDesc;
	gbufferSamplerDesc.minFilter = FilterMode::Nearest;
	gbufferSamplerDesc.magFilter = FilterMode::Nearest;
	mSampler = rhi->createSampler(gbufferSamplerDesc);

	RenderPassDesc geometryPassDesc;
	geometryPassDesc.colorAttachmentCount = 4;
	geometryPassDesc.colorAttachments[0] = { AttachmentLoadOp::Clear, AttachmentStoreOp::Store, {0.f, 0.f, 0.f, 1.f}, TextureFormat::RGBA8 };
	geometryPassDesc.colorAttachments[1] = { AttachmentLoadOp::Clear, AttachmentStoreOp::Store, {0.f, 0.f, 0.f, 1.f}, TextureFormat::RGBA16F };
	geometryPassDesc.colorAttachments[2] = { AttachmentLoadOp::Clear, AttachmentStoreOp::Store, {0.f, 0.f, 0.f, 1.f}, TextureFormat::RGBA16F };
	geometryPassDesc.colorAttachments[3] = { AttachmentLoadOp::Clear, AttachmentStoreOp::Store, {0.f, 0.f, 0.f, 0.f}, TextureFormat::RGBA16F };
	geometryPassDesc.hasDepthStencil = true;
	geometryPassDesc.depthAttachment = { AttachmentLoadOp::Clear, AttachmentStoreOp::Store, 1.f, 0, false, TextureFormat::DEPTH_COMPONENT24 };
	geometryPass = rhi->createRenderPass(geometryPassDesc);
	// The second occlusion phase draws over the first one's targets.
	for (auto& attachment : geometryPassDesc.colorAttachments) {
		attachment.loadOp = AttachmentLoadOp::Load;
	}
	geometryPassDesc.depthAttachment.loadOp = AttachmentLoadOp::Load;
	mGeometryLoadPass = rhi->createRenderPass(geometryPassDesc);

	GraphicsPipelineDesc pipelineDesc;

	Render::VertexLayoutDesc& layout = pipelineDesc.layout;
	layout.bindings.push_back({ 0,sizeof(Vertex3D), Render::VertexInputRate::PerVertex });
	layout.attributes.push_back({ 0,0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, position)), true });
	layout.attributes.push_back({ 1,0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, normal)),  true });
	layout.attributes.push_back({ 2,0, Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex3D, texCoords)), true });
	layout.attributes.push_back({ 3,0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, tangent)), true });
	layout.attributes.push_back({ 4,0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, biTangent)), true });
	layout.attributes.push_back({ 5,0, Render::VertexFormat::Int4, static_cast<std::uint32_t>(offsetof(Vertex3D, boneIDs)), false }); //todo only for dynamic mesh
	layout.attributes.push_back({ 6,0, Render::VertexFormat::Float4, static_cast<std::uint32_t>(offsetof(Vertex3D, weights)), false });
	layout.attributes.push_back({ 7,0, Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex3D, texCoords1)), true });
	pipelineDesc.topology = PrimitiveTopology::Triangles;
	pipelineDesc.cullMode = CullMode::None;
	pipelineDesc.frontFace = FrontFace::CounterClockWise;
	pipelineDesc.fillMode = PolygonFillMode::Fill;
	pipelineDesc.depthStencilState.depthTest = true;
	pipelineDesc.depthStencilState.depthWrite = true;
	pipelineDesc.depthStencilState.depthCompareOp = CompareOp::Less;
	pipelineDesc.blend.enabled = false;
	pipelineDesc.renderPass = geometryPass;
	if (frameLayout) {
		pipelineDesc.descriptorSetLayouts = { frameLayout };
	}
	pipelineDesc.shaders.vertex = Engine::instance()->rhi()->loadShader(ShaderStage::Vertex, "shaders/g_buffer.vs");
	pipelineDesc.shaders.fragment = Engine::instance()->rhi()->loadShader(ShaderStage::Fragment, "shaders/g_buffer.fs");

	Render::DescriptorSetLayoutDesc texLayoutDescGBuf;
	texLayoutDescGBuf.bindings.emplace_back(1, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDescGBuf.bindings.emplace_back(2, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDescGBuf.bindings.emplace_back(3, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDescGBuf.bindings.emplace_back(4, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDescGBuf.bindings.emplace_back(5, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDescGBuf.bindings.emplace_back(6, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDescGBuf.bindings.emplace_back(7, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDescGBuf.bindings.emplace_back(8, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDescGBuf.bindings.emplace_back(9, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDescGBuf.bindings.emplace_back(10, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDescGBuf.bindings.emplace_back(11, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	gBuffetLayout = rhi->createDescriptorSetLayout(texLayoutDescGBuf);
	gBuffetLayoutSet = rhi->allocateDescriptorSet(gBuffetLayout);
	if (frameLayout) {
		pipelineDesc.descriptorSetLayouts.push_back(gBuffetLayout);
	}

	geometryPipeline = Engine::instance()->rhi()->createGraphicsPipeline(pipelineDesc);
	mGBufferPipelineDesc = pipelineDesc;
	mGBufferPipelines.emplace(std::string(ComponentsModule::DefaultGBufferVS) + "|" + ComponentsModule::DefaultGBufferFS, geometryPipeline);

	RenderPassDesc gbrp;
	gbrp.colorAttachmentCount = 1;
	gbrp.colorAttachments[0] = { AttachmentLoadOp::Clear, AttachmentStoreOp::Store, {0.f, 0.f, 0.f, 0.f}, TextureFormat::RGBA8 };
	outlineBufferRenderPass = rhi->createRenderPass(gbrp);

	RenderPassDesc outlineRPD;
	outlineRPD.colorAttachmentCount = 1;
	outlineRPD.colorAttachments[0] = { AttachmentLoadOp::Clear, AttachmentStoreOp::Store, {0.f, 0.f, 0.f, 0.f}, TextureFormat::RGBA8 };
	outlineRenderPass = rhi->createRenderPass(outlineRPD);

	resize(renderDataHandle);

	GraphicsPipelineDesc gBufferOutlinePDesc;
	gBufferOutlinePDesc.layout = layout;
	gBufferOutlinePDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/g_buffer_outlines.vs");
	gBufferOutlinePDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/g_buffer_outlines.fs");
	gBufferOutlinePDesc.topology = PrimitiveTopology::Triangles;
	gBufferOutlinePDesc.cullMode = CullMode::Back;
	gBufferOutlinePDesc.frontFace = FrontFace::CounterClockWise;
	gBufferOutlinePDesc.depthStencilState.depthTest = false;
	gBufferOutlinePDesc.depthStencilState.depthWrite = false;
	gBufferOutlinePDesc.renderPass = outlineBufferRenderPass;
	if (frameLayout) {
		gBufferOutlinePDesc.descriptorSetLayouts = { frameLayout };
		// One set per frame slot, like the outline texture set below: their inputs alternate per slot, and writing
		// a set the previous frame still binds costs a copy of it.
		mOutlineFrameSets.resize(mGpuFrames);
		for (auto& set : mOutlineFrameSets) {
			set = rhi->allocateDescriptorSet(frameLayout);
		}
	}
	gBufferOutlinePipeline = rhi->createGraphicsPipeline(gBufferOutlinePDesc);

	Render::DescriptorSetLayoutDesc texLayoutDesc;
	texLayoutDesc.bindings.emplace_back(25, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(26, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	texLayoutDesc.bindings.emplace_back(27, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Fragment);
	mTexSetLayoutOutlines = rhi->createDescriptorSetLayout(texLayoutDesc);
	mTexSetsOutlines.resize(mGpuFrames);
	for (auto& set : mTexSetsOutlines) {
		set.set = rhi->allocateDescriptorSet(mTexSetLayoutOutlines);
	}

	GraphicsPipelineDesc outlinePDesc;
	outlinePDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/g_outline.vs");
	outlinePDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/g_outline.fs");
	outlinePDesc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(Vertex2D)), Render::VertexInputRate::PerVertex);
	outlinePDesc.layout.attributes.emplace_back(0, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, position)), false);
	outlinePDesc.layout.attributes.emplace_back(1, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, texCoords)), false);
	outlinePDesc.topology = PrimitiveTopology::Triangles;
	outlinePDesc.cullMode = CullMode::None;
	outlinePDesc.depthStencilState.depthTest = false;
	outlinePDesc.depthStencilState.depthWrite = false;
	outlinePDesc.renderPass = outlineRenderPass;
	outlinePDesc.descriptorSetLayouts = { mTexSetLayoutOutlines };
	outlinePipeline = rhi->createGraphicsPipeline(outlinePDesc);

	initGrid(frameLayout);
}

void GeometryPass::resize(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();
	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	const auto target = [&](TextureHandle& texture, const TextureFormat format, const TextureUsage usage) {
		if (texture) {
			rhi->destroyTexture(texture);
		}
		TextureDesc desc;
		desc.width = screen.renderW;
		desc.height = screen.renderH;
		desc.format = format;
		desc.usage = usage;
		texture = rhi->createTexture(desc);
	};
	const auto color = TextureUsage::Sampled | TextureUsage::ColorAttachment;
	for (auto& slot : mFrames) {
		if (slot.gFramebuffer) {
			rhi->destroyFramebuffer(slot.gFramebuffer);
		}
		target(slot.materialBuffer, TextureFormat::RGBA8, color);
		target(slot.normalBuffer, TextureFormat::RGBA16F, color);
		target(slot.albedoBuffer, TextureFormat::RGBA16F, color);
		// Where each pixel was last frame, in UV. TAA reprojects its history by this instead of by the camera alone,
		// which is what let a skinned character smear a trail of itself.
		target(slot.velocityBuffer, TextureFormat::RGBA16F, color);
		target(slot.depthsBuffer, TextureFormat::DEPTH_COMPONENT24, TextureUsage::DepthStencilAttachment | TextureUsage::Sampled);
		FramebufferDesc mainFboDesc;
		mainFboDesc.width = screen.renderW;
		mainFboDesc.height = screen.renderH;
		mainFboDesc.renderPass = geometryPass;
		mainFboDesc.colorAttachments = { {slot.materialBuffer}, {slot.normalBuffer}, {slot.albedoBuffer}, {slot.velocityBuffer} };
		mainFboDesc.depthStencilAttachment = FramebufferAttachmentDesc{.texture = slot.depthsBuffer };
		slot.gFramebuffer = rhi->createFramebuffer(mainFboDesc);
	}
	target(lightsBuffer, TextureFormat::RGBA8, color);
	Utils::screenTarget(outlinesMask, outlineFramebuffer, outlineBufferRenderPass, TextureFormat::RGBA8);
	Utils::screenTarget(outlinesBuffer, outlineResultFbo, outlineRenderPass, TextureFormat::RGBA8);
	// The outline sets bind them again the next frame.
	for (auto& textures : mTexSetsOutlines) {
		textures.lights = {};
		textures.normal = {};
		textures.mask = {};
	}
	publish(renderDataHandle);
}

void GeometryPass::publish(SystemsModule::RenderData& renderDataHandle) {
	auto& frame = renderDataHandle.frame;
	const auto s = mSampler;
	const auto& slot = mFrames[Engine::instance()->rhi()->frameInFlight() % mGpuFrames];
	frame.set(FrameId::GBufferFbo, { .framebuffer = slot.gFramebuffer });
	frame.set(FrameId::GBufferOutlineFbo, { .framebuffer = outlineFramebuffer });
	frame.set(FrameId::GBufferMaterial, { .texture = slot.materialBuffer, .sampler = s });
	frame.set(FrameId::GBufferNormal, { .texture = slot.normalBuffer, .sampler = s });
	frame.set(FrameId::GBufferAlbedo, { .texture = slot.albedoBuffer, .sampler = s });
	frame.set(FrameId::GBufferVelocity, { .texture = slot.velocityBuffer, .sampler = s });
	frame.set(FrameId::GBufferOutlines, { .texture = outlinesBuffer, .sampler = s });
	frame.set(FrameId::GBufferLights, { .texture = lightsBuffer, .sampler = s });
	frame.set(FrameId::GBufferDepth, { .texture = slot.depthsBuffer, .sampler = s });
}

void GeometryPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	if (!mInited) {
		return;
	}

	publish(renderDataHandle);
	auto rhi = Engine::instance()->rhi();
	const auto& slot = mFrames[rhi->frameInFlight() % mGpuFrames];
	const bool wireframe = renderDataHandle.mRenderType == SystemsModule::RenderMode::WIREFRAME;

	RenderPassBeginInfo geometryPassBegin;
	geometryPassBegin.renderPass = geometryPass;
	geometryPassBegin.framebuffer = slot.gFramebuffer;
	geometryPassBegin.width = Engine::instance()->getWindow()->getScreenData().renderW;
	geometryPassBegin.height = Engine::instance()->getWindow()->getScreenData().renderH;

	// Culling is a compute dispatch, so it is recorded before the render pass opens.
	auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
	const auto materials = renderSystem->materials().upload();
	auto& scene = renderSystem->scene();
	const bool sceneVisible = scene.cullVisibleLastFrame(cmd, GpuScene::List::GBuffer, mCameraView, renderDataHandle.mCamFrustum);
	const bool outlineVisible = scene.cullFrustum(cmd, GpuScene::List::Outline, mOutlineView, renderDataHandle.mCamFrustum);

	rhi->cmdBeginRenderPass(cmd, geometryPassBegin);
	const auto fillMode = wireframe ? PolygonFillMode::Line : PolygonFillMode::Fill;
	if (sceneVisible) {
		drawGBuffer(cmd, mCameraView, renderSystem->frameSetHandle(), materials, fillMode, false);
	}
	rhi->cmdEndRenderPass(cmd);

	// What was visible last frame is in the depth now: everything else is drawn only where it is not behind it.
	if (sceneVisible) {
		scene.buildDepthPyramid(cmd, mCameraView, slot.depthsBuffer, mSampler, geometryPassBegin.width, geometryPassBegin.height);
	}
	const bool occludedVisible = sceneVisible && scene.cullOccluded(cmd, GpuScene::List::GBuffer, mCameraView, renderDataHandle.mCamFrustum, renderDataHandle.current.PV);
	geometryPassBegin.renderPass = mGeometryLoadPass;
	rhi->cmdBeginRenderPass(cmd, geometryPassBegin);
	if (occludedVisible) {
		drawGBuffer(cmd, mCameraView, renderSystem->frameSetHandle(), materials, fillMode, true);
	}
	// The grid keeps its own pipeline, so it stays filled in wireframe mode -- which is what it
	// already did, since binding a pipeline reset the polygon mode on OpenGL anyway.
	// An outdoor scene has its own ground; the grid would only cut through it.
	if (!Atmosphere::sceneSky()) {
		drawGrid(renderDataHandle, cmd);
	}
	rhi->cmdEndRenderPass(cmd);

	if (outlinePipeline && outlineFramebuffer && outlineVisible) {
		const int rw = Engine::instance()->getWindow()->getScreenData().renderW;
		const int rh = Engine::instance()->getWindow()->getScreenData().renderH;
		needClearOutlines = true;

		RenderPassBeginInfo beginGBuf;
		beginGBuf.renderPass = outlineBufferRenderPass;
		beginGBuf.framebuffer = outlineFramebuffer;
		beginGBuf.width = rw;
		beginGBuf.height = rh;
		rhi->cmdBeginRenderPass(cmd, beginGBuf);
		rhi->cmdBindPipeline(cmd, gBufferOutlinePipeline);
		const auto outlineFrameSet = mOutlineFrameSets[rhi->frameInFlight() % mGpuFrames];
		auto& outlineTextures = mTexSetsOutlines[rhi->frameInFlight() % mGpuFrames];
		if (outlineFrameSet && renderDataHandle.transformsSSBO) {
			rhi->updateDescriptorSets({
				{
					.set = outlineFrameSet,
					.binding = 1,
					
					.buffers{ {renderDataHandle.transformsSSBO} }
				},
				{
					.set = outlineFrameSet,
					.binding = 2,
					
					.buffers{ {renderDataHandle.bonesSSBO} }
				},
				{
					.set = outlineFrameSet,
					.binding = 5,

					.buffers{ {renderDataHandle.cameraMatricesUBO} }
				},
				{ .set = outlineFrameSet, .binding = 3, .buffers{ { mOutlineView.visible } } },
			});
		}
		rhi->cmdBindVertexBuffer(cmd, 0, MeshVaoRegistry::instance()->vertexBuffer(), 0);
		rhi->cmdBindIndexBuffer(cmd, MeshVaoRegistry::instance()->indexBuffer(), IndexType::Uint32, 0);
		rhi->cmdBindDescriptorSets(cmd, gBufferOutlinePipeline, 0, &outlineFrameSet, 1);
		for (const auto& group : scene.groups(GpuScene::List::Outline)) {
			scene.draw(cmd, mOutlineView, group);
		}
		rhi->cmdEndRenderPass(cmd);

		if (outlineResultFbo) {
			RenderPassBeginInfo beginOutlines;
			beginOutlines.renderPass = outlineRenderPass;
			beginOutlines.framebuffer = outlineResultFbo;
			beginOutlines.width = rw;
			beginOutlines.height = rh;
			rhi->cmdBeginRenderPass(cmd, beginOutlines);
			rhi->cmdBindPipeline(cmd, outlinePipeline);

			std::vector<Render::WriteDescriptor> writes;
			auto bindIfChanged = [&](uint32_t binding, TextureHandle tex, TextureHandle& prev) {
				if (tex.id == prev.id) {
					return;
				}
				prev = tex;
				writes.push_back({
					.set = outlineTextures.set,
					.binding = binding,
					
					.images{ {tex, mSampler} }
				});
			};
			bindIfChanged(25, lightsBuffer, outlineTextures.lights);
			bindIfChanged(26, slot.normalBuffer, outlineTextures.normal);
			bindIfChanged(27, outlinesMask, outlineTextures.mask);
			if (!writes.empty()) {
				rhi->updateDescriptorSets(writes);
			}
			rhi->cmdBindDescriptorSets(cmd, outlinePipeline, 0, &outlineTextures.set, 1);
			Utils::renderQuad(cmd);
			rhi->cmdEndRenderPass(cmd);
		}
	}
	else if (outlinePipeline && needClearOutlines) {
		const int rw = Engine::instance()->getWindow()->getScreenData().renderW;
		const int rh = Engine::instance()->getWindow()->getScreenData().renderH;
		if (outlineFramebuffer) {
			RenderPassBeginInfo beginGBuf;
			beginGBuf.renderPass = outlineBufferRenderPass;
			beginGBuf.framebuffer = outlineFramebuffer;
			beginGBuf.width = rw;
			beginGBuf.height = rh;
			rhi->cmdBeginRenderPass(cmd, beginGBuf);
			rhi->cmdEndRenderPass(cmd);
		}
		if (outlineResultFbo) {
			RenderPassBeginInfo beginOutlines;
			beginOutlines.renderPass = outlineRenderPass;
			beginOutlines.framebuffer = outlineResultFbo;
			beginOutlines.width = rw;
			beginOutlines.height = rh;
			rhi->cmdBeginRenderPass(cmd, beginOutlines);
			rhi->cmdEndRenderPass(cmd);
		}
		needClearOutlines = false;
	}
}

void GeometryPass::drawGBuffer(const CommandListHandle cmd, const GpuScene::View& view, const DescriptorSetHandle frameSet,
	const BufferHandle materials, const PolygonFillMode fillMode, const bool secondPhase) {
	auto* rhi = Engine::instance()->rhi();
	auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();

	auto& written = mFrameSetBuffers[frameSet.id];
	if (written.first != view.visible.id || written.second != materials.id) {
		written = { view.visible.id, materials.id };
		rhi->updateDescriptorSets({
			{ .set = frameSet, .binding = 3, .buffers{ { view.visible } } },
			{ .set = frameSet, .binding = 7, .buffers{ { materials } } },
		});
	}

	const auto* meshes = SFE::MeshVaoRegistry::instance();
	PipelineHandle bound{};
	auto& scene = renderSystem->scene();
	// Solid surfaces first, cut-out ones (leaves, grass cards) after: a discarding shader turns early depth testing off,
	// so a leaf hidden behind a trunk or the ground is only rejected before shading if their depth is already there.
	for (const bool cutoutPass : { false, true })
	for (const auto& group : scene.groups(GpuScene::List::GBuffer)) {
		if (group.batchCount == 0 || group.cutout != cutoutPass) {
			continue;
		}
		const auto pipeline = getGBufferPipeline(group.vertexShader, group.fragmentShader, fillMode);
		if (pipeline.id != bound.id) {
			bound = pipeline;
			rhi->cmdBindPipeline(cmd, pipeline);
			// After the pipeline: OpenGL keeps the vertex and index buffers on the pipeline's VAO.
			rhi->cmdBindVertexBuffer(cmd, 0, meshes->vertexBuffer(), 0);
			rhi->cmdBindIndexBuffer(cmd, meshes->indexBuffer(), IndexType::Uint32, 0);
		}

		auto& textures = mTextureSets[group.textureKey];
		if (!textures) {
			textures = rhi->allocateDescriptorSet(gBuffetLayout);
			rhi->updateDescriptorSets(materialTextureWrites(textures, group.textures));
		}
		const DescriptorSetHandle sets[] = { frameSet, textures };
		rhi->cmdBindDescriptorSets(cmd, pipeline, 0, sets, 2);
		scene.draw(cmd, view, group, secondPhase);
	}
}

SFE::Render::PipelineHandle GeometryPass::getGBufferPipeline(const std::string& vertexShader, const std::string& fragmentShader,
	const Render::PolygonFillMode fillMode) {
	const std::string vs = vertexShader.empty() ? ComponentsModule::DefaultGBufferVS : vertexShader;
	const std::string fs = fragmentShader.empty() ? ComponentsModule::DefaultGBufferFS : fragmentShader;
	const auto key = vs + "|" + fs + (fillMode == PolygonFillMode::Line ? "|line" : "|fill");
	if (const auto it = mGBufferPipelines.find(key); it != mGBufferPipelines.end()) {
		return it->second;
	}

	auto* rhi = Engine::instance()->rhi();
	const auto vert = rhi->loadShader(ShaderStage::Vertex, vs);
	const auto frag = rhi->loadShader(ShaderStage::Fragment, fs);
	if (!vert || !frag) {
		LogsModule::Logger::LOG_WARNING("GBuffer shader missing: %s / %s", vs.c_str(), fs.c_str());
		// Remember the failure: otherwise the broken shader is recompiled every draw group, every frame.
		mGBufferPipelines.emplace(key, geometryPipeline);
		return geometryPipeline;
	}

	auto desc = mGBufferPipelineDesc;
	desc.shaders.vertex = vert;
	desc.shaders.fragment = frag;
	desc.fillMode = fillMode;
	const auto pipeline = rhi->createGraphicsPipeline(desc);
	mGBufferPipelines.emplace(key, pipeline);
	return pipeline;
}

void GeometryPass::initGrid(const Render::DescriptorSetLayoutHandle frameLayout) {
	auto* rhi = Engine::instance()->rhi();
	std::vector<SFE::Vertex3D> vertices = {
		{{ 1.f, 0.f, -1.f}, {1.f,1.f}, {0.f,1.f,0.f}},
		{{ 1.f, 0.f,  1.f}, {1.f,0.f}, {0.f,1.f,0.f}},
		{{-1.f, 0.f,  1.f}, {0.f,0.f}, {0.f,1.f,0.f}},
		{{-1.f, 0.f, -1.f}, {0.f,1.f}, {0.f,1.f,0.f}},
	};
	std::vector<unsigned> indices = { 0, 2, 1, 3, 2, 0 };

	BufferDesc vboDesc;
	vboDesc.size = vertices.size() * sizeof(SFE::Vertex3D);
	vboDesc.usage = BufferUsage::Vertex;
	vboDesc.cpuAccess = CpuAccess::None;
	mGridVBO = rhi->createBuffer(vboDesc, vertices.data());

	BufferDesc eboDesc;
	eboDesc.size = indices.size() * sizeof(unsigned);
	eboDesc.usage = BufferUsage::Index;
	eboDesc.cpuAccess = CpuAccess::None;
	mGridEBO = rhi->createBuffer(eboDesc, indices.data());

	GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.layout.bindings.push_back({ 0, sizeof(SFE::Vertex3D), VertexInputRate::PerVertex });
	pipelineDesc.layout.attributes.push_back({ 0, 0, VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, position)), false });
	pipelineDesc.layout.attributes.push_back({ 1, 0, VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, normal)), true });
	pipelineDesc.layout.attributes.push_back({ 2, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SFE::Vertex3D, texCoords)), true });
	pipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/grid.vs");
	pipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/grid.fs");
	pipelineDesc.renderPass = geometryPass;
	if (frameLayout) {
		pipelineDesc.descriptorSetLayouts = { frameLayout };
	}
	pipelineDesc.pushConstantSize = static_cast<uint32_t>(sizeof(GridDrawData));
	pipelineDesc.topology = PrimitiveTopology::Triangles;
	pipelineDesc.cullMode = CullMode::None;
	pipelineDesc.frontFace = FrontFace::CounterClockWise;
	pipelineDesc.fillMode = PolygonFillMode::Fill;
	pipelineDesc.depthStencilState = { true, true, CompareOp::LessEqual };
	pipelineDesc.blend = { false };
	mGridPipeline = rhi->createGraphicsPipeline(pipelineDesc);
}

void GeometryPass::drawGrid(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	if (!mGridPipeline) {
		return;
	}

	auto camera = ECSHandler::getSystem<SFE::SystemsModule::CameraSystem>()->getCurrentCamera();
	auto cameraComp = ECSHandler::registry().pinComponent<const CameraComponent>(camera);
	auto* rhi = Engine::instance()->rhi();
	rhi->cmdBindPipeline(cmd, mGridPipeline);
	if (auto* renderSystem = ECSHandler::getSystem<SFE::SystemsModule::RenderSystem>()) {
		const auto frameSet = renderSystem->frameSetHandle();
		if (frameSet) {
			rhi->cmdBindDescriptorSets(cmd, mGridPipeline, 0, &frameSet, 1);
		}
	}
	rhi->cmdBindIndexBuffer(cmd, mGridEBO, IndexType::Uint32);
	rhi->cmdBindVertexBuffer(cmd, 0, mGridVBO);
	mGridDrawData.viewPos.xyz = renderDataHandle.mCameraPos;
	mGridDrawData.far = cameraComp->getProjection().getFar();
	mGridDrawData.near = cameraComp->getProjection().getNear();
	rhi->cmdPushConstants(cmd, mGridPipeline, mGridDrawData);
	rhi->cmdDrawIndexed(cmd, 6);
}
