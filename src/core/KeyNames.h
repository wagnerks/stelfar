#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace SFE::CoreModule {
	enum class InputKey;
	enum class MouseButton;

	// A key by the name a command gives it, as a script writes it: every letter and digit by itself ("W", "3"), F1 to F12,
	// and the rest by a word ("SHIFT", "PAGEUP"), all in capitals; InputKey::UNKNOWN for a name of no key.
	InputKey keyNamed(std::string_view name);
	// A mouse button by the name a command gives it: "LMB", "RMB" and "MMB", the left, right and middle ones; none for a
	// name of no button. No button is a key (keyNamed).
	std::optional<MouseButton> buttonNamed(std::string_view name);
	// Every name keyNamed knows, for a command to list: "A-Z, 0-9, F1-F12, SHIFT, ...".
	std::string keyNames();
}
