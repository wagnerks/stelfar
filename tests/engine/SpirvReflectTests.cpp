#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "rhi/SpirvCompiler.h"
#include "rhi/SpirvReflect.h"

namespace SFE::EngineTests {
	using namespace SFE::Render;

	namespace {
		// GLSL compiled as the Vulkan backend compiles it, without its cache.
		std::vector<uint32_t> compiled(const ShaderStage stage, const char* source) {
			auto code = compileSpirv(stage, "spirv_reflect_test", source, std::filesystem::path{});
			EXPECT_FALSE(code.empty()) << "did not compile:\n" << source;
			return code;
		}

		const SpirvResource* resourceAt(const SpirvReflection& reflection, const uint32_t set, const uint32_t binding) {
			for (const auto& resource : reflection.resources) {
				if (resource.set == set && resource.binding == binding) {
					return &resource;
				}
			}
			return nullptr;
		}

		SpirvResource resource(const uint32_t set, const uint32_t binding, const SpirvResourceKind kind, const uint32_t count = 1, const bool used = true) {
			SpirvResource made;
			made.set = set;
			made.binding = binding;
			made.kind = kind;
			made.count = count;
			made.used = used;
			return made;
		}

		DescriptorSetLayoutDesc layoutOf(std::vector<DescriptorSetLayoutBinding> bindings) {
			DescriptorSetLayoutDesc desc;
			desc.bindings = std::move(bindings);
			return desc;
		}

		const ShaderStage kGraphics = ShaderStage::Vertex | ShaderStage::Fragment;
	}

	// Used is named by a function the entry point reaches, at any depth; a variable read only in a function nobody
	// calls, or never named at all, is declared and unused.
	TEST(SpirvReflect, UsedOnlyThroughCalledFunctions) {
		const auto code = compiled(ShaderStage::Fragment, R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D readInHelper;
layout(set = 0, binding = 1) uniform sampler2D readInUncalled;
layout(set = 0, binding = 2) uniform sampler2D neverNamed;
layout(location = 0) out vec4 color;
vec4 helper() { return texture(readInHelper, vec2(0.5)); }
vec4 deeper() { return helper() * 2.0; }
vec4 uncalled() { return texture(readInUncalled, vec2(0.5)); }
void main() { color = deeper(); }
)");
		const auto reflection = reflectSpirv(code);
		ASSERT_TRUE(reflection.ok);
		const auto* helper = resourceAt(reflection, 0, 0);
		const auto* uncalled = resourceAt(reflection, 0, 1);
		const auto* never = resourceAt(reflection, 0, 2);
		ASSERT_TRUE(helper && uncalled && never);
		EXPECT_TRUE(helper->used);
		EXPECT_FALSE(uncalled->used);
		EXPECT_FALSE(never->used);
		EXPECT_EQ(helper->kind, SpirvResourceKind::Texture);
	}

	// A uniform block, a storage block with a runtime array, an array of eight samplers and a 2D array sampler.
	TEST(SpirvReflect, KindsAndCounts) {
		const auto code = compiled(ShaderStage::Fragment, R"(#version 450
layout(set = 0, binding = 0, std140) uniform Settings { vec4 tint; } settings;
layout(set = 0, binding = 1, std430) readonly buffer Items { vec4 items[]; };
layout(set = 1, binding = 2) uniform sampler2D maps[8];
layout(set = 1, binding = 3) uniform sampler2DArray layers;
layout(location = 0) out vec4 color;
void main() {
    color = settings.tint + items[0] + texture(maps[3], vec2(0.5)) + texture(layers, vec3(0.5, 0.5, 0.0));
}
)");
		const auto reflection = reflectSpirv(code);
		ASSERT_TRUE(reflection.ok);
		ASSERT_EQ(reflection.resources.size(), 4u);
		const auto* settings = resourceAt(reflection, 0, 0);
		const auto* items = resourceAt(reflection, 0, 1);
		const auto* maps = resourceAt(reflection, 1, 2);
		const auto* layers = resourceAt(reflection, 1, 3);
		ASSERT_TRUE(settings && items && maps && layers);
		EXPECT_EQ(settings->kind, SpirvResourceKind::UniformBuffer);
		EXPECT_EQ(settings->count, 1u);
		EXPECT_EQ(items->kind, SpirvResourceKind::StorageBuffer);
		EXPECT_EQ(items->count, 1u) << "the runtime array is the block's member, not a descriptor array";
		EXPECT_EQ(maps->kind, SpirvResourceKind::Texture);
		EXPECT_EQ(maps->count, 8u);
		EXPECT_EQ(layers->kind, SpirvResourceKind::Texture);
		EXPECT_EQ(layers->count, 1u);
		for (const auto& resource : reflection.resources) {
			EXPECT_TRUE(resource.used) << "set " << resource.set << " binding " << resource.binding;
		}
	}

	// A sampler handed to a function is used where it is handed on.
	TEST(SpirvReflect, SamplerPassedAsAnArgumentIsUsed) {
		const auto code = compiled(ShaderStage::Fragment, R"(#version 450
layout(set = 0, binding = 4) uniform sampler2D handedOn;
layout(location = 0) out vec4 color;
vec4 sampleIt(sampler2D map) { return texture(map, vec2(0.5)); }
void main() { color = sampleIt(handedOn); }
)");
		const auto reflection = reflectSpirv(code);
		ASSERT_TRUE(reflection.ok);
		const auto* handedOn = resourceAt(reflection, 0, 4);
		ASSERT_TRUE(handedOn);
		EXPECT_TRUE(handedOn->used);
	}

	// Asking a buffer's runtime array its length reads the buffer.
	TEST(SpirvReflect, ArrayLengthAloneIsAUse) {
		const auto code = compiled(ShaderStage::Fragment, R"(#version 450
layout(set = 0, binding = 5, std430) readonly buffer Counted { vec4 counted[]; };
layout(location = 0) out vec4 color;
void main() { color = vec4(float(counted.length())); }
)");
		const auto reflection = reflectSpirv(code);
		ASSERT_TRUE(reflection.ok);
		const auto* counted = resourceAt(reflection, 0, 5);
		ASSERT_TRUE(counted);
		EXPECT_EQ(counted->kind, SpirvResourceKind::StorageBuffer);
		EXPECT_TRUE(counted->used);
	}

	// Push constants have no set or binding: no descriptor.
	TEST(SpirvReflect, PushConstantsAreNotResources) {
		const auto code = compiled(ShaderStage::Fragment, R"(#version 450
layout(push_constant) uniform Push { vec4 tint; } pc;
layout(set = 0, binding = 6) uniform sampler2D tinted;
layout(location = 0) out vec4 color;
void main() { color = pc.tint * texture(tinted, vec2(0.5)); }
)");
		const auto reflection = reflectSpirv(code);
		ASSERT_TRUE(reflection.ok);
		ASSERT_EQ(reflection.resources.size(), 1u);
		EXPECT_EQ(reflection.resources[0].binding, 6u);
		EXPECT_TRUE(reflection.resources[0].used);
	}

	// Words that are not SPIR-V, or a module cut off inside an instruction, are refused without reading past them.
	TEST(SpirvReflect, NotSpirvIsRefused) {
		EXPECT_FALSE(reflectSpirv(nullptr, 0).ok);
		const std::vector<uint32_t> garbage{ 0xDEADBEEFu, 1u, 2u, 3u, 4u, 5u, 6u };
		EXPECT_FALSE(reflectSpirv(garbage).ok);
		const std::vector<uint32_t> headerOnly{ 0x07230203u, 0x00010500u, 0u, 1u };
		EXPECT_FALSE(reflectSpirv(headerOnly).ok) << "shorter than a header";
		const std::vector<uint32_t> zeroLength{ 0x07230203u, 0x00010500u, 0u, 1u, 0u, 0u };
		EXPECT_FALSE(reflectSpirv(zeroLength).ok) << "an instruction of no words";

		const auto code = compiled(ShaderStage::Fragment, R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D map;
layout(location = 0) out vec4 color;
void main() { color = texture(map, vec2(0.5)); }
)");
		ASSERT_TRUE(reflectSpirv(code).ok);
		// Cut inside the first instruction of more than one word.
		size_t at = 5;
		while (at < code.size() && (code[at] >> 16) <= 1u) {
			at += (std::max)(1u, code[at] >> 16);
		}
		ASSERT_LT(at, code.size());
		EXPECT_FALSE(reflectSpirv(code.data(), at + 1).ok) << "cut short";
	}

	// Each fault of a layout named once, an unused binding never; a layout that gives everything, nothing.
	TEST(LayoutCheck, NamesWhatTheLayoutLacks) {
		const std::vector<SpirvResource> fragment{
			resource(0, 0, SpirvResourceKind::Texture),
			resource(0, 1, SpirvResourceKind::UniformBuffer),
			resource(0, 2, SpirvResourceKind::Texture, 8),
			resource(0, 3, SpirvResourceKind::StorageBuffer),
			resource(1, 0, SpirvResourceKind::Texture),
			resource(0, 9, SpirvResourceKind::Texture, 1, false),
			resource(2, 0, SpirvResourceKind::Texture, 1, false),
		};
		const std::vector<SpirvResource> vertex{ resource(0, 0, SpirvResourceKind::Texture) };
		const std::vector<SpirvStage> stages{ { ShaderStage::Vertex, "a.vs", &vertex }, { ShaderStage::Fragment, "a.fs", &fragment } };

		const auto lacking = layoutOf({
			{ 0, DescriptorType::CombinedImageSampler, ShaderStage::Vertex },
			{ 1, DescriptorType::StorageBuffer, ShaderStage::Fragment },
			{ 2, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 4 },
		});
		EXPECT_EQ(layoutMismatches(stages, { &lacking, nullptr }), (std::vector<std::string>{
			"a.fs: set 0 binding 0 (texture) is not given to the fragment stage",
			"a.fs: set 0 binding 1 (uniform buffer) is a storage buffer in its layout",
			"a.fs: set 0 binding 2 (texture) is an array of 8, its layout has 4",
			"a.fs: set 0 binding 3 (storage buffer) is not in its layout",
			"a.fs: set 1 binding 0 (texture) is not in its layout",
		}));

		const auto giving = layoutOf({
			{ 0, DescriptorType::CombinedImageSampler, kGraphics },
			{ 1, DescriptorType::UniformBufferDynamic, ShaderStage::Fragment },
			{ 2, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 8 },
			{ 3, DescriptorType::StorageBuffer, kGraphics },
		});
		const auto second = layoutOf({ { 0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment } });
		EXPECT_TRUE(layoutMismatches(stages, { &giving, &second }).empty());
	}

	// Layouts defined alike, their bindings in any order, are one id at every index.
	TEST(LayoutCompat, AlikeLayoutsShareIds) {
		LayoutCompat compat;
		const auto first = layoutOf({ { 5, DescriptorType::UniformBuffer, kGraphics }, { 7, DescriptorType::StorageBuffer, ShaderStage::Vertex } });
		const auto reordered = layoutOf({ { 7, DescriptorType::StorageBuffer, ShaderStage::Vertex }, { 5, DescriptorType::UniformBuffer, kGraphics } });
		const auto textures = layoutOf({ { 1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment } });
		const auto a = compat.idsOf({ &first, &textures }, 0, kGraphics);
		const auto b = compat.idsOf({ &reordered, &textures }, 0, kGraphics);
		ASSERT_EQ(a.size(), 2u);
		EXPECT_EQ(a, b);
		EXPECT_NE(a[0], a[1]);
	}

	// A set layout that differs makes the ids differ from its index on, and only from there.
	TEST(LayoutCompat, ADifferentLayoutSplitsFromItsIndexOn) {
		LayoutCompat compat;
		const auto own = layoutOf({ { 5, DescriptorType::UniformBuffer, kGraphics } });
		const auto textures = layoutOf({ { 1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment } });
		const auto otherTextures = layoutOf({ { 1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 2 } });
		const auto user = layoutOf({ { 40, DescriptorType::CombinedImageSampler, kGraphics } });
		const auto a = compat.idsOf({ &own, &textures, &user }, 0, kGraphics);
		const auto b = compat.idsOf({ &own, &otherTextures, &user }, 0, kGraphics);
		ASSERT_EQ(a.size(), 3u);
		ASSERT_EQ(b.size(), 3u);
		EXPECT_EQ(a[0], b[0]);
		EXPECT_NE(a[1], b[1]);
		EXPECT_NE(a[2], b[2]) << "compatible for set 2 only with sets 0 and 1 alike";
	}

	// The push range is part of every prefix; without one its stages make no range and do not count.
	TEST(LayoutCompat, PushRangeCounts) {
		LayoutCompat compat;
		const auto own = layoutOf({ { 5, DescriptorType::UniformBuffer, kGraphics } });
		const auto small = compat.idsOf({ &own }, 16, kGraphics);
		const auto large = compat.idsOf({ &own }, 32, kGraphics);
		EXPECT_NE(small[0], large[0]);
		EXPECT_EQ(compat.idsOf({ &own }, 0, kGraphics), compat.idsOf({ &own }, 0, ShaderStage::Vertex));
		EXPECT_NE(compat.idsOf({ &own }, 16, kGraphics), compat.idsOf({ &own }, 16, ShaderStage::Vertex));
	}

	// An index without a live layout is a layout of nothing.
	TEST(LayoutCompat, PadIsAnEmptyLayout) {
		LayoutCompat compat;
		const auto own = layoutOf({ { 5, DescriptorType::UniformBuffer, kGraphics } });
		const auto user = layoutOf({ { 40, DescriptorType::CombinedImageSampler, kGraphics } });
		const DescriptorSetLayoutDesc empty{};
		EXPECT_EQ(compat.idsOf({ &own, nullptr, &user }, 0, kGraphics), compat.idsOf({ &own, &empty, &user }, 0, kGraphics));
	}

	namespace {
		// The pipeline layouts of the bound-set tests: an own pair (A, its alike B), one unlike at 0 (C), a pipeline with
		// the user set behind two pads (P), and another pipeline's pair (Q).
		struct Layouts {
			DescriptorSetLayoutDesc own = layoutOf({ { 5, DescriptorType::UniformBuffer, kGraphics } });
			DescriptorSetLayoutDesc textures = layoutOf({ { 1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment } });
			DescriptorSetLayoutDesc other = layoutOf({ { 7, DescriptorType::StorageBuffer, ShaderStage::Vertex } });
			DescriptorSetLayoutDesc user = layoutOf({ { 40, DescriptorType::CombinedImageSampler, kGraphics } });
			LayoutCompat compat;
			std::vector<uint32_t> a = compat.idsOf({ &own, &textures }, 0, kGraphics);
			std::vector<uint32_t> b = compat.idsOf({ &own, &textures }, 0, kGraphics);
			std::vector<uint32_t> c = compat.idsOf({ &other, &textures }, 0, kGraphics);
			std::vector<uint32_t> p = compat.idsOf({ &own, &textures, nullptr, &user }, 0, kGraphics);
			std::vector<uint32_t> q = compat.idsOf({ &other, &other }, 0, kGraphics);
		};

		// Every binding of every set written.
		bool allWritten(uint32_t, uint32_t) {
			return true;
		}

		constexpr std::array<uint32_t, BoundSets::kMaxSets> kNoneKnown{ BoundSets::kNone, BoundSets::kNone, BoundSets::kNone, BoundSets::kNone,
			BoundSets::kNone, BoundSets::kNone, BoundSets::kNone, BoundSets::kNone };

		const std::vector<SpirvResource> kOwnUsed{ resource(0, 5, SpirvResourceKind::UniformBuffer), resource(1, 1, SpirvResourceKind::Texture) };
	}

	// Sets bound with one layout are read by a pipeline whose layout is alike.
	TEST(BoundSets, ACompatiblePipelineSeesTheSets) {
		Layouts layouts;
		BoundSets bound;
		const uint32_t sets[] = { 100, 101 };
		bound.bind(layouts.a, 0, sets, 2);
		EXPECT_TRUE(bound.missing(layouts.b, kOwnUsed, kNoneKnown, allWritten).empty());
		EXPECT_EQ(bound.setAt(0), 100u);
		EXPECT_EQ(bound.setAt(1), 101u);
		EXPECT_EQ(bound.setAt(2), BoundSets::kNone);
	}

	// The pads of a pipeline with the user set need no set: nothing there is used.
	TEST(BoundSets, UnusedSetsNeedNoBind) {
		Layouts layouts;
		BoundSets bound;
		const uint32_t own[] = { 100, 101 };
		const uint32_t user[] = { 103 };
		bound.bind(layouts.p, 0, own, 2);
		bound.bind(layouts.p, 3, user, 1);
		const std::vector<SpirvResource> used{ resource(0, 5, SpirvResourceKind::UniformBuffer), resource(3, 40, SpirvResourceKind::Texture) };
		EXPECT_TRUE(bound.missing(layouts.p, used, kNoneKnown, allWritten).empty());
	}

	TEST(BoundSets, UsedSetNeverBound) {
		Layouts layouts;
		BoundSets bound;
		const uint32_t own[] = { 100, 101 };
		bound.bind(layouts.p, 0, own, 2);
		const std::vector<SpirvResource> used{ resource(0, 5, SpirvResourceKind::UniformBuffer), resource(3, 40, SpirvResourceKind::Texture) };
		EXPECT_EQ(bound.missing(layouts.p, used, kNoneKnown, allWritten), (std::vector<std::string>{ "set 3 unbound" }));
	}

	// A bind with a layout unlike the bound one at a lower index takes that lower set away.
	TEST(BoundSets, AnIncompatibleBindDisturbsTheLower) {
		Layouts layouts;
		BoundSets bound;
		const uint32_t own[] = { 100, 101 };
		const uint32_t textures[] = { 201 };
		bound.bind(layouts.a, 0, own, 2);
		bound.bind(layouts.c, 1, textures, 1);
		const std::vector<SpirvResource> used{ resource(0, 7, SpirvResourceKind::StorageBuffer), resource(1, 1, SpirvResourceKind::Texture) };
		EXPECT_EQ(bound.missing(layouts.c, used, kNoneKnown, allWritten), (std::vector<std::string>{ "set 0 unbound" }));
	}

	// Set 3 bound before the lower sets, while another pipeline's were there, goes when the pass binds its own; bound
	// after them it stays, and binding them again alike keeps it.
	TEST(BoundSets, HigherSetsGoWhenTheLowerAreReplaced) {
		Layouts layouts;
		const uint32_t mine[] = { 100, 101 };
		const uint32_t theirs[] = { 300, 301 };
		const uint32_t user[] = { 103 };
		const std::vector<SpirvResource> used{ resource(0, 5, SpirvResourceKind::UniformBuffer), resource(1, 1, SpirvResourceKind::Texture),
			resource(3, 40, SpirvResourceKind::Texture) };
		{
			BoundSets bound;
			bound.bind(layouts.q, 0, theirs, 2);
			bound.bind(layouts.p, 3, user, 1);
			bound.bind(layouts.p, 0, mine, 2);
			EXPECT_EQ(bound.missing(layouts.p, used, kNoneKnown, allWritten), (std::vector<std::string>{ "set 3 unbound" }));
		}
		{
			BoundSets bound;
			bound.bind(layouts.p, 0, mine, 2);
			bound.bind(layouts.p, 3, user, 1);
			EXPECT_TRUE(bound.missing(layouts.p, used, kNoneKnown, allWritten).empty());
			bound.bind(layouts.p, 0, mine, 2);
			EXPECT_TRUE(bound.missing(layouts.p, used, kNoneKnown, allWritten).empty()) << "the lower bound again alike";
			EXPECT_EQ(bound.setAt(3), 103u);
		}
	}

	// A used binding the bound set never had is named, unless the set is the one last found complete.
	TEST(BoundSets, AnUnwrittenBindingIsNamed) {
		Layouts layouts;
		BoundSets bound;
		const uint32_t own[] = { 100 };
		bound.bind(layouts.a, 0, own, 1);
		const std::vector<SpirvResource> used{ resource(0, 5, SpirvResourceKind::UniformBuffer), resource(0, 38, SpirvResourceKind::Texture) };
		const std::set<std::pair<uint32_t, uint32_t>> written{ { 100, 5 } };
		const auto has = [&](const uint32_t set, const uint32_t binding) { return written.contains({ set, binding }); };
		EXPECT_EQ(bound.missing(layouts.a, used, kNoneKnown, has), (std::vector<std::string>{ "set 0 binding 38 never written" }));
		auto known = kNoneKnown;
		known[0] = 100;
		EXPECT_TRUE(bound.missing(layouts.a, used, known, has).empty());
	}
}
