#include "RenderSystem.h"
#include "renderModule/Utils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include "core/Clocks.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/EntityUser.h"
#include "renderModule/FrameCapture.h"
#include "renderModule/TerrainPatch.h"
#include "renderModule/UserFrameSet.h"

#include "CameraSystem.h"
#include "OcTreeSystem.h"
#include "systemsModule/SystemManager.h"
#include "renderModule/PassList.h"
#include "core/EngineConfig.h"
#include "componentsModule/ArmatureComponent.h"
#include "componentsModule/CameraComponent.h"
#include "componentsModule/IsDrawableComponent.h"
#include "componentsModule/LightSourceComponent.h"
#include "componentsModule/ModelComponent.h"
#include "componentsModule/OutlineComponent.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "core/FileSystem.h"
#include "core/Params.h"
#include "debugModule/Benchmark.h"
#include "debugModule/Profiler.h"
#include "ecss/Registry.h"
#include "logsModule/logger.h"
#include "renderModule/Utils.h"
#include "renderModule/renderPasses/CascadedShadowPass.h"
#include "renderModule/renderPasses/LightingPass.h"
#include "renderModule/renderPasses/SSAOPass.h"
#include "renderModule/renderPasses/DebugPass.h"
#include "renderModule/renderPasses/PointLightPass.h"
#include "propertiesModule/SceneEvents.h"
#include "runtimeBridge/RuntimeBridge.h"

#include "componentsModule/TransformComponent.h"
#include "renderModule/TextRenderer.h"
#include "renderModule/renderPasses/GUIPass.h"

#include "componentsModule/MaterialComponent.h"
#include "componentsModule/MeshComponent.h"
#include "componentsModule/ArmatureComponent.h"
#include "renderModule/renderPasses/GeometryPass.h"
#include "renderModule/renderPasses/AnimationComputePass.h"
#include "containersModule/Vector.h"
// Last: it brings <windows.h>, whose near/far macros no header after it may meet (GeometryPass.h's GridDrawData).
#include <debugModule/SystemLoadHelper.h>

namespace SFE::SystemsModule {

	namespace {
		// Writes one PNG per pass output plus a crc index, so a comparison script can find the
		// FIRST pass where two backends diverge instead of guessing from the final image.
		class PassDumpWriter {
		public:
			explicit PassDumpWriter(std::string dir) : mDir(std::move(dir)) {
				if (mDir.empty()) {
					return;
				}
				std::error_code ec;
				std::filesystem::create_directories(mDir, ec);
				mIndex.open(std::filesystem::path(mDir) / "passes.csv", std::ios::trunc);
				if (mIndex) {
					mIndex << "index,pass,slot,width,height,crc\n";
				}
			}

			// Stage 0: what every slot holds before any pass has run. Written, so a comparison always has a
			// baseline for a slot that one backend changes in a pass and the other leaves as it was.
			void seed(const Render::FrameBlackboard& frame) {
				if (mDir.empty()) {
					return;
				}
				dumpAll(0, "seed", frame);
			}

			void write(size_t passIndex, const char* passName, const Render::FrameBlackboard& frame) {
				if (mDir.empty()) {
					return;
				}
				dumpAll(passIndex + 1, passName, frame);
			}

		private:
			// Every texture on the blackboard, so each intermediate stage is compared, not a hand-picked
			// few. dump() skips a slot whose content has not changed, so a pass only reports what it wrote.
			void dumpAll(size_t passIndex, const char* passName, const Render::FrameBlackboard& frame) {
				for (const auto& name : frame.textureNames()) {
					const auto tex = frame.texture(name.c_str());
					if (name == Render::FrameId::ShadowCsmDepth) {
						// A depth array: the RHI cannot report its layer count, and the engine uses four cascades.
						for (uint32_t cascade = 0; cascade < 4; ++cascade) {
							dump(passIndex, passName, name + ".cascade" + std::to_string(cascade), tex, cascade);
						}
						continue;
					}
					dump(passIndex, passName, name, tex);
				}
			}

			void dump(size_t passIndex, const char* passName, const std::string& slot, Render::TextureHandle tex, uint32_t layer = 0) {
				auto* rhi = Engine::instance()->rhi();
				if (!tex || !rhi) {
					return;
				}
				std::vector<uint8_t> rgba;
				int width = 0;
				int height = 0;
				if (!rhi->readbackTexture(tex, rgba, width, height, layer)) {
					return;
				}

				// A slot a pass did not change is not written again; the comparison carries the last dump forward.
				const uint32_t crc = Render::crcOfPixels(rgba);
				auto& last = mLastCrc[slot];
				if (last == crc) {
					return;
				}
				last = crc;

				char name[160];
				std::snprintf(name, sizeof(name), "%02d_%s_%s.png", static_cast<int>(passIndex), passName, slot.c_str());
				const auto path = (std::filesystem::path(mDir) / name).string();
				if (Render::writePng(path, rgba, width, height) && mIndex) {
					mIndex << passIndex << ',' << passName << ',' << slot << ','
						<< width << ',' << height << ',' << crc << '\n';
					mIndex.flush();
				}
			}

			std::string mDir;
			std::ofstream mIndex;
			std::unordered_map<std::string, uint32_t> mLastCrc;
		};

		// The graphics settings as numbers to tune (Params), bound to the ones this system draws with: those tuned before
		// under the keys they had, the rest in "Graphics" under their label in the Graphics panel, with its range for the
		// drag.
		void registerGraphics(GraphicsSettings& gfx) {
			constexpr const char* kGraphics = "Graphics";
			Params::bind(kGraphics, "ssao", gfx.ssao, { .help = "The ambient occlusion's pass (SSAOPass) on or off." });
			Params::bind(kGraphics, "ssr", gfx.ssr, { .help = "The screen-space reflections' pass (SSRPass) on or off." });
			Params::bind(kGraphics, "volumetric", gfx.volumetric, { .help = "The light in the air (VolumetricPass) on or off." });
			Params::bind(kGraphics, "taa", gfx.taa, { .help = "The temporal anti-aliasing (TAAPass) on or off; --no-taa holds "
				"it off for the run." });
			Params::bind(kGraphics, "dof", gfx.dof, { .help = "The depth of field (DoFPass) on or off." });
			Params::bind(kGraphics, "bloom", gfx.bloom, { .help = "The bloom (FinalPass) on or off." });
			Params::bind(kGraphics, "sheen", gfx.sheen, { .help = "The soft sheen on rough dielectrics on or off." });
			Params::bind(kGraphics, "sss", gfx.sss, { .help = "The light spread under the skin (SSSPass) on or off." });
			Params::bind(kGraphics, "contact gi", gfx.contactGi, { .help = "The contact GI (ContactGIPass) on or off." });

			Params::bind(kGraphics, "ssr distance", gfx.ssrDistance, { .min = 2.f, .max = 80.f });
			Params::bind(kGraphics, "vol density", gfx.volumetricDensity, { .min = 0.f, .max = 0.08f });
			Params::bind(kGraphics, "vol intensity", gfx.volumetricIntensity, { .min = 0.f, .max = 8.f });
			Params::bind(kGraphics, "vol anisotropy", gfx.volumetricAnisotropy, { .min = -0.9f, .max = 0.9f });
			Params::bind(kGraphics, "vol distance", gfx.volumetricDistance, { .min = 10.f, .max = 200.f });
			Params::bind(kGraphics, "history blend", gfx.taaBlend, { .help = "TAA's history weight. Must stay high enough "
				"that the jitter pattern averages out over its 8 phases: at 0.72 the resolve settles in ~3.6 frames and the "
				"pattern shows as shimmer.", .min = 0.5f, .max = 0.99f });
			Params::bind(kGraphics, "dof focus", gfx.dofFocus, { .min = 1.f, .max = 80.f });
			Params::bind(kGraphics, "dof range", gfx.dofRange, { .min = 1.f, .max = 80.f });
			Params::bind(kGraphics, "dof blur", gfx.dofBlur, { .min = 0.f, .max = 16.f });

			// Its own name: the Graphics panel has the bloom's pass and its strength both under "bloom".
			Params::bind(kGraphics, "bloom strength", gfx.bloomStrength, { .min = 0.f, .max = 2.f });
			Params::bind(kGraphics, "exposure", gfx.exposure, { .min = 0.2f, .max = 2.5f });
			Params::bind(kGraphics, "ca", gfx.chromaticAberration, { .help = "Chromatic aberration.", .min = 0.f, .max = 0.012f });
			Params::bind(kGraphics, "sharpness", gfx.sharpness, { .help = "Contrast adaptive sharpening after the tonemap: 0 "
				"leaves the image as TAA resolved it, 1 is as far as the filter goes before it starts ringing.", .min = 0.f,
				.max = 1.f });
			Params::bind(kGraphics, "auto exposure", gfx.autoExposure, { .help = "The eye following the scene's brightness: "
				"exposure then only nudges it." });
			Params::bind(kGraphics, "adaptation speed", gfx.adaptationSpeed, { .help = "How fast the eye follows the scene's "
				"brightness, per second.", .min = 0.1f, .max = 8.f });
			Params::bind(kGraphics, "lens flare", gfx.flareStrength, { .min = 0.f, .max = 2.f });
			Params::bind(kGraphics, "warmth", gfx.warmth, { .help = "The grade: -1 cool .. 1 warm.", .min = -1.f, .max = 1.f });
			Params::bind(kGraphics, "saturation", gfx.saturation, { .help = "The grade's saturation, around 1.", .min = 0.f,
				.max = 2.f });
			Params::bind(kGraphics, "contrast", gfx.contrast, { .help = "The grade's contrast, around 1.", .min = 0.5f,
				.max = 1.5f });

			Params::bind(kGraphics, "fog start", gfx.fogStart, { .help = "The share of the far plane at which the fog begins.",
				.min = 0.1f, .max = 1.f });
			Params::bind(kGraphics, "view distance", gfx.viewDistance, { .help = "Metres the camera sees at most: its far "
				"plane. Past it the scene fades into the haze, and nothing further out is drawn at all.", .min = 100.f,
				.max = 5000.f });
			// Below 0 the scene's own: this run's only, or one drag and a save would hold the sun and the wind for good.
			Params::bind(kGraphics, "time of day", gfx.timeOfDay, { .help = "Hours, 0..24, placing the sun of an outdoor "
				"scene; below 0 the scene's own sun.", .min = 0.f, .max = 24.f, .transient = true });
			Params::bind(kGraphics, "wind", gfx.windStrength, { .help = "0 still air, 1 a strong breeze; below 0 the scene's "
				"own wind.", .min = 0.f, .max = 1.f, .transient = true });
			Params::bind(kGraphics, "glass roughness", gfx.glassRoughness, { .min = 0.04f, .max = 0.6f });

			// Skin: the sun scattered under it and through it, and the soft light and rim on whatever is rough.
			Params::bind("Skin", "sheen", gfx.sheenStrength, { .min = 0.f, .max = 3.f });
			Params::bind("Skin", "rim", gfx.rimStrength, { .min = 0.f, .max = 2.f });
			Params::bind("Skin", "scatter wrap", gfx.sssWrap, { .min = 0.f, .max = 3.f });
			Params::bind("Skin", "scatter", gfx.sssStrength, { .min = 0.f, .max = 2.5f });
			Params::bind("Skin", "scatter radius", gfx.sssRadius, { .help = "Metres a tap of the light spread under the skin "
				"(sss.fs), the red reaching three taps.", .min = 0.0005f, .max = 0.01f });
			Params::bind(kGraphics, "contact", gfx.contactStrength, { .min = 0.f, .max = 2.f });
			Params::bind(kGraphics, "contact dist", gfx.contactDistance, { .min = 0.2f, .max = 4.f });
			Params::bind(kGraphics, "contact thick", gfx.contactThickness, { .min = 0.01f, .max = 0.5f });
			Params::bind(kGraphics, "gi", gfx.giStrength, { .min = 0.f, .max = 2.f });
			Params::bind(kGraphics, "gi radius", gfx.giRadius, { .min = 0.1f, .max = 2.5f });

			Params::bind(kGraphics, "sun intensity", gfx.sunIntensity, { .min = 0.f, .max = 8.f });
			Params::bind("Light", "sky", gfx.iblStrength, { .help = "The sky's light against the sun's.", .min = 0.f,
				.max = 2.f });
			Params::bind("Light", "shadow fill", gfx.shadowAmbient, { .help = "How much of the sky's light is left in the "
				"sun's shade and on a side turned from the sun.", .min = 0.f, .max = 1.f });
			Params::bind("AO", "radius", gfx.ssaoRadius, { .help = "Metres round a point the ambient occlusion (ssao.fs, "
				"horizon based) looks for what covers it." });
			Params::bind("AO", "thickness", gfx.ssaoThickness, { .help = "Metres thick the ambient occlusion takes what "
				"stands in front to be." });
			Params::bind("AO", "intensity", gfx.ssaoIntensity, { .help = "The power the ambient occlusion's visibility is "
				"raised to." });
			Params::bind("AO", "sun share", gfx.ssaoSun, { .help = "The share of the ambient occlusion the sun's light takes "
				"as well (painted: a crease is dark in the sun too)." });
			Params::bind("AO", "foliage floor", gfx.ssaoFoliageFloor, { .help = "The least of the ambient occlusion foliage "
				"keeps (a leaf, a blade of grass)." });
			Params::bind("Shadows", "sun width", gfx.shadowSunWidth, { .help = "The sun's width, as the penumbra's metres a "
				"metre from the caster: the shadow is sharp where it leaves what casts it, wider the further it falls." });
			Params::bind("Shadows", "penumbra most", gfx.shadowPenumbraMost, { .help = "The widest penumbra of the sun's "
				"shadow, metres." });
			Params::bind("Shadows", "slope bias", gfx.shadowSlopeBias, { .help = "The casters' depth pushed off the sun by "
				"this many times their own depth's change across a texel (CascadedShadowPass), so a coarse mesh does not "
				"shadow itself in facets along the edge the light leaves." });
			Params::bind("Shadows", "slope bias most", gfx.shadowSlopeBiasMost, { .help = "The most the casters' depth is "
				"pushed off the sun by the slope bias, metres." });
			Params::bind("Shadows", "leaf light", gfx.shadowLeafLight, { .help = "The share of the sun a tree's or a plant's "
				"leaves let through to what lies under them (cascadeShadowMap.fs)." });
			Params::bind("Shadows", "refresh rate", gfx.shadowRefreshRate, { .help = "How many times a second each cascade "
				"of the sun's shadow is drawn anew at most, 0 every frame (CascadedShadowPass)." });

			Params::bind("Render", "lod pixels", gfx.lodPixels, { .help = "The pixels on screen a coarser level of a model "
				"may change from the full one for it to be drawn: smaller keeps the detail further.", .min = 0.25f,
				.max = 8.f });
			Params::bind("Terrain", "coarser past", gfx.terrainLodDistance, { .help = "Metres from the eye to a chunk of "
				"the ground past which it is drawn a level coarser (about half its triangles)." });
			Params::bind("Foliage", "depth prepass", gfx.foliagePrepass, { .help = "The foliage's cut-out leaves drawn for "
				"their depth alone before they are shaded, so each pixel of them is shaded once (GeometryPass): 1 on, 0 "
				"off." });

			Params::bind(kGraphics, "stylise", gfx.stylise, { .help = "How far the picture is pushed from what the light "
				"really does towards a painted one: 0 leaves it, 1 puts the sun's light in steps, takes the highlights off "
				"and flattens the colour.", .min = 0.f, .max = 1.f });
			Params::bind(kGraphics, "time of day grade", gfx.timeGrade, { .help = "How much the time of day grades the "
				"picture (warm low sun, blue night).", .min = 0.f, .max = 2.f });
			Params::bind(kGraphics, "sun rays", gfx.sunRays, { .help = "Shafts of light from the sun through the gaps in "
				"the clouds and the trees, gathered over the bloom pass.", .min = 0.f, .max = 3.f });
		}

		// The passes in draw order, for one start to be put beside another's: to the log, and to load_times.log beside the
		// executable, after the systems' order (SystemManager.cpp).
		void writePassOrder(const std::vector<std::string>& names) {
			std::string line = "passes, in order: ";
			for (size_t i = 0; i < names.size(); ++i) {
				line += (i == 0 ? "" : ", ") + names[i];
			}
			if (names.empty()) {
				line += "none";
			}
			LogsModule::Logger::LOG_INFO("%s", line.c_str());
			if (FILE* file = std::fopen((FileSystem::executableDir() / "load_times.log").string().c_str(), "a")) {
				std::fputs(line.c_str(), file);
				std::fputs("\n\n", file);
				std::fclose(file);
			}
		}
	}

	RenderSystem::RenderSystem(Render::PassList passes) : System({ SFE::SystemsModule::TaskType::TRAHSFORM_RELOADED , SFE::SystemsModule::TaskType::ARMATURE_UPDATED, ANIMATION_UPDATED, MATERIAL_UPDATED, MESH_UPDATED, ENTITY_DESTROYED }) {
		// Before anything reads them: some passes read theirs as they are made, below.
		registerGraphics(mRenderData.graphics);
		auto* rhi = Engine::instance()->rhi();
		mGpuFrames = (std::max)(1u, rhi->framesInFlight());
		mCameraUbo.resize(mGpuFrames);
		mFrameSet.resize(mGpuFrames);
		constexpr size_t maxDrawSize = 4096;
		mGpuCapacity = maxDrawSize;

		{
			Render::DescriptorSetLayoutDesc frameLayout;
			frameLayout.bindings = {
				{ 5, Render::DescriptorType::UniformBuffer, Render::ShaderStage::Vertex | Render::ShaderStage::Fragment | Render::ShaderStage::Geometry },
				{ 1, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex },
				{ 2, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex | Render::ShaderStage::Compute },
				{ 3, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex | Render::ShaderStage::Fragment },
				{ 8, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex },
				{ 9, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex | Render::ShaderStage::Compute },
				// Vertex too: the G-buffer vertex shader reads how a material sways in the wind.
				{ 7, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex | Render::ShaderStage::Fragment },
				// The heights under the ground drawn dense near the camera (TerrainPatch): its producer's, or a texel of 0.
				{ 35, Render::DescriptorType::CombinedImageSampler, Render::ShaderStage::Vertex },
				// Each entity's words (EntityUser): eight an entity, an application's for its shaders.
				{ 37, Render::DescriptorType::StorageBuffer, Render::ShaderStage::Vertex },
			};
			frameSetLayout = rhi->createDescriptorSetLayout(frameLayout);
			mRenderData.frameSetLayout = frameSetLayout;
		}
		// The channels an application hands the engine's shaders (its frame data, each entity's words, the ground drawn
		// dense), made before any pass is: a pass lays out the user frame set in its init.
		Render::UserFrameSet::instance().create(*rhi, mGpuFrames);
		Render::EntityUser::instance().create(*rhi);
		Render::createTerrainPatch(*rhi);

		createSceneBuffers(maxDrawSize);
		growBones(4096);
		mScene.init();
		for (uint32_t slot = 0; slot < mGpuFrames; ++slot) {
			createGpuFrameResources(slot);
		}
		bindGpuFrame();

		// What the application asked of its list and could not be had is said first, and loud: a pass left out or put
		// elsewhere draws another picture. Then the passes, made and initialised one by one in the list's order.
		for (const auto& error : passes.errors()) {
			LogsModule::Logger::LOG_FATAL(false, "RenderSystem: the pass list was not built as asked: %s", error.c_str());
		}
		mRenderPasses.reserve(passes.entries().size());
		for (const auto& entry : passes.entries()) {
			if (!entry.enabled) {
				continue;
			}
			auto pass = entry.make ? entry.make() : std::unique_ptr<Render::RenderPass>();
			if (!pass) {
				LogsModule::Logger::LOG_FATAL(false, "RenderSystem: the pass %s cannot be made", entry.name.c_str());
				continue;
			}
			placePass(entry, std::move(pass), mRenderPasses.size());
		}

		// What is in the world already -- a scene loaded before this ctor -- never reached us through ARMATURE_UPDATED
		// or ANIMATION_UPDATED, so its bones and clips are taken up here. Transforms are refreshed on the first frame,
		// and GpuScene starts dirty.
		auto& live = ECSHandler::registry();
		for (auto id : live.getAllEntities()) {
			if (live.hasComponent<ComponentsModule::ArmatureBonesComponent>(id)) {
				markGpuDirty(mGpuDirtyBones, id);
			}
			if (live.hasComponent<ComponentsModule::AnimationComponent>(id)) {
				markGpuDirty(mDirtyAnimations, id);
			}
		}
	}

	namespace {
		constexpr int kTaaJitterPhases = 8;

		// Radical inverse of `index` in `base`, in [0, 1).
		float halton(int index, int base) {
			float f = 1.f;
			float r = 0.f;
			while (index > 0) {
				f /= static_cast<float>(base);
				r += f * static_cast<float>(index % base);
				index /= base;
			}
			return r;
		}
	}

	void RenderSystem::update(float_t dt) {
		FUNCTION_BENCHMARK;

		auto* rhi = Engine::instance()->rhi();
		// Systems that ran earlier this frame may have queued asynchronous work (AABB, octree) that the
		// culling below reads; a still picture lets it finish so what is drawn does not depend on scheduling.
		if (Clocks::stillPicture()) {
			ThreadPool::instance()->waitIdle();
		}
		// The frame slot was waited for in Engine::update, before the frame's time was taken.
		bindGpuFrame();
		auto frameLock = tryLockWorldRead();

		// This frame's camera, read before it becomes current: taken after, it was the previous frame's camera drawing
		// this frame's objects, and anything the camera follows shook by however long the last frame took.
		{
			auto playerCamera = ECSHandler::getSystem<CameraSystem>()->getCurrentCamera();
			const auto cameraComp = ECSHandler::registry().pinComponent<const CameraComponent>(playerCamera);
			// The copy the transform worker publishes, not TransformComponent itself: the worker rewrites that
			// matrix while this thread reads it, and a moving camera now and then got a half-written view.
			// Replacing the copy waits for pins, so a pinned copy is whole.
			Math::Mat4 cameraTransform{ 1.f };
			if (const auto copy = ECSHandler::registry().pinComponent<const ComponentsModule::TransformMatComp>(playerCamera)) {
				cameraTransform = copy->mTransform;
			}
			else if (const auto transformComp = ECSHandler::registry().pinComponent<const TransformComponent>(playerCamera)) {
				cameraTransform = transformComp->copyTransform(); // before the first reload reached RenderSystem
			}

			mRenderData.nextCameraProjection = cameraComp->getProjection();
			const float viewDistance = (std::max)(mRenderData.graphics.viewDistance, mRenderData.nextCameraProjection.getNear() + 1.f);
			mRenderData.nextCameraProjection.setFar((std::min)(mRenderData.nextCameraProjection.getFar(), viewDistance));
			mRenderData.next.projection = mRenderData.nextCameraProjection.getProjectionsMatrix();
			mRenderData.next.view = inverse(cameraTransform);
			mRenderData.next.PV = mRenderData.next.projection * mRenderData.next.view;

			mRenderData.mNextCameraPos = cameraTransform[3].xyz;
			const Math::Vec3 forward = -cameraTransform[2];
			mRenderData.mNextViewDir = normalize(forward);
			mRenderData.mNextCamFrustum = FrustumModule::createFrustum(mRenderData.next.PV);
		}

		mRenderData.prevPV = mRenderData.current.PV;
		const auto windBefore = mRenderData.current.wind;
		mRenderData.current = mRenderData.next;
		{
			// Wrapped so the float keeps its precision in a long session; the sway jumps once an hour.
			const float seconds = static_cast<float>(std::fmod(Engine::instance()->animationClock(), 3600.0));
			const auto wind = Render::Atmosphere::sceneWind();
			const auto along = wind ? wind->blowing() : Math::Vec2{ 1.f, 0.f };
			mRenderData.current.wind = { along.x, wind ? wind->strength : 0.f, along.y, seconds };
			// Nothing moved from the last frame when the pictures start again: the clock was set, not run.
			const bool fresh = windBefore.w == 0.f || Clocks::restarting();
			mRenderData.current.windBefore = fresh ? mRenderData.current.wind : windBefore;
		}
		mRenderData.cameraProjection = mRenderData.nextCameraProjection;

		mRenderData.mCameraPos = mRenderData.mNextCameraPos;
		mRenderData.mViewDir = mRenderData.mNextViewDir;
		mRenderData.mCamFrustum = mRenderData.mNextCamFrustum;

		// Counted from the frame the pictures start again, so a still picture lands on the same jitter every run.
		mRenderData.taaFrame = Clocks::restarting() ? 0 : mRenderData.taaFrame + 1;
		mRenderData.taaJitterX = 0.f;
		mRenderData.taaJitterY = 0.f;

		// TAA resolves nothing without a sub-pixel offset per frame: every frame would sample the
		// same points and the history would already equal the current image. Only the matrices the
		// GPU rasterizes with are jittered -- mRenderData.current stays clean, so prevPV, culling
		// and the post-TAA debug overlay keep working in unjittered screen space.
		RenderMatrices gpuMatrices = mRenderData.current;
		// The motion vectors compare these two, so neither carries the jitter the rasterizer uses.
		gpuMatrices.PVNoJitter = mRenderData.current.PV;
		gpuMatrices.prevPVNoJitter = mRenderData.prevPV;
		if (mRenderData.graphics.taa) {
			const auto& screen = Engine::instance()->getWindow()->getScreenData();
			if (screen.renderW > 0 && screen.renderH > 0) {
				const int phase = mRenderData.taaFrame % kTaaJitterPhases + 1;
				mRenderData.taaJitterX = halton(phase, 2) - 0.5f;
				mRenderData.taaJitterY = halton(phase, 3) - 0.5f;

				// perspectiveRH_NO gives w = -z_view, so a term on the z column survives the
				// perspective divide as a constant NDC shift, unlike one on the translation column.
				gpuMatrices.projection[2][0] -= 2.f * mRenderData.taaJitterX / static_cast<float>(screen.renderW);
				gpuMatrices.projection[2][1] -= 2.f * mRenderData.taaJitterY / static_cast<float>(screen.renderH);
				gpuMatrices.PV = gpuMatrices.projection * gpuMatrices.view;
			}
		}

		gpuMatrices.invProjection = Math::inverse(gpuMatrices.projection);
		gpuMatrices.invView = Math::inverse(gpuMatrices.view);
		gpuMatrices.style = { mRenderData.graphics.stylise, 0.f, 0.f, 0.f };
		Engine::instance()->rhi()->updateBuffer(cameraMatricesUBO, 0, sizeof(RenderMatrices), &gpuMatrices);


		if (frameLock) {
			if (mRefreshTransforms) {
				// A reload that finished before we subscribed -- or while the constructor ran -- never reached
				// notify, so the copies are taken from the source once, now that every later reload does.
				mRefreshTransforms = false;
				auto& live = ECSHandler::registry();
				for (auto [id, matComp, transform] : live.view<ComponentsModule::TransformMatComp, const TransformComponent>()) {
					if (matComp && transform) {
						matComp->mTransform = transform->copyTransform();
						markGpuDirty(mGpuDirtyTransforms, id);
					}
				}
			}
			{
				SFE_PROFILE_ZONE("Render dirty upload");
				uploadDirtyGpuData();
			}
			{
				SFE_PROFILE_ZONE("Render scene");
				mScene.update(mMaterials);
			}
			SFE_PROFILE_ZONE("Render added");
			// An entity can join the scene after this frame's dirty transforms were taken, its id past the
			// buffer; the grow uploads every live transform, its own included.
			ensureGpuCapacity(mScene.maxEntity());
			// The dirty transforms were taken before the scene read its pending entities, and a transform the worker
			// published in between would reach the GPU a frame after its instances. Most of these went up already;
			// sending them again is cheap and closes that gap.
			uploadTransforms(mScene.added(), true);
			// Before the frame is recorded, so new materials go up with the frame's queued uploads; the passes'
			// own upload() calls then find nothing to send. Sent after the list began, Vulkan would submit and
			// wait for each one.
			SFE_PROFILE_ZONE("Render materials");
			mMaterials.upload();
		}

		renderFrame();
	}

	void RenderSystem::debugUpdate(float dt) {
	}

	void RenderSystem::bindGpuFrame() {
		if (mGpuFrames == 0) {
			return;
		}
		auto* rhi = Engine::instance()->rhi();
		const uint32_t slot = rhi->frameInFlight() % mGpuFrames;
		cameraMatricesUBO = mCameraUbo[slot];
		frameSet = mFrameSet[slot];
		mRenderData.transformsSSBO = transforms;
		mRenderData.bonesSSBO = bones;
		mRenderData.prevTransformsSSBO = prevTransforms;
		mRenderData.prevBonesSSBO = prevBones;
		mRenderData.cameraMatricesUBO = cameraMatricesUBO;
	}

	void RenderSystem::updateTerrainPatch() {
		auto* rhi = Engine::instance()->rhi();
		// The ground drawn dense near the camera (TerrainPatch): how deep the mesh under it goes, its grid and its square in
		// the camera block's last three lanes, its heights at binding 35.
		const auto& patch = Render::terrainPatch();
		const Math::Vec4 lanes[] = { { patch.deepest, 0.f, 0.f, 0.f }, patch.grid, patch.square };
		static_assert(offsetof(RenderMatrices, patchGrid) == offsetof(RenderMatrices, patchDeepest) + sizeof(Math::Vec4)
			&& offsetof(RenderMatrices, patchSquare) == offsetof(RenderMatrices, patchDeepest) + 2 * sizeof(Math::Vec4)
			&& sizeof(RenderMatrices) == 544, "the last three lanes of SFE_SHARED_MATRICES (sfe.glsl), 544 bytes in all");
		rhi->updateBuffer(cameraMatricesUBO, offsetof(RenderMatrices, patchDeepest), sizeof(lanes), lanes);
		// Rewritten only when the producer publishes other heights.
		const auto heights = Render::terrainPatchHeights();
		const std::pair ids{ heights.first.id, heights.second.id };
		const auto [written, fresh] = mFrameSetPatchHeights.try_emplace(frameSet.id, ids);
		if (fresh || written->second != ids) {
			written->second = ids;
			rhi->updateDescriptorSets({ { .set = frameSet, .binding = 35, .images{ { heights.first, heights.second } } } });
		}
	}

	void RenderSystem::updateEntityWords() {
		// Each entity's words (EntityUser): those written since the last frame go up, then binding 37 is the store's
		// buffer, written again when a grow has replaced it.
		auto& words = Render::EntityUser::instance();
		words.upload();
		const auto buffer = words.buffer();
		const auto [written, fresh] = mFrameSetWords.try_emplace(frameSet.id, buffer.id);
		if (fresh || written->second != buffer.id) {
			written->second = buffer.id;
			Engine::instance()->rhi()->updateDescriptorSets({ { .set = frameSet, .binding = 37, .buffers{ { buffer } } } });
		}
	}

	void RenderSystem::createGpuFrameResources(const uint32_t slot) {
		auto* rhi = Engine::instance()->rhi();
		Render::BufferDesc uboDesc;
		uboDesc.size = sizeof(RenderMatrices);
		uboDesc.usage = Render::BufferUsage::Uniform;
		uboDesc.cpuAccess = Render::CpuAccess::Write;
		uboDesc.debugName = "CameraMatricesUBO";
		mCameraUbo[slot] = rhi->createBuffer(uboDesc);

		mFrameSet[slot] = rhi->allocateDescriptorSet(frameSetLayout);
		rhi->updateDescriptorSets({
			{ mFrameSet[slot], 5, {{ mCameraUbo[slot] }} },
			{ mFrameSet[slot], 1, {{ transforms }} },
			{ mFrameSet[slot], 2, {{ bones }} },
			{ mFrameSet[slot], 8, {{ prevTransforms }} },
			{ mFrameSet[slot], 9, {{ prevBones }} },
		});
	}

	void RenderSystem::createSceneBuffers(const size_t capacity) {
		auto* rhi = Engine::instance()->rhi();
		// CpuAccess::None: device-local, written only through updateBuffer, which on Vulkan is copied
		// in the frame's own command list instead of racing the frames still in flight.
		Render::BufferDesc transformDesc;
		transformDesc.size = sizeof(Math::Mat4) * capacity;
		transformDesc.usage = Render::BufferUsage::Storage;
		transformDesc.cpuAccess = Render::CpuAccess::None;
		transformDesc.debugName = "TransformsSSBO";
		transforms = rhi->createBuffer(transformDesc);
		transformDesc.debugName = "PrevTransformsSSBO";
		prevTransforms = rhi->createBuffer(transformDesc);

		mGpuCapacity = capacity;
	}

	void RenderSystem::ensureGpuCapacity(ecss::EntityId id) {
		const size_t needed = static_cast<size_t>(id) + 1;
		if (needed <= mGpuCapacity) {
			return;
		}

		size_t newCap = mGpuCapacity < 4096 ? 4096 : mGpuCapacity;
		while (newCap < needed) {
			newCap *= 2;
		}

		auto* rhi = Engine::instance()->rhi();
		// The RHI keeps a destroyed buffer until the frames that may still bind it have finished.
		rhi->destroyBuffer(transforms);
		rhi->destroyBuffer(prevTransforms);
		createSceneBuffers(newCap);
		for (uint32_t slot = 0; slot < mGpuFrames; ++slot) {
			rhi->updateDescriptorSets({
				{ mFrameSet[slot], 1, {{ transforms }} },
				{ mFrameSet[slot], 8, {{ prevTransforms }} },
			});
		}
		bindGpuFrame();

		// The new buffers start empty: everything live goes up again.
		std::vector<ecss::EntityId> ids;
		for (auto [entId, comp] : ECSHandler::registry().view<const ComponentsModule::TransformMatComp>()) {
			ids.push_back(entId);
		}
		std::sort(ids.begin(), ids.end());
		uploadTransforms(ids);
	}

	RenderSystem::BoneRange RenderSystem::boneRange(const ecss::EntityId entity) {
		// An id destroyed and taken again before the release was processed: the range held is the old entity's,
		// and releasing it later would free it under the new one.
		bool destroyed = false;
		{
			std::lock_guard lock(mGpuDirtyMutex);
			if (const auto it = std::find(mDestroyedBones.begin(), mDestroyedBones.end(), entity); it != mDestroyedBones.end()) {
				mDestroyedBones.erase(it);
				destroyed = true;
			}
		}
		if (destroyed) {
			releaseBones(entity);
		}
		if (const auto it = mBoneRanges.find(entity); it != mBoneRanges.end()) {
			return it->second;
		}
		auto& registry = ECSHandler::registry();
		uint32_t count = 0;
		// Without the armature its meshes' bounds slots are unknown, so it gets them all.
		uint32_t bounds = AssetsModule::Armature::kMaxPoseBounds;
		if (const auto armature = registry.pinComponent<const ComponentsModule::ArmatureComponent>(entity)) {
			count = static_cast<uint32_t>(armature->armature.bones.size());
			bounds = armature->armature.poseBoundsCount;
		}
		if (count == 0) {
			if (const auto pose = registry.pinComponent<const ComponentsModule::ArmatureBonesComponent>(entity)) {
				count = static_cast<uint32_t>(pose->boneMatrices.size());
			}
		}
		if (count == 0) {
			return {};
		}

		// Free ranges count raw slots: the bounds and the bones together.
		const uint32_t slots = bounds + count;
		BoneRange range{ 0, count, bounds };
		const auto fit = std::find_if(mFreeBoneRanges.begin(), mFreeBoneRanges.end(), [slots](const BoneRange& r) { return r.count >= slots; });
		if (fit != mFreeBoneRanges.end()) {
			range.offset = fit->offset + bounds;
			fit->offset += slots;
			fit->count -= slots;
			if (fit->count == 0) {
				mFreeBoneRanges.erase(fit);
			}
		}
		else {
			range.offset = static_cast<uint32_t>(mBonesUsed) + bounds;
			mBonesUsed += slots;
			if (mBonesUsed > mBoneCapacity) {
				size_t capacity = mBoneCapacity;
				while (capacity < mBonesUsed) {
					capacity *= 2;
				}
				mBoneRanges.emplace(entity, range);
				growBones(capacity); // uploads every range, this one included
				return range;
			}
		}
		mBoneRanges.emplace(entity, range);
		// Its pose goes up with the next upload, until an animation writes it on the GPU.
		markGpuDirty(mGpuDirtyBones, entity);
		return range;
	}

	void RenderSystem::releaseBones(const ecss::EntityId entity) {
		mLastBones.erase(entity);
		const auto it = mBoneRanges.find(entity);
		if (it == mBoneRanges.end()) {
			return;
		}
		const BoneRange range{ it->second.offset - it->second.bounds, it->second.count + it->second.bounds };
		mBoneRanges.erase(it);
		auto next = std::lower_bound(mFreeBoneRanges.begin(), mFreeBoneRanges.end(), range.offset,
			[](const BoneRange& r, uint32_t offset) { return r.offset < offset; });
		next = mFreeBoneRanges.insert(next, range);
		if (next + 1 != mFreeBoneRanges.end() && next->offset + next->count == (next + 1)->offset) {
			next->count += (next + 1)->count;
			mFreeBoneRanges.erase(next + 1);
		}
		if (next != mFreeBoneRanges.begin() && (next - 1)->offset + (next - 1)->count == next->offset) {
			(next - 1)->count += next->count;
			mFreeBoneRanges.erase(next);
		}
	}

	void RenderSystem::growBones(const size_t capacity) {
		auto* rhi = Engine::instance()->rhi();
		// The RHI keeps a destroyed buffer until the frames that may still bind it have finished.
		rhi->destroyBuffer(bones);
		rhi->destroyBuffer(prevBones);
		Render::BufferDesc boneDesc;
		boneDesc.size = sizeof(Math::Mat4) * capacity;
		boneDesc.usage = Render::BufferUsage::Storage;
		boneDesc.cpuAccess = Render::CpuAccess::None;
		boneDesc.debugName = "BonesSSBO";
		bones = rhi->createBuffer(boneDesc);
		boneDesc.debugName = "PrevBonesSSBO";
		prevBones = rhi->createBuffer(boneDesc);
		mBoneCapacity = capacity;
		for (uint32_t slot = 0; slot < mGpuFrames; ++slot) {
			if (mFrameSet[slot]) {
				rhi->updateDescriptorSets({
					{ mFrameSet[slot], 2, {{ bones }} },
					{ mFrameSet[slot], 9, {{ prevBones }} },
				});
			}
		}
		bindGpuFrame();

		// Offsets stay where they were; the new buffer starts empty, so every pose goes up again.
		std::vector<ecss::EntityId> ids;
		ids.reserve(mBoneRanges.size());
		for (const auto& [entity, range] : mBoneRanges) {
			ids.push_back(entity);
		}
		uploadBones(ids);
	}

	void RenderSystem::markGpuDirty(std::vector<ecss::EntityId>& list, const ecss::EntityId id) {
		std::lock_guard lock(mGpuDirtyMutex);
		list.push_back(id);
	}

	void RenderSystem::uploadTransforms(const std::vector<ecss::EntityId>& sortedIds, const bool previousToo) {
		auto* rhi = Engine::instance()->rhi();
		auto& registry = ECSHandler::registry();
		// Consecutive ids go up as one range, so a spawned chunk is one copy rather than one per entity.
		std::vector<Math::Mat4> run;
		ecss::EntityId runStart = 0;
		const auto flush = [&] {
			if (!run.empty()) {
				rhi->updateBuffer(transforms, static_cast<size_t>(runStart) * sizeof(Math::Mat4), run.size() * sizeof(Math::Mat4), run.data());
				if (previousToo) {
					rhi->updateBuffer(prevTransforms, static_cast<size_t>(runStart) * sizeof(Math::Mat4), run.size() * sizeof(Math::Mat4), run.data());
				}
				run.clear();
			}
		};
		for (const auto id : sortedIds) {
			if (static_cast<size_t>(id) >= mGpuCapacity) {
				break;
			}
			const auto comp = registry.pinComponent<const ComponentsModule::TransformMatComp>(id);
			if (!comp) {
				flush();
				continue;
			}
			if (!run.empty() && id != runStart + run.size()) {
				flush();
			}
			if (run.empty()) {
				runStart = id;
			}
			run.push_back(comp->mTransform);
		}
		flush();
	}

	namespace {
		// The pose boxes of a skeleton posed by hand (`skins`, the bone matrices as the component has them), grown as
		// animateBones.cs grows a played one's: every posed joint by the farthest its bone's vertices lie from it, into the
		// boxes of the meshes it moves -- `slots`' first `bounds`, the last box first. Left empty, the cull would keep to
		// the bind pose, and a part posed far from it, a tool in the hands, would be culled where it is drawn.
		void growPoseBounds(ecss::EntityId entity, const std::vector<Math::Mat4>& skins, size_t count, uint32_t bounds, std::vector<Math::Mat4>& slots) {
			const auto skeleton = ECSHandler::registry().pinComponent<const ComponentsModule::ArmatureComponent>(entity);
			if (!skeleton || bounds == 0) {
				return;
			}
			const auto& rig = skeleton->armature;
			for (uint32_t j = 0; j < bounds; ++j) {
				slots[j][0] = Math::Vec4(Math::Vec3(3.0e38f), 1.f);
				slots[j][1] = Math::Vec4(Math::Vec3(-3.0e38f), 1.f);
			}
			for (const auto& bone : rig.bones) {
				if (bone.id >= count || bone.id >= rig.boneBoundsMask.size() || rig.boneBoundsMask[bone.id] == 0u) {
					continue;
				}
				const auto posed = skins[bone.id] * Math::inverse(bone.offset);
				const auto length = [&](int axis) { return Math::length(Math::Vec3(posed[axis])); };
				const Math::Vec3 reach(rig.boneRadius[bone.id] * (std::max)({ length(0), length(1), length(2) }));
				const Math::Vec3 at(posed[3]);
				for (uint32_t j = 0; j < bounds; ++j) {
					if ((rig.boneBoundsMask[bone.id] & (1u << j)) != 0u) {
						auto& box = slots[bounds - 1 - j];
						for (int k = 0; k < 3; ++k) {
							box[0][k] = (std::min)(box[0][k], at[k] - reach[k]);
							box[1][k] = (std::max)(box[1][k], at[k] + reach[k]);
						}
					}
				}
			}
		}
	}

	void RenderSystem::uploadBones(const std::vector<ecss::EntityId>& sortedIds) {
		auto* rhi = Engine::instance()->rhi();
		auto& registry = ECSHandler::registry();
		for (const auto id : sortedIds) {
			const auto armature = registry.pinComponent<const ComponentsModule::ArmatureBonesComponent>(id);
			if (!armature || armature->boneMatrices.empty()) {
				continue;
			}
			const auto range = boneRange(id);
			const size_t boneCount = (std::min)(armature->boneMatrices.size(), static_cast<size_t>(range.count));
			if (boneCount == 0) {
				continue;
			}
			// Empty bounds, so the cull falls back to the bind pose until an animation writes them. An animation,
			// playing or stopped, writes the bones straight into the buffer in AnimationComputePass, after this upload.
			const bool animated = [&] {
				const auto anim = registry.pinComponent<const ComponentsModule::AnimationComponent>(id);
				// Posed by hand, the clip is not what is drawn: the bones written into the component are.
				return anim && anim->mCurrentAnimation && !anim->mPosedByHand;
			}();
			std::vector<Math::Mat4> slots(range.bounds + (animated ? 0 : boneCount));
			for (uint32_t i = 0; i < range.bounds; ++i) {
				slots[i][0] = Math::Vec4(1.f);
				slots[i][1] = Math::Vec4(-1.f);
			}
			if (!animated) {
				std::copy_n(armature->boneMatrices.begin(), boneCount, slots.begin() + range.bounds);
				growPoseBounds(id, armature->boneMatrices, boneCount, range.bounds, slots);
			}
			if (!slots.empty()) {
				const auto offset = static_cast<size_t>(range.offset - range.bounds) * sizeof(Math::Mat4);
				rhi->updateBuffer(bones, offset, slots.size() * sizeof(Math::Mat4), slots.data());
				// What the bones were last frame, for the motion vectors. A pose written here and left alone does not
				// move, so last frame's is this one -- without that, the first frames of a static pose would have
				// motion vectors pointing at whatever the buffer held. A pose written afresh every frame does move,
				// though, and handing it its own bones as the last frame's tells the temporal pass the skin stood
				// still while it swept across the screen, which smears it. So that one is remembered.
				auto& was = mLastBones[id];
				const bool moving = !animated && was.size() == slots.size();
				rhi->updateBuffer(prevBones, offset, slots.size() * sizeof(Math::Mat4),
					moving ? was.data() : slots.data());
				if (animated) {
					mLastBones.erase(id);   // the device writes this one's bones; nothing to remember here
				}
				else {
					was = slots;
				}
			}
		}
	}

	void RenderSystem::uploadDirtyGpuData() {
		std::vector<ecss::EntityId> dirtyTransforms;
		std::vector<ecss::EntityId> dirtyBones;
		std::vector<ecss::EntityId> destroyed;
		{
			std::lock_guard lock(mGpuDirtyMutex);
			dirtyTransforms.swap(mGpuDirtyTransforms);
			dirtyBones.swap(mGpuDirtyBones);
			destroyed.swap(mDestroyedBones);
		}
		// Before the uploads: a reused id takes a range of its own when it is next asked for.
		for (const auto entity : destroyed) {
			releaseBones(entity);
		}
		if (dirtyTransforms.empty() && dirtyBones.empty()) {
			return;
		}
		for (auto* ids : { &dirtyTransforms, &dirtyBones }) {
			std::sort(ids->begin(), ids->end());
			ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
		}

		const size_t capacityBefore = mGpuCapacity;
		ecss::EntityId maxId = 0;
		if (!dirtyTransforms.empty()) {
			maxId = dirtyTransforms.back();
		}
		ensureGpuCapacity(maxId);
		if (mGpuCapacity == capacityBefore) {
			uploadTransforms(dirtyTransforms); // otherwise the grow already uploaded everything live
		}
		uploadBones(dirtyBones);
	}

	void RenderSystem::notify(Task task) {
		if (task.type == TRAHSFORM_RELOADED) {
			auto& registry = ECSHandler::registry();
			const bool first = !registry.hasComponent<ComponentsModule::TransformMatComp>(task.entity);
			registry.addComponent<ComponentsModule::TransformMatComp>(task.entity, static_cast<TransformComponent*>(task.customData)->copyTransform());
			markGpuDirty(mGpuDirtyTransforms, task.entity);
			if (first) {
				// The scene skips an entity with no transform yet, so one whose mesh came first is read again now.
				mScene.markEntity(task.entity);
			}
		}
		else if (task.type == ARMATURE_UPDATED) {
			markGpuDirty(mGpuDirtyBones, task.entity);
			markGpuDirty(mDirtyAnimations, task.entity);
			// Its instances carry the bone offset, which the armature decides.
			mScene.markEntity(task.entity);
		}
		else if (task.type == ANIMATION_UPDATED) {
			markGpuDirty(mDirtyAnimations, task.entity);
		}
		else if (task.type == MATERIAL_UPDATED) {
			mScene.markEntity(task.entity);
		}
		else if (task.type == MESH_UPDATED) {
			mScene.markEntity(task.entity);
		}
		else if (task.type == ENTITY_DESTROYED) {
			// Told before the id can be taken again: a new entity under it starts with no words.
			Render::EntityUser::instance().clear(task.entity);
			mScene.markEntity(task.entity);
			std::lock_guard lock(mGpuDirtyMutex);
			mDestroyedBones.push_back(task.entity);
			mDirtyAnimations.push_back(task.entity);
		}
	}

	std::vector<ecss::EntityId> RenderSystem::takeDirtyAnimations() {
		std::vector<ecss::EntityId> dirty;
		std::lock_guard lock(mGpuDirtyMutex);
		dirty.swap(mDirtyAnimations);
		return dirty;
	}

	void RenderSystem::placePass(const Render::PassEntry& entry, std::unique_ptr<Render::RenderPass> pass, const size_t index) {
		pass->init(mRenderData);
		const auto at = static_cast<std::ptrdiff_t>(index);
		mRenderPasses.insert(mRenderPasses.begin() + at, std::move(pass));
		mPassZones.insert(mPassZones.begin() + at, Debug::Profiler::intern(entry.name));
		mPassNames.insert(mPassNames.begin() + at, entry.name);
		mPassRuns.insert(mPassRuns.begin() + at, entry.runs);
	}

	bool RenderSystem::addRenderPass(Render::PassEntry entry, const std::string_view after) {
		const auto anchor = std::find(mPassNames.begin(), mPassNames.end(), after);
		const char* refused = after.empty() ? "no pass named to go after"
			: anchor == mPassNames.end() ? "no pass of that name to go after"
			: entry.name.empty() ? "an entry with no name"
			: std::find(mPassNames.begin(), mPassNames.end(), entry.name) != mPassNames.end() ? "a name already in the list"
			: !entry.enabled ? "a disabled entry"
			: !entry.make ? "no way to make it" : nullptr;
		auto pass = refused ? std::unique_ptr<Render::RenderPass>() : entry.make();
		if (!refused && !pass) {
			refused = "it cannot be made";
		}
		if (refused) {
			LogsModule::Logger::LOG_ERROR("RenderSystem: pass \"%s\" not added after \"%s\": %s", entry.name.c_str(),
				std::string(after).c_str(), refused);
			return false;
		}
		placePass(entry, std::move(pass), static_cast<size_t>(anchor - mPassNames.begin()) + 1);
		return true;
	}

	bool RenderSystem::removeRenderPass(const std::string_view name) {
		const auto it = std::find(mPassNames.begin(), mPassNames.end(), name);
		if (it == mPassNames.end()) {
			return false;
		}
		const auto index = it - mPassNames.begin();
		// Told of no scene once it is gone (SceneEvents.h).
		PropertiesModule::forgetSceneListenersOf(mRenderPasses[static_cast<size_t>(index)].get());
		mRenderPasses.erase(mRenderPasses.begin() + index);
		mPassZones.erase(mPassZones.begin() + index);
		mPassRuns.erase(mPassRuns.begin() + index);
		mPassNames.erase(it);
		return true;
	}

	void RenderSystem::resize() {
		Engine::instance()->rhi()->waitIdle();
		for (auto& pass : mRenderPasses) {
			pass->resize(mRenderData);
		}
	}

	bool RenderSystem::shouldRunPass(size_t passIndex) const
	{
		// The pass's own predicate (PassEntry::runs); one with none runs every frame.
		return passIndex >= mPassRuns.size() || !mPassRuns[passIndex] || mPassRuns[passIndex](mRenderData);
	}

	void RenderSystem::renderFrame()
	{
		using Clock = std::chrono::steady_clock;
		const auto ms = [](Clock::time_point from, Clock::time_point to) {
			return std::chrono::duration<float, std::milli>(to - from).count();
		};
		// A span of the frame for the profiler, from the same readings of the clock as the times kept here.
		const auto profile = [](const char* name, Clock::time_point from, Clock::time_point to) {
			Debug::Profiler::zone(name, from.time_since_epoch().count(), to.time_since_epoch().count());
		};

		auto* info = RenderDebugInfo::instance();
		info->objectsRendered = 0;
		info->verticesRendered = 0;
		info->drawCalls = 0;
		info->passCount = 0;

		const auto tFrame = Clock::now();

		auto rhi = Engine::instance()->rhi();
		info->takeGpuSpans(rhi->gpuSpans());
		profile("GPU spans", tFrame, Clock::now());

		// One diagnostic frame can snapshot every pass in turn. That needs each pass submitted on
		// its own so the GPU has actually produced the image before it is read back, which is far
		// too expensive for a normal frame -- hence the narrow condition.
		++mRenderedFrames;
		// Said once, as the first frame finds them, after the systems' order the system manager has just written.
		if (mRenderedFrames == 1) {
			writePassOrder(mPassNames);
		}
		const auto& capture = Engine::instance()->config().capture;
		const bool dumpPasses = capture.enabled
			&& !capture.passDumpDir.empty()
			&& mRenderedFrames == capture.frame;
		PassDumpWriter passDump(dumpPasses ? capture.passDumpDir : std::string{});
		passDump.seed(mRenderData.frame);

		// No frame-wide bind here: every pass binds its own sets against its pipeline, as Vulkan requires, and a bind
		// with no pipeline would be kept by OpenGL alone and hide a pass that forgot one.
		// Each pass's prepare a zone of its own, "Prepare <pass>": a long "Prepare passes" names the pass it was in.
		static std::unordered_map<const char*, const char*> prepareZones;
		for (size_t i = 0; i < mRenderPasses.size(); ++i) {
			if (shouldRunPass(i)) {
				const auto tPrepare = Clock::now();
				mRenderPasses[i]->prepare(mRenderData);
				auto& zone = prepareZones[mPassZones[i]];
				if (!zone) {
					zone = Debug::Profiler::intern(std::string("Prepare ") + mPassZones[i]);
				}
				profile(zone, tPrepare, Clock::now());
			}
		}
		// After prepare, before the frame's commands: what the producers published in theirs (the ground drawn dense, the
		// entities' words, the application's frame data) goes up and into the sets, device-local writes among it queued
		// in the frame.
		const auto tChannels = Clock::now();
		updateTerrainPatch();
		updateEntityWords();
		Render::UserFrameSet::instance().write();
		const auto tBegin = Clock::now();
		profile("Frame channels", tChannels, tBegin);
		info->prepareMs = ms(tFrame, tBegin);
		profile("Prepare passes", tFrame, tBegin);
		auto cmd = rhi->beginCommandList(Render::CommandListType::Graphics);
		const auto tBegun = Clock::now();
		info->waitMs = ms(tBegin, tBegun);
		profile(Debug::Profiler::kWaitGpu, tBegin, tBegun);
		rhi->cmdGpuMark(cmd, nullptr, false);
		for (size_t i = 0; i < mRenderPasses.size(); ++i) {
			if (!shouldRunPass(i)) {
				continue;
			}
			const auto tPass = Clock::now();
			rhi->cmdBeginRegion(cmd, mPassZones[i]);
			mRenderPasses[i]->render(mRenderData, cmd);
			rhi->cmdEndRegion(cmd);
			const auto tPassEnd = Clock::now();
			profile(mPassZones[i], tPass, tPassEnd);
			rhi->cmdGpuMark(cmd, mPassZones[i], true);
			if (info->passCount < static_cast<int>(std::size(info->passTimes))) {
				info->passTimes[info->passCount] = { mPassZones[i], ms(tPass, tPassEnd), info->gpuMsOf(mPassZones[i]) };
				++info->passCount;
			}

			if (dumpPasses) {
				rhi->endCommandList(cmd);
				rhi->submit(cmd);
				rhi->waitIdle();
				passDump.write(i, mPassNames[i].c_str(), mRenderData.frame);
				cmd = rhi->beginCommandList(Render::CommandListType::Graphics);
			}
		}
		// What this frame drew with becomes what the next one calls the previous frame. The whole buffer
		// rather than the entities that moved: an entity is uploaded when it changes, so one that stopped
		// moving -- or never moved since it spawned -- would keep whatever its previous slot held forever,
		// and for a static entity that was the identity matrix the buffer was allocated with. Its motion
		// vector then pointed from the world origin, TAA pulled that pixel's history from the other side of
		// the screen, and the models washed out into the floor behind them.
		if (transforms && prevTransforms && mGpuCapacity > 0) {
			rhi->cmdCopyBuffer(cmd, transforms, prevTransforms, 0, 0, mGpuCapacity * sizeof(Math::Mat4));
			rhi->cmdPipelineBarrier(cmd, {
				Render::PipelineStage::Transfer,
				Render::PipelineStage::VertexShader,
				Render::AccessMask::TransferWrite,
				Render::AccessMask::ShaderRead,
			});
		}
		const auto tEnd = Clock::now();
		rhi->endCommandList(cmd);

		const auto tSubmit = Clock::now();
		profile("End list", tEnd, tSubmit);
		rhi->submit(cmd);
		const auto tSubmitted = Clock::now();
		info->submitMs = ms(tSubmit, tSubmitted);
		profile("Submit", tSubmit, tSubmitted);
		info->gpuMs = static_cast<float>(rhi->lastGpuMs());
		{
			auto presentTex = mRenderData.frame.texture(Render::FrameId::Final);
			auto presentSampler = mRenderData.frame.sampler(Render::FrameId::Final);
			if (!presentTex) {
				presentTex = mRenderData.frame.texture(Render::FrameId::GBufferAlbedo);
				presentSampler = mRenderData.frame.sampler(Render::FrameId::GBufferAlbedo);
			}
			if (presentTex) {
				rhi->setPresentBlitSource(presentTex, presentSampler);
			}
		}
		// Asked of the bridge only if there is one: instance() would make it.
		if (RuntimeBridge::RuntimeBridge::isAlive() && RuntimeBridge::RuntimeBridge::instance()->publishEditorDisplay()) {
			SFE_PROFILE_ZONE("Stream frame");
			RuntimeBridge::RuntimeBridge::instance()->streamPresentedFrame();
		}

		const auto tPresent = Clock::now();
		rhi->present();
		const auto tPresented = Clock::now();
		info->presentMs = ms(tPresent, tPresented);
		profile(Debug::Profiler::kPresent, tPresent, tPresented);
		info->cpuMs = ms(tFrame, tPresented);
		// The whole frame, not just what this function took: the graph is about what the player feels.
		info->pushFrame(Engine::instance()->getDeltaTime() * 1000.f, info->gpuMs);
	}

	void RenderDebugInfo::takeGpuSpans(const std::vector<Render::RenderHardwareInterface::GpuSpan>& spans) {
		static constexpr const char* kOther = "other";
		gpuTimes.clear();
		size_t first = 0; // where the pass being read starts in gpuTimes
		float whole = 0.f;
		bool byShader = false;
		for (const auto& span : spans) {
			const auto ms = static_cast<float>(span.ms);
			whole += ms;
			// A pipeline's part of the pass, or the pass's rest; each once a pass, its spans summed.
			const char* part = span.endsPass || !span.label ? kOther : span.label;
			byShader |= !span.endsPass && span.label;
			auto it = std::find_if(gpuTimes.begin() + static_cast<std::ptrdiff_t>(first), gpuTimes.end(),
				[&](const GpuTime& time) { return time.shader == part; });
			if (it == gpuTimes.end()) {
				it = gpuTimes.insert(gpuTimes.end(), { nullptr, part, 0.f });
			}
			it->ms += ms;
			if (!span.endsPass) {
				continue;
			}
			// The pass ends here: its parts kept if it was marked by pipeline, and its whole after them.
			if (byShader) {
				for (size_t i = first; i < gpuTimes.size(); ++i) {
					gpuTimes[i].pass = span.label;
				}
			}
			else {
				gpuTimes.resize(first);
			}
			gpuTimes.push_back({ span.label, nullptr, whole });
			first = gpuTimes.size();
			whole = 0.f;
			byShader = false;
		}
		gpuTimes.resize(first);
	}

	float RenderDebugInfo::gpuMsOf(const char* pass) const {
		for (const auto& time : gpuTimes) {
			if (!time.shader && time.pass == pass) {
				return time.ms;
			}
		}
		return 0.f;
	}

	RenderSystem::~RenderSystem() {
		// What the channels made, while the RHI is still there; the device is idle (Engine::~Engine waits first). The
		// passes go after, with the members: one that published into them unpublishes, which only records.
		Render::UserFrameSet::instance().destroy();
		Render::EntityUser::instance().destroy();
		Render::destroyTerrainPatch();
		// The settings go with this system; the registry keeps their values until another binds them.
		Params::unbind(&mRenderData.graphics, &mRenderData.graphics + 1);
		// A pass is told of no scene once it is going (SceneEvents.h); the passes themselves go with the members.
		for (const auto& pass : mRenderPasses) {
			PropertiesModule::forgetSceneListenersOf(pass.get());
		}
	}
}
