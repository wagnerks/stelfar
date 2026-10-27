#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	// The application's frame data: descriptor set 3 of every pipeline that takes it, bindings 40-63, declared by the
	// application before the renderer is made (2D textures, 2D array textures, storage buffers and one uniform block).
	// A pass whose shaders compile an application's hook (sfe_user_*.glsl) or read the set lays it out (extend) and binds
	// it (bind); producers publish into it in their prepare(); RenderSystem makes, writes and frees it. On OpenGL the set
	// index is dropped, so 40-63 belong to this set alone in every shader (ruleBreaks).
	class UserFrameSet {
	public:
		static constexpr uint32_t kSet = 3;
		static constexpr uint32_t kFirstBinding = 40;
		static constexpr uint32_t kLastBinding = 63;

		enum class Kind : uint8_t { Texture2D, Texture2DArray, StorageBuffer, UniformBlock };
		// What a texture binding holds while nothing is published: one texel (one layer) of RGBA8 0 or 255.
		enum class Fill : uint8_t { Zero, White };
		struct Binding {
			uint32_t binding = 0;
			Kind kind = Kind::Texture2D;
			Fill fill = Fill::Zero;  // textures only
			uint32_t bytes = 0;      // UniformBlock: the block's size; StorageBuffer: its zero fallback's size; a multiple of 16
			const char* name = "";   // for messages
		};

		// The one RenderSystem makes and the application declares on.
		static UserFrameSet& instance();

		// Before the renderer is made, main thread. False, logged (LOG_ERROR "UserFrameSet: ..."), for a binding outside
		// 40-63, one declared already, a second UniformBlock, a block or buffer of 0 bytes or not a multiple of 16, or
		// after create(). The block's CPU copy (zeros) exists from its declaration on.
		bool declare(const Binding& binding);
		const std::vector<Binding>& declared() const; // in binding order

		// RenderSystem's constructor, before any pass is made: the layout (every declared binding, stages Vertex |
		// Fragment | Geometry), an empty layout for the indices below kSet, one sampler (linear, clamp) and one fallback
		// per (texture kind, fill), one zero buffer per StorageBuffer binding, and per frame slot a set and a block
		// buffer (Uniform, CpuAccess::Write); every binding of every slot's set written with its fallback (a block binding
		// with its slot's buffer). Main thread.
		void create(RenderHardwareInterface& rhi, uint32_t frameSlots);
		// ~RenderSystem, before the RHI goes: everything create() made; declarations and the block's bytes stay.
		void destroy();
		bool created() const;

		// A pass's init, before it creates a pipeline from `desc`: the empty layout at every index below kSet the desc
		// leaves free, the user layout at kSet. A desc that already has it there is left as it is; one with another
		// layout at kSet or more, or a call before create(), is logged and left as it is.
		void extend(GraphicsPipelineDesc& desc) const;
		// After the pass has bound its own sets for a draw of a pipeline made from an extended desc (Vulkan keeps a
		// higher set only while the lower ones are bound with a compatible layout, so after, not before): this frame
		// slot's set at kSet, a call of its own (firstSet 3, count 1, no dynamic offsets). Nothing before create().
		void bind(CommandListHandle cmd, PipelineHandle pipeline) const;

		// Producers, in a pass's prepare(), main thread: what `binding` holds from now on, until published again or
		// unpublished. A binding not declared, or of another kind, is logged once and ignored. Both only record what is
		// published (write() acts on it), so they work before create() and after destroy() too: a producer that frees
		// what it published unpublishes it first, in its destructor as well.
		void publish(uint32_t binding, TextureHandle texture, SamplerHandle sampler);
		void publish(uint32_t binding, BufferHandle buffer, size_t offset = 0, size_t range = 0);
		void unpublish(uint32_t binding); // back to its fallback
		// The block's bytes [offset, offset + bytes) in its CPU copy, uploaded whole once a frame by write(): each
		// producer writes its own part, and a part nobody writes again keeps its last bytes. Out of range or no block
		// declared: logged, ignored.
		void writeBlock(size_t offset, const void* data, size_t bytes);
		void readBlock(size_t offset, void* out, size_t bytes) const;

		// RenderSystem, after every pass's prepare and before the frame's commands: this slot's block buffer given the
		// CPU copy, and this slot's set given what each binding holds now, a binding rewritten only when that changed.
		void write();

		// For tests: the texture's or buffer's id slot `slot`'s set was last written with at `binding` (0xFFFFFFFF none);
		// the layouts; the layout's desc; the RHI objects create() made that destroy() has not freed.
		uint32_t writtenId(uint32_t slot, uint32_t binding) const;
		DescriptorSetLayoutHandle layout() const;
		DescriptorSetLayoutHandle emptyLayout() const;
		const DescriptorSetLayoutDesc& layoutDesc() const;
		size_t ownedObjects() const;

		// The rule every shader's text keeps (OpenGL drops the set index, so 40-63 must be set 3's alone): every binding
		// a declaration takes is in set 3 exactly when it is in 40-63. A declaration is SFE_SET(s, b), or a raw
		// layout(... binding = b ...) with an optional set = s (set 0 without); it takes b to b + N - 1 when it declares
		// an array `name[N]` before its `;` and outside a block's braces (OpenGL gives an array's elements units
		// b..b+N-1, OpenGLRHI.cpp:1606-1612), else b alone. One line per break, "<file>:<line>: SFE_SET(s, b) ...". A
		// declaration whose set or binding is not a number (sfe.glsl's macro itself) is skipped.
		static std::vector<std::string> ruleBreaks(std::string_view file, std::string_view glsl);

	private:
		static constexpr size_t kBindings = kLastBinding - kFirstBinding + 1;
		static constexpr uint32_t kNone = 0xFFFFFFFFu;

		// What a binding holds, published or its fallback: a texture with its sampler, or a buffer's range.
		struct Held {
			uint32_t id = kNone;
			uint32_t sampler = kNone;
			size_t offset = 0;
			size_t range = 0;
			bool operator==(const Held&) const = default;
		};
		struct Published {
			bool texture = false; // which of the two publish() calls gave it
			bool buffer = false;
			TextureHandle image{};
			SamplerHandle sampler{};
			BufferHandle data{};
			size_t offset = 0;
			size_t range = 0;
		};

		const Binding* find(uint32_t binding) const;
		// What `binding` is to hold in slot `slot`'s set: what is published, else its fallback.
		Held target(const Binding& binding, uint32_t slot, bool fallbackOnly) const;
		void writeSlot(uint32_t slot, bool fallbackOnly);
		uint32_t slot() const;
		// LOG_ERROR once per (binding, what).
		void sayOnce(uint32_t binding, int what, const char* message) const;

		std::vector<Binding> mDeclared;
		std::array<Published, kBindings> mPublished{};
		std::vector<uint8_t> mBlock; // the uniform block's CPU copy
		uint32_t mBlockBinding = kNone;

		RenderHardwareInterface* mRhi = nullptr;
		uint32_t mSlots = 0;
		DescriptorSetLayoutDesc mLayoutDesc;
		DescriptorSetLayoutHandle mLayout{};
		DescriptorSetLayoutHandle mEmptyLayout{};
		SamplerHandle mSampler{};
		std::array<std::array<TextureHandle, 2>, 2> mFallbacks{}; // [Texture2D, Texture2DArray][Zero, White]
		std::array<BufferHandle, kBindings> mZeroBuffers{};       // a StorageBuffer binding's fallback
		std::vector<DescriptorSetHandle> mSets;                    // a frame slot's
		std::vector<BufferHandle> mBlockBuffers;                   // a frame slot's
		std::vector<std::array<Held, kBindings>> mWritten;         // a frame slot's set, as last written
		mutable std::set<std::pair<uint32_t, int>> mSaid;
	};
}
