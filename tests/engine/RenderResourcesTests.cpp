#include <utility>

#include "EngineTestFixture.h"
#include "renderModule/Held.h"
#include "renderModule/RenderResources.h"
#include "renderModule/renderPasses/FinalDepthTargets.h"
#include "rhi/NullRHI.h"
#include "systemsModule/systems/RenderSystem.h"

// What the passes hold their objects by (Render::Held) and their screen targets from (Render::Resources): a handle held
// is destroyed with its last holder, a target asked for by name is one object for every pass that asks, remade in its
// place at a new size; and the framebuffers over the frame's targets are made again when those change.
namespace SFE::EngineTests {
	using Render::NullRHI;

	namespace {
		NullRHI& nullRhi() {
			auto* rhi = dynamic_cast<NullRHI*>(Engine::instance()->rhi());
			EXPECT_NE(rhi, nullptr) << "the tests' engine is not on the None backend";
			return *rhi;
		}
	}

	// A copy of a held handle shares it; the copies let go of one by one, the last destroys it, and a held nothing
	// destroys nothing.
	TEST(Held, TheLastHolderDestroys) {
		auto& rhi = nullRhi();
		const auto before = rhi.live(NullRHI::Kind::Sampler);
		Render::Held<Render::SamplerHandle> first = Render::own(rhi.createSampler({}));
		ASSERT_TRUE(first);
		EXPECT_EQ(rhi.live(NullRHI::Kind::Sampler), before + 1);
		auto second = first;
		EXPECT_EQ(second.get().id, first.get().id);
		EXPECT_EQ(first.holders(), 2);
		first.reset();
		EXPECT_FALSE(first);
		EXPECT_EQ(rhi.live(NullRHI::Kind::Sampler), before + 1) << "destroyed while a copy still held it";
		auto moved = std::move(second);
		EXPECT_EQ(rhi.live(NullRHI::Kind::Sampler), before + 1);
		moved = Render::Held<Render::SamplerHandle>();
		EXPECT_EQ(rhi.live(NullRHI::Kind::Sampler), before) << "the last holder gone, the sampler is still there";
		const Render::Held<Render::SamplerHandle> none = Render::own(Render::SamplerHandle{});
		EXPECT_FALSE(none);
		EXPECT_EQ(none.holders(), 0);
	}

	// One name is one target for every holder, freed with the last of them; asked as another, it is refused while held.
	TEST(RenderResources, ANameIsOneTargetFreedWithItsLastHolder) {
		auto& rhi = nullRhi();
		Render::Resources resources;
		const auto before = rhi.live(NullRHI::Kind::Texture);
		auto a = resources.target("test.a", {});
		auto b = resources.target("test.a", {});
		ASSERT_TRUE(a);
		EXPECT_EQ(a.get().id, b.get().id) << "one name gave two targets";
		EXPECT_EQ(rhi.live(NullRHI::Kind::Texture), before + 1);
		EXPECT_EQ(resources.holders("test.a"), 2);
		const auto& screen = Engine::instance()->getWindow()->getScreenData();
		EXPECT_EQ(resources.bytes("test.a"), static_cast<uint64_t>(screen.renderW) * screen.renderH * 8u) << "an RGBA16F target";
		ASSERT_EQ(resources.held().size(), 1u);
		EXPECT_EQ(resources.held().front().name, "test.a");

		EXPECT_FALSE(resources.target("test.a", { Render::TextureFormat::R8 })) << "a name held was given as another format";
		EXPECT_FALSE(resources.target("test.a", { Render::TextureFormat::RGBA16F, 2 })) << "a name held was given at another size";
		auto other = resources.target("test.b", { Render::TextureFormat::R8, 2 });
		EXPECT_NE(other.get().id, a.get().id);
		EXPECT_EQ(resources.bytes("test.b"), static_cast<uint64_t>(screen.renderW / 2) * (screen.renderH / 2));
		other.reset();

		a.reset();
		EXPECT_EQ(rhi.live(NullRHI::Kind::Texture), before + 1) << "freed while another pass held it";
		b.reset();
		EXPECT_EQ(rhi.live(NullRHI::Kind::Texture), before) << "held by no pass, the target is still there";
		EXPECT_EQ(resources.holders("test.a"), 0);
		EXPECT_EQ(resources.bytes("test.a"), 0u);
		EXPECT_TRUE(resources.held().empty());
		EXPECT_TRUE(resources.target("test.a", { Render::TextureFormat::R8 })) << "a name no pass holds was refused as another";
	}

	// Asked at a new size, a target is made again in its place: every holder holds the new one, and the old is gone.
	TEST(RenderResources, ANewSizeRemakesTheTargetForEveryHolder) {
		auto& rhi = nullRhi();
		Render::Resources resources;
		auto& screen = Engine::instance()->getWindow()->getScreenData();
		const auto size = std::pair{ screen.renderW, screen.renderH };
		const Finally restore([&] {
			screen.renderW = size.first;
			screen.renderH = size.second;
		});
		const auto before = rhi.live(NullRHI::Kind::Texture);
		auto a = resources.target("test.sized", {});
		const auto b = resources.target("test.sized", {});
		const auto first = a.get();
		screen.renderW = size.first / 2;
		screen.renderH = size.second / 2;
		a = resources.target("test.sized", {});
		EXPECT_NE(a.get().id, first.id) << "the target was not made again at the new size";
		EXPECT_EQ(b.get().id, a.get().id) << "a holder kept the old target";
		EXPECT_EQ(rhi.live(NullRHI::Kind::Texture), before + 1) << "the old target was not let go of";
		EXPECT_EQ(resources.bytes("test.sized"), static_cast<uint64_t>(screen.renderW) * screen.renderH * 8u);
	}

	// screenTarget gives the target by name and a framebuffer of the holder's own over it, made again with every call.
	TEST(RenderResources, AScreenTargetIsTheNamedOneWithAFramebufferOfItsOwn) {
		auto& rhi = nullRhi();
		Render::Resources resources;
		const auto framebuffers = rhi.live(NullRHI::Kind::Framebuffer);
		const auto textures = rhi.live(NullRHI::Kind::Texture);
		{
			Render::Held<Render::TextureHandle> firstTexture, secondTexture;
			Render::Held<Render::FramebufferHandle> firstFbo, secondFbo;
			Render::screenTarget(resources, "test.shared", firstTexture, firstFbo, {});
			Render::screenTarget(resources, "test.shared", secondTexture, secondFbo, {});
			EXPECT_EQ(firstTexture.get().id, secondTexture.get().id);
			EXPECT_NE(firstFbo.get().id, secondFbo.get().id);
			EXPECT_EQ(rhi.live(NullRHI::Kind::Framebuffer), framebuffers + 2);
			EXPECT_EQ(rhi.live(NullRHI::Kind::Texture), textures + 1);
			Render::screenTarget(resources, "test.shared", firstTexture, firstFbo, {});
			EXPECT_EQ(rhi.live(NullRHI::Kind::Framebuffer), framebuffers + 2) << "a resize kept the old framebuffer";
		}
		EXPECT_EQ(rhi.live(NullRHI::Kind::Framebuffer), framebuffers);
		EXPECT_EQ(rhi.live(NullRHI::Kind::Texture), textures);
	}

	// The same frame textures give the same framebuffer, to every pass asking with the same render pass: the frame's
	// registry keeps it. Once the frame's targets change (targetsChanged), every one goes at once -- not at the next ask,
	// which a pass with nothing to draw may not make -- and the next ask makes a new one.
	TEST(FinalDepthTargets, KeptByTheFrameUntilItsTargetsChange) {
		auto& rhi = nullRhi();
		const auto before = rhi.live(NullRHI::Kind::Framebuffer);
		const Render::Held<Render::TextureHandle> color = Render::own(rhi.createTexture({}));
		const Render::Held<Render::TextureHandle> depth = Render::own(rhi.createTexture({}));
		const Render::Held<Render::RenderPassHandle> pass = Render::own(rhi.createRenderPass({}));
		SystemsModule::RenderData data;
		data.frame.set(Render::FrameId::Final, { .texture = color });
		data.frame.set(Render::FrameId::GBufferDepth, { .texture = depth });
		Render::RenderPasses::FinalDepthTargets targets;
		EXPECT_FALSE(targets.get(data, pass)) << "a framebuffer with no registry to keep it";
		{
			Render::Resources resources;
			data.resources = &resources;
			const auto first = targets.get(data, pass);
			ASSERT_TRUE(first);
			EXPECT_EQ(targets.get(data, pass).id, first.id);
			Render::RenderPasses::FinalDepthTargets another;
			EXPECT_EQ(another.get(data, pass).id, first.id) << "two passes asking alike were given two framebuffers";
			EXPECT_EQ(rhi.live(NullRHI::Kind::Framebuffer), before + 1);
			resources.targetsChanged();
			EXPECT_EQ(rhi.live(NullRHI::Kind::Framebuffer), before) << "a framebuffer outlived the targets' change";
			const auto second = targets.get(data, pass);
			EXPECT_NE(second.id, first.id) << "a framebuffer over the targets before was kept";
			EXPECT_EQ(rhi.live(NullRHI::Kind::Framebuffer), before + 1);
		}
		EXPECT_EQ(rhi.live(NullRHI::Kind::Framebuffer), before) << "the registry gone, a framebuffer it kept is still there";
	}
}
