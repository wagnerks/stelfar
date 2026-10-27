#include "renderModule/UserFrameSet.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>

#include "logsModule/logger.h"

namespace SFE::Render {
	namespace {
		bool isTexture(const UserFrameSet::Kind kind) {
			return kind == UserFrameSet::Kind::Texture2D || kind == UserFrameSet::Kind::Texture2DArray;
		}

		DescriptorType typeOf(const UserFrameSet::Kind kind) {
			switch (kind) {
			case UserFrameSet::Kind::StorageBuffer: return DescriptorType::StorageBuffer;
			case UserFrameSet::Kind::UniformBlock: return DescriptorType::UniformBuffer;
			default: return DescriptorType::CombinedImageSampler;
			}
		}

		// One texel of RGBA8, every channel `value`: a 2D texture given it at its making, an array's one layer written
		// after it (an array's initial data is not taken on Vulkan, VulkanRHIResources.cpp createImage).
		TextureHandle makeTexel(RenderHardwareInterface& rhi, const bool array, const uint8_t value) {
			TextureDesc desc;
			desc.width = 1;
			desc.height = 1;
			desc.format = TextureFormat::RGBA8;
			desc.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
			const uint8_t texel[4] = { value, value, value, value };
			if (!array) {
				return rhi.createTexture(desc, texel);
			}
			desc.dimension = TextureDimension::Array;
			const auto texture = rhi.createTexture(desc);
			rhi.updateTexture(texture, 0, 0, 1, 1, TextureFormat::RGBA8, texel);
			return texture;
		}

		bool identChar(const char c) {
			return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
		}

		// The text with its comments blanked and every line end kept: a declaration commented out declares nothing, and
		// every place keeps its line.
		std::string withoutComments(const std::string_view glsl) {
			std::string text(glsl);
			size_t i = 0;
			while (i < text.size()) {
				if (text.compare(i, 2, "//") == 0) {
					while (i < text.size() && text[i] != '\n') {
						text[i++] = ' ';
					}
				}
				else if (text.compare(i, 2, "/*") == 0) {
					const size_t end = text.find("*/", i + 2);
					const size_t stop = end == std::string::npos ? text.size() : end + 2;
					for (; i < stop; ++i) {
						if (text[i] != '\n') {
							text[i] = ' ';
						}
					}
				}
				else {
					++i;
				}
			}
			return text;
		}

		std::optional<uint32_t> number(std::string_view text) {
			while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
				text.remove_prefix(1);
			}
			while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
				text.remove_suffix(1);
			}
			if (text.empty() || text.size() > 9) {
				return std::nullopt;
			}
			uint32_t value = 0;
			for (const char c : text) {
				if (!std::isdigit(static_cast<unsigned char>(c))) {
					return std::nullopt;
				}
				value = value * 10 + static_cast<uint32_t>(c - '0');
			}
			return value;
		}

		// `key = value` inside a layout's parentheses: whether the key is there, and its value when it is a number.
		struct Assigned {
			bool present = false;
			std::optional<uint32_t> value;
		};
		Assigned assigned(const std::string_view content, const std::string_view key) {
			for (size_t at = content.find(key); at != std::string_view::npos; at = content.find(key, at + 1)) {
				const size_t end = at + key.size();
				if ((at > 0 && identChar(content[at - 1])) || (end < content.size() && identChar(content[end]))) {
					continue;
				}
				size_t i = end;
				while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i]))) {
					++i;
				}
				if (i >= content.size() || content[i] != '=') {
					continue;
				}
				++i;
				while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i]))) {
					++i;
				}
				size_t digits = i;
				while (digits < content.size() && identChar(content[digits])) {
					++digits;
				}
				return { true, number(content.substr(i, digits - i)) };
			}
			return {};
		}

		// Whether the line `at` is on is a preprocessor directive: a declaration there ends with the line.
		bool onDirective(const std::string& text, const size_t at) {
			size_t start = at == 0 ? std::string::npos : text.rfind('\n', at - 1);
			start = start == std::string::npos ? 0 : start + 1;
			while (start < at && (text[start] == ' ' || text[start] == '\t')) {
				++start;
			}
			return start < text.size() && text[start] == '#';
		}

		// How many bindings a declaration whose head ends at `from` takes: N for an array `name[N]` before its `;` and
		// outside a block's braces, else 1.
		uint32_t bindingsTaken(const std::string& text, const size_t from, const bool directive) {
			int depth = 0;
			for (size_t i = from; i < text.size(); ++i) {
				const char c = text[i];
				if (c == '\n' && directive) {
					size_t k = i;
					while (k > from && text[k - 1] == '\r') {
						--k;
					}
					if (k > from && text[k - 1] == '\\') {
						continue; // the directive goes on on the next line
					}
					return 1;
				}
				if (c == '{') {
					++depth;
				}
				else if (c == '}') {
					depth = (std::max)(0, depth - 1);
				}
				else if (c == ';' && depth == 0) {
					return 1;
				}
				else if (c == '[' && depth == 0) {
					const size_t close = text.find(']', i);
					if (close == std::string::npos) {
						return 1;
					}
					const auto length = number(std::string_view(text).substr(i + 1, close - i - 1));
					return length && *length > 0 ? (std::min)(*length, 4096u) : 1u;
				}
			}
			return 1;
		}

		struct Declaration {
			size_t at = 0;
			uint32_t set = 0;
			uint32_t binding = 0;
			uint32_t count = 1;
		};
	}

	UserFrameSet& UserFrameSet::instance() {
		static UserFrameSet set;
		return set;
	}

	const UserFrameSet::Binding* UserFrameSet::find(const uint32_t binding) const {
		const auto it = std::lower_bound(mDeclared.begin(), mDeclared.end(), binding,
			[](const Binding& declared, const uint32_t number) { return declared.binding < number; });
		return it != mDeclared.end() && it->binding == binding ? &*it : nullptr;
	}

	void UserFrameSet::sayOnce(const uint32_t binding, const int what, const char* message) const {
		if (!mSaid.insert({ binding, what }).second) {
			return;
		}
		if (binding == kNone) {
			LogsModule::Logger::LOG_ERROR("UserFrameSet: %s", message);
		}
		else {
			LogsModule::Logger::LOG_ERROR("UserFrameSet: binding %u: %s", binding, message);
		}
	}

	bool UserFrameSet::declare(const Binding& binding) {
		const auto refuse = [&](const char* why) {
			LogsModule::Logger::LOG_ERROR("UserFrameSet: binding %u (%s) not declared: %s", binding.binding, binding.name ? binding.name : "", why);
			return false;
		};
		if (created()) {
			return refuse("the set is made already; an application declares before the renderer is made");
		}
		if (binding.binding < kFirstBinding || binding.binding > kLastBinding) {
			return refuse("outside 40-63");
		}
		if (find(binding.binding)) {
			return refuse("declared already");
		}
		if (binding.kind == Kind::UniformBlock && mBlockBinding != kNone) {
			return refuse("a second uniform block; the set has one, its producers writing their parts of it");
		}
		if ((binding.kind == Kind::UniformBlock || binding.kind == Kind::StorageBuffer) && (binding.bytes == 0 || binding.bytes % 16 != 0)) {
			return refuse("its bytes are 0 or not a multiple of 16");
		}
		const auto at = std::lower_bound(mDeclared.begin(), mDeclared.end(), binding.binding,
			[](const Binding& declared, const uint32_t number) { return declared.binding < number; });
		mDeclared.insert(at, binding);
		if (binding.kind == Kind::UniformBlock) {
			mBlockBinding = binding.binding;
			mBlock.assign(binding.bytes, 0);
		}
		return true;
	}

	const std::vector<UserFrameSet::Binding>& UserFrameSet::declared() const {
		return mDeclared;
	}

	void UserFrameSet::create(RenderHardwareInterface& rhi, const uint32_t frameSlots) {
		if (created()) {
			LogsModule::Logger::LOG_ERROR("UserFrameSet: made a second time; the first stands");
			return;
		}
		mRhi = &rhi;
		mSlots = (std::max)(1u, frameSlots);

		mLayoutDesc.bindings.clear();
		for (const auto& binding : mDeclared) {
			mLayoutDesc.bindings.push_back({ binding.binding, typeOf(binding.kind), ShaderStage::Vertex | ShaderStage::Fragment | ShaderStage::Geometry });
		}
		mLayout = rhi.createDescriptorSetLayout(mLayoutDesc);
		mEmptyLayout = rhi.createDescriptorSetLayout(DescriptorSetLayoutDesc{});
		mSampler = rhi.createSampler(SamplerDesc{});

		// The fallbacks the declarations ask for: one texel a (kind, fill), one zero buffer a storage binding.
		for (const auto& binding : mDeclared) {
			const size_t index = binding.binding - kFirstBinding;
			if (isTexture(binding.kind)) {
				const bool array = binding.kind == Kind::Texture2DArray;
				auto& fallback = mFallbacks[array ? 1 : 0][binding.fill == Fill::White ? 1 : 0];
				if (!fallback) {
					fallback = makeTexel(rhi, array, static_cast<uint8_t>(binding.fill == Fill::White ? 255 : 0));
				}
			}
			else if (binding.kind == Kind::StorageBuffer) {
				BufferDesc desc;
				desc.size = binding.bytes;
				desc.usage = BufferUsage::Storage;
				desc.cpuAccess = CpuAccess::None;
				desc.debugName = "UserFrameSet fallback";
				const std::vector<uint8_t> zeros(binding.bytes, 0);
				mZeroBuffers[index] = rhi.createBuffer(desc, zeros.data());
			}
		}

		mSets.assign(mSlots, {});
		mBlockBuffers.assign(mSlots, {});
		mWritten.assign(mSlots, {});
		for (uint32_t slot = 0; slot < mSlots; ++slot) {
			mSets[slot] = rhi.allocateDescriptorSet(mLayout);
			if (mBlockBinding != kNone) {
				BufferDesc desc;
				desc.size = mBlock.size();
				desc.usage = BufferUsage::Uniform;
				desc.cpuAccess = CpuAccess::Write;
				desc.debugName = "UserFrameSet block";
				mBlockBuffers[slot] = rhi.createBuffer(desc, mBlock.data());
			}
		}
		for (uint32_t slot = 0; slot < mSlots; ++slot) {
			writeSlot(slot, true);
		}
	}

	void UserFrameSet::destroy() {
		if (!created()) {
			return;
		}
		for (const auto set : mSets) {
			if (set) {
				mRhi->freeDescriptorSet(set);
			}
		}
		for (const auto buffer : mBlockBuffers) {
			if (buffer) {
				mRhi->destroyBuffer(buffer);
			}
		}
		for (auto& buffer : mZeroBuffers) {
			if (buffer) {
				mRhi->destroyBuffer(buffer);
			}
			buffer = {};
		}
		for (auto& kind : mFallbacks) {
			for (auto& fallback : kind) {
				if (fallback) {
					mRhi->destroyTexture(fallback);
				}
				fallback = {};
			}
		}
		if (mSampler) {
			mRhi->destroySampler(mSampler);
		}
		if (mLayout) {
			mRhi->destroyDescriptorSetLayout(mLayout);
		}
		if (mEmptyLayout) {
			mRhi->destroyDescriptorSetLayout(mEmptyLayout);
		}
		mSampler = {};
		mLayout = {};
		mEmptyLayout = {};
		mLayoutDesc = {};
		mSets.clear();
		mBlockBuffers.clear();
		mWritten.clear();
		mSlots = 0;
		mRhi = nullptr;
	}

	bool UserFrameSet::created() const {
		return mRhi != nullptr;
	}

	void UserFrameSet::extend(GraphicsPipelineDesc& desc) const {
		if (!created()) {
			LogsModule::Logger::LOG_ERROR("UserFrameSet: a pipeline extended before the set was made; it is left without set 3");
			return;
		}
		auto& layouts = desc.descriptorSetLayouts;
		if (layouts.size() > kSet) {
			if (layouts[kSet].id != mLayout.id) {
				LogsModule::Logger::LOG_ERROR("UserFrameSet: a pipeline with %zu sets of its own cannot take set 3; it is left as it was",
					layouts.size());
			}
			return;
		}
		// The indices it leaves free, its own sets' holes among them, padded with the layout of nothing.
		for (auto& layout : layouts) {
			if (!layout) {
				layout = mEmptyLayout;
			}
		}
		layouts.resize(kSet, mEmptyLayout);
		layouts.push_back(mLayout);
	}

	uint32_t UserFrameSet::slot() const {
		return mRhi->frameInFlight() % mSlots;
	}

	void UserFrameSet::bind(const CommandListHandle cmd, const PipelineHandle pipeline) const {
		if (!created()) {
			return;
		}
		const auto set = mSets[slot()];
		mRhi->cmdBindDescriptorSets(cmd, pipeline, kSet, &set, 1);
	}

	void UserFrameSet::publish(const uint32_t binding, const TextureHandle texture, const SamplerHandle sampler) {
		const auto* declared = find(binding);
		if (!declared) {
			sayOnce(binding, 0, "published, but not declared; ignored");
			return;
		}
		if (!isTexture(declared->kind)) {
			sayOnce(binding, 1, "a texture published on a binding declared as a buffer; ignored");
			return;
		}
		// A dead handle published is nothing published: the binding keeps a valid descriptor, its fallback.
		auto& published = mPublished[binding - kFirstBinding];
		published = {};
		published.texture = static_cast<bool>(texture);
		published.image = texture;
		published.sampler = sampler;
	}

	void UserFrameSet::publish(const uint32_t binding, const BufferHandle buffer, const size_t offset, const size_t range) {
		const auto* declared = find(binding);
		if (!declared) {
			sayOnce(binding, 0, "published, but not declared; ignored");
			return;
		}
		// The block's buffers are the set's own: its producers write their parts of it (writeBlock).
		if (declared->kind != Kind::StorageBuffer) {
			sayOnce(binding, 1, "a buffer published on a binding not declared as a storage buffer; ignored");
			return;
		}
		auto& published = mPublished[binding - kFirstBinding];
		published = {};
		published.buffer = static_cast<bool>(buffer);
		published.data = buffer;
		published.offset = offset;
		published.range = range;
	}

	void UserFrameSet::unpublish(const uint32_t binding) {
		if (!find(binding)) {
			sayOnce(binding, 0, "unpublished, but not declared; ignored");
			return;
		}
		mPublished[binding - kFirstBinding] = {};
	}

	void UserFrameSet::writeBlock(const size_t offset, const void* data, const size_t bytes) {
		if (mBlockBinding == kNone) {
			sayOnce(kNone, 3, "the block written, but none is declared; ignored");
			return;
		}
		if (!data || offset > mBlock.size() || bytes > mBlock.size() - offset) {
			sayOnce(mBlockBinding, 2, "a write past the block's end; ignored");
			return;
		}
		std::memcpy(mBlock.data() + offset, data, bytes);
	}

	void UserFrameSet::readBlock(const size_t offset, void* out, const size_t bytes) const {
		if (!out) {
			return;
		}
		if (mBlockBinding == kNone || offset > mBlock.size() || bytes > mBlock.size() - offset) {
			sayOnce(mBlockBinding, 4, "a read past the block's end, or of no block: zeros");
			std::memset(out, 0, bytes);
			return;
		}
		std::memcpy(out, mBlock.data() + offset, bytes);
	}

	UserFrameSet::Held UserFrameSet::target(const Binding& binding, const uint32_t slot, const bool fallbackOnly) const {
		const size_t index = binding.binding - kFirstBinding;
		const auto& published = mPublished[index];
		if (binding.kind == Kind::UniformBlock) {
			return { mBlockBuffers[slot].id };
		}
		if (binding.kind == Kind::StorageBuffer) {
			if (!fallbackOnly && published.buffer) {
				return { published.data.id, kNone, published.offset, published.range };
			}
			return { mZeroBuffers[index].id };
		}
		if (!fallbackOnly && published.texture) {
			// Read with the set's own sampler when it came without one: Vulkan takes no image without a sampler.
			return { published.image.id, published.sampler ? published.sampler.id : mSampler.id };
		}
		const bool array = binding.kind == Kind::Texture2DArray;
		return { mFallbacks[array ? 1 : 0][binding.fill == Fill::White ? 1 : 0].id, mSampler.id };
	}

	void UserFrameSet::writeSlot(const uint32_t slot, const bool fallbackOnly) {
		std::vector<WriteDescriptor> writes;
		auto& written = mWritten[slot];
		for (const auto& binding : mDeclared) {
			const size_t index = binding.binding - kFirstBinding;
			const Held held = target(binding, slot, fallbackOnly);
			if (written[index] == held) {
				continue;
			}
			written[index] = held;
			WriteDescriptor write{ .set = mSets[slot], .binding = binding.binding };
			if (isTexture(binding.kind)) {
				write.images = { { TextureHandle{ held.id }, SamplerHandle{ held.sampler } } };
			}
			else {
				write.buffers = { { BufferHandle{ held.id }, held.offset, held.range } };
			}
			writes.push_back(std::move(write));
		}
		if (!writes.empty()) {
			mRhi->updateDescriptorSets(writes);
		}
	}

	void UserFrameSet::write() {
		if (!created()) {
			return;
		}
		const uint32_t current = slot();
		if (mBlockBinding != kNone && mBlockBuffers[current]) {
			mRhi->updateBuffer(mBlockBuffers[current], 0, mBlock.size(), mBlock.data());
		}
		writeSlot(current, false);
	}

	uint32_t UserFrameSet::writtenId(const uint32_t slot, const uint32_t binding) const {
		if (!created() || slot >= mSlots || !find(binding)) {
			return kNone;
		}
		return mWritten[slot][binding - kFirstBinding].id;
	}

	DescriptorSetLayoutHandle UserFrameSet::layout() const {
		return mLayout;
	}

	DescriptorSetLayoutHandle UserFrameSet::emptyLayout() const {
		return mEmptyLayout;
	}

	const DescriptorSetLayoutDesc& UserFrameSet::layoutDesc() const {
		return mLayoutDesc;
	}

	size_t UserFrameSet::ownedObjects() const {
		const auto live = [](const auto& handles) {
			return static_cast<size_t>(std::count_if(handles.begin(), handles.end(), [](const auto handle) { return static_cast<bool>(handle); }));
		};
		size_t owned = (mLayout ? 1u : 0u) + (mEmptyLayout ? 1u : 0u) + (mSampler ? 1u : 0u);
		for (const auto& kind : mFallbacks) {
			owned += live(kind);
		}
		return owned + live(mZeroBuffers) + live(mSets) + live(mBlockBuffers);
	}

	std::vector<std::string> UserFrameSet::ruleBreaks(const std::string_view file, const std::string_view glsl) {
		const std::string text = withoutComments(glsl);
		std::vector<Declaration> declarations;

		// SFE_SET(s, b), wherever it stands: in a layout, or in a macro a layout names.
		constexpr std::string_view kMacro = "SFE_SET";
		for (size_t at = text.find(kMacro); at != std::string::npos; at = text.find(kMacro, at + 1)) {
			const size_t end = at + kMacro.size();
			if ((at > 0 && identChar(text[at - 1])) || (end < text.size() && identChar(text[end]))) {
				continue;
			}
			size_t open = end;
			while (open < text.size() && std::isspace(static_cast<unsigned char>(text[open]))) {
				++open;
			}
			if (open >= text.size() || text[open] != '(') {
				continue;
			}
			const size_t close = text.find(')', open);
			const size_t comma = text.find(',', open);
			if (close == std::string::npos || comma == std::string::npos || comma > close) {
				continue;
			}
			const auto set = number(std::string_view(text).substr(open + 1, comma - open - 1));
			const auto binding = number(std::string_view(text).substr(comma + 1, close - comma - 1));
			if (!set || !binding) {
				continue;
			}
			declarations.push_back({ at, *set, *binding, bindingsTaken(text, close + 1, onDirective(text, at)) });
		}

		// A raw layout(... binding = b ...), with set = s or in set 0.
		constexpr std::string_view kLayout = "layout";
		for (size_t at = text.find(kLayout); at != std::string::npos; at = text.find(kLayout, at + 1)) {
			const size_t end = at + kLayout.size();
			if ((at > 0 && identChar(text[at - 1])) || (end < text.size() && identChar(text[end]))) {
				continue;
			}
			size_t open = end;
			while (open < text.size() && std::isspace(static_cast<unsigned char>(text[open]))) {
				++open;
			}
			if (open >= text.size() || text[open] != '(') {
				continue;
			}
			size_t close = open;
			for (int depth = 0; close < text.size(); ++close) {
				if (text[close] == '(') {
					++depth;
				}
				else if (text[close] == ')' && --depth == 0) {
					break;
				}
			}
			if (close >= text.size()) {
				continue;
			}
			const auto content = std::string_view(text).substr(open + 1, close - open - 1);
			if (content.find(kMacro) != std::string_view::npos) {
				continue; // the macro's, counted above
			}
			const auto binding = assigned(content, "binding");
			const auto set = assigned(content, "set");
			if (!binding.present || !binding.value || (set.present && !set.value)) {
				continue;
			}
			declarations.push_back({ at, set.present ? *set.value : 0u, *binding.value, bindingsTaken(text, close + 1, onDirective(text, at)) });
		}

		std::sort(declarations.begin(), declarations.end(), [](const Declaration& a, const Declaration& b) { return a.at < b.at; });
		std::vector<size_t> lineStarts{ 0 };
		for (size_t i = 0; i < text.size(); ++i) {
			if (text[i] == '\n') {
				lineStarts.push_back(i + 1);
			}
		}

		std::vector<std::string> breaks;
		for (const auto& declaration : declarations) {
			const uint32_t last = declaration.binding + declaration.count - 1;
			const bool anyInside = last >= kFirstBinding && declaration.binding <= kLastBinding;
			const bool allInside = declaration.binding >= kFirstBinding && last <= kLastBinding;
			if (declaration.set == kSet ? allInside : !anyInside) {
				continue;
			}
			const size_t line = static_cast<size_t>(std::upper_bound(lineStarts.begin(), lineStarts.end(), declaration.at) - lineStarts.begin());
			const std::string taken = declaration.count > 1
				? "bindings " + std::to_string(declaration.binding) + "-" + std::to_string(last)
				: "binding " + std::to_string(declaration.binding);
			std::string message = std::string(file) + ":" + std::to_string(line) + ": SFE_SET(" + std::to_string(declaration.set) + ", "
				+ std::to_string(declaration.binding) + ") takes " + taken;
			message += declaration.set == kSet
				? ", outside 40-63: set 3 is the application's user frame set (UserFrameSet) and has 40-63 alone"
				: ", inside 40-63: those are set 3's alone, the user frame set's, as OpenGL drops the set index";
			breaks.push_back(std::move(message));
		}
		return breaks;
	}
}
