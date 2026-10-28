#pragma once

#include "componentsModule/CascadeShadowComponent.h"
#include "ecss/Types.h"
#include "mathModule/Forward.h"
#include "rhi/RenderHardwareInterface.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace SFE::Render {

	struct FrameResource {
		TextureHandle texture{};
		SamplerHandle sampler{};
		BufferHandle buffer{};
		FramebufferHandle framebuffer{};
	};

	struct CsmFrame {
		static constexpr size_t kMaxCascades = 6; // the shaders' `mat4 lightSpaceMatrices[6]`
		TextureHandle shadowMapTexture;
		SamplerHandle shadowMapSampler;
		Math::Vec3 lightDirection = {};
		Math::Vec3 lightColor = {};
		Math::Vec2 resolution = {};
		float cameraFarPlane = 0.f;
		std::vector<float> shadowCascadeLevels;
		ecss::EntityId shadows = ecss::INVALID_ID;
		std::vector<ComponentsModule::ShadowCascade> shadowCascades;
		float shadowsIntensity = 0.f;
		std::vector<Math::Mat4> lightSpaceMatrices;
	};

	struct PointShadowFrame {
		static constexpr size_t kMaxShadowFaces = 72; // the shaders' `mat4 pointLightMatrices[72]`
		// A point light or a spot lit this frame, the nearest the camera first.
		struct Light {
			ecss::SectorId entity = 0;
			Math::Vec3 position{ 0.f, 0.f, 0.f }; // where its light comes from, its flicker in it
			bool spot = false;
			Math::Vec3 direction{ 0.f, 0.f, -1.f }; // a spot's beam, of unit length
			float cosInner = 1.f;                   // a spot's cone: lit fully inside, none outside cosOuter
			float cosOuter = 0.f;
			int firstFace = 0; // its shadow's first face in `matrices`
			int faces = 0;     // 6 a point light's, 1 a spot's, 0 for one without shadows
		};
		std::vector<Light> lights;
		std::vector<Math::Mat4> matrices;
	};

	class FrameBlackboard {
	public:
		void set(const char* name, FrameResource resource) {
			mGpu[name] = resource;
		}

		TextureHandle texture(const char* name) const {
			const auto* r = find(name);
			return r ? r->texture : TextureHandle{};
		}

		SamplerHandle sampler(const char* name) const {
			const auto* r = find(name);
			return r ? r->sampler : SamplerHandle{};
		}

		BufferHandle buffer(const char* name) const {
			const auto* r = find(name);
			return r ? r->buffer : BufferHandle{};
		}

		FramebufferHandle framebuffer(const char* name) const {
			const auto* r = find(name);
			return r ? r->framebuffer : FramebufferHandle{};
		}

		// Sorted, so anything that walks the frame's textures does it in the same order every run.
		std::vector<std::string> textureNames() const {
			std::vector<std::string> names;
			for (const auto& [name, resource] : mGpu) {
				if (resource.texture) {
					names.push_back(name);
				}
			}
			std::sort(names.begin(), names.end());
			return names;
		}

		template<typename T>
		void setData(const char* name, T* ptr) {
			mCpu[name] = ptr;
		}

		template<typename T>
		T* data(const char* name) const {
			auto it = mCpu.find(name);
			if (it == mCpu.end()) {
				return nullptr;
			}
			return static_cast<T*>(it->second);
		}

	private:
		const FrameResource* find(const char* name) const {
			auto it = mGpu.find(name);
			return it == mGpu.end() ? nullptr : &it->second;
		}

		std::unordered_map<std::string, FrameResource> mGpu;
		std::unordered_map<std::string, void*> mCpu;
	};

	namespace FrameId {
		constexpr const char* GBufferFbo = "gbuffer.fbo";
		constexpr const char* GBufferOutlineFbo = "gbuffer.outlineFbo";
		// RGBA8: metallic, specular factor, clearcoat, clearcoat roughness; see sfeGBufferMaterial.
		constexpr const char* GBufferMaterial = "gbuffer.material";
		// R32F, the depth buffer copied once the G-buffer is complete; see sfeScenePosition.
		constexpr const char* SceneDepth = "scene.depth";
		constexpr const char* GBufferNormal = "gbuffer.normal";
		// Screen-space motion per pixel, in UV: where this pixel was last frame is TexCoords - velocity.xy (TAA). b: how much a
		// material's occlusion map shuts out the ambient light, 1 - AO (LightingPass); see sfeGBufferVelocity.
		constexpr const char* GBufferVelocity = "gbuffer.velocity";
		constexpr const char* GBufferAlbedo = "gbuffer.albedo";
		// RGBA16F: the light the surfaces give off themselves, rgb, in the lighting's units (MaterialComponent::emissive).
		constexpr const char* GBufferEmissive = "gbuffer.emissive";
		constexpr const char* GBufferViewPosition = "gbuffer.viewPosition";
		constexpr const char* GBufferOutlines = "gbuffer.outlines";
		constexpr const char* GBufferLights = "gbuffer.lights";
		constexpr const char* GBufferDepth = "gbuffer.depth";
		constexpr const char* ShadowCsm = "shadow.csm";
		constexpr const char* ShadowCsmDepth = "shadow.csmDepth";
		constexpr const char* SsaoColor = "ssao.color";
		constexpr const char* SsaoBlur = "ssao.blur";
		constexpr const char* Lit = "lighting.color";
		constexpr const char* IblIrradiance = "ibl.irradiance";
		constexpr const char* IblPrefilter = "ibl.prefilter";
		constexpr const char* IblBrdf = "ibl.brdf";
		constexpr const char* Final = "final";
		constexpr const char* Csm = "csm";
		constexpr const char* PointShadow = "pointShadow";
		constexpr const char* ShadowPoint = "shadow.point";
	}

}
