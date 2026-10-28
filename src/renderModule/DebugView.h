#pragma once

namespace SFE::Render {
	inline constexpr const char* kDebugViewGroup = "Render";
	inline constexpr const char* kDebugViewName = "debug view";
	// What the lighting draws instead of the lit picture (deferred_shading.fs's debugTerm): 0 the picture, 1 shadow, ...,
	// 15 the material's AO (its help says each). The registry's number Render / debug view, this run's only (never read
	// from the numbers file); registered at the first call, which Engine::initThread makes before the file is read.
	int& debugView();
}
