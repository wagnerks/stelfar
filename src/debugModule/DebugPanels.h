#pragma once

#include <functional>
#include <string>
#include <vector>

#include "mathModule/Forward.h"

namespace SFE::DebugPanels {
	// A window of the application's in the debug UI (ImGuiPass), drawn after the engine's own -- Graphics and Tweaks --
	// in the order added; it first opens at `size`. Main thread, before or between frames.
	struct Panel {
		std::string title;
		std::function<void()> draw;
		Math::Vec2 size;
	};
	void add(std::string title, std::function<void()> draw, Math::Vec2 size = { 340.f, 170.f });
	const std::vector<Panel>& list();
}
