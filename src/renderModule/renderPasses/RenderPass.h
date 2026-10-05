#pragma once

#include "mathModule/Forward.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE {
	namespace SystemsModule {
		struct RenderData;
	}
	namespace FrustumModule {
		struct Frustum;
	}
}

namespace SFE::Render {
	class RenderPass {
	public:
		virtual ~RenderPass() = default;
		virtual void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) = 0;
		virtual void init(SystemsModule::RenderData& renderDataHandle) {}
		// Before the frame's first command list, for every pass that runs this frame: the place to write buffers
		// that live on the device, which Vulkan uploads with the frame's first submit.
		virtual void prepare(SystemsModule::RenderData& renderDataHandle) {}
		// The size the picture is drawn at (ViewportData::renderW, renderH) has changed, between frames with the device
		// idle (RenderSystem::resize): a pass with targets of that size makes them again at the new one and lets the
		// old ones go. A pass's init makes them the first time through it too.
		virtual void resize(SystemsModule::RenderData& renderDataHandle) {}
	};

	/// @brief One cascade being filled, as a pass that draws its own casters sees it.
	struct ShadowCascade {
		SystemsModule::RenderData& data;
		CommandListHandle cmd;
		/// @brief The light's view-projection for this cascade.
		const Math::Mat4& lightSpace;
		/// @brief What may cast into it: the cascade's volume, widened, and reaching towards the sun.
		const FrustumModule::Frustum& casterVolume;
		/// @brief The direction the light travels.
		Math::Vec3 lightDirection;
		/// @brief Texels of the shadow map one metre covers, for dropping casters smaller than one.
		float texelsPerMetre = 0.f;
		uint32_t layer = 0;
	};

	/**
	 * @brief A pass that draws casters of its own into the shadow cascades.
	 *
	 * Grass, impostors and anything else a game draws outside the scene's mesh lists have to be in
	 * the shadow maps too. The shadow pass finds them by this interface among the passes it is given,
	 * so the engine does not name them: a game brings its own and they cast like everything else.
	 */
	class ShadowCasterPass {
	public:
		virtual ~ShadowCasterPass() = default;
		/// @brief Once before the cascades are drawn: uploads and bakes the casting needs.
		virtual void prepareShadows(SystemsModule::RenderData& data, CommandListHandle cmd) {}
		/// @brief Draw into one cascade, inside its render pass.
		virtual void drawShadowCasters(const ShadowCascade& cascade) = 0;
	};

	struct RenderPassEntry {
		const char* name = "";
		bool enabled = true;
		RenderPass* (*create)() = nullptr;
	};
}
