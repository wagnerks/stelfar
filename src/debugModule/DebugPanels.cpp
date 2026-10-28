#include "debugModule/DebugPanels.h"

namespace SFE::DebugPanels {
	namespace {
		std::vector<Panel>& panels() {
			static std::vector<Panel> added;
			return added;
		}
	}

	void add(std::string title, std::function<void()> draw, Math::Vec2 size) {
		// One that draws nothing is no window.
		if (draw) {
			panels().push_back({ std::move(title), std::move(draw), size });
		}
	}

	const std::vector<Panel>& list() {
		return panels();
	}
}
