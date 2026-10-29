#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "renderModule/Held.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	// What a screen target is: its format, the picture's size over `divisor`, and how it is used.
	struct TargetDesc {
		TextureFormat format = TextureFormat::RGBA16F;
		int divisor = 1;
		TextureUsage usage = TextureUsage::ColorAttachment | TextureUsage::Sampled;
	};

	// The screen targets the passes hold, by name, and the framebuffers built over the frame's targets: a name is one
	// texture, shared by every pass that asks for it and freed with its last holder (Held). A pass names its own after its
	// entry, `<RenderPass::name>.<what>`, so they are its entry's alone, however many entries its class has; a name meant
	// to be shared is one target every pass asking for it draws into, each filling it before it reads it (kSceneCopy).
	// RenderSystem keeps it; passes reach it through RenderData::resources. Main thread.
	class Resources {
	public:
		// The target of that name at the picture's size now: made on the first ask, the same object on every other while
		// anyone holds it, made again in its place when asked at another size (a pass's resize) -- every holder then holds
		// the new one, and makes again only what it built over it. Asked as another than it is held as (format, divisor,
		// usage), refused and said: none.
		Held<TextureHandle> target(const std::string& name, const TargetDesc& desc);

		// A framebuffer over `color` with `depth` for `renderPass`, the picture's size: one for each three of them, kept
		// until the frame's targets change (targetsChanged). What FinalDepthTargets gives the passes drawing into the lit
		// picture, kept here so that none of them outlives a target let go of, whether or not its pass draws again.
		FramebufferHandle framebuffer(TextureHandle color, TextureHandle depth, RenderPassHandle renderPass);
		// The set of the frame's targets has changed -- the picture drawn at a new size, a pass made or let go (RenderSystem,
		// as it counts RenderData::targetsVersion up): every framebuffer kept over them is let go of, at once.
		void targetsChanged();

		// The targets held now, by name, and what each one is.
		struct HeldTarget {
			std::string name;
			long holders = 0;
			uint64_t bytes = 0;
		};
		std::vector<HeldTarget> held() const;
		// What the target of that name takes on the device, 0 when none holds it; and how many hold it.
		uint64_t bytes(std::string_view name) const;
		long holders(std::string_view name) const;

	private:
		struct Target {
			std::weak_ptr<TextureHandle> slot;
			TargetDesc desc;
			uint32_t width = 0;
			uint32_t height = 0;
		};
		const Target* live(std::string_view name) const;
		std::map<std::string, Target, std::less<>> mTargets;
		struct Framebuffer {
			TextureHandle color;
			TextureHandle depth;
			RenderPassHandle renderPass;
			Held<FramebufferHandle> fbo;
		};
		std::vector<Framebuffer> mFramebuffers;
	};

	// The picture's copy: the one name meant to be shared, by every pass that reads what is behind what it draws (the
	// water, the glass, the drops' clear liquid), each blitting the picture into it before it reads it.
	inline constexpr const char* kSceneCopy = "scene.copy";

	// A screen pass's target from `resources` under `name` and a framebuffer over it for `renderPass` (none for a copy's
	// target, which is blitted into): what Utils::screenTarget makes, held and shared by name. The pass's resize calls it
	// again: the target is made again only if no other holder has done so at the new size, the framebuffer always.
	void screenTarget(Resources& resources, const std::string& name, Held<TextureHandle>& texture, Held<FramebufferHandle>& framebuffer,
		RenderPassHandle renderPass, TextureFormat format = TextureFormat::RGBA16F, int divisor = 1);
}
