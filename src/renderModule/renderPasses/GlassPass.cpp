#include "GlassPass.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "assetsModule/shaderModule/ShaderBase.h"
#include "assetsModule/TextureHandler.h"
#include "assetsModule/modelModule/Material.h"
#include "assetsModule/modelModule/MeshVaoRegistry.h"
#include "assetsModule/modelModule/Vertex.h"
#include "componentsModule/ArmatureComponent.h"
#include "componentsModule/MaterialComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "logsModule/logger.h"
#include "ecss/Registry.h"
#include "mathModule/VectorOperations.h"
#include "mathModule/MatrixOperators.h"
#include "multithreading/ThreadPool.h"
#include "renderModule/EntityUser.h"
#include "renderModule/UserFrameSet.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/OcTreeSystem.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

void GlassPass::init(SystemsModule::RenderData& renderDataHandle) {

	auto* rhi = Engine::instance()->rhi();

	SamplerDesc samplerDesc;
	samplerDesc.minFilter = FilterMode::Linear;
	samplerDesc.magFilter = FilterMode::Linear;
	// Off-screen refraction must not ClampToEdge — that stretches the last
	// column/row into a visible frame. Mirror keeps real scene content.
	samplerDesc.addressU = AddressMode::MirrorRepeat;
	samplerDesc.addressV = AddressMode::MirrorRepeat;
	samplerDesc.addressW = AddressMode::MirrorRepeat;
	mSampler = rhi->createSampler(samplerDesc);
	resize(renderDataHandle);

	RenderPassDesc rpDesc;
	rpDesc.colorAttachmentCount = 1;
	rpDesc.colorAttachments[0].loadOp = AttachmentLoadOp::Load;
	rpDesc.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
	rpDesc.colorAttachments[0].format = TextureFormat::RGBA16F;
	rpDesc.hasDepthStencil = true;
	rpDesc.depthAttachment.loadOp = AttachmentLoadOp::Load;
	rpDesc.depthAttachment.storeOp = AttachmentStoreOp::Store;
	rpDesc.depthAttachment.format = TextureFormat::DEPTH_COMPONENT24;
	mRenderPass = rhi->createRenderPass(rpDesc);

	DescriptorSetLayoutDesc layoutDesc;
	layoutDesc.bindings.emplace_back(6, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(13, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.push_back({ 16, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, static_cast<uint32_t>(GpuMaterialTable::kGlassTextureSlots) });
	layoutDesc.bindings.emplace_back(1, DescriptorType::StorageBuffer, ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(2, DescriptorType::StorageBuffer, ShaderStage::Vertex);
	// g_buffer.vs reads last frame's transforms and bones for the motion vectors.
	layoutDesc.bindings.emplace_back(8, DescriptorType::StorageBuffer, ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(9, DescriptorType::StorageBuffer, ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(3, DescriptorType::StorageBuffer, ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(7, DescriptorType::StorageBuffer, ShaderStage::Vertex | ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(5, DescriptorType::UniformBuffer, ShaderStage::Vertex | ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(15, DescriptorType::UniformBufferDynamic, ShaderStage::Fragment);
	// g_buffer.vs reads each entity's words (EntityUser).
	layoutDesc.bindings.emplace_back(37, DescriptorType::StorageBuffer, ShaderStage::Vertex);
	mLayout = rhi->createDescriptorSetLayout(layoutDesc);

	auto& pipelineDesc = mPipelineDesc;
	Render::VertexLayoutDesc& layout = pipelineDesc.layout;
	layout.bindings.push_back({ 0, sizeof(Vertex3D), Render::VertexInputRate::PerVertex });
	layout.attributes.push_back({ 0, 0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, position)), true });
	layout.attributes.push_back({ 1, 0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, normal)), true });
	layout.attributes.push_back({ 2, 0, Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex3D, texCoords)), true });
	layout.attributes.push_back({ 3, 0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, tangent)), true });
	layout.attributes.push_back({ 4, 0, Render::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(Vertex3D, biTangent)), true });
	layout.attributes.push_back({ 5, 0, Render::VertexFormat::Int4, static_cast<std::uint32_t>(offsetof(Vertex3D, boneIDs)), false });
	layout.attributes.push_back({ 6, 0, Render::VertexFormat::Float4, static_cast<std::uint32_t>(offsetof(Vertex3D, weights)), false });
	layout.attributes.push_back({ 7, 0, Render::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex3D, texCoords1)), true });
	pipelineDesc.topology = PrimitiveTopology::Triangles;
	pipelineDesc.cullMode = CullMode::None;
	pipelineDesc.frontFace = FrontFace::CounterClockWise;
	pipelineDesc.depthStencilState.depthTest = true;
	pipelineDesc.depthStencilState.depthWrite = false;
	pipelineDesc.depthStencilState.depthCompareOp = CompareOp::LessEqual;
	pipelineDesc.blend.enabled = true;
	pipelineDesc.blend.srcColor = BlendFactor::One;
	pipelineDesc.blend.dstColor = BlendFactor::OneMinusSrcAlpha;
	pipelineDesc.renderPass = mRenderPass;
	pipelineDesc.descriptorSetLayouts = { mLayout };
	// Set 3, the application's frame data (UserFrameSet), for what its glass hook (sfe_user_glass.glsl) reads of its own;
	// every pipeline pipelineFor makes is a copy of this desc.
	UserFrameSet::instance().extend(pipelineDesc);
	pipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/g_buffer.vs");
}

void GlassPass::resize(SystemsModule::RenderData&) {
	auto* rhi = Engine::instance()->rhi();
	for (const auto& target : mGlassTargets) {
		rhi->destroyFramebuffer(target.fbo);
	}
	mGlassTargets.clear();
	mGlassFbo = {};
	Utils::screenTarget(mSceneCopy, mSceneCopyFbo, {});
}

SFE::Render::PipelineHandle GlassPass::pipelineFor(const std::string& vertexShader, const std::string& fragmentShader, bool depthOnly) {
	const auto key = vertexShader + '|' + (depthOnly ? fragmentShader + "|depth" : fragmentShader);
	if (const auto it = mPipelines.find(key); it != mPipelines.end()) {
		return it->second;
	}
	auto* rhi = Engine::instance()->rhi();
	auto desc = mPipelineDesc;
	// Its own vertex shader where the material names one (a held thing drawn from the eyes, fp_held.vs), else g_buffer.vs.
	if (vertexShader != ComponentsModule::DefaultGBufferVS) {
		desc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, vertexShader);
	}
	desc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, fragmentShader);
	// Only where it is, through the shader's own discards: the colour under it is kept as it is.
	if (depthOnly) {
		desc.depthStencilState.depthWrite = true;
		desc.blend.srcColor = BlendFactor::Zero;
		desc.blend.dstColor = BlendFactor::One;
	}
	// A missing shader is remembered too, as nothing drawn: otherwise it is compiled again every frame.
	const auto pipeline = desc.shaders.vertex && desc.shaders.fragment ? rhi->createGraphicsPipeline(desc) : PipelineHandle{};
	if (!pipeline) {
		LogsModule::Logger::LOG_WARNING("Glass shader missing: %s or %s", vertexShader.c_str(), fragmentShader.c_str());
	}
	mPipelines.emplace(key, pipeline);
	return pipeline;
}

void GlassPass::ensureTargets(SystemsModule::RenderData& renderDataHandle) {
	auto& frame = renderDataHandle.frame;
	const auto finalTex = frame.texture(FrameId::Final);
	const auto depthTex = frame.texture(FrameId::GBufferDepth);
	if (!finalTex || !depthTex) {
		return;
	}
	for (const auto& target : mGlassTargets) {
		if (target.finalTex.id == finalTex.id && target.depthTex.id == depthTex.id) {
			mGlassFbo = target.fbo;
			return;
		}
	}

	auto* rhi = Engine::instance()->rhi();
	const int w = Engine::instance()->getWindow()->getScreenData().renderW;
	const int h = Engine::instance()->getWindow()->getScreenData().renderH;

	FramebufferDesc glassFb;
	glassFb.width = w;
	glassFb.height = h;
	glassFb.renderPass = mRenderPass;
	glassFb.colorAttachments = { { finalTex } };
	glassFb.depthStencilAttachment = { .texture = depthTex };
	mGlassFbo = rhi->createFramebuffer(glassFb);
	mGlassTargets.push_back({ finalTex, depthTex, mGlassFbo });
}

void GlassPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;

	auto* rhi = Engine::instance()->rhi();
	ensureTargets(renderDataHandle);
	const auto srcFbo = renderDataHandle.frame.framebuffer(FrameId::Final);
	if (!mGlassFbo || !srcFbo) {
		return;
	}

	// Each group culled and sorted far to near on the GPU; outside the render pass, since it is compute. The groups
	// are drawn one after another: a shader's surfaces are in order among themselves, not among another's.
	auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
	auto& scene = renderSystem->scene();
	const auto materials = renderSystem->materials().upload();
	const auto& groups = scene.groups(GpuScene::List::Glass);
	if (mGroups.size() < groups.size()) {
		mGroups.resize(groups.size());
	}
	// A solid group's depth first, then its colour only where that left its nearest surface: none of it seen through
	// the rest of it.
	struct Drawn {
		uint32_t group;
		PipelineHandle pipeline;
		PipelineHandle depthFirst;
	};
	std::vector<Drawn> drawn;
	for (uint32_t group = 0; group < groups.size(); ++group) {
		const auto pipeline = pipelineFor(groups[group].vertexShader, groups[group].fragmentShader);
		if (pipeline && scene.sortGlass(cmd, mGroups[group].view, group, renderDataHandle.mCamFrustum, renderDataHandle.mCameraPos,
			renderDataHandle.mViewDir)) {
			drawn.push_back({ group, pipeline, groups[group].solid ? pipelineFor(groups[group].vertexShader, groups[group].fragmentShader, true) : PipelineHandle{} });
		}
	}
	if (drawn.empty()) {
		return;
	}

	auto& frame = renderDataHandle.frame;

	mParams.viewPosX = renderDataHandle.mCameraPos.x;
	mParams.viewPosY = renderDataHandle.mCameraPos.y;
	mParams.viewPosZ = renderDataHandle.mCameraPos.z;
	mParams.roughness = renderDataHandle.graphics.glassRoughness;
	{
		const auto& screen = Engine::instance()->getWindow()->getScreenData();
		mParams.resolutionX = screen.renderW > 0 ? static_cast<float>(screen.renderW) : 1.f;
		mParams.resolutionY = screen.renderH > 0 ? static_cast<float>(screen.renderH) : 1.f;
	}
	if (const auto* csm = frame.data<CsmFrame>(FrameId::Csm)) {
		mParams.lightX = csm->lightDirection.x;
		mParams.lightY = csm->lightDirection.y;
		mParams.lightZ = csm->lightDirection.z;
		mParams.colorX = csm->lightColor.x;
		mParams.colorY = csm->lightColor.y;
		mParams.colorZ = csm->lightColor.z;
	}
	// Per-material ior, opacity and tint come from the material table; the rest is the same for every draw.
	const auto params = rhi->allocTransient(mParams);

	// The inputs every group's set has; each also has its own visible list.
	const DescriptorSetHandle glassSet{};
	std::vector<WriteDescriptor> writes;
	{
		WriteDescriptor w{};
		w.set = glassSet;
		w.binding = 6;
		w.images.push_back({ mSceneCopy, mSampler });
		writes.push_back(std::move(w));
	}
	{
		WriteDescriptor w{};
		w.set = glassSet;
		w.binding = 15;
		w.buffers.push_back({ params.buffer, 0, sizeof(Params) });
		writes.push_back(std::move(w));
	}
	if (frame.texture(FrameId::IblPrefilter)) {
		WriteDescriptor w{};
		w.set = glassSet;
		w.binding = 13;
		w.images.push_back({ frame.texture(FrameId::IblPrefilter), frame.sampler(FrameId::IblPrefilter) });
		writes.push_back(std::move(w));
	}
	if (renderDataHandle.cameraMatricesUBO) {
		WriteDescriptor w{};
		w.set = glassSet;
		w.binding = 5;
		w.buffers.push_back({ renderDataHandle.cameraMatricesUBO });
		writes.push_back(std::move(w));
	}
	if (renderDataHandle.transformsSSBO) {
		WriteDescriptor w{};
		w.set = glassSet;
		w.binding = 1;
		w.buffers.push_back({ renderDataHandle.transformsSSBO });
		writes.push_back(std::move(w));
	}
	if (renderDataHandle.bonesSSBO) {
		WriteDescriptor w{};
		w.set = glassSet;
		w.binding = 2;
		w.buffers.push_back({ renderDataHandle.bonesSSBO });
		writes.push_back(std::move(w));
	}
	if (renderDataHandle.prevTransformsSSBO) {
		WriteDescriptor w{};
		w.set = glassSet;
		w.binding = 8;
		w.buffers.push_back({ renderDataHandle.prevTransformsSSBO });
		writes.push_back(std::move(w));
	}
	if (renderDataHandle.prevBonesSSBO) {
		WriteDescriptor w{};
		w.set = glassSet;
		w.binding = 9;
		w.buffers.push_back({ renderDataHandle.prevBonesSSBO });
		writes.push_back(std::move(w));
	}
	// The glass albedo textures as one array, so every glass instance draws from the same set.
	{
		const auto* white = AssetsModule::TextureHandler::loadTexture("white.png");
		const auto textures = renderSystem->materials().glassTextures();
		WriteDescriptor albedo{};
		albedo.set = glassSet;
		albedo.binding = 16;
		for (size_t i = 0; i < GpuMaterialTable::kGlassTextureSlots; ++i) {
			const auto tex = i < textures.size() && textures[i] ? textures[i] : white->texture;
			albedo.images.push_back({ tex, white->sampler });
		}
		writes.push_back(std::move(albedo));
	}
	writes.push_back({ .set = glassSet, .binding = 7, .buffers{ { materials } } });
	writes.push_back({ .set = glassSet, .binding = 37, .buffers{ { EntityUser::instance().buffer() } } });
	// One set per frame slot: its inputs (the scene copy's transient parameters, the visible list) differ per
	// frame, and writing a set the previous frame still binds costs a copy of it.
	const auto slot = rhi->frameInFlight() % (std::max)(1u, rhi->framesInFlight());
	std::vector<WriteDescriptor> all;
	for (const auto& [group, pipeline, depthFirst] : drawn) {
		auto& sets = mGroups[group].sets;
		sets.resize((std::max)(1u, rhi->framesInFlight()));
		if (!sets[slot]) {
			sets[slot] = rhi->allocateDescriptorSet(mLayout);
		}
		for (auto write : writes) {
			write.set = sets[slot];
			all.push_back(std::move(write));
		}
		all.push_back({ .set = sets[slot], .binding = 3, .buffers{ { mGroups[group].view.visible } } });
	}
	rhi->updateDescriptorSets(all);

	const int w = Engine::instance()->getWindow()->getScreenData().renderW;
	const int h = Engine::instance()->getWindow()->getScreenData().renderH;
	RenderPassBeginInfo beginInfo;
	beginInfo.framebuffer = mGlassFbo;
	beginInfo.renderPass = mRenderPass;
	beginInfo.width = w;
	beginInfo.height = h;

	rhi->cmdBlit(cmd, srcFbo, mSceneCopyFbo, BlitMask::Color, BlitFilter::Nearest);
	rhi->cmdBeginRenderPass(cmd, beginInfo);
	const auto* meshes = SFE::MeshVaoRegistry::instance();
	for (const auto& [group, pipeline, depthFirst] : drawn) {
		for (const auto& one : { depthFirst, pipeline }) {
			if (!one) {
				continue;
			}
			rhi->cmdBindPipeline(cmd, one);
			rhi->cmdBindVertexBuffer(cmd, 0, meshes->vertexBuffer(), 0);
			rhi->cmdBindIndexBuffer(cmd, meshes->indexBuffer(), IndexType::Uint32, 0);
			rhi->cmdBindDescriptorSets(cmd, one, 0, &mGroups[group].sets[slot], 1, &params.offset, 1);
			// The application's frame data, after the pass's own set (UserFrameSet::bind).
			UserFrameSet::instance().bind(cmd, one);
			scene.drawSorted(cmd, mGroups[group].view, group);
		}
	}
	rhi->cmdEndRenderPass(cmd);
}
