#include "renderModule/RenderResources.h"

#include <algorithm>

#include "core/Engine.h"
#include "logsModule/logger.h"

namespace SFE::Render {
	namespace {
		// The RHI while there is an engine with one; none once the engine has gone, the RHI with it and all it made.
		RenderHardwareInterface* liveRhi() {
			return Singleton<Engine>::isAlive() ? Engine::instance()->rhi() : nullptr;
		}

		// A target of `desc` at the picture's size now.
		TextureDesc sized(const TargetDesc& desc) {
			const auto& screen = Engine::instance()->getWindow()->getScreenData();
			const int divisor = (std::max)(1, desc.divisor);
			TextureDesc texture;
			texture.width = static_cast<uint32_t>((std::max)(1, screen.renderW / divisor));
			texture.height = static_cast<uint32_t>((std::max)(1, screen.renderH / divisor));
			texture.format = desc.format;
			texture.usage = desc.usage;
			return texture;
		}
	}

	void release(const TextureHandle handle) {
		if (auto* rhi = liveRhi()) {
			rhi->destroyTexture(handle);
		}
	}

	void release(const BufferHandle handle) {
		if (auto* rhi = liveRhi()) {
			rhi->destroyBuffer(handle);
		}
	}

	void release(const SamplerHandle handle) {
		if (auto* rhi = liveRhi()) {
			rhi->destroySampler(handle);
		}
	}

	void release(const PipelineHandle handle) {
		if (auto* rhi = liveRhi()) {
			rhi->destroyGraphicsPipeline(handle);
		}
	}

	void release(const RenderPassHandle handle) {
		if (auto* rhi = liveRhi()) {
			rhi->destroyRenderPass(handle);
		}
	}

	void release(const FramebufferHandle handle) {
		if (auto* rhi = liveRhi()) {
			rhi->destroyFramebuffer(handle);
		}
	}

	void release(const DescriptorSetLayoutHandle handle) {
		if (auto* rhi = liveRhi()) {
			rhi->destroyDescriptorSetLayout(handle);
		}
	}

	void release(const DescriptorSetHandle handle) {
		if (auto* rhi = liveRhi()) {
			rhi->freeDescriptorSet(handle);
		}
	}

	void release(const ShaderModuleHandle handle) {
		if (auto* rhi = liveRhi()) {
			rhi->destroyShaderModule(handle);
		}
	}

	Held<TextureHandle> Resources::target(const std::string& name, const TargetDesc& desc) {
		auto* rhi = Engine::instance()->rhi();
		const auto made = sized(desc);
		auto& target = mTargets[name];
		auto slot = target.slot.lock();
		Held<TextureHandle> held;
		if (!slot) {
			held = own(rhi->createTexture(made));
			target = { held.mSlot, desc, made.width, made.height };
			return held;
		}
		if (target.desc.format != desc.format || target.desc.divisor != desc.divisor || target.desc.usage != desc.usage) {
			LogsModule::Logger::LOG_ERROR("Render::Resources: the target \"%s\" asked for as another than it is held as", name.c_str());
			return held;
		}
		if (target.width != made.width || target.height != made.height) {
			rhi->destroyTexture(*slot);
			*slot = rhi->createTexture(made);
			target.width = made.width;
			target.height = made.height;
		}
		held.mSlot = std::move(slot);
		return held;
	}

	FramebufferHandle Resources::framebuffer(const TextureHandle color, const TextureHandle depth, const RenderPassHandle renderPass) {
		for (const auto& kept : mFramebuffers) {
			if (kept.color.id == color.id && kept.depth.id == depth.id && kept.renderPass.id == renderPass.id) {
				return kept.fbo;
			}
		}
		const auto& screen = Engine::instance()->getWindow()->getScreenData();
		FramebufferDesc desc;
		desc.width = static_cast<uint32_t>((std::max)(1, screen.renderW));
		desc.height = static_cast<uint32_t>((std::max)(1, screen.renderH));
		desc.renderPass = renderPass;
		desc.colorAttachments = { { color } };
		desc.depthStencilAttachment = { .texture = depth };
		auto fbo = own(Engine::instance()->rhi()->createFramebuffer(desc));
		const FramebufferHandle made = fbo;
		mFramebuffers.push_back({ color, depth, renderPass, std::move(fbo) });
		return made;
	}

	void Resources::targetsChanged() {
		mFramebuffers.clear();
	}

	const Resources::Target* Resources::live(const std::string_view name) const {
		const auto found = mTargets.find(name);
		return found != mTargets.end() && !found->second.slot.expired() ? &found->second : nullptr;
	}

	std::vector<Resources::HeldTarget> Resources::held() const {
		std::vector<HeldTarget> held;
		for (const auto& [name, target] : mTargets) {
			if (!target.slot.expired()) {
				held.push_back({ name, target.slot.use_count(), bytes(name) });
			}
		}
		return held;
	}

	uint64_t Resources::bytes(const std::string_view name) const {
		const auto* target = live(name);
		if (!target) {
			return 0;
		}
		TextureDesc desc;
		desc.width = target->width;
		desc.height = target->height;
		desc.format = target->desc.format;
		return textureBytes(desc);
	}

	long Resources::holders(const std::string_view name) const {
		const auto* target = live(name);
		return target ? target->slot.use_count() : 0;
	}

	void screenTarget(Resources& resources, const std::string& name, Held<TextureHandle>& texture, Held<FramebufferHandle>& framebuffer,
		const RenderPassHandle renderPass, const TextureFormat format, const int divisor) {
		framebuffer.reset();
		texture = resources.target(name, { format, divisor });
		if (!texture) {
			return;
		}
		const auto& screen = Engine::instance()->getWindow()->getScreenData();
		FramebufferDesc fb;
		fb.width = static_cast<uint32_t>((std::max)(1, screen.renderW / (std::max)(1, divisor)));
		fb.height = static_cast<uint32_t>((std::max)(1, screen.renderH / (std::max)(1, divisor)));
		fb.renderPass = renderPass;
		fb.colorAttachments = { { texture.get() } };
		framebuffer = own(Engine::instance()->rhi()->createFramebuffer(fb));
	}
}
