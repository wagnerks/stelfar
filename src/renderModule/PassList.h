#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "renderModule/renderPasses/RenderPass.h"

namespace SFE::Render {
	// One pass of the renderer's list: its name (the profiler's zone, the log's, others' anchor), how it is made, whether
	// it exists, the frames it runs on, and the frame's keys it gives others and needs of them.
	//
	// Two facts, at two paces:
	// - `on`: whether the pass exists. A setting (a GraphicsSettings switch) or a fact of the scene's content (a water, a
	//   sky, glass, a drop ever let go): it changes when the settings or the scene do, never with the frame. Asked every
	//   frame before the frame is recorded (RenderSystem::settlePasses): come true, the pass is made and initialised and
	//   draws that frame; gone false, it is destroyed and everything it held goes with it -- its objects held through
	//   Render::Held, its screen targets from Render::Resources -- what it shares staying while another holder does. A
	//   per-frame fact put here makes and destroys the pass at every flip: that is what `runs` is for. None: it always
	//   exists.
	// - `runs`: whether it draws this frame. Asked before the passes' prepare and again before their render: false skips
	//   both that frame, the pass still made, resized and found by those who walk the passes. None: every frame.
	//
	// A pass exists while its `on` holds and every key it `needs` has a giver earlier in the list that exists. The keys
	// it `gives` are emptied when it goes, so a reader that can do without (the AO's white stand-in) gets an empty key,
	// never a stale one, and a reader that cannot names the key in its `needs` and goes with it.
	struct PassEntry {
		using Make = std::function<std::unique_ptr<RenderPass>()>;
		using Runs = std::function<bool(const SystemsModule::RenderData&)>;
		using On = std::function<bool(const SystemsModule::RenderData&)>;
		std::string name;
		Make make;
		Runs runs;
		bool enabled = true;  // off: kept in its place, an anchor for others, never made
		On on;
		std::vector<std::string> gives;  // the frame's keys it publishes for others (FrameBlackboard)
		std::vector<std::string> needs;  // the frame's keys of others it cannot draw without

		// A pass of type T, value-initialised (as the old table's `new T()`), that always exists.
		template <typename T>
		static PassEntry of(std::string name, Runs runs = {}) {
			return { std::move(name), [] { return std::unique_ptr<RenderPass>(std::make_unique<T>()); }, std::move(runs) };
		}

		// A pass of type T that exists while `on` holds and every key of `needs` has a giver.
		template <typename T>
		static PassEntry feature(std::string name, On on, std::vector<std::string> gives = {}, std::vector<std::string> needs = {}) {
			auto entry = of<T>(std::move(name));
			entry.on = std::move(on);
			entry.gives = std::move(gives);
			entry.needs = std::move(needs);
			return entry;
		}
	};

	// The renderer's passes in draw order, built by the application before the renderer is made: the engine's
	// (defaultPasses) with its own put among them by name. What cannot be done -- an anchor or a name no entry has, a name
	// already in the list, an entry with no name or no way to make it -- changes nothing, returns false and is kept in
	// errors(), which RenderSystem reports as it is made; so are a key a pass needs that no enabled pass before it gives,
	// and a key two of them give. The list is the order, writers before readers: nothing is sorted. Order, the two
	// predicates and the keys are all a pass gets from the list; a replacement keeps what others find a pass by besides
	// its name (WaterPass finds LightingPass by type, CascadedShadowPass the ShadowCasterPass ones).
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
		// What was refused, in order, then what is wrong with the keys of the enabled entries.
		std::vector<std::string> errors() const;
	private:
		std::vector<PassEntry> mEntries;
		std::vector<std::string> mErrors;
	};

	// The engine's passes in draw order: the core, which always exists, and the features, each existing while its setting
	// is on (GraphicsSettings' ssao, ssr, volumetric, taa, dof, sss, contactGi) or while the scene has what it draws (a
	// water under a sky, a sky, glass, particles or a drop ever let go). ImGui runs on a frame shown on the screen, Debug
	// on all but a frame that goes to the editor alone; TextOverlay in it, disabled.
	PassList defaultPasses();
}
