#pragma once

#include <string>

#include "systemsModule/systems/RenderSystem.h"

namespace Json { class Value; }

namespace SFE::SystemsModule {
	bool drawGraphicsSettings(GraphicsSettings& gfx);
	// Time of day and wind, over the scene's own until moved. True when one changed.
	bool environmentSliders(GraphicsSettings& gfx);
	// AudioSystem's master volume.
	void volumeSlider();
	// The graphics settings as JSON, every field by its name in GraphicsSettings (one line), and back: each field the
	// JSON names set, the rest left -- what an editor sends with EditorProtocol's SetGraphics.
	std::string graphicsToJson(const GraphicsSettings& gfx);
	void graphicsFromJson(const Json::Value& in, GraphicsSettings& gfx);
	bool graphicsFromJson(const std::string& json, GraphicsSettings& gfx);
}
