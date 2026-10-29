#include "renderModule/PassList.h"

#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "renderModule/Atmosphere.h"
#include "renderModule/GpuScene.h"
#include "renderModule/Particles.h"
#include "runtimeBridge/RuntimeBridge.h"
#include "systemsModule/systems/RenderSystem.h"

#include "renderModule/renderPasses/AnimationComputePass.h"
#include "renderModule/renderPasses/CascadedShadowPass.h"
#include "renderModule/renderPasses/ContactGIPass.h"
#include "renderModule/renderPasses/DebugPass.h"
#include "renderModule/renderPasses/DoFPass.h"
#include "renderModule/renderPasses/FinalPass.h"
#include "renderModule/renderPasses/GeometryPass.h"
#include "renderModule/renderPasses/GlassPass.h"
#include "renderModule/renderPasses/GUIPass.h"
#include "renderModule/renderPasses/ImGuiPass.h"
#include "renderModule/renderPasses/LightingPass.h"
#include "renderModule/renderPasses/MistPass.h"
#include "renderModule/renderPasses/ParticlePass.h"
#include "renderModule/renderPasses/PointLightPass.h"
#include "renderModule/renderPasses/SceneDepthPass.h"
#include "renderModule/renderPasses/SkyPass.h"
#include "renderModule/renderPasses/SSAOPass.h"
#include "renderModule/renderPasses/SSRPass.h"
#include "renderModule/renderPasses/SSSPass.h"
#include "renderModule/renderPasses/SunShadowPass.h"
#include "renderModule/renderPasses/TAAPass.h"
#include "renderModule/renderPasses/TextOverlayPass.h"
#include "renderModule/renderPasses/VolumetricPass.h"
#include "renderModule/renderPasses/WaterPass.h"

namespace SFE::Render {
	namespace {
		using SystemsModule::GraphicsSettings;
		using SystemsModule::RenderData;

		// Made while its setting is on (the Graphics panel's "Passes"), let go of while it is off.
		PassEntry::On whileOn(bool GraphicsSettings::* setting) {
			return [setting](const RenderData& data) { return data.graphics.*setting; };
		}

		// While the scene has a sky (a SkyComponent), as the sky's passes ask it in render (Atmosphere::skyParams).
		bool underSky(const RenderData&) {
			return Atmosphere::sceneSky(false).has_value();
		}

		// While the scene's glass list has an instance: glass goes with the last of it, not with its group, which stays
		// until the list is built again.
		bool glassInScene(const RenderData&) {
			auto* renderSystem = ECSHandler::getSystem<SystemsModule::RenderSystem>();
			return renderSystem && renderSystem->scene().instances(GpuScene::List::Glass) > 0;
		}

		// From the first particle let go, for good: what is let go comes and goes with play (a step in the water, a wheel
		// in the mud), and a pass made and destroyed with each burst would be a hitch with each.
		bool particlesLetGo(const RenderData&) {
			return Particles::emitted() > 0;
		}

		bool dropsLetGo(const RenderData&) {
			return Particles::emitted(Particles::Look::Drop) > 0;
		}
	}

	PassList defaultPasses() {
		using namespace RenderPasses;
		const std::vector<std::string> liquid{ FrameId::LiquidHow, FrameId::LiquidWhere, FrameId::LiquidFacing };
		PassList list;
		list.append(PassEntry::of<AnimationComputePass>("AnimationCompute"));
		list.append(PassEntry::of<CascadedShadowPass>("CascadedShadow"));
		list.append(PassEntry::of<PointLightPass>("PointLight"));
		list.append(PassEntry::of<GeometryPass>("Geometry"));
		list.append(PassEntry::feature<ParticlePass>("Particles", particlesLetGo, { FrameId::ParticleBuffer }));
		list.append(PassEntry::feature<ParticleLiquidPass>("ParticleLiquid", dropsLetGo, liquid, { FrameId::ParticleBuffer }));
		list.append(PassEntry::of<SceneDepthPass>("SceneDepth"));
		list.append(PassEntry::feature<SSAOPass>("SSAO", whileOn(&GraphicsSettings::ssao), { FrameId::SsaoColor, FrameId::SsaoBlur }));
		list.append(PassEntry::of<SunShadowPass>("SunShadow"));
		list.append(PassEntry::of<LightingPass>("Lighting"));
		list.append(PassEntry::feature<SkyPass>("Sky", underSky));
		list.append(PassEntry::feature<SSSPass>("SSS", whileOn(&GraphicsSettings::sss)));
		list.append(PassEntry::feature<ContactGIPass>("ContactGI", whileOn(&GraphicsSettings::contactGi)));
		list.append(PassEntry::feature<SSRPass>("SSR", whileOn(&GraphicsSettings::ssr)));
		list.append(PassEntry::feature<WaterPass>("Water", waterUnderSky));
		list.append(PassEntry::feature<MistPass>("Mist", underSky));
		list.append(PassEntry::feature<GlassPass>("Glass", glassInScene));
		list.append(PassEntry::feature<VolumetricPass>("Volumetric", whileOn(&GraphicsSettings::volumetric)));
		list.append(PassEntry::feature<UnderwaterPass>("Underwater", waterUnderSky));
		list.append(PassEntry::feature<TAAPass>("TAA", whileOn(&GraphicsSettings::taa)));
		list.append(PassEntry::feature<ParticleClearLiquidPass>("ParticleClearLiquid", underSky, {}, liquid));
		list.append(PassEntry::feature<ParticleSpritePass>("ParticleSprites", underSky, {}, { FrameId::ParticleBuffer }));
		list.append(PassEntry::feature<DoFPass>("DoF", whileOn(&GraphicsSettings::dof)));
		list.append(PassEntry::of<FinalPass>("Final"));
		list.append(PassEntry::of<GUIPass>("GUI"));
		// Not on a frame that goes to an editor alone (published to it, nothing shown on the screen). The flag is the
		// bridge's to set, so with no bridge there is no such frame.
		list.append(PassEntry::of<DebugPass>("Debug", [](const RenderData&) {
			return !(RuntimeBridge::RuntimeBridge::isAlive() && RuntimeBridge::RuntimeBridge::instance()->publishEditorDisplay()
				&& !Engine::instance()->presentToScreen());
		}));
		// Kept as an anchor, never made.
		auto overlay = PassEntry::of<TextOverlayPass>("TextOverlay");
		overlay.enabled = false;
		list.append(std::move(overlay));
		// Only on a frame shown on the screen: the debug UI is no part of a picture that goes elsewhere.
		list.append(PassEntry::of<ImGuiPass>("ImGui", [](const RenderData&) { return Engine::instance()->presentToScreen(); }));
		return list;
	}
}
