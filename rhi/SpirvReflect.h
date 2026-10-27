#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	// What a SPIR-V module binds: every variable decorated with a descriptor set and a binding, and whether its entry
	// point uses it -- names it in a function the entry point calls at any depth (Vulkan's "statically used"). A pure
	// reader of the words: no Vulkan, no compiler.
	enum class SpirvResourceKind : uint8_t { Texture, UniformBuffer, StorageBuffer, Other };
	struct SpirvResource {
		uint32_t set = 0;
		uint32_t binding = 0;
		SpirvResourceKind kind = SpirvResourceKind::Other;
		uint32_t count = 1;  // an array's length; 0 a runtime array
		bool used = false;
	};
	struct SpirvReflection {
		bool ok = false;                      // false: not SPIR-V, or cut short
		std::vector<SpirvResource> resources; // in the module's order
	};
	SpirvReflection reflectSpirv(const uint32_t* words, size_t count);
	inline SpirvReflection reflectSpirv(const std::vector<uint32_t>& code) { return reflectSpirv(code.data(), code.size()); }

	// One stage of a pipeline as the layout check sees it.
	struct SpirvStage {
		ShaderStage stage;
		const char* name;                            // the shader's, for messages
		const std::vector<SpirvResource>* resources; // its module's
	};
	// What the stages use that the pipeline's set layouts (in set order; nullptr where an index has no live layout)
	// do not give them, one line each: "<shader>: set S binding B (texture) is not in its layout" / "is a uniform buffer
	// in its layout" / "is not given to the fragment stage" / "is an array of 8, its layout has 4". Unused ones are
	// never named. Empty when they agree.
	std::vector<std::string> layoutMismatches(const std::vector<SpirvStage>& stages, const std::vector<const DescriptorSetLayoutDesc*>& sets);

	// Pipeline layouts as Vulkan compares them: two are "compatible for set N" when their push constant ranges and their
	// set layouts 0..N are defined alike (the same bindings, types, counts and stages, in any order). Each pipeline
	// layout gets one small id per set index, equal ids at index N meaning compatible for set N -- what Vulkan's
	// validation layers call compat ids.
	class LayoutCompat {
	public:
		// `sets` in set order, nullptr for an index without a live layout (a pad: no bindings).
		std::vector<uint32_t> idsOf(const std::vector<const DescriptorSetLayoutDesc*>& sets, uint32_t pushSize, ShaderStage pushStages);
	private:
		std::vector<std::vector<uint64_t>> mKnown; // each prefix seen, by its id: push, then each layout's sorted bindings
	};

	// The descriptor sets one command list has bound at one bind point, as Vulkan keeps them between binds (the
	// validation layers' "last bound" state): a bind puts its sets at firstSet..last; one bound below firstSet goes
	// ("disturbed") unless its compat id there equals the bind's; those above last go unless what was bound at last
	// was bound compatibly with the bind for last. Nothing is assumed bound when a list begins.
	class BoundSets {
	public:
		static constexpr uint32_t kMaxSets = 8;
		static constexpr uint32_t kNone = 0xFFFFFFFFu;
		// `compat` the bind's pipeline layout's ids (LayoutCompat::idsOf); `sets` the bound sets' ids.
		void bind(const std::vector<uint32_t>& compat, uint32_t firstSet, const uint32_t* sets, uint32_t count);
		// What a draw with a pipeline of `compat` whose stages use `used` (SpirvResource::used) cannot read, one line
		// each: "set S unbound" for a used set with nothing bound there compatibly; "set S binding B never written" for
		// a used binding `written(set id, B)` says the bound set has never had. A set index whose bound id equals
		// known[S] is taken as written (the caller's memory of a set it found complete). Empty when it can read them all.
		std::vector<std::string> missing(const std::vector<uint32_t>& compat, const std::vector<SpirvResource>& used,
			const std::array<uint32_t, kMaxSets>& known, const std::function<bool(uint32_t set, uint32_t binding)>& written) const;
		uint32_t setAt(uint32_t index) const; // kNone for none
	private:
		struct Bound { uint32_t set = kNone; uint32_t compat = kNone; };
		std::array<Bound, kMaxSets> mBound{};
	};
}
