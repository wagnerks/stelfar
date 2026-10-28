#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render {
	// One pass of the renderer's list: its name (the profiler's zone, the log's, others' anchor), how it is made, and the
	// frames it runs on.
	struct PassEntry {
		using Make = std::function<std::unique_ptr<RenderPass>()>;
		// Asked before the passes' prepare and again before their render, each frame: false skips both that frame (the
		// pass is still made, initialised, resized, and found by those who walk the passes). None: every frame.
		using Runs = std::function<bool(const SystemsModule::RenderData&)>;
		std::string name;
		Make make;
		Runs runs;
		bool enabled = true;  // off: kept in its place, an anchor for others, never made

		// A pass of type T, value-initialised (as the old table's `new T()`).
		template <typename T>
		static PassEntry of(std::string name, Runs runs = {}) {
			return { std::move(name), [] { return std::unique_ptr<RenderPass>(std::make_unique<T>()); }, std::move(runs) };
		}
	};

	// The renderer's passes in draw order, built by the application before the renderer is made: the engine's
	// (defaultPasses) with its own put among them by name. What cannot be done -- an anchor or a name no entry has, a name
	// already in the list, an entry with no name or no way to make it -- changes nothing, returns false and is kept in
	// errors(), which RenderSystem reports as it is made. Order and the predicate are all a pass gets from the list; a
	// replacement keeps what others find a pass by besides its name (WaterPass finds LightingPass by type, CascadedShadowPass
	// the ShadowCasterPass ones).
	class PassList {
	public:
		bool append(PassEntry entry);
		bool insertAfter(std::string_view anchor, PassEntry entry);
		bool insertAfter(std::string_view anchor, std::vector<PassEntry> run);   // in its order, the first right after
		bool insertBefore(std::string_view anchor, PassEntry entry);
		bool insertBefore(std::string_view anchor, std::vector<PassEntry> run);  // in its order, the last right before
		bool replace(std::string_view name, PassEntry entry);  // `entry`, under its own name, in that one's place
		bool disable(std::string_view name);
		bool enable(std::string_view name);
		bool remove(std::string_view name);
		const PassEntry* find(std::string_view name) const;
		const std::vector<PassEntry>& entries() const { return mEntries; }
		std::vector<std::string> names() const;   // the enabled ones, in order: what the renderer makes
		const std::vector<std::string>& errors() const { return mErrors; }
	private:
		std::vector<PassEntry> mEntries;
		std::vector<std::string> mErrors;
	};

	// The engine's passes in draw order, each run by the setting that turns it off (GraphicsSettings' ssao, ssr,
	// volumetric, taa, dof, sss, contactGi; ImGui on a frame shown on the screen, Debug not on a frame that goes to the
	// editor alone); TextOverlay in it, disabled.
	PassList defaultPasses();
}
