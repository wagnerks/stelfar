#include "renderModule/PassList.h"

#include "core/Engine.h"
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

		// Run while its setting is on (the Graphics panel's "Passes"), off the frames it is off.
		PassEntry::Runs whileOn(bool GraphicsSettings::* setting) {
			return [setting](const RenderData& data) { return data.graphics.*setting; };
		}
	}

	PassList defaultPasses() {
		using namespace RenderPasses;
		PassList list;
		list.append(PassEntry::of<AnimationComputePass>("AnimationCompute"));
		list.append(PassEntry::of<CascadedShadowPass>("CascadedShadow"));
		list.append(PassEntry::of<PointLightPass>("PointLight"));
		list.append(PassEntry::of<GeometryPass>("Geometry"));
		list.append(PassEntry::of<ParticlePass>("Particles"));
		list.append(PassEntry::of<ParticleLiquidPass>("ParticleLiquid"));
		list.append(PassEntry::of<SceneDepthPass>("SceneDepth"));
		list.append(PassEntry::of<SSAOPass>("SSAO", whileOn(&GraphicsSettings::ssao)));
		list.append(PassEntry::of<SunShadowPass>("SunShadow"));
		list.append(PassEntry::of<LightingPass>("Lighting"));
		list.append(PassEntry::of<SkyPass>("Sky"));
		list.append(PassEntry::of<SSSPass>("SSS", whileOn(&GraphicsSettings::sss)));
		list.append(PassEntry::of<ContactGIPass>("ContactGI", whileOn(&GraphicsSettings::contactGi)));
		list.append(PassEntry::of<SSRPass>("SSR", whileOn(&GraphicsSettings::ssr)));
		list.append(PassEntry::of<WaterPass>("Water"));
		list.append(PassEntry::of<MistPass>("Mist"));
		list.append(PassEntry::of<GlassPass>("Glass"));
		list.append(PassEntry::of<VolumetricPass>("Volumetric", whileOn(&GraphicsSettings::volumetric)));
		list.append(PassEntry::of<UnderwaterPass>("Underwater"));
		list.append(PassEntry::of<TAAPass>("TAA", whileOn(&GraphicsSettings::taa)));
		list.append(PassEntry::of<ParticleClearLiquidPass>("ParticleClearLiquid"));
		list.append(PassEntry::of<ParticleSpritePass>("ParticleSprites"));
		list.append(PassEntry::of<DoFPass>("DoF", whileOn(&GraphicsSettings::dof)));
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
