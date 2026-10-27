#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "EngineTestFixture.h"
#include "core/FileSystem.h"
#include "renderModule/TerrainPatch.h"
#include "renderModule/UserFrameSet.h"

namespace SFE::EngineTests {
	using namespace SFE::Render;

	namespace {
		constexpr uint32_t kNone = 0xFFFFFFFFu;

		// An RHI that keeps what it is asked: the objects made and not yet freed, every descriptor write, every buffer
		// write and set bind, and how many calls it took. Ids are its own and never repeat.
		class RecordingRhi final : public RenderHardwareInterface {
		public:
			struct BufferWrite {
				uint32_t buffer = 0;
				size_t offset = 0;
				std::vector<uint8_t> bytes;
			};
			struct SetBind {
				uint32_t pipeline = 0;
				uint32_t firstSet = 0;
				uint32_t count = 0;
				uint32_t set = 0;
			};

			uint32_t frame = 0;
			size_t calls = 0;
			std::set<uint32_t> live;
			std::vector<DescriptorSetLayoutDesc> layouts;
			std::vector<WriteDescriptor> writes;
			std::vector<BufferWrite> bufferWrites;
			std::vector<SetBind> binds;
			std::vector<uint32_t> destroyedTextures;
			std::vector<uint32_t> destroyedSamplers;

			void init() override {}
			void cleanup() override {}
			GraphicsBackend backend() const override { return GraphicsBackend::None; }
			uint32_t frameInFlight() const override { return frame; }
			uint32_t framesInFlight() const override { return 2; }

			PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc&) override { return { make() }; }
			PipelineHandle createComputePipeline(const ComputePipelineDesc&) override { return { make() }; }
			void destroyGraphicsPipeline(PipelineHandle pipeline) override { release(pipeline.id); }
			TextureHandle createTexture(const TextureDesc&, const void* = nullptr) override { return { make() }; }
			void destroyTexture(TextureHandle texture) override {
				destroyedTextures.push_back(texture.id);
				release(texture.id);
			}

			SamplerHandle createSampler(const SamplerDesc&) override { return { make() }; }
			void destroySampler(SamplerHandle sampler) override {
				destroyedSamplers.push_back(sampler.id);
				release(sampler.id);
			}

			ShaderModuleHandle createShaderModule(const ShaderModuleDesc&) override { return { make() }; }
			void destroyShaderModule(ShaderModuleHandle module) override { release(module.id); }

			FramebufferHandle createFramebuffer(const FramebufferDesc&) override { return { make() }; }
			void destroyFramebuffer(FramebufferHandle framebuffer) override { release(framebuffer.id); }
			FramebufferHandle getBackbufferFramebuffer() override { ++calls; return {}; }

			BufferHandle createBuffer(const BufferDesc&, const void* = nullptr) override { return { make() }; }
			void destroyBuffer(BufferHandle buffer) override { release(buffer.id); }
			void updateBuffer(BufferHandle buffer, size_t offset = 0, size_t size = 0, const void* data = nullptr) override {
				++calls;
				const auto* bytes = static_cast<const uint8_t*>(data);
				bufferWrites.push_back({ buffer.id, offset, bytes ? std::vector<uint8_t>(bytes, bytes + size) : std::vector<uint8_t>{} });
			}
			void* mapBuffer(BufferHandle, MapMode) override { ++calls; return nullptr; }
			void unmapBuffer(BufferHandle) override { ++calls; }
			TransientAllocation allocTransient(size_t) override { ++calls; return {}; }
			void beginFrame() override { ++calls; }

			RenderPassHandle createRenderPass(const RenderPassDesc&) override { return { make() }; }
			void destroyRenderPass(RenderPassHandle renderPass) override { release(renderPass.id); }

			DescriptorSetLayoutHandle createDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override {
				layouts.push_back(desc);
				return { make() };
			}
			void destroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) override { release(layout.id); }

			DescriptorSetHandle allocateDescriptorSet(DescriptorSetLayoutHandle) override { return { make() }; }
			void freeDescriptorSet(DescriptorSetHandle set) override { release(set.id); }
			void updateDescriptorSets(const std::vector<WriteDescriptor>& given) override {
				++calls;
				writes.insert(writes.end(), given.begin(), given.end());
			}

			CommandListHandle beginCommandList(CommandListType) override { ++calls; return { make() }; }
			void endCommandList(CommandListHandle) override { ++calls; }

			void cmdBeginRenderPass(CommandListHandle, const RenderPassBeginInfo&) override { ++calls; }
			void cmdEndRenderPass(CommandListHandle) override { ++calls; }
			void cmdBindPipeline(CommandListHandle, PipelineHandle) override { ++calls; }
			void cmdBindVertexBuffer(CommandListHandle, uint32_t, BufferHandle, uint32_t = 0) override { ++calls; }
			void cmdBindIndexBuffer(CommandListHandle, BufferHandle, IndexType, uint32_t = 0) override { ++calls; }
			void cmdBindDescriptorSets(CommandListHandle, PipelineHandle pipeline, uint32_t firstSet, const DescriptorSetHandle* sets, uint32_t count,
				const uint32_t* = nullptr, uint32_t = 0) override
			{
				++calls;
				binds.push_back({ pipeline.id, firstSet, count, sets && count > 0 ? sets[0].id : kNone });
			}
			void cmdPushConstants(CommandListHandle, PipelineHandle, ShaderStage = ShaderStage::Vertex | ShaderStage::Fragment, const void* = nullptr, uint32_t = 0) override { ++calls; }

			void cmdDraw(CommandListHandle, uint32_t, uint32_t = 1, uint32_t = 0, uint32_t = 0) override { ++calls; }
			void cmdDrawIndexed(CommandListHandle, uint32_t, uint32_t = 1, uint32_t = 0, int32_t = 0, uint32_t = 0) override { ++calls; }
			void cmdDrawIndirect(CommandListHandle, BufferHandle, size_t = 0, uint32_t = 1, uint32_t = 16) override { ++calls; }
			void cmdDrawIndexedIndirect(CommandListHandle, BufferHandle, size_t = 0, uint32_t = 1, uint32_t = 20) override { ++calls; }
			void cmdDrawIndexedIndirectCount(CommandListHandle, BufferHandle, size_t, BufferHandle, size_t, uint32_t, uint32_t = 20) override { ++calls; }
			void cmdDispatch(CommandListHandle, uint32_t, uint32_t = 1, uint32_t = 1) override { ++calls; }

			void cmdBlit(CommandListHandle, FramebufferHandle, FramebufferHandle, BlitMask = BlitMask::Color, BlitFilter = BlitFilter::Nearest) override { ++calls; }
			void cmdPipelineBarrier(CommandListHandle, const PipelineBarrier&) override { ++calls; }
			void cmdUpdateBuffer(CommandListHandle, BufferHandle, size_t, size_t, const void*) override { ++calls; }
			void cmdCopyBuffer(CommandListHandle, BufferHandle, BufferHandle, size_t, size_t, size_t) override { ++calls; }

			void submit(CommandListHandle) override { ++calls; }
			void present() override { ++calls; }

			void updateTexture(TextureHandle, uint32_t, uint32_t, uint32_t, uint32_t, TextureFormat, const void*, uint32_t, uint32_t) override { ++calls; }
			void updateCubeFace(TextureHandle, uint32_t, uint32_t, const void*) override { ++calls; }
			void updateCubeMip(TextureHandle, uint32_t, const void*, size_t) override { ++calls; }
			uint64_t getNativeTextureId(TextureHandle texture) const override { return texture.id; }

		private:
			uint32_t make() {
				++calls;
				const uint32_t id = mNext++;
				live.insert(id);
				return id;
			}
			void release(const uint32_t id) {
				++calls;
				live.erase(id);
			}

			uint32_t mNext = 1000000;
		};

		UserFrameSet::Binding declared(const uint32_t binding, const UserFrameSet::Kind kind, const UserFrameSet::Fill fill = UserFrameSet::Fill::Zero,
			const uint32_t bytes = 0)
		{
			return { binding, kind, fill, bytes, "test" };
		}

		std::vector<uint32_t> idsOf(const GraphicsPipelineDesc& desc) {
			std::vector<uint32_t> ids;
			for (const auto layout : desc.descriptorSetLayouts) {
				ids.push_back(layout.id);
			}
			return ids;
		}

		size_t breaksIn(const std::string_view glsl) {
			return UserFrameSet::ruleBreaks("test.glsl", glsl).size();
		}
	}

	// Only 40-63, each once, one uniform block, and a block or buffer of whole vec4s; nothing once the set is made.
	TEST(UserFrameSet, DeclaresOnlyItsRange) {
		using Kind = UserFrameSet::Kind;
		UserFrameSet set;
		EXPECT_FALSE(set.declare(declared(39, Kind::Texture2D)));
		EXPECT_FALSE(set.declare(declared(64, Kind::Texture2D)));
		EXPECT_TRUE(set.declare(declared(63, Kind::StorageBuffer, UserFrameSet::Fill::Zero, 16)));
		EXPECT_TRUE(set.declare(declared(40, Kind::Texture2D)));
		EXPECT_FALSE(set.declare(declared(40, Kind::Texture2DArray))) << "declared already";
		EXPECT_TRUE(set.declare(declared(41, Kind::UniformBlock, UserFrameSet::Fill::Zero, 32)));
		EXPECT_FALSE(set.declare(declared(42, Kind::UniformBlock, UserFrameSet::Fill::Zero, 32))) << "a second block";
		EXPECT_FALSE(set.declare(declared(43, Kind::StorageBuffer, UserFrameSet::Fill::Zero, 0)));
		EXPECT_FALSE(set.declare(declared(43, Kind::StorageBuffer, UserFrameSet::Fill::Zero, 24)));
		EXPECT_FALSE(set.declare(declared(43, Kind::UniformBlock, UserFrameSet::Fill::Zero, 24)));

		RecordingRhi rhi;
		set.create(rhi, 1);
		EXPECT_FALSE(set.declare(declared(50, Kind::Texture2D))) << "after create";
		std::vector<uint32_t> bindings;
		for (const auto& binding : set.declared()) {
			bindings.push_back(binding.binding);
		}
		EXPECT_EQ(bindings, (std::vector<uint32_t>{ 40, 41, 63 })) << "in binding order";
		set.destroy();
	}

	// Every declared binding in the layout, in binding order, one descriptor each, given to every graphics stage.
	TEST(UserFrameSet, LayoutGivesEveryBindingToEveryGraphicsStage) {
		using Kind = UserFrameSet::Kind;
		UserFrameSet set;
		ASSERT_TRUE(set.declare(declared(43, Kind::UniformBlock, UserFrameSet::Fill::Zero, 176)));
		ASSERT_TRUE(set.declare(declared(40, Kind::Texture2D, UserFrameSet::Fill::White)));
		ASSERT_TRUE(set.declare(declared(42, Kind::StorageBuffer, UserFrameSet::Fill::Zero, 176)));
		ASSERT_TRUE(set.declare(declared(41, Kind::Texture2DArray)));
		RecordingRhi rhi;
		set.create(rhi, 2);

		const auto& bindings = set.layoutDesc().bindings;
		ASSERT_EQ(bindings.size(), 4u);
		const DescriptorType types[] = { DescriptorType::CombinedImageSampler, DescriptorType::CombinedImageSampler,
			DescriptorType::StorageBuffer, DescriptorType::UniformBuffer };
		for (size_t i = 0; i < bindings.size(); ++i) {
			EXPECT_EQ(bindings[i].binding, 40u + i);
			EXPECT_EQ(bindings[i].type, types[i]) << "binding " << bindings[i].binding;
			EXPECT_EQ(bindings[i].stageMask, ShaderStage::Vertex | ShaderStage::Fragment | ShaderStage::Geometry) << "binding " << bindings[i].binding;
			EXPECT_EQ(bindings[i].count, 1u);
		}
		// The RHI was asked for that layout and for one of nothing.
		ASSERT_EQ(rhi.layouts.size(), 2u);
		EXPECT_EQ(rhi.layouts[0].bindings.size(), 4u);
		EXPECT_TRUE(rhi.layouts[1].bindings.empty());
		set.destroy();
	}

	// A desc of 0-3 sets of its own takes the user layout at 3 and the empty one at the indices it leaves free, its holes
	// among them; extended twice it stays as it was, and one with a fourth set of its own, or extended before the set is
	// made, is left alone.
	TEST(UserFrameSet, ExtendPutsItAtSetThree) {
		{
			UserFrameSet unmade;
			GraphicsPipelineDesc desc;
			desc.descriptorSetLayouts = { { 900 } };
			unmade.extend(desc);
			EXPECT_EQ(idsOf(desc), (std::vector<uint32_t>{ 900 })) << "before create";
		}

		UserFrameSet set;
		ASSERT_TRUE(set.declare(declared(40, UserFrameSet::Kind::Texture2D)));
		RecordingRhi rhi;
		set.create(rhi, 1);
		const uint32_t pad = set.emptyLayout().id;
		const uint32_t user = set.layout().id;
		ASSERT_NE(pad, kNone);
		ASSERT_NE(user, kNone);
		ASSERT_NE(pad, user);

		for (uint32_t own = 0; own <= 3; ++own) {
			GraphicsPipelineDesc desc;
			for (uint32_t i = 0; i < own; ++i) {
				desc.descriptorSetLayouts.push_back({ 900 + i });
			}
			set.extend(desc);
			std::vector<uint32_t> expected;
			for (uint32_t i = 0; i < 3; ++i) {
				expected.push_back(i < own ? 900 + i : pad);
			}
			expected.push_back(user);
			EXPECT_EQ(idsOf(desc), expected) << own << " sets of its own";
			set.extend(desc);
			EXPECT_EQ(idsOf(desc), expected) << own << " sets of its own, extended twice";
		}

		GraphicsPipelineDesc holed;
		holed.descriptorSetLayouts = { { 900 }, {}, { 902 } };
		set.extend(holed);
		EXPECT_EQ(idsOf(holed), (std::vector<uint32_t>{ 900, pad, 902, user }));

		GraphicsPipelineDesc four;
		four.descriptorSetLayouts = { { 800 }, { 801 }, { 802 }, { 803 } };
		set.extend(four);
		EXPECT_EQ(idsOf(four), (std::vector<uint32_t>{ 800, 801, 802, 803 }));
		set.destroy();
	}

	// Each slot's set holds the fallbacks from its making, what is published from the next write of that slot on, keeps
	// it through writes with nothing new, and goes back to the fallback once it is unpublished.
	TEST(UserFrameSet, FallbackUntilPublishedAndKeptAfter) {
		using Kind = UserFrameSet::Kind;
		UserFrameSet set;
		ASSERT_TRUE(set.declare(declared(40, Kind::Texture2D, UserFrameSet::Fill::White)));
		ASSERT_TRUE(set.declare(declared(41, Kind::Texture2DArray)));
		ASSERT_TRUE(set.declare(declared(42, Kind::StorageBuffer, UserFrameSet::Fill::Zero, 16)));
		ASSERT_TRUE(set.declare(declared(43, Kind::UniformBlock, UserFrameSet::Fill::Zero, 16)));
		RecordingRhi rhi;
		set.create(rhi, 2);

		for (uint32_t slot = 0; slot < 2; ++slot) {
			for (uint32_t binding = 40; binding <= 43; ++binding) {
				EXPECT_NE(set.writtenId(slot, binding), kNone) << "slot " << slot << " binding " << binding;
			}
		}
		const uint32_t fallback = set.writtenId(0, 40);
		EXPECT_EQ(set.writtenId(1, 40), fallback) << "one texel a kind and fill, for every slot";
		EXPECT_NE(set.writtenId(0, 43), set.writtenId(1, 43)) << "a block buffer a slot";

		set.publish(40, TextureHandle{ 5000 }, SamplerHandle{ 5001 });
		rhi.frame = 0;
		set.write();
		EXPECT_EQ(set.writtenId(0, 40), 5000u);
		EXPECT_EQ(set.writtenId(1, 40), fallback) << "slot 1 is written in its own frame";
		rhi.frame = 1;
		set.write();
		EXPECT_EQ(set.writtenId(1, 40), 5000u);

		const size_t writes = rhi.writes.size();
		for (const uint32_t frame : { 0u, 1u, 0u }) {
			rhi.frame = frame;
			set.write();
		}
		EXPECT_EQ(rhi.writes.size(), writes) << "nothing published anew, nothing written";
		EXPECT_EQ(set.writtenId(0, 40), 5000u);
		EXPECT_EQ(set.writtenId(1, 40), 5000u);

		set.unpublish(40);
		rhi.frame = 0;
		set.write();
		EXPECT_EQ(set.writtenId(0, 40), fallback);

		set.publish(42, BufferHandle{ 6000 }, 16, 32);
		set.write();
		EXPECT_EQ(set.writtenId(0, 42), 6000u);
		ASSERT_FALSE(rhi.writes.empty());
		const auto& last = rhi.writes.back();
		EXPECT_EQ(last.binding, 42u);
		ASSERT_EQ(last.buffers.size(), 1u);
		EXPECT_EQ(last.buffers[0].offset, 16u);
		EXPECT_EQ(last.buffers[0].range, 32u);
		set.destroy();
	}

	// A binding not declared, or published as what it is not declared as -- a buffer on a texture, a texture on a buffer,
	// a buffer on the block -- changes nothing.
	TEST(UserFrameSet, WrongKindOrUndeclaredIsIgnored) {
		using Kind = UserFrameSet::Kind;
		UserFrameSet set;
		ASSERT_TRUE(set.declare(declared(40, Kind::Texture2D)));
		ASSERT_TRUE(set.declare(declared(42, Kind::StorageBuffer, UserFrameSet::Fill::Zero, 16)));
		ASSERT_TRUE(set.declare(declared(43, Kind::UniformBlock, UserFrameSet::Fill::Zero, 16)));
		RecordingRhi rhi;
		set.create(rhi, 1);
		const uint32_t texture = set.writtenId(0, 40);
		const uint32_t buffer = set.writtenId(0, 42);
		const uint32_t block = set.writtenId(0, 43);
		const size_t writes = rhi.writes.size();

		set.publish(41, TextureHandle{ 5000 }, SamplerHandle{ 5001 });
		set.publish(40, BufferHandle{ 6000 });
		set.publish(42, TextureHandle{ 5002 }, SamplerHandle{ 5001 });
		set.publish(43, BufferHandle{ 6001 });
		set.write();

		EXPECT_EQ(set.writtenId(0, 40), texture);
		EXPECT_EQ(set.writtenId(0, 42), buffer);
		EXPECT_EQ(set.writtenId(0, 43), block);
		EXPECT_EQ(set.writtenId(0, 41), kNone);
		EXPECT_EQ(rhi.writes.size(), writes);
		set.destroy();
	}

	// Two producers write their parts of the block: each keeps its bytes while the other writes again, a write past the
	// end changes nothing, and the whole block goes up at each write().
	TEST(UserFrameSet, BlockPartsKeepTheirBytes) {
		UserFrameSet set;
		ASSERT_TRUE(set.declare(declared(40, UserFrameSet::Kind::UniformBlock, UserFrameSet::Fill::Zero, 32)));
		const std::array<float, 4> first{ 1.f, 2.f, 3.f, 4.f };
		const std::array<float, 4> second{ 5.f, 6.f, 7.f, 8.f };
		const std::array<float, 4> again{ 9.f, 10.f, 11.f, 12.f };
		set.writeBlock(0, first.data(), sizeof(first));
		set.writeBlock(16, second.data(), sizeof(second));
		set.writeBlock(0, again.data(), sizeof(again));
		const std::array<float, 8> expected{ 9.f, 10.f, 11.f, 12.f, 5.f, 6.f, 7.f, 8.f };
		std::array<float, 8> read{};
		set.readBlock(0, read.data(), sizeof(read));
		EXPECT_EQ(read, expected);

		set.writeBlock(24, first.data(), sizeof(first));
		read = {};
		set.readBlock(0, read.data(), sizeof(read));
		EXPECT_EQ(read, expected) << "a write past the end";

		RecordingRhi rhi;
		set.create(rhi, 1);
		set.write();
		ASSERT_FALSE(rhi.bufferWrites.empty());
		const auto& upload = rhi.bufferWrites.back();
		EXPECT_EQ(upload.buffer, set.writtenId(0, 40));
		EXPECT_EQ(upload.offset, 0u);
		ASSERT_EQ(upload.bytes.size(), sizeof(expected));
		EXPECT_EQ(std::memcmp(upload.bytes.data(), expected.data(), sizeof(expected)), 0);
		set.destroy();
	}

	// Each frame slot's set, at set 3, in a call of its own.
	TEST(UserFrameSet, BoundAtSetThreeWithItsSlotsSet) {
		UserFrameSet set;
		ASSERT_TRUE(set.declare(declared(40, UserFrameSet::Kind::Texture2D)));
		RecordingRhi rhi;
		set.create(rhi, 2);
		// create() wrote slot 0's set, then slot 1's.
		ASSERT_EQ(rhi.writes.size(), 2u);
		const uint32_t sets[] = { rhi.writes[0].set.id, rhi.writes[1].set.id };
		ASSERT_NE(sets[0], sets[1]);
		for (const uint32_t frame : { 0u, 1u, 2u }) {
			rhi.frame = frame;
			set.bind(CommandListHandle{ 1 }, PipelineHandle{ 77 });
			ASSERT_FALSE(rhi.binds.empty());
			const auto& bind = rhi.binds.back();
			EXPECT_EQ(bind.pipeline, 77u);
			EXPECT_EQ(bind.firstSet, UserFrameSet::kSet);
			EXPECT_EQ(bind.count, 1u);
			EXPECT_EQ(bind.set, sets[frame % 2]) << "frame " << frame;
		}
		set.destroy();
	}

	// destroy() frees whatever create() made, and the declarations stay for the next create().
	TEST(UserFrameSet, DestroyFreesEverythingItMade) {
		using Kind = UserFrameSet::Kind;
		UserFrameSet set;
		ASSERT_TRUE(set.declare(declared(40, Kind::Texture2D, UserFrameSet::Fill::White)));
		ASSERT_TRUE(set.declare(declared(41, Kind::Texture2D)));
		ASSERT_TRUE(set.declare(declared(42, Kind::Texture2DArray)));
		ASSERT_TRUE(set.declare(declared(43, Kind::StorageBuffer, UserFrameSet::Fill::Zero, 16)));
		ASSERT_TRUE(set.declare(declared(44, Kind::StorageBuffer, UserFrameSet::Fill::Zero, 32)));
		ASSERT_TRUE(set.declare(declared(45, Kind::UniformBlock, UserFrameSet::Fill::Zero, 16)));
		RecordingRhi rhi;
		set.create(rhi, 2);
		EXPECT_TRUE(set.created());
		// Two layouts, a sampler, three texels, two zero buffers, and a set and a block buffer for each slot.
		EXPECT_EQ(set.ownedObjects(), 12u);
		EXPECT_EQ(set.ownedObjects(), rhi.live.size());

		set.destroy();
		EXPECT_FALSE(set.created());
		EXPECT_EQ(set.ownedObjects(), 0u);
		EXPECT_TRUE(rhi.live.empty()) << rhi.live.size() << " left";
		EXPECT_EQ(set.declared().size(), 6u);
	}

	// RenderSystem made the engine's before its passes.
	TEST(UserFrameSet, TheEngineMadeOne) {
		const auto& set = UserFrameSet::instance();
		EXPECT_TRUE(set.created());
		EXPECT_TRUE(set.layout());
		EXPECT_TRUE(set.emptyLayout());
		EXPECT_EQ(set.layoutDesc().bindings.size(), set.declared().size());
	}

	// What a producer does as it goes after the set -- unpublish, even publish -- asks nothing of the RHI.
	TEST(UserFrameSet, UnpublishAfterDestroyIsHarmless) {
		UserFrameSet set;
		ASSERT_TRUE(set.declare(declared(40, UserFrameSet::Kind::Texture2D)));
		ASSERT_TRUE(set.declare(declared(41, UserFrameSet::Kind::UniformBlock, UserFrameSet::Fill::Zero, 16)));
		RecordingRhi rhi;
		set.create(rhi, 1);
		set.destroy();
		const size_t calls = rhi.calls;

		set.unpublish(40);
		set.publish(40, TextureHandle{ 5000 }, SamplerHandle{ 5001 });
		const std::array<float, 4> bytes{ 1.f, 2.f, 3.f, 4.f };
		set.writeBlock(0, bytes.data(), sizeof(bytes));
		set.write();
		set.bind(CommandListHandle{ 1 }, PipelineHandle{ 2 });
		set.unpublish(40);

		EXPECT_EQ(rhi.calls, calls);
		EXPECT_EQ(set.writtenId(0, 40), kNone);
	}

	// 40-63 are set 3's and set 3 has those alone; an array takes a binding an element, a block's members none.
	TEST(ShaderBindingRule, RuleBreaksOnText) {
		EXPECT_EQ(breaksIn("layout(SFE_SET(3, 39)) uniform sampler2D a;"), 1u);
		EXPECT_EQ(breaksIn("layout(SFE_SET(0, 40)) uniform sampler2D a;"), 1u);
		EXPECT_EQ(breaksIn("layout(SFE_SET(3, 64)) uniform sampler2D a;"), 1u);
		EXPECT_EQ(breaksIn("layout(SFE_SET(3, 40)) uniform sampler2D a;"), 0u);
		EXPECT_EQ(breaksIn("layout(SFE_SET(3, 63), std140) uniform B { vec4 v; } b;"), 0u);
		EXPECT_EQ(breaksIn("layout(SFE_SET(0, 37), std430) readonly buffer W { uvec4 w[]; };"), 0u);
		EXPECT_EQ(breaksIn("#define SFE_SET(setIdx, bindingIdx) set = setIdx, binding = bindingIdx"), 0u);

		EXPECT_EQ(breaksIn("SFE_SET(0, 36)) uniform sampler2D x[8];"), 1u) << "37-43 reach 40-43";
		EXPECT_EQ(breaksIn("SFE_SET(0, 30)) uniform sampler2D x[8];"), 0u);
		EXPECT_EQ(breaksIn("layout(SFE_SET(3, 60)) uniform sampler2D x[8];"), 1u) << "61-67 run past 63";
		EXPECT_EQ(breaksIn("SFE_SET(0, 38), std430) buffer B { T t[16]; };"), 0u) << "a block's array takes 38 alone";

		EXPECT_EQ(breaksIn("layout(binding = 41) uniform sampler2D a;"), 1u) << "set 0 without a set";
		EXPECT_EQ(breaksIn("layout(set = 3, binding = 41) uniform sampler2D a;"), 0u);
		EXPECT_EQ(breaksIn("layout(std140, binding = 15) uniform Push { vec4 v; };"), 0u);

		EXPECT_EQ(breaksIn("// layout(SFE_SET(0, 40)) uniform sampler2D a;\n/* SFE_SET(0, 41) */"), 0u) << "commented out";
		EXPECT_EQ(breaksIn("#define SFE_X SFE_SET(0, 41)\nlayout(SFE_X) uniform sampler2D a[4];"), 1u) << "a macro's declaration, once";

		const auto lines = UserFrameSet::ruleBreaks("a.fs", "\n\nlayout(SFE_SET(0, 40)) uniform sampler2D a;");
		ASSERT_EQ(lines.size(), 1u);
		EXPECT_EQ(lines[0].rfind("a.fs:3: SFE_SET(0, 40)", 0), 0u) << lines[0];
	}

	// The engine's own shaders keep the rule, and leave set 3 to the application altogether.
	TEST(ShaderBindingRule, EngineShadersKeepTheUserSetToItself) {
		const auto folder = FileSystem::root() / "shaders";
		ASSERT_TRUE(std::filesystem::is_directory(folder)) << folder.string();
		const std::regex inSetThree(R"(SFE_SET\s*\(\s*3\s*,|\bset\s*=\s*3\b)");
		size_t read = 0;
		std::string breaks;
		std::string setThree;
		for (const auto& entry : std::filesystem::directory_iterator(folder)) {
			if (!entry.is_regular_file()) {
				continue;
			}
			const auto extension = entry.path().extension().string();
			if (extension != ".vs" && extension != ".fs" && extension != ".gs" && extension != ".cs" && extension != ".glsl") {
				continue;
			}
			std::ifstream in(entry.path(), std::ios::binary);
			const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			++read;
			for (const auto& line : UserFrameSet::ruleBreaks(entry.path().filename().string(), text)) {
				breaks += line + "\n";
			}
			if (std::regex_search(text, inSetThree)) {
				setThree += entry.path().filename().string() + "\n";
			}
		}
		EXPECT_GT(read, 50u) << "the engine's shaders were not found in " << folder.string();
		EXPECT_TRUE(breaks.empty()) << breaks;
		EXPECT_TRUE(setThree.empty()) << "declare in set 3:\n" << setThree;
	}

	// The patch published is kept until the next, its heights read with the nearest sampler, the texel of 0 without
	// them; destroyTerrainPatch frees the sampler and the texel alone, never the heights, which are their producer's.
	TEST(TerrainPatch, KeptUntilPublishedAgain) {
		auto* engineRhi = Engine::instance()->rhi();
		RecordingRhi rhi;
		// The engine's put back as RenderSystem made it, whatever happens below.
		Finally restore([engineRhi] {
			destroyTerrainPatch();
			publishTerrainPatch({});
			createTerrainPatch(*engineRhi);
		});

		const auto none = terrainPatchHeights();
		EXPECT_TRUE(none.first);
		EXPECT_TRUE(none.second);

		TerrainPatch patch;
		patch.heights = TextureHandle{ 777777 };
		patch.grid = { 1.f, 2.f, 3.f, 4.f };
		patch.square = { 5.f, 6.f, 7.f, 8.f };
		patch.deepest = 0.3f;
		publishTerrainPatch(patch);
		EXPECT_EQ(terrainPatch().heights.id, 777777u);
		EXPECT_EQ(terrainPatch().grid.w, 4.f);
		EXPECT_EQ(terrainPatch().square.z, 7.f);
		EXPECT_EQ(terrainPatch().deepest, 0.3f);
		EXPECT_EQ(terrainPatchHeights().first.id, 777777u);
		EXPECT_EQ(terrainPatchHeights().second.id, none.second.id);
		publishTerrainPatch({});
		EXPECT_EQ(terrainPatchHeights().first.id, none.first.id) << "nothing published: the texel of 0";

		destroyTerrainPatch();
		createTerrainPatch(rhi);
		const auto own = terrainPatchHeights();
		publishTerrainPatch(patch);
		destroyTerrainPatch();
		EXPECT_TRUE(rhi.live.empty()) << "the sampler and the texel freed";
		EXPECT_EQ(rhi.destroyedTextures, (std::vector<uint32_t>{ own.first.id })) << "the heights are their producer's";
		EXPECT_EQ(rhi.destroyedSamplers, (std::vector<uint32_t>{ own.second.id }));
		EXPECT_FALSE(terrainPatch().heights);
		EXPECT_EQ(terrainPatch().square.z, 0.f);
	}
}
