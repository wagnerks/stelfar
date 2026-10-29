#pragma once

#include <memory>
#include <vector>

#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	class Resources;

	// What the last holder of a handle does with it: the RHI's destroy of its kind, while there is an engine with an RHI
	// to do it (none after the engine has gone, when the RHI took everything with it). Main thread.
	void release(TextureHandle handle);
	void release(BufferHandle handle);
	void release(SamplerHandle handle);
	void release(PipelineHandle handle);
	void release(RenderPassHandle handle);
	void release(FramebufferHandle handle);
	void release(DescriptorSetLayoutHandle handle);
	void release(DescriptorSetHandle handle);
	void release(ShaderModuleHandle handle);

	// One RHI object held as a shared_ptr holds what it points at: copies share it, and the last one to go destroys it
	// (release). A pass that holds every object it makes this way frees all of them when it goes, with no destructor of
	// its own to forget one in; an object two holders share lives while either does. Read as the handle it holds, so the
	// calls that take a handle take it as they are.
	template <typename H>
	class Held {
	public:
		Held() = default;
		// `handle`, just made, held from here; an empty one holds nothing.
		explicit Held(const H handle) {
			if (handle) {
				mSlot = std::shared_ptr<H>(new H(handle), [](H* held) {
					release(*held);
					delete held;
				});
			}
		}

		H get() const { return mSlot ? *mSlot : H{}; }
		operator H() const { return get(); }
		explicit operator bool() const { return static_cast<bool>(get()); }
		// How many hold it, 0 for none.
		long holders() const { return mSlot.use_count(); }
		// Let go of here; destroyed if this was its last holder.
		void reset() { mSlot.reset(); }

	private:
		// The registry remakes a shared target in its slot (Resources::target), for every holder at once.
		friend class Resources;
		std::shared_ptr<H> mSlot;
	};

	// `handle`, just made, held: `mPipeline = own(rhi->createGraphicsPipeline(desc))`.
	template <typename H>
	Held<H> own(const H handle) {
		return Held<H>(handle);
	}

	// `handle` held in `into` and given back, for what a pass makes several of as it goes (the modules its pipelines are
	// made of): `desc.shaders.vertex = keep(mShaders, rhi->loadShader(...))`.
	template <typename H>
	H keep(std::vector<Held<H>>& into, const H handle) {
		into.push_back(own(handle));
		return handle;
	}
}
