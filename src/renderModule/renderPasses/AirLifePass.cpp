#include "AirLifePass.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "assetsModule/modelModule/Terrain.h"
#include "assetsModule/modelModule/Vertex.h"
#include "componentsModule/TransformComponent.h"
#include "componentsModule/WaterComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "debugModule/Benchmark.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/FrameBlackboard.h"
#include "renderModule/Utils.h"
#include "systemsModule/systems/RenderSystem.h"

using namespace SFE::Render::RenderPasses;

namespace {
	// Mirrors AirLifeBlock in air_life.vs. std140.
	struct AirLifeParams {
		SFE::Render::Atmosphere::SkyParams sky;
		float life[4]{};
		float view[4]{};
		float swarms[8][4]{};
	};

	constexpr uint32_t kMotes = 5000;
	constexpr float kMoteBox = 22.f;
	constexpr uint32_t kInsectsPerSwarm = 40;
	// Metres along a side of the cells a swarm may dance in, and how far from the camera they are looked for.
	constexpr float kSwarmCell = 17.f;
	constexpr float kSwarmReach = 30.f;

	float cellRandom(const int x, const int z, const uint32_t salt) {
		uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(z) * 19349663u ^ salt * 83492791u;
		h ^= h >> 13;
		h *= 0x5bd1e995u;
		h ^= h >> 15;
		return static_cast<float>(h & 0xffffff) / 16777216.f;
	}
}

void AirLifePass::init(SystemsModule::RenderData&) {
	auto* rhi = Engine::instance()->rhi();
	mRenderPass = createFinalDepthRenderPass();

	DescriptorSetLayoutDesc layoutDesc;
	layoutDesc.bindings.emplace_back(5, DescriptorType::UniformBuffer, ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(8, DescriptorType::CombinedImageSampler, ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(11, DescriptorType::UniformBufferDynamic, ShaderStage::Vertex);
	layoutDesc.bindings.emplace_back(15, DescriptorType::UniformBufferDynamic, ShaderStage::Vertex);
	mLayout = rhi->createDescriptorSetLayout(layoutDesc);
	mSets.resize((std::max)(1u, rhi->framesInFlight()));
	for (auto& set : mSets) {
		set = rhi->allocateDescriptorSet(mLayout);
	}

	const std::array<Vertex2D, 6> quad{ {
		{ { -1.f, -1.f }, { 0.f, 0.f } }, { { 1.f, -1.f }, { 1.f, 0.f } }, { { 1.f, 1.f }, { 1.f, 1.f } },
		{ { -1.f, -1.f }, { 0.f, 0.f } }, { { 1.f, 1.f }, { 1.f, 1.f } }, { { -1.f, 1.f }, { 0.f, 1.f } },
	} };
	BufferDesc bufferDesc;
	bufferDesc.usage = BufferUsage::Vertex;
	bufferDesc.size = sizeof(quad);
	bufferDesc.cpuAccess = CpuAccess::None;
	mQuad = rhi->createBuffer(bufferDesc, quad.data());

	GraphicsPipelineDesc desc;
	desc.layout.bindings.emplace_back(0, static_cast<uint32_t>(sizeof(Vertex2D)), VertexInputRate::PerVertex);
	desc.layout.attributes.emplace_back(0, 0, VertexFormat::Float2, static_cast<uint32_t>(offsetof(Vertex2D, position)), false);
	desc.topology = PrimitiveTopology::Triangles;
	desc.cullMode = CullMode::None;
	desc.depthStencilState.depthTest = true;
	desc.depthStencilState.depthWrite = false;
	desc.depthStencilState.depthCompareOp = CompareOp::LessEqual;
	// Premultiplied: the light a speck adds, over what it covers.
	desc.blend.enabled = true;
	desc.blend.srcColor = BlendFactor::One;
	desc.blend.dstColor = BlendFactor::OneMinusSrcAlpha;
	desc.renderPass = mRenderPass;
	desc.descriptorSetLayouts = { mLayout };
	desc.shaders.vertex = rhi->loadShader(ShaderStage::Vertex, "shaders/air_life.vs");
	desc.shaders.fragment = rhi->loadShader(ShaderStage::Fragment, "shaders/air_life.fs");
	mPipeline = rhi->createGraphicsPipeline(desc);
}

bool AirLifePass::findSwarms(const Math::Vec3& camera, const float sunHeight, float swarms[kSwarms][4]) {
	if (!mTerrainLooked) {
		mTerrainLooked = true;
		auto& registry = ECSHandler::registry();
		for (const auto& [id, terrain, transform] : registry.view<const ComponentsModule::TerrainComponent, const TransformComponent>()) {
			if (terrain && transform) {
				mTerrain = *terrain;
				mTerrainPos = transform->getPos(true);
				mHeights = &AssetsModule::terrainHeights(mTerrain);
				break;
			}
		}
		for (const auto& [id, water, transform] : registry.view<const ComponentsModule::WaterComponent, const TransformComponent>()) {
			if (water && transform) {
				mWaterLevel = transform->getPos(true).y;
				break;
			}
		}
	}
	if (!mHeights) {
		return false;
	}

	struct Swarm {
		float at[4];
		float distance2;
	};
	std::vector<Swarm> found;
	const float half = mTerrain.size * 0.5f;
	const int x0 = static_cast<int>(std::floor((camera.x - kSwarmReach) / kSwarmCell));
	const int x1 = static_cast<int>(std::floor((camera.x + kSwarmReach) / kSwarmCell));
	const int z0 = static_cast<int>(std::floor((camera.z - kSwarmReach) / kSwarmCell));
	const int z1 = static_cast<int>(std::floor((camera.z + kSwarmReach) / kSwarmCell));
	for (int cz = z0; cz <= z1; ++cz) {
		for (int cx = x0; cx <= x1; ++cx) {
			const float x = (static_cast<float>(cx) + 0.15f + 0.7f * cellRandom(cx, cz, 1)) * kSwarmCell;
			const float z = (static_cast<float>(cz) + 0.15f + 0.7f * cellRandom(cx, cz, 2)) * kSwarmCell;
			if (std::fabs(x - mTerrainPos.x) >= half || std::fabs(z - mTerrainPos.z) >= half) {
				continue;
			}
			const float ground = mTerrainPos.y + AssetsModule::terrainSurfaceHeight(mTerrain, *mHeights, x - mTerrainPos.x, z - mTerrainPos.z);
			const bool shore = ground > mWaterLevel - 0.6f && ground < mWaterLevel + 1.2f;
			const bool dry = ground >= mWaterLevel + 1.2f;
			const bool day = sunHeight > 0.12f;
			const float roll = cellRandom(cx, cz, 3);
			// Midges keep to the shallows and the shore at any hour. On dry ground it is butterflies and flies by day,
			// and fireflies once the sun is down.
			float kind = 0.f;
			if (shore && roll < 0.7f) {
				kind = 1.f;
			}
			else if (dry && day && roll < 0.38f) {
				kind = 3.f;
			}
			else if (dry && day && roll < 0.6f) {
				kind = 2.f;
			}
			else if (dry && sunHeight < 0.06f && roll < 0.55f) {
				kind = 4.f;
			}
			if (kind == 0.f) {
				continue;
			}
			const float base = (std::max)(ground, mWaterLevel);
			const float lift = cellRandom(cx, cz, 5);
			const float y = base + (kind == 1.f ? 0.7f + 1.1f * lift : kind == 4.f ? 0.4f + 0.5f * lift : 0.9f + 0.8f * lift);
			const float dx = x - camera.x;
			const float dz = z - camera.z;
			found.push_back({ { x, y, z, kind }, dx * dx + dz * dz });
		}
	}
	std::sort(found.begin(), found.end(), [](const Swarm& a, const Swarm& b) { return a.distance2 < b.distance2; });
	for (size_t i = 0; i < kSwarms && i < found.size(); ++i) {
		std::copy(std::begin(found[i].at), std::end(found[i].at), swarms[i]);
	}
	return true;
}

void AirLifePass::render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) {
	FUNCTION_BENCHMARK;
	const auto& graphics = renderDataHandle.graphics;
	AirLifeParams params;
	if (!mPipeline || !graphics.airLife || graphics.airLifeAmount <= 0.f || !Atmosphere::skyParams(renderDataHandle, params.sky)) {
		return;
	}
	const auto* csm = renderDataHandle.frame.data<CsmFrame>(FrameId::Csm);
	const auto shadow = renderDataHandle.frame.texture(FrameId::ShadowCsm);
	const auto fbo = mTargets.get(renderDataHandle, mRenderPass);
	if (!fbo || !csm || !shadow) {
		return;
	}

	// By the animation clock: the specks stand still while it is held, in the same places every run.
	const double seconds = std::fmod(Engine::instance()->animationClock(), 3600.0);
	params.life[1] = static_cast<float>(seconds);
	params.life[2] = kMoteBox;
	params.life[3] = graphics.airLifeAmount;
	const auto& screen = Engine::instance()->getWindow()->getScreenData();
	params.view[0] = static_cast<float>((std::max)(1, screen.renderW));
	params.view[1] = static_cast<float>((std::max)(1, screen.renderH));
	params.view[2] = static_cast<float>(csm->shadowCascades.size());
	params.view[3] = graphics.sunIntensity;
	const bool swarms = findSwarms(renderDataHandle.mCameraPos, params.sky.toSun[1], params.swarms);

	auto* rhi = Engine::instance()->rhi();
	const auto matrices = Utils::allocMatrices(csm->lightSpaceMatrices, CsmFrame::kMaxCascades);
	const auto set = mSets[rhi->frameInFlight() % mSets.size()];
	RenderPassBeginInfo begin;
	begin.renderPass = mRenderPass;
	begin.framebuffer = fbo;
	begin.width = screen.renderW;
	begin.height = screen.renderH;

	const auto motes = rhi->allocTransient(params);
	params.life[0] = 1.f;
	const auto insects = rhi->allocTransient(params);
	rhi->updateDescriptorSets({
		{ .set = set, .binding = 5, .buffers{ { renderDataHandle.cameraMatricesUBO } } },
		{ .set = set, .binding = 8, .images{ { shadow, renderDataHandle.frame.sampler(FrameId::ShadowCsm) } } },
		{ .set = set, .binding = 11, .buffers{ { matrices.buffer, 0, sizeof(Math::Mat4) * CsmFrame::kMaxCascades } } },
		{ .set = set, .binding = 15, .buffers{ { motes.buffer, 0, sizeof(AirLifeParams) } } },
	});

	rhi->cmdBeginRenderPass(cmd, begin);
	rhi->cmdBindPipeline(cmd, mPipeline);
	rhi->cmdBindVertexBuffer(cmd, 0, mQuad);
	{
		const uint32_t offsets[] = { matrices.offset, motes.offset };
		rhi->cmdBindDescriptorSets(cmd, mPipeline, 0, &set, 1, offsets, 2);
		rhi->cmdDraw(cmd, 6, kMotes);
	}
	if (swarms) {
		const uint32_t offsets[] = { matrices.offset, insects.offset };
		rhi->cmdBindDescriptorSets(cmd, mPipeline, 0, &set, 1, offsets, 2);
		rhi->cmdDraw(cmd, 6, kSwarms * kInsectsPerSwarm);
	}
	rhi->cmdEndRenderPass(cmd);
}
