#include "GraphicsSettingsGui.h"

#include "imgui.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "renderModule/Atmosphere.h"
#include "systemsModule/systems/AudioSystem.h"
#include "windowsModule/Window.h"

#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <sstream>

namespace SFE::SystemsModule {
	bool environmentSliders(GraphicsSettings& gfx) {
		bool changed = false;
		// Until moved, the sliders start from the scene's own sun and wind.
		const auto sky = Render::Atmosphere::sceneSky(false);
		float hours = gfx.timeOfDay >= 0.f ? gfx.timeOfDay : (sky ? sky->timeOfDay() : 12.f);
		if (ImGui::SliderFloat("time of day", &hours, 0.f, 24.f, "%.1f h")) {
			gfx.timeOfDay = hours;
			changed = true;
		}
		const auto wind = Render::Atmosphere::sceneWind(false);
		float strength = gfx.windStrength >= 0.f ? gfx.windStrength : (wind ? wind->strength : 0.f);
		if (ImGui::SliderFloat("wind", &strength, 0.f, 1.f, "%.2f")) {
			gfx.windStrength = strength;
			changed = true;
		}
		return changed;
	}

	void volumeSlider() {
		auto* audio = ECSHandler::getSystem<AudioSystem>();
		if (!audio) {
			return;
		}
		float volume = audio->masterVolume();
		if (ImGui::SliderFloat("volume", &volume, 0.f, 1.f, "%.2f")) {
			audio->setMasterVolume(volume);
		}
	}

	// The game's own window (Window::display): put on the screen at the next frame, and kept in display.json.
	void displaySettings(Render::Window& window) {
		auto settings = *window.display();
		bool changed = false;
		int mode = static_cast<int>(settings.mode);
		if (ImGui::Combo("mode", &mode, Render::kDisplayModeNames, IM_ARRAYSIZE(Render::kDisplayModeNames))) {
			settings.mode = static_cast<Render::DisplayMode>(mode);
			changed = true;
		}
		// Borderless has the monitor's own size only.
		ImGui::BeginDisabled(settings.mode == Render::DisplayMode::Borderless);
		const auto size = std::to_string(settings.width) + " x " + std::to_string(settings.height);
		if (ImGui::BeginCombo("resolution", size.c_str())) {
			for (const auto& [w, h] : window.displaySizes(settings.mode)) {
				if (ImGui::Selectable((std::to_string(w) + " x " + std::to_string(h)).c_str(), w == settings.width && h == settings.height)) {
					settings.width = w;
					settings.height = h;
					changed = true;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();
		changed |= ImGui::Checkbox("frame info", &settings.frameInfo);
		ImGui::SetItemTooltip("The frame's timings and memory in the corner of the screen.");
		if (changed) {
			window.setDisplay(settings);
			Render::saveDisplaySettings(*window.display());
		}
	}

	bool drawGraphicsSettings(GraphicsSettings& gfx) {
		bool changed = false;

		if (auto* window = Engine::instance()->getWindow(); window && window->display()
			&& ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
			displaySettings(*window);
		}

		if (ImGui::Button("Reset defaults")) {
			gfx = GraphicsSettings{};
			changed = true;
		}

		if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
			changed |= ImGui::SliderFloat("view distance", &gfx.viewDistance, 100.f, 5000.f, "%.0f m", ImGuiSliderFlags_Logarithmic);
			changed |= ImGui::SliderFloat("lod detail", &gfx.lodDetail, 0.3f, 4.f, "%.2f");
			changed |= ImGui::SliderFloat("stylise", &gfx.stylise, 0.f, 1.f, "%.2f");
			changed |= environmentSliders(gfx);
		}

		if (ImGui::CollapsingHeader("Debug view")) {
			// One term of the lighting instead of the lit image: see deferred_shading.fs's debugTerm.
			static constexpr const char* kViews[] = { "off", "shadow", "ambient", "direct light", "albedo", "normals", "SSAO", "irradiance",
				"reflection", "BRDF", "cascade", "cascade coverage" };
			int view = SFE::Engine::instance()->lightingDebugTerm();
			if (ImGui::Combo("view", &view, kViews, IM_ARRAYSIZE(kViews))) {
				SFE::Engine::instance()->setLightingDebugTerm(view);
			}
		}

		if (ImGui::CollapsingHeader("Sound", ImGuiTreeNodeFlags_DefaultOpen)) {
			volumeSlider();
		}

		if (ImGui::CollapsingHeader("Look", ImGuiTreeNodeFlags_DefaultOpen)) {
			changed |= ImGui::SliderFloat("exposure", &gfx.exposure, 0.2f, 2.5f, "%.2f");
			changed |= ImGui::Checkbox("auto exposure", &gfx.autoExposure);
			changed |= ImGui::SliderFloat("adaptation speed", &gfx.adaptationSpeed, 0.1f, 8.f, "%.2f");
			changed |= ImGui::SliderFloat("warmth", &gfx.warmth, -1.f, 1.f, "%.2f");
			changed |= ImGui::SliderFloat("saturation", &gfx.saturation, 0.f, 2.f, "%.2f");
			changed |= ImGui::SliderFloat("contrast", &gfx.contrast, 0.5f, 1.5f, "%.2f");
			changed |= ImGui::SliderFloat("lens flare", &gfx.flareStrength, 0.f, 2.f, "%.2f");
			changed |= ImGui::SliderFloat("sun intensity", &gfx.sunIntensity, 0.f, 8.f, "%.2f");
			changed |= ImGui::SliderFloat("IBL", &gfx.iblStrength, 0.f, 2.f, "%.2f");
			changed |= ImGui::SliderFloat("shadow fill", &gfx.shadowAmbient, 0.f, 1.f, "%.2f");
			changed |= ImGui::SliderFloat("bloom", &gfx.bloomStrength, 0.f, 2.f, "%.2f");
			changed |= ImGui::SliderFloat("time of day grade", &gfx.timeGrade, 0.f, 2.f, "%.2f");
			changed |= ImGui::SliderFloat("sun rays", &gfx.sunRays, 0.f, 3.f, "%.2f");
			changed |= ImGui::SliderFloat("air life", &gfx.airLifeAmount, 0.f, 3.f, "%.2f");
		}

		if (ImGui::CollapsingHeader("Grass", ImGuiTreeNodeFlags_DefaultOpen)) {
			changed |= ImGui::SliderFloat("grass density", &gfx.grassDensity, 0.2f, 2.f, "%.2f");
			changed |= ImGui::SliderFloat("grass distance", &gfx.grassDistance, 20.f, 200.f, "%.0f m");
		}
		if (ImGui::CollapsingHeader("Fur / skin", ImGuiTreeNodeFlags_DefaultOpen)) {
			changed |= ImGui::Checkbox("sheen", &gfx.sheen);
			changed |= ImGui::SliderFloat("sheen strength", &gfx.sheenStrength, 0.f, 3.f, "%.2f");
			changed |= ImGui::SliderFloat("rim", &gfx.rimStrength, 0.f, 2.f, "%.2f");
			changed |= ImGui::Checkbox("SSS", &gfx.sss);
			changed |= ImGui::SliderFloat("sss wrap", &gfx.sssWrap, 0.f, 3.f, "%.2f");
			changed |= ImGui::SliderFloat("sss strength", &gfx.sssStrength, 0.f, 2.5f, "%.2f");
			changed |= ImGui::SliderFloat("sss radius", &gfx.sssRadius, 1.f, 16.f, "%.1f");
		}

		if (ImGui::CollapsingHeader("Passes")) {
			changed |= ImGui::Checkbox("SSAO", &gfx.ssao);
			changed |= ImGui::Checkbox("SSR", &gfx.ssr);
			changed |= ImGui::Checkbox("Volumetric", &gfx.volumetric);
			changed |= ImGui::Checkbox("TAA", &gfx.taa);
			changed |= ImGui::Checkbox("DoF", &gfx.dof);
			changed |= ImGui::Checkbox("Bloom", &gfx.bloom);
			changed |= ImGui::Checkbox("Air life", &gfx.airLife);
			changed |= ImGui::Checkbox("Contact GI", &gfx.contactGi);
		}

		if (ImGui::CollapsingHeader("SSR / Volume")) {
			changed |= ImGui::SliderFloat("ssr distance", &gfx.ssrDistance, 2.f, 80.f, "%.1f");
			changed |= ImGui::SliderFloat("vol density", &gfx.volumetricDensity, 0.f, 0.08f, "%.4f");
			changed |= ImGui::SliderFloat("vol intensity", &gfx.volumetricIntensity, 0.f, 8.f, "%.2f");
			changed |= ImGui::SliderFloat("vol anisotropy", &gfx.volumetricAnisotropy, -0.9f, 0.9f, "%.2f");
			changed |= ImGui::SliderFloat("vol distance", &gfx.volumetricDistance, 10.f, 200.f, "%.1f");
		}

		if (ImGui::CollapsingHeader("TAA / DoF / fog")) {
			changed |= ImGui::SliderFloat("history blend", &gfx.taaBlend, 0.5f, 0.99f, "%.2f");
			changed |= ImGui::SliderFloat("dof focus", &gfx.dofFocus, 1.f, 80.f, "%.1f");
			changed |= ImGui::SliderFloat("dof range", &gfx.dofRange, 1.f, 80.f, "%.1f");
			changed |= ImGui::SliderFloat("dof blur", &gfx.dofBlur, 0.f, 16.f, "%.1f");
			changed |= ImGui::SliderFloat("fog start", &gfx.fogStart, 0.1f, 1.f, "%.2f");
			changed |= ImGui::SliderFloat("CA", &gfx.chromaticAberration, 0.f, 0.012f, "%.4f");
			changed |= ImGui::SliderFloat("sharpness", &gfx.sharpness, 0.f, 1.f, "%.2f");
			changed |= ImGui::SliderFloat("glass roughness", &gfx.glassRoughness, 0.04f, 0.6f, "%.2f");
		}

		if (ImGui::CollapsingHeader("GI")) {
			changed |= ImGui::SliderFloat("contact", &gfx.contactStrength, 0.f, 2.f, "%.2f");
			changed |= ImGui::SliderFloat("contact dist", &gfx.contactDistance, 0.2f, 4.f, "%.2f");
			changed |= ImGui::SliderFloat("contact thick", &gfx.contactThickness, 0.01f, 0.5f, "%.2f");
			changed |= ImGui::SliderFloat("gi", &gfx.giStrength, 0.f, 2.f, "%.2f");
			changed |= ImGui::SliderFloat("gi radius", &gfx.giRadius, 0.1f, 2.5f, "%.2f");
		}

		return changed;
	}

	std::string graphicsToJson(const GraphicsSettings& gfx) {
		Json::Value root;
		root["ssao"] = gfx.ssao;
		root["ssr"] = gfx.ssr;
		root["volumetric"] = gfx.volumetric;
		root["taa"] = gfx.taa;
		root["dof"] = gfx.dof;
		root["bloom"] = gfx.bloom;
		root["sheen"] = gfx.sheen;
		root["sss"] = gfx.sss;
		root["contactGi"] = gfx.contactGi;
		root["ssrDistance"] = gfx.ssrDistance;
		root["volumetricDensity"] = gfx.volumetricDensity;
		root["volumetricIntensity"] = gfx.volumetricIntensity;
		root["volumetricAnisotropy"] = gfx.volumetricAnisotropy;
		root["volumetricDistance"] = gfx.volumetricDistance;
		root["taaBlend"] = gfx.taaBlend;
		root["dofFocus"] = gfx.dofFocus;
		root["dofRange"] = gfx.dofRange;
		root["dofBlur"] = gfx.dofBlur;
		root["bloomStrength"] = gfx.bloomStrength;
		root["exposure"] = gfx.exposure;
		root["chromaticAberration"] = gfx.chromaticAberration;
		root["sharpness"] = gfx.sharpness;
		root["fogStart"] = gfx.fogStart;
		root["viewDistance"] = gfx.viewDistance;
		root["autoExposure"] = gfx.autoExposure;
		root["adaptationSpeed"] = gfx.adaptationSpeed;
		root["flareStrength"] = gfx.flareStrength;
		root["warmth"] = gfx.warmth;
		root["saturation"] = gfx.saturation;
		root["contrast"] = gfx.contrast;
		root["timeOfDay"] = gfx.timeOfDay;
		root["windStrength"] = gfx.windStrength;
		root["glassRoughness"] = gfx.glassRoughness;
		root["sheenStrength"] = gfx.sheenStrength;
		root["rimStrength"] = gfx.rimStrength;
		root["sssWrap"] = gfx.sssWrap;
		root["sssStrength"] = gfx.sssStrength;
		root["sssRadius"] = gfx.sssRadius;
		root["contactStrength"] = gfx.contactStrength;
		root["contactDistance"] = gfx.contactDistance;
		root["contactThickness"] = gfx.contactThickness;
		root["giStrength"] = gfx.giStrength;
		root["giRadius"] = gfx.giRadius;
		root["sunIntensity"] = gfx.sunIntensity;
		root["iblStrength"] = gfx.iblStrength;
		root["shadowAmbient"] = gfx.shadowAmbient;
		root["grassDensity"] = gfx.grassDensity;
		root["grassDistance"] = gfx.grassDistance;
		root["lodDetail"] = gfx.lodDetail;
		root["stylise"] = gfx.stylise;
		root["timeGrade"] = gfx.timeGrade;
		root["sunRays"] = gfx.sunRays;
		root["airLife"] = gfx.airLife;
		root["airLifeAmount"] = gfx.airLifeAmount;
		Json::StreamWriterBuilder builder;
		builder["indentation"] = "";
		return Json::writeString(builder, root);
	}

	void graphicsFromJson(const Json::Value& in, GraphicsSettings& gfx) {
		auto b = [&](const char* k, bool& v) {
			if (in.isMember(k)) {
				v = in[k].asBool();
			}
		};
		auto f = [&](const char* k, float& v) {
			if (in.isMember(k)) {
				v = in[k].asFloat();
			}
		};
		b("ssao", gfx.ssao);
		b("ssr", gfx.ssr);
		b("volumetric", gfx.volumetric);
		b("taa", gfx.taa);
		b("dof", gfx.dof);
		b("bloom", gfx.bloom);
		b("sheen", gfx.sheen);
		b("sss", gfx.sss);
		b("contactGi", gfx.contactGi);
		f("ssrDistance", gfx.ssrDistance);
		f("volumetricDensity", gfx.volumetricDensity);
		f("volumetricIntensity", gfx.volumetricIntensity);
		f("volumetricAnisotropy", gfx.volumetricAnisotropy);
		f("volumetricDistance", gfx.volumetricDistance);
		f("taaBlend", gfx.taaBlend);
		f("dofFocus", gfx.dofFocus);
		f("dofRange", gfx.dofRange);
		f("dofBlur", gfx.dofBlur);
		f("bloomStrength", gfx.bloomStrength);
		f("exposure", gfx.exposure);
		f("chromaticAberration", gfx.chromaticAberration);
		f("sharpness", gfx.sharpness);
		f("fogStart", gfx.fogStart);
		f("viewDistance", gfx.viewDistance);
		b("autoExposure", gfx.autoExposure);
		f("adaptationSpeed", gfx.adaptationSpeed);
		f("flareStrength", gfx.flareStrength);
		f("warmth", gfx.warmth);
		f("saturation", gfx.saturation);
		f("contrast", gfx.contrast);
		f("timeOfDay", gfx.timeOfDay);
		f("windStrength", gfx.windStrength);
		f("glassRoughness", gfx.glassRoughness);
		f("sheenStrength", gfx.sheenStrength);
		f("rimStrength", gfx.rimStrength);
		f("sssWrap", gfx.sssWrap);
		f("sssStrength", gfx.sssStrength);
		f("sssRadius", gfx.sssRadius);
		f("contactStrength", gfx.contactStrength);
		f("contactDistance", gfx.contactDistance);
		f("contactThickness", gfx.contactThickness);
		f("giStrength", gfx.giStrength);
		f("giRadius", gfx.giRadius);
		f("sunIntensity", gfx.sunIntensity);
		f("iblStrength", gfx.iblStrength);
		f("shadowAmbient", gfx.shadowAmbient);
		f("grassDensity", gfx.grassDensity);
		f("grassDistance", gfx.grassDistance);
		f("lodDetail", gfx.lodDetail);
		f("stylise", gfx.stylise);
		f("timeGrade", gfx.timeGrade);
		f("sunRays", gfx.sunRays);
		b("airLife", gfx.airLife);
		f("airLifeAmount", gfx.airLifeAmount);
	}

	bool graphicsFromJson(const std::string& json, GraphicsSettings& gfx) {
		if (json.empty()) {
			return false;
		}
		Json::Value root;
		Json::CharReaderBuilder builder;
		std::string errors;
		std::istringstream stream(json);
		if (!Json::parseFromStream(builder, stream, &root, &errors)) {
			return false;
		}
		graphicsFromJson(root, gfx);
		return true;
	}
}
