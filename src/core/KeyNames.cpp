#include "KeyNames.h"

#include <utility>

#include "InputHandler.h"

namespace SFE::CoreModule {
	namespace {
		// The keys named by a word. A shift, control or alt is the left one, as a script means whichever.
		constexpr std::pair<std::string_view, InputKey> kWords[] = {
			{ "SHIFT", InputKey::KEY_LEFT_SHIFT }, { "CTRL", InputKey::KEY_LEFT_CONTROL }, { "ALT", InputKey::KEY_LEFT_ALT },
			{ "SPACE", InputKey::KEY_SPACE }, { "TAB", InputKey::KEY_TAB }, { "ENTER", InputKey::KEY_ENTER },
			{ "ESCAPE", InputKey::KEY_ESCAPE }, { "BACKSPACE", InputKey::KEY_BACKSPACE },
			{ "UP", InputKey::KEY_UP }, { "DOWN", InputKey::KEY_DOWN }, { "LEFT", InputKey::KEY_LEFT }, { "RIGHT", InputKey::KEY_RIGHT },
			{ "PAGEUP", InputKey::KEY_PAGE_UP }, { "PAGEDOWN", InputKey::KEY_PAGE_DOWN }, { "HOME", InputKey::KEY_HOME },
			{ "END", InputKey::KEY_END },
		};
		constexpr int kFunctionKeys = 12;
		constexpr std::pair<std::string_view, MouseButton> kButtons[] = {
			{ "LMB", MouseButton::MOUSE_BUTTON_LEFT }, { "RMB", MouseButton::MOUSE_BUTTON_RIGHT },
			{ "MMB", MouseButton::MOUSE_BUTTON_MIDDLE },
		};

		// A key some way along a run of keys numbered one after another (A to Z, 0 to 9, F1 to F12, as GLFW numbers them).
		InputKey after(InputKey first, int steps) {
			return static_cast<InputKey>(static_cast<int>(first) + steps);
		}
	}

	InputKey keyNamed(std::string_view name) {
		if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
			return after(InputKey::KEY_A, name[0] - 'A');
		}
		if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
			return after(InputKey::KEY_0, name[0] - '0');
		}
		// F and a number with no leading zero, 1 to 12.
		if ((name.size() == 2 || name.size() == 3) && name[0] == 'F' && name[1] >= '1' && name[1] <= '9') {
			int number = name[1] - '0';
			if (name.size() == 3) {
				if (name[2] < '0' || name[2] > '9') {
					return InputKey::UNKNOWN;
				}
				number = number * 10 + (name[2] - '0');
			}
			return number <= kFunctionKeys ? after(InputKey::KEY_F1, number - 1) : InputKey::UNKNOWN;
		}
		for (const auto& [word, key] : kWords) {
			if (word == name) {
				return key;
			}
		}
		return InputKey::UNKNOWN;
	}

	std::optional<MouseButton> buttonNamed(std::string_view name) {
		for (const auto& [word, button] : kButtons) {
			if (word == name) {
				return button;
			}
		}
		return std::nullopt;
	}

	std::string keyNames() {
		std::string names = "A-Z, 0-9, F1-F" + std::to_string(kFunctionKeys);
		for (const auto& named : kWords) {
			names += ", ";
			names += named.first;
		}
		return names;
	}
}
