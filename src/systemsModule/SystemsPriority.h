#pragma once

#include "renderModule/renderPasses/AirLifePass.h"
#include "renderModule/renderPasses/AnimationComputePass.h"
#include "renderModule/renderPasses/CascadedShadowPass.h"
#include "renderModule/renderPasses/DebugPass.h"
#include "renderModule/renderPasses/FinalPass.h"
#include "renderModule/renderPasses/GeometryPass.h"
#include "renderModule/renderPasses/GUIPass.h"
#include "renderModule/renderPasses/LightingPass.h"
#include "renderModule/renderPasses/ParticlePass.h"
#include "renderModule/renderPasses/SceneDepthPass.h"
#include "renderModule/renderPasses/PointLightPass.h"
#include "renderModule/renderPasses/RenderPass.h"
#include "renderModule/renderPasses/SSAOPass.h"
#include "renderModule/renderPasses/SSRPass.h"
#include "renderModule/renderPasses/SunShadowPass.h"
#include "renderModule/renderPasses/SkyPass.h"
#include "renderModule/renderPasses/WaterPass.h"
#include "renderModule/renderPasses/GlassPass.h"
#include "renderModule/renderPasses/MistPass.h"
#include "renderModule/renderPasses/VolumetricPass.h"
#include "renderModule/renderPasses/TAAPass.h"
#include "renderModule/renderPasses/DoFPass.h"
#include "renderModule/renderPasses/TextOverlayPass.h"
#include "renderModule/renderPasses/ImGuiPass.h"
#include "renderModule/renderPasses/SSSPass.h"
#include "renderModule/renderPasses/ContactGIPass.h"

namespace SFE::SystemsModule {

	template<typename PassType>
	Render::RenderPass* createRenderPass() {
		return new PassType();
	}

	// Order is draw order. Flip `enabled` to include a pass; insert a row to add one.
	inline const Render::RenderPassEntry RENDER_PASSES[] = {
		{ "AnimationCompute", true,  &createRenderPass<Render::RenderPasses::AnimationComputePass> },
		{ "CascadedShadow", true,  &createRenderPass<Render::RenderPasses::CascadedShadowPass> },
		{ "PointLight",     true,  &createRenderPass<Render::RenderPasses::PointLightPass> },
		{ "Geometry",       true,  &createRenderPass<Render::RenderPasses::GeometryPass> },
		{ "Particles",      true,  &createRenderPass<Render::RenderPasses::ParticlePass> },
		{ "SceneDepth",     true,  &createRenderPass<Render::RenderPasses::SceneDepthPass> },
		{ "SSAO",           true,  &createRenderPass<Render::RenderPasses::SSAOPass> },
		{ "SunShadow",      true,  &createRenderPass<Render::RenderPasses::SunShadowPass> },
		{ "Lighting",       true,  &createRenderPass<Render::RenderPasses::LightingPass> },
		{ "Sky",            true,  &createRenderPass<Render::RenderPasses::SkyPass> },
		{ "SSS",            true,  &createRenderPass<Render::RenderPasses::SSSPass> },
		{ "ContactGI",      true,  &createRenderPass<Render::RenderPasses::ContactGIPass> },
		{ "SSR",            true,  &createRenderPass<Render::RenderPasses::SSRPass> },
		{ "Water",          true,  &createRenderPass<Render::RenderPasses::WaterPass> },
		{ "Mist",           true,  &createRenderPass<Render::RenderPasses::MistPass> },
		{ "Glass",          true,  &createRenderPass<Render::RenderPasses::GlassPass> },
		{ "Volumetric",     true,  &createRenderPass<Render::RenderPasses::VolumetricPass> },
		{ "Underwater",     true,  &createRenderPass<Render::RenderPasses::UnderwaterPass> },
		{ "TAA",            true,  &createRenderPass<Render::RenderPasses::TAAPass> },
		{ "ParticleSprites", true, &createRenderPass<Render::RenderPasses::ParticleSpritePass> },
		{ "AirLife",        true,  &createRenderPass<Render::RenderPasses::AirLifePass> },
		{ "DoF",            true,  &createRenderPass<Render::RenderPasses::DoFPass> },
		{ "Final",          true,  &createRenderPass<Render::RenderPasses::FinalPass> },
		{ "GUI",            true,  &createRenderPass<Render::RenderPasses::GUIPass> },
		{ "Debug",          true,  &createRenderPass<Render::RenderPasses::DebugPass> },
		{ "TextOverlay",    false, &createRenderPass<Render::RenderPasses::TextOverlayPass> },
		{ "ImGui",          true,  &createRenderPass<Render::RenderPasses::ImGuiPass> },
	};

}
