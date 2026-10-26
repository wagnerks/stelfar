#include "FinalPass.h"

#include <algorithm>
#include <cstdio>

#include "assetsModule/shaderModule/ShaderBase.h"
#include "assetsModule/modelModule/Vertex.h"
#include "core/Clocks.h"
#include "core/Engine.h"
#include "debugModule/ProcessMemory.h"
#include "rhi/GraphicsBackend.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/TextRenderer.h"
#include "renderModule/Utils.h"
#include "componentsModule/ShelterComponent.h"
#include "componentsModule/TransformComponent.h"
#include "core/ECSHandler.h"
#include "systemsModule/systems/CameraSystem.h"
#include "systemsModule/systems/RenderSystem.h"
#include "debugModule/Profiler.h"

using namespace SFE::Render::RenderPasses;

namespace {
	// The frame a glare source was last seen in, which bloom_extract.fs writes, and the number of this one it is told
	// (PostParams::extra[3]), both counted modulo kGlareFrames so a float holds it exactly: the glare's star is gathered
	// only in the frames just after one with a source, as most have none and it costs most of that pass.
	constexpr uint32_t kGlareFrames = 1u << 20;
	SFE::Render::BufferHandle gGlareSeen{};
	uint32_t gFrame = 0;
}

SFE::Render::PipelineHandle FinalPass::makeFullscreen(SFE::Render::RenderHardwareInterface* rhi, RenderPassHandle renderPass, const char* vsPath, const char* fsPath) {
	GraphicsPipelineDesc pipelineDesc;
	pipelineDesc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(Vertex2D)), VertexInputRate::PerVertex);
	pipelineDesc.layout.attributes.emplace_back(0, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, position)), false);
	pipelineDesc.layout.attributes.emplace_back(1, 0, VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(Vertex2D, texCoords)), false);
	pipelineDesc.topology = PrimitiveTopology::Triangles;
	pipelineDesc.cullMode = CullMode::None;
	pipelineDesc.depthStencilState.depthTest = false;
	pipelineDesc.depthStencilState.depthWrite = false;
	pipelineDesc.renderPass = renderPass;
	pipelineDesc.descriptorSetLayouts = { mLayout };
	pipelineDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, vsPath);
	pipelineDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, fsPath);
	return rhi->createGraphicsPipeline(pipelineDesc);
}

void FinalPass::init(SystemsModule::RenderData& renderDataHandle) {
	auto* rhi = Engine::instance()->rhi();

	SamplerDesc samplerDesc;
	samplerDesc.minFilter = FilterMode::Linear;
	samplerDesc.magFilter = FilterMode::Linear;
	mSampler = rhi->createSampler(samplerDesc);

	DescriptorSetLayoutDesc layoutDesc;
	layoutDesc.bindings.emplace_back(0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(2, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(3, DescriptorType::CombinedImageSampler, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(15, DescriptorType::UniformBufferDynamic, ShaderStage::Fragment);
	layoutDesc.bindings.emplace_back(16, DescriptorType::StorageBuffer, ShaderStage::Fragment); // gGlareSeen
	mLayout = rhi->createDescriptorSetLayout(layoutDesc);
	if (!gGlareSeen) {
		BufferDesc glare;
		glare.size = sizeof(uint32_t);
		glare.usage = BufferUsage::Storage;
		glare.cpuAccess = CpuAccess::None;
		glare.debugName = "Glare seen";
		gGlareSeen = rhi->createBuffer(glare);
	}
	// One set per frame slot: what it binds alternates with the slot, and writing a set the previous
	// frame still binds costs a copy of it.
	for (uint32_t i = 0; i < (std::max)(1u, rhi->framesInFlight()); ++i) {
		mBloomSets.push_back(rhi->allocateDescriptorSet(mLayout));
		mPostSets.push_back(rhi->allocateDescriptorSet(mLayout));
		mToneSets.push_back(rhi->allocateDescriptorSet(mLayout));
	}

	RenderPassDesc bloomRp;
	bloomRp.colorAttachmentCount = 1;
	bloomRp.colorAttachments[0].loadOp = AttachmentLoadOp::Clear;
	bloomRp.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
	bloomRp.colorAttachments[0].format = TextureFormat::RGBA16F;
	mBloomPass = rhi->createRenderPass(bloomRp);

	RenderPassDesc displayRp;
	displayRp.colorAttachmentCount = 1;
	displayRp.colorAttachments[0].loadOp = AttachmentLoadOp::Clear;
	displayRp.colorAttachments[0].storeOp = AttachmentStoreOp::Store;
	displayRp.colorAttachments[0].format = TextureFormat::RGBA8;
	mDisplayPass = rhi->createRenderPass(displayRp);

	mBloomPipeline = makeFullscreen(rhi, mBloomPass, "shaders/deferred_shading.vs", "shaders/bloom_extract.fs");
	mPostPipeline = makeFullscreen(rhi, mDisplayPass, "shaders/deferred_shading.vs", "shaders/post.fs");
	mTonePipeline = makeFullscreen(rhi, mBloomPass, "shaders/deferred_shading.vs", "shaders/post_tonemap.fs");
	mLuminanceLogPipeline = makeFullscreen(rhi, mBloomPass, "shaders/deferred_shading.vs", "shaders/luminance_log.fs");
	mLuminanceDownPipeline = makeFullscreen(rhi, mBloomPass, "shaders/deferred_shading.vs", "shaders/luminance_down.fs");
	mAdaptPipeline = makeFullscreen(rhi, mBloomPass, "shaders/deferred_shading.vs", "shaders/luminance_adapt.fs");
	const auto target = [&](const uint32_t size) {
		TextureDesc desc;
		desc.width = size;
		desc.height = size;
		desc.format = TextureFormat::RGBA16F;
		desc.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled;
		Target t;
		t.size = size;
		t.texture = rhi->createTexture(desc);
		FramebufferDesc fb;
		fb.width = size;
		fb.height = size;
		fb.renderPass = mBloomPass;
		fb.colorAttachments = { { t.texture } };
		t.fbo = rhi->createFramebuffer(fb);
		return t;
	};
	for (const uint32_t size : { 64u, 16u, 4u, 1u }) {
		mLuminance.push_back(target(size));
	}
	mAdapted[0] = target(1);
	mAdapted[1] = target(1);
	mAdaptSets.resize((std::max)(1u, rhi->framesInFlight()));
	for (auto& sets : mAdaptSets) {
		for (size_t i = 0; i < mLuminance.size() + 1; ++i) {
			sets.push_back(rhi->allocateDescriptorSet(mLayout));
		}
	}

	resize(renderDataHandle);

	TextRenderer::instance()->bindRenderPass(mDisplayPass);
	mOverlayFont = FontsRegistry::instance()->getFont("fonts/DroidSans.ttf", 22);

	GraphicsPipelineDesc shapeDesc;
	shapeDesc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/overlay_shape.vs");
	shapeDesc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/overlay_shape.fs");
	shapeDesc.layout.bindings.push_back({ 0, static_cast<uint32_t>(sizeof(Math::Vec2)), VertexInputRate::PerVertex });
	shapeDesc.layout.attributes.push_back({ 0, 0, VertexFormat::Float2, 0, false });
	shapeDesc.topology = PrimitiveTopology::Triangles;
	shapeDesc.cullMode = CullMode::None;
	shapeDesc.depthStencilState.depthTest = false;
	shapeDesc.depthStencilState.depthWrite = false;
	shapeDesc.blend.enabled = true;
	shapeDesc.blend.srcColor = BlendFactor::SrcAlpha;
	shapeDesc.blend.dstColor = BlendFactor::OneMinusSrcAlpha;
	shapeDesc.renderPass = mDisplayPass;
	shapeDesc.pushConstantSize = static_cast<uint32_t>(sizeof(Math::Vec4));
	shapeDesc.pushConstantStages = ShaderStage::Fragment;
	mOverlayFillPipeline = rhi->createGraphicsPipeline(shapeDesc);
	shapeDesc.topology = PrimitiveTopology::Lines;
	mOverlayLinePipeline = rhi->createGraphicsPipeline(shapeDesc);
}

void FinalPass::resize(SystemsModule::RenderData&) {
	Utils::screenTarget(mBloomTex, mBloomFbo, mBloomPass, TextureFormat::RGBA16F, 4);
	// Half floats: the tonemapped colour is in 0..1, and 8 bits would round the neighbours the sharpening compares.
	Utils::screenTarget(mToneTex, mToneFbo, mBloomPass);
	Utils::screenTarget(mDisplayTex, mDisplayFbo, mDisplayPass, TextureFormat::RGBA8);
	// What the screen shows until the next frame is drawn.
	Engine::instance()->rhi()->setPresentBlitSource(mDisplayTex, mSampler);
}


namespace {
	// The overlay panel, in pixels from the top left.
	constexpr float kPanelX = 12.f;
	constexpr float kPanelY = 12.f;
	constexpr float kPanelWidth = 700.f;
	constexpr float kTextHeight = 156.f;
	constexpr float kGraphHeight = 44.f;
	constexpr float kGraphGap = 6.f;

	const SFE::Math::Vec4 kPanelColor{ 0.f, 0.f, 0.f, 0.45f };
	const SFE::Math::Vec4 kFrameColor{ 1.f, 0.92f, 0.2f, 1.f };
	const SFE::Math::Vec4 kGpuColor{ 0.45f, 0.9f, 0.55f, 1.f };
}

void FinalPass::addRect(const Rect& rect, std::vector<Math::Vec2>& out) const {
	const auto toClip = [this](float x, float y) {
		return Math::Vec2{ x / mScreenWidth * 2.f - 1.f, 1.f - y / mScreenHeight * 2.f };
	};
	const auto topLeft = toClip(rect.x, rect.y);
	const auto bottomRight = toClip(rect.x + rect.width, rect.y + rect.height);
	out.push_back(topLeft);
	out.push_back({ bottomRight.x, topLeft.y });
	out.push_back(bottomRight);
	out.push_back(topLeft);
	out.push_back(bottomRight);
	out.push_back({ topLeft.x, bottomRight.y });
}

void FinalPass::addGraph(const Rect& rect, const float* samples, const int count, const int cursor, const float scale,
	std::vector<Math::Vec2>& out) const {
	if (count < 2 || scale <= 0.f) {
		return;
	}
	const auto point = [&](int i) {
		// The cursor holds the oldest sample, so time runs left to right from there.
		const float value = samples[(cursor + i) % count];
		const float x = rect.x + rect.width * static_cast<float>(i) / static_cast<float>(count - 1);
		const float y = rect.y + rect.height * (1.f - (std::min)(value, scale) / scale);
		return Math::Vec2{ x / mScreenWidth * 2.f - 1.f, 1.f - y / mScreenHeight * 2.f };
	};
	auto previous = point(0);
	for (int i = 1; i < count; ++i) {
		const auto current = point(i);
		out.push_back(previous);
		out.push_back(current);
		previous = current;
	}
}

void FinalPass::drawShapes(const Render::CommandListHandle cmd, const PipelineHandle pipeline,
	const std::vector<Math::Vec2>& vertices, const Math::Vec4& color) {
	if (vertices.empty() || !pipeline) {
		return;
	}
	auto* rhi = Engine::instance()->rhi();
	const auto allocation = rhi->allocTransient(vertices.size() * sizeof(Math::Vec2));
	if (!allocation) {
		return;
	}
	std::memcpy(allocation.data, vertices.data(), vertices.size() * sizeof(Math::Vec2));
	rhi->cmdBindPipeline(cmd, pipeline);
	rhi->cmdBindVertexBuffer(cmd, 0, allocation.buffer, allocation.offset);
	rhi->cmdPushConstants(cmd, pipeline, ShaderStage::Fragment, &color, sizeof(color));
	rhi->cmdDraw(cmd, static_cast<uint32_t>(vertices.size()));
}

// Which backend and GPU the frame ran on, what it cost, and the same over the last few seconds.
void FinalPass::drawOverlay(const Render::CommandListHandle cmd, const int screenWidth, const int screenHeight) {
	auto* rhi = Engine::instance()->rhi();
	const auto* info = SystemsModule::RenderDebugInfo::instance();
	mScreenWidth = static_cast<float>(screenWidth);
	mScreenHeight = static_cast<float>(screenHeight);
	if (mScreenWidth <= 0.f || mScreenHeight <= 0.f) {
		return;
	}

	float worst = 0.f;
	float sum = 0.f;
	int samples = 0;
	for (const auto ms : info->frameMsHistory) {
		if (ms <= 0.f) {
			continue; // the ring has not filled yet
		}
		worst = (std::max)(worst, ms);
		sum += ms;
		++samples;
	}
	const float average = samples ? sum / static_cast<float>(samples) : 0.f;
	// One stall would flatten every other frame against the bottom, so the scale follows the average and lets
	// a spike past four times it run off the top; the text still says how bad the worst frame was.
	const float scale = (std::max)(8.f, (std::min)(worst, average * 4.f)) * 1.2f;

	const Rect frameGraph{ kPanelX + 8.f, kPanelY + kTextHeight, kPanelWidth - 16.f, kGraphHeight };
	const Rect gpuGraph{ frameGraph.x, frameGraph.y + kGraphHeight + kGraphGap, frameGraph.width, kGraphHeight };

	mOverlayVertices.clear();
	addRect({ kPanelX, kPanelY, kPanelWidth, gpuGraph.y + gpuGraph.height + 8.f - kPanelY }, mOverlayVertices);
	drawShapes(cmd, mOverlayFillPipeline, mOverlayVertices, kPanelColor);

	mOverlayVertices.clear();
	addGraph(frameGraph, info->frameMsHistory, SystemsModule::RenderDebugInfo::kHistory, info->historyCursor, scale, mOverlayVertices);
	drawShapes(cmd, mOverlayLinePipeline, mOverlayVertices, kFrameColor);

	mOverlayVertices.clear();
	addGraph(gpuGraph, info->gpuMsHistory, SystemsModule::RenderDebugInfo::kHistory, info->historyCursor, scale, mOverlayVertices);
	drawShapes(cmd, mOverlayLinePipeline, mOverlayVertices, kGpuColor);

	constexpr double kMegabyte = 1024.0 * 1024.0;
	const auto gpuMemory = rhi->gpuMemory();
	const auto processMemory = Debug::processMemory();
	// Where the camera is and which way it looks, in the form --camera and --camera-rot take.
	Math::Vec3 cameraPos{};
	Math::Vec3 cameraRot{};
	if (auto* cameras = ECSHandler::getSystem<SystemsModule::CameraSystem>()) {
		if (const auto transform = ECSHandler::registry().pinComponent<const TransformComponent>(cameras->getCurrentCamera())) {
			cameraPos = transform->getPos(true);
			cameraRot = transform->getRotate();
		}
	}
	// The GPU's work and the main thread's own (its waits left out), each with its share of the frame.
	const auto load = SFE::Debug::Profiler::load();
	char overlay[512];
	std::snprintf(
		overlay,
		sizeof(overlay),
		"%s | %s\nframe %.1f ms (%.0f fps)  worst %.1f\ngpu %.1f ms %.0f%%  cpu %.1f ms %.0f%%\nram %.0f MB (committed %.0f)\nvram %.0f MB: targets %.0f  depth %.0f  textures %.0f  meshes %.0f  buffers %.0f\ncamera %.1f,%.1f,%.1f  rot %.1f,%.1f,%.1f",
		graphicsBackendName(activeGraphicsBackend()),
		rhi->deviceName(),
		static_cast<double>(average),
		average > 0.f ? 1000.0 / static_cast<double>(average) : 0.0,
		static_cast<double>(worst),
		static_cast<double>(load.gpuMs),
		static_cast<double>(load.gpu) * 100.0,
		static_cast<double>(load.cpuMs),
		static_cast<double>(load.cpu) * 100.0,
		static_cast<double>(processMemory.inRam) / kMegabyte,
		static_cast<double>(processMemory.committed) / kMegabyte,
		static_cast<double>(gpuMemory.total()) / kMegabyte,
		static_cast<double>(gpuMemory.targets) / kMegabyte,
		static_cast<double>(gpuMemory.depth) / kMegabyte,
		static_cast<double>(gpuMemory.textures) / kMegabyte,
		static_cast<double>(gpuMemory.meshes) / kMegabyte,
		static_cast<double>(gpuMemory.buffers) / kMegabyte,
		static_cast<double>(cameraPos.x), static_cast<double>(cameraPos.y), static_cast<double>(cameraPos.z),
		static_cast<double>(cameraRot.x), static_cast<double>(cameraRot.y), static_cast<double>(cameraRot.z)
	);
	TextRenderer::instance()->renderText(cmd, overlay, kPanelX + 8.f, kPanelY + 22.f, 1.f, Math::Vec3{ 1.f, 0.92f, 0.2f }, mOverlayFont);
}

void FinalPass::adapt(SystemsModule::RenderData& renderDataHandle, const Render::CommandListHandle cmd, const TextureHandle src,
	const TransientAllocation& post) {
	auto* rhi = Engine::instance()->rhi();
	auto& sets = mAdaptSets[rhi->frameInFlight() % mAdaptSets.size()];
	const auto position = renderDataHandle.frame.texture(FrameId::SceneDepth);
	const auto run = [&](const PipelineHandle pipeline, const DescriptorSetHandle set, const Target& target, std::vector<WriteDescriptor> writes) {
		writes.push_back({ .set = set, .binding = 15, .buffers{ { post.buffer, 0, sizeof(PostParams) } } });
		rhi->updateDescriptorSets(writes);
		RenderPassBeginInfo begin;
		begin.renderPass = mBloomPass;
		begin.framebuffer = target.fbo;
		begin.width = target.size;
		begin.height = target.size;
		rhi->cmdBeginRenderPass(cmd, begin);
		rhi->cmdBindPipeline(cmd, pipeline);
		rhi->cmdBindDescriptorSets(cmd, pipeline, 0, &set, 1, &post.offset, 1);
		Utils::renderQuad(cmd);
		rhi->cmdEndRenderPass(cmd);
	};

	run(mLuminanceLogPipeline, sets[0], mLuminance[0], { { .set = sets[0], .binding = 0, .images{ { src, mSampler } } } });
	for (size_t i = 1; i < mLuminance.size(); ++i) {
		run(mLuminanceDownPipeline, sets[i], mLuminance[i], { { .set = sets[i], .binding = 0, .images{ { mLuminance[i - 1].texture, mSampler } } } });
	}
	const uint32_t next = 1 - mAdaptedCurrent;
	const auto& previous = mAdaptedValid ? mAdapted[mAdaptedCurrent].texture : mLuminance.back().texture;
	const auto adaptSet = sets[mLuminance.size()];
	run(mAdaptPipeline, adaptSet, mAdapted[next], {
		{ .set = adaptSet, .binding = 0, .images{ { mLuminance.back().texture, mSampler } } },
		{ .set = adaptSet, .binding = 1, .images{ { previous, mSampler } } },
		{ .set = adaptSet, .binding = 2, .images{ { position ? position : src, mSampler } } },
	});
	mAdaptedCurrent = next;
	mAdaptedValid = true;
}

void FinalPass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	const auto src = renderDataHandle.frame.texture(FrameId::Final);
		if (!src || !mPostPipeline) {
			return;
		}

	auto* rhi = Engine::instance()->rhi();
	const auto mBloomSet = mBloomSets[rhi->frameInFlight() % mBloomSets.size()];
	const auto mPostSet = mPostSets[rhi->frameInFlight() % mPostSets.size()];
	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	auto& gfx = renderDataHandle.graphics;

	mPostParams.bloomStrength = gfx.bloom ? gfx.bloomStrength : 0.f;
	mPostParams.exposure = gfx.exposure;
	mPostParams.ca = gfx.chromaticAberration;
	mPostParams.sharpness = gfx.sharpness;
	mPostParams.autoExposure = gfx.autoExposure ? 1.f : 0.f;
	mPostParams.flareStrength = gfx.flareStrength;
	mPostParams.warmth = gfx.warmth;
	mPostParams.saturation = gfx.saturation;
	mPostParams.contrast = gfx.contrast;
	mPostParams.aspect = static_cast<float>((std::max)(1, screen.renderW)) / static_cast<float>((std::max)(1, screen.renderH));
	mPostParams.adaptSpeed = gfx.adaptationSpeed;
	// By the frame's step, so the same frames adapt alike. A still picture adapts at once, so the same frame comes out the
	// same whatever ran before it.
	mPostParams.adaptSeconds = !mAdaptedValid || SFE::Clocks::stillPicture() ? 1e6f : (std::min)(Engine::instance()->getDeltaTime(), 1.f);
	// Where the sun is on screen, and how bright, for the flare.
	mPostParams.sunInFront = 0.f;
	mPostParams.sunColor[0] = mPostParams.sunColor[1] = mPostParams.sunColor[2] = 0.f;
	Atmosphere::SkyParams sky;
	const bool outdoor = Atmosphere::skyParams(renderDataHandle, sky);
	if (outdoor && sky.toSun[1] > -0.05f) {
		const Math::Vec3 far = renderDataHandle.mCameraPos + Math::Vec3{ sky.toSun[0], sky.toSun[1], sky.toSun[2] } * 1000.f;
		const auto clip = renderDataHandle.current.PV * Math::Vec4{ far.x, far.y, far.z, 1.f };
		if (clip.w > 0.f) {
			mPostParams.sunU = clip.x / clip.w * 0.5f + 0.5f;
			mPostParams.sunV = clip.y / clip.w * 0.5f + 0.5f;
			mPostParams.sunInFront = 1.f;
		}
		std::copy(std::begin(sky.sunColor), std::begin(sky.sunColor) + 3, mPostParams.sunColor);
	}
	mPostParams.sunHeight = outdoor ? sky.toSun[1] : 1.f;
	mPostParams.extra[0] = gfx.sunRays;
	mPostParams.extra[2] = gfx.timeGrade;
	// The eye under a roof makes up only part of the dark the roof keeps the sky's light out by.
	{
		float skyIn = 1.f;
		for (const auto& [id, shelter, transform] : ECSHandler::registry().view<const ShelterComponent, const TransformComponent>()) {
			if (shelter && transform) {
				const auto local = Math::inverseAffine(transform->getTransform()) * Math::Vec4{ renderDataHandle.mCameraPos, 1.f };
				skyIn = (std::min)(skyIn, 1.f + (shelter->sky - 1.f) * shelter->inside({ local.x, local.y, local.z }));
			}
		}
		mPostParams.extra[1] = std::pow((std::max)(skyIn, 1e-3f), 1.f - ShelterComponent::eyeAdapts);
	}
	// A debug term carries a value to be read off the image, not a picture to be graded. Exposure
	// silently rescales it, which turns any absolute reading into nonsense.
	if (Engine::instance()->lightingDebugTerm() != 0) {
		mPostParams.bloomStrength = 0.f;
		mPostParams.exposure = 1.f;
		mPostParams.ca = 0.f;
	}
	gFrame = SFE::Clocks::restarting() ? 0 : (gFrame + 1) % kGlareFrames;
	mPostParams.extra[3] = static_cast<float>(gFrame);
	const auto post = rhi->allocTransient(mPostParams);
	adapt(renderDataHandle, cmd, src, post);

	if (gfx.bloom && mBloomPipeline) {
		WriteDescriptor w{};
		w.set = mBloomSet;
		w.binding = 0;
		w.images.push_back({ src, mSampler });
		// The layout's dynamic binding 15 must hold a buffer, and Vulkan wants its offset at the bind.
		WriteDescriptor params{};
		params.set = mBloomSet;
		params.binding = 15;
		params.buffers.push_back({ post.buffer, 0, sizeof(PostParams) });
		WriteDescriptor glare{};
		glare.set = mBloomSet;
		glare.binding = 16;
		glare.buffers.push_back({ gGlareSeen, 0, sizeof(uint32_t) });
		rhi->updateDescriptorSets({ w, params, glare });

		RenderPassBeginInfo beginInfo;
		beginInfo.framebuffer = mBloomFbo;
		beginInfo.renderPass = mBloomPass;
		beginInfo.width = std::max(1, screen.renderW / 4);
		beginInfo.height = std::max(1, screen.renderH / 4);
		rhi->cmdBeginRenderPass(cmd, beginInfo);
		rhi->cmdBindPipeline(cmd, mBloomPipeline);
		rhi->cmdBindDescriptorSets(cmd, mBloomPipeline, 0, &mBloomSet, 1, &post.offset, 1);
		Utils::renderQuad(cmd);
		rhi->cmdEndRenderPass(cmd);
	}

	if (mTonePipeline) {
		const auto toneSet = mToneSets[rhi->frameInFlight() % mToneSets.size()];
		rhi->updateDescriptorSets({
			{ .set = toneSet, .binding = 0, .images{ { src, mSampler } } },
			{ .set = toneSet, .binding = 1, .images{ { mBloomTex, mSampler } } },
			{ .set = toneSet, .binding = 2, .images{ { mAdapted[mAdaptedCurrent].texture, mSampler } } },
			{ .set = toneSet, .binding = 15, .buffers{ { post.buffer, 0, sizeof(PostParams) } } },
		});
		RenderPassBeginInfo beginInfo;
		beginInfo.framebuffer = mToneFbo;
		beginInfo.renderPass = mBloomPass;
		beginInfo.width = screen.renderW;
		beginInfo.height = screen.renderH;
		rhi->cmdBeginRenderPass(cmd, beginInfo);
		rhi->cmdBindPipeline(cmd, mTonePipeline);
		rhi->cmdBindDescriptorSets(cmd, mTonePipeline, 0, &toneSet, 1, &post.offset, 1);
		Utils::renderQuad(cmd);
		rhi->cmdEndRenderPass(cmd);
	}

	{
		std::vector<WriteDescriptor> writes;
		{
			WriteDescriptor w{};
			w.set = mPostSet;
			w.binding = 0;
			w.images.push_back({ src, mSampler });
			writes.push_back(std::move(w));
		}
		{
			WriteDescriptor w{};
			w.set = mPostSet;
			w.binding = 3;
			w.images.push_back({ mToneTex, mSampler });
			writes.push_back(std::move(w));
		}
		{
			WriteDescriptor w{};
			w.set = mPostSet;
			w.binding = 1;
			w.images.push_back({ mBloomTex, mSampler });
			writes.push_back(std::move(w));
		}
		{
			WriteDescriptor w{};
			w.set = mPostSet;
			w.binding = 2;
			w.images.push_back({ mAdapted[mAdaptedCurrent].texture, mSampler });
			writes.push_back(std::move(w));
		}
		{
			WriteDescriptor w{};
			w.set = mPostSet;
			w.binding = 15;
			w.buffers.push_back({ post.buffer, 0, sizeof(PostParams) });
			writes.push_back(std::move(w));
		}
		rhi->updateDescriptorSets(writes);

		RenderPassBeginInfo beginInfo;
		beginInfo.framebuffer = mDisplayFbo;
		beginInfo.renderPass = mDisplayPass;
		beginInfo.width = screen.renderW;
		beginInfo.height = screen.renderH;
		rhi->cmdBeginRenderPass(cmd, beginInfo);
		rhi->cmdBindPipeline(cmd, mPostPipeline);
		rhi->cmdBindDescriptorSets(cmd, mPostPipeline, 0, &mPostSet, 1, &post.offset, 1);
		Utils::renderQuad(cmd);
		// Unless the player turned the frame's timings off (Window::display).
		const auto* display = Engine::instance()->getWindow()->display();
		if (mOverlayFont && !SFE::Clocks::stillPicture() && (!display || display->frameInfo)) {
			drawOverlay(cmd, screen.renderW, screen.renderH);
		}
		rhi->cmdEndRenderPass(cmd);
	}

	renderDataHandle.frame.set(FrameId::Final, { .texture = mDisplayTex, .sampler = mSampler, .framebuffer = mDisplayFbo });
	rhi->setPresentBlitSource(mDisplayTex, mSampler);
}
