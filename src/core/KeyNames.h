#pragma once

#include <string>
#include <string_view>

namespace SFE::CoreModule {
	enum class InputKey;

	// A key by the name a command gives it, as a script writes it: every letter and digit by itself ("W", "3"), F1 to F12,
	// and the rest by a word ("SHIFT", "PAGEUP"), all in capitals; InputKey::UNKNOWN for a name of no key.
	InputKey keyNamed(std::string_view name);
	// Every name keyNamed knows, for a command to list: "A-Z, 0-9, F1-F12, SHIFT, ...".
	std::string keyNames();
}
