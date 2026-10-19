#include "ParticlePass.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "assetsModule/modelModule/Vertex.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/FrameBlackboard.h"
#include "renderModule/GBuffer.h"
#include "renderModule/Particles.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

namespace {
	using SFE::Render::Particles::GpuCatcher;
	using SFE::Render::Particles::GpuParticle;
	using SFE::Render::Particles::kCatchers;
	using SFE::Render::Particles::kMaxCaught;
	using SFE::Render::Particles::kShapeWords;
	using SFE::Render::Particles::kCapacity;
	using SFE::Render::Particles::kMaxSpawn;

	// Mirrors particles.cs's Push.
	struct StepPush {
		float dt = 0.f;
		uint32_t start = 0;
		uint32_t count = 0;
		float windSpeed = 0.f;
		uint32_t catchers = 0;
	};
	// The drops caught in a frame slot (particles.cs's SfeCaught): the count and three words to keep the rest aligned,
	// then two vec4 each.
	constexpr size_t kCaughtHead = 4 * sizeof(uint32_t);
	constexpr size_t kCaughtSize = kCaughtHead + kMaxCaught * 8 * sizeof(float);

	// Metres a second of a wind of strength 1 (SharedMatrices.wind), what drifting particles are carried at.
	constexpr float kWindSpeed = 3.f;
	// One particle step's work group (particles.cs local_size_x).
	constexpr uint32_t kGroup = 64;

	// A particle's corners, two triangles round the middle.
	SFE::Render::BufferHandle makeQuad() {
		const std::array<SFE::Vertex2D, 6> quad{ {
			{ { -1.f, -1.f }, { 0.f, 0.f } }, { { 1.f, -1.f }, { 1.f, 0.f } }, { { 1.f, 1.f }, { 1.f, 1.f } },
			{ { -1.f, -1.f }, { 0.f, 0.f } }, { { 1.f, 1.f }, { 1.f, 1.f } }, { { -1.f, 1.f }, { 0.f, 1.f } },
		} };
		SFE::Render::BufferDesc desc;
		desc.usage = SFE::Render::BufferUsage::Vertex;
		desc.size = sizeof(quad);
		desc.cpuAccess = SFE::Render::CpuAccess::None;
		return SFE::Engine::instance()->rhi()->createBuffer(desc, quad.data());
	}

	// The pipeline drawing particles from makeQuad's corners, one instance a particle.
	SFE::Render::GraphicsPipelineDesc quadPipeline() {
		SFE::Render::GraphicsPipelineDesc desc;
		desc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(SFE::Vertex2D)), SFE::Render::VertexInputRate::PerVertex);
		desc.layout.attributes.emplace_back(0, 0, SFE::Render::VertexFormat::Float2, static_cast<uint32_t>(offsetof(SFE::Vertex2D, position)), false);
		desc.topology = SFE::Render::PrimitiveTopology::Triangles;
		desc.cullMode = SFE::Render::CullMode::None;
		desc.depthStencilState.depthTest = true;
		return desc;
	}
}

void ParticlePass::init(SystemsModule::RenderData&) {
	auto* rhi = Engine::instance()->rhi();
	const auto slots = (std::max)(1u, rhi->framesInFlight());

	DescriptorSetLayoutDesc stepLayout;
	stepLayout.bindings.emplace_back(1, DescriptorType::StorageBuffer, ShaderStage::Compute);
	stepLayout.bindings.emplace_back(2, DescriptorType::UniformBufferDynamic, ShaderStage::Compute);
	stepLayout.bindings.emplace_back(3, DescriptorType::StorageBuffer, ShaderStage::Compute);
	stepLayout.bindings.emplace_back(4, DescriptorType::UniformBufferDynamic, ShaderStage::Compute);
	stepLayout.bindings.emplace_back(5, DescriptorType::UniformBuffer, ShaderStage::Compute);
	stepLayout.bindings.emplace_back(6, DescriptorType::CombinedImageSampler, ShaderStage::Compute);
	stepLayout.bindings.emplace_back(7, DescriptorType::CombinedImageSampler, ShaderStage::Compute);
	stepLayout.bindings.emplace_back(8, DescriptorType::StorageBuffer, ShaderStage::Compute);
	mStepLayout = rhi->createDescriptorSetLayout(stepLayout);

	DescriptorSetLayoutDesc chipLayout;
	chipLayout.bindings.emplace_back(1, DescriptorType::StorageBuffer, ShaderStage::Vertex);
	chipLayout.bindings.emplace_back(5, DescriptorType::UniformBuffer, ShaderStage::Vertex);
	mChipLayout = rhi->createDescriptorSetLayout(chipLayout);

	for (uint32_t i = 0; i < slots; ++i) {
		mStepSets.push_back(rhi->allocateDescriptorSet(mStepLayout));
		mChipSets.push_back(rhi->allocateDescriptorSet(mChipLayout));
		BufferDesc caught;
		caught.size = kCaughtSize;
		caught.usage = BufferUsage::Storage;
		caught.cpuAccess = CpuAccess::Read;
		caught.debugName = "particles caught";
		mCaught.push_back(rhi->createBuffer(caught));
		mCaughtRead.push_back(0);
	}
	BufferDesc shapes;
	shapes.size = sizeof(uint32_t) * kShapeWords * kCatchers;
	shapes.usage = BufferUsage::Storage;
	shapes.cpuAccess = CpuAccess::None;
	shapes.debugName = "particle catchers' shapes";
	mShapes = rhi->createBuffer(shapes);

	ComputePipelineDesc step;
	step.compute = rhi->loadShader(ShaderStage::Compute, "shaders/particles.cs");
	step.descriptorSetLayouts = { mStepLayout };
	step.pushConstantSize = sizeof(StepPush);
	mStep = rhi->createComputePipeline(step);

	mQuad = makeQuad();
	mGBufferPass = rhi->createRenderPass(gBufferPassDesc(true));
	auto chips = quadPipeline();
	chips.depthStencilState.depthWrite = true;
	chips.depthStencilState.depthCompareOp = CompareOp::Less;
	chips.blend.enabled = false;
	chips.renderPass = mGBufferPass;
	chips.descriptorSetLayouts = { mChipLayout };
	chips.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/particle_chip.vs");
	chips.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/particle_chip.fs");
	mChips = rhi->createGraphicsPipeline(chips);
}

void ParticlePass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;
	const double now = Engine::clockSeconds();
	const float dt = mLast < 0.0 ? 0.f : static_cast<float>(std::clamp(now - mLast, 0.0, 0.1));
	mLast = now;
	const auto spawned = Particles::take(now);
	auto* rhi = Engine::instance()->rhi();
	const auto slot = rhi->frameInFlight() % mStepSets.size();
	// What this slot's last step caught, done with now that the slot comes round again: told on.
	if (const auto* head = static_cast<const uint32_t*>(rhi->mapBuffer(mCaught[slot], MapMode::Read))) {
		const uint32_t count = head[0];
		const uint32_t fresh = (std::min)(count - mCaughtRead[slot], kMaxCaught);
		if (fresh > 0) {
			const auto* entries = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(head) + kCaughtHead);
			std::vector<Particles::Caught> caught;
			caught.reserve(fresh);
			for (uint32_t k = count - fresh; k != count; ++k) {
				const float* entry = entries + 8 * (k % kMaxCaught);
				caught.push_back({ static_cast<uint32_t>(entry[4]), { entry[0], entry[1], entry[2] }, entry[3] });
			}
			Particles::tell(caught);
		}
		mCaughtRead[slot] = count;
		rhi->unmapBuffer(mCaught[slot]);
	}
	auto& frame = renderDataHandle.frame;
	const auto depth = frame.texture(FrameId::GBufferDepth);
	const auto normal = frame.texture(FrameId::GBufferNormal);
	const auto gBuffer = frame.framebuffer(FrameId::GBufferFbo);
	if (!mStep || !mChips || !Particles::alive(now) || !depth || !normal || !gBuffer) {
		return;
	}
	auto& particles = Particles::buffer();
	if (!particles) {
		const std::vector<GpuParticle> none(kCapacity);
		BufferDesc desc;
		desc.size = sizeof(GpuParticle) * kCapacity;
		desc.usage = BufferUsage::Storage;
		desc.cpuAccess = CpuAccess::None;
		desc.debugName = "particles";
		particles = rhi->createBuffer(desc, none.data());
	}
	const auto block = rhi->allocTransient(sizeof(GpuParticle) * kMaxSpawn);
	if (!block) {
		return;
	}
	const auto count = static_cast<uint32_t>(spawned.size());
	std::memcpy(block.data, spawned.data(), sizeof(GpuParticle) * count);
	// The catchers where they are now, a shape given anew put in.
	std::array<GpuCatcher, kCatchers> catchers;
	std::array<uint64_t, kCatchers> versions{};
	const uint32_t catching = Particles::catchers(catchers, versions);
	for (uint32_t id = 0; id < kCatchers; ++id) {
		if (versions[id] != mShapeVersions[id]) {
			mShapeVersions[id] = versions[id];
			auto filled = Particles::filledOf(id);
			filled.resize(kShapeWords, 0u);
			rhi->updateBuffer(mShapes, sizeof(uint32_t) * kShapeWords * id, sizeof(uint32_t) * kShapeWords, filled.data());
		}
	}
	const auto placed = rhi->allocTransient(sizeof(catchers));
	if (!placed) {
		return;
	}
	std::memcpy(placed.data, catchers.data(), sizeof(catchers));
	const StepPush push{ dt, mHead, count, kWindSpeed, catching };
	mHead = (mHead + count) % kCapacity;

	const auto stepSet = mStepSets[slot];
	const auto chipSet = mChipSets[slot];
	rhi->updateDescriptorSets({
		{ .set = stepSet, .binding = 1, .buffers{ { particles } } },
		{ .set = stepSet, .binding = 2, .buffers{ { block.buffer, 0, sizeof(GpuParticle) * kMaxSpawn } } },
		{ .set = stepSet, .binding = 3, .buffers{ { mShapes } } },
		{ .set = stepSet, .binding = 4, .buffers{ { placed.buffer, 0, sizeof(catchers) } } },
		{ .set = stepSet, .binding = 5, .buffers{ { renderDataHandle.cameraMatricesUBO } } },
		{ .set = stepSet, .binding = 6, .images{ { depth, frame.sampler(FrameId::GBufferDepth) } } },
		{ .set = stepSet, .binding = 7, .images{ { normal, frame.sampler(FrameId::GBufferNormal) } } },
		{ .set = stepSet, .binding = 8, .buffers{ { mCaught[slot] } } },
		{ .set = chipSet, .binding = 1, .buffers{ { particles } } },
		{ .set = chipSet, .binding = 5, .buffers{ { renderDataHandle.cameraMatricesUBO } } },
	});

	// What the G-buffer passes drew finished before it is read, and last frame's draws of the particles before they move.
	rhi->cmdPipelineBarrier(cmd, { PipelineStage::AllGraphics, PipelineStage::Compute,
		AccessMask::ColorAttachmentWrite | AccessMask::DepthStencilAttachmentWrite | AccessMask::ShaderRead, AccessMask::ShaderRead | AccessMask::ShaderWrite });
	rhi->cmdBindPipeline(cmd, mStep);
	const uint32_t offsets[] = { block.offset, placed.offset };
	rhi->cmdBindDescriptorSets(cmd, mStep, 0, &stepSet, 1, offsets, 2);
	rhi->cmdPushConstants(cmd, mStep, ShaderStage::Compute, &push, sizeof(push));
	rhi->cmdDispatch(cmd, (kCapacity + kGroup - 1) / kGroup);
	rhi->cmdPipelineBarrier(cmd, { PipelineStage::Compute, PipelineStage::VertexShader, AccessMask::ShaderWrite, AccessMask::ShaderRead });
	// And what was caught, for its slot's next turn to read.
	rhi->cmdPipelineBarrier(cmd, { PipelineStage::Compute, PipelineStage::Host, AccessMask::ShaderWrite, AccessMask::HostRead });

	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	RenderPassBeginInfo begin;
	begin.renderPass = mGBufferPass;
	begin.framebuffer = gBuffer;
	begin.width = screen.renderW;
	begin.height = screen.renderH;
	rhi->cmdBeginRenderPass(cmd, begin);
	rhi->cmdBindPipeline(cmd, mChips);
	rhi->cmdBindVertexBuffer(cmd, 0, mQuad);
	rhi->cmdBindDescriptorSets(cmd, mChips, 0, &chipSet, 1);
	rhi->cmdDraw(cmd, 6, kCapacity);
	rhi->cmdEndRenderPass(cmd);
}

namespace {
	// Mirrors particle_sprite.vs's SpriteBlock.
	struct SpriteParams {
		SFE::Render::Atmosphere::SkyParams sky;
		float view[4]{};
	};
}

void ParticleSpritePass::init(SystemsModule::RenderData&) {
	auto* rhi = Engine::instance()->rhi();
	mRenderPass = createFinalDepthRenderPass();

	DescriptorSetLayoutDesc layout;
	layout.bindings.emplace_back(1, DescriptorType::StorageBuffer, ShaderStage::Vertex);
	layout.bindings.emplace_back(5, DescriptorType::UniformBuffer, ShaderStage::Vertex | ShaderStage::Fragment);
	layout.bindings.emplace_back(8, DescriptorType::CombinedImageSampler, ShaderStage::Vertex);
	layout.bindings.emplace_back(11, DescriptorType::UniformBufferDynamic, ShaderStage::Vertex);
	layout.bindings.emplace_back(15, DescriptorType::UniformBufferDynamic, ShaderStage::Vertex);
	layout.bindings.emplace_back(4, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	mLayout = rhi->createDescriptorSetLayout(layout);
	mSets.resize((std::max)(1u, rhi->framesInFlight()));
	for (auto& set : mSets) {
		set = rhi->allocateDescriptorSet(mLayout);
	}

	mQuad = makeQuad();
	auto desc = quadPipeline();
	desc.depthStencilState.depthWrite = false;
	desc.depthStencilState.depthCompareOp = CompareOp::LessEqual;
	// Premultiplied: a mote hides what is behind it as much as it covers; a spark only adds its light.
	desc.blend.enabled = true;
	desc.blend.srcColor = BlendFactor::One;
	desc.blend.dstColor = BlendFactor::OneMinusSrcAlpha;
	desc.renderPass = mRenderPass;
	desc.descriptorSetLayouts = { mLayout };
	desc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/particle_sprite.vs");
	desc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/particle_sprite.fs");
	mPipeline = rhi->createGraphicsPipeline(desc);
}

void ParticleSpritePass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;
	const auto particles = Particles::buffer();
	SpriteParams params;
	if (!mPipeline || !particles || !Particles::alive(Engine::clockSeconds()) || !Atmosphere::skyParams(renderDataHandle, params.sky)) {
		return;
	}
	auto& frame = renderDataHandle.frame;
	const auto* csm = frame.data<CsmFrame>(FrameId::Csm);
	const auto shadow = frame.texture(FrameId::ShadowCsm);
	const auto sceneDepth = frame.texture(FrameId::SceneDepth);
	const auto fbo = mTargets.get(renderDataHandle, mRenderPass);
	if (!fbo || !csm || !shadow || !sceneDepth) {
		return;
	}
	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	params.view[0] = static_cast<float>((std::max)(1, screen.renderW));
	params.view[1] = static_cast<float>((std::max)(1, screen.renderH));
	params.view[2] = static_cast<float>(csm->shadowCascades.size());
	params.view[3] = renderDataHandle.graphics.sunIntensity;

	auto* rhi = Engine::instance()->rhi();
	const auto matrices = Utils::allocMatrices(csm->lightSpaceMatrices, CsmFrame::kMaxCascades);
	const auto block = rhi->allocTransient(params);
	const auto set = mSets[rhi->frameInFlight() % mSets.size()];
	rhi->updateDescriptorSets({
		{ .set = set, .binding = 1, .buffers{ { particles } } },
		{ .set = set, .binding = 5, .buffers{ { renderDataHandle.cameraMatricesUBO } } },
		{ .set = set, .binding = 8, .images{ { shadow, frame.sampler(FrameId::ShadowCsm) } } },
		{ .set = set, .binding = 11, .buffers{ { matrices.buffer, 0, sizeof(Math::Mat4) * CsmFrame::kMaxCascades } } },
		{ .set = set, .binding = 15, .buffers{ { block.buffer, 0, sizeof(SpriteParams) } } },
		{ .set = set, .binding = 4, .images{ { sceneDepth, frame.sampler(FrameId::SceneDepth) } } },
	});

	RenderPassBeginInfo begin;
	begin.renderPass = mRenderPass;
	begin.framebuffer = fbo;
	begin.width = screen.renderW;
	begin.height = screen.renderH;
	rhi->cmdBeginRenderPass(cmd, begin);
	rhi->cmdBindPipeline(cmd, mPipeline);
	rhi->cmdBindVertexBuffer(cmd, 0, mQuad);
	const uint32_t offsets[] = { matrices.offset, block.offset };
	rhi->cmdBindDescriptorSets(cmd, mPipeline, 0, &set, 1, offsets, 2);
	rhi->cmdDraw(cmd, 6, kCapacity);
	rhi->cmdEndRenderPass(cmd);
}
