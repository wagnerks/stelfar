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

	// A world map (x, z) of how far each place has gone towards a tint: r its foliage, g its water, b its air. Put by
	// the game (under FrameId::AreaTint); without it nothing is tinted.
	struct AreaTintFrame {
		TextureHandle map;
		SamplerHandle sampler;
		Math::Vec4 area;    // min x, min z, 1 / size along x and z
		Math::Vec4 foliage; // rgb what the foliage's grey is multiplied by, a how far a full map value takes it
		Math::Vec4 water;
		Math::Vec4 air;
		// The land by its map value: from x it is foul, the bank turning to mud and the grass's ground with it, wholly
		// by y; z the metres over the water the litter lies up to, all the bank that turns.
		Math::Vec4 shore;
		// Fumes off the water: a low mist over it where its map value is past fumes.x, thickest at 1, thinning to 1/e
		// fumes.y metres up; fumesColour.rgb what the light in it is multiplied by, .a its density per metre at the
		// water line over the foulest water. A zero density, none.
		Math::Vec4 fumes;
		Math::Vec4 fumesColour;
	};

	// The marks in the ground round the camera (the game's GroundMarks): a 2D array, layer 0 a fine window and layer 1 a
	// coarse one, each wrapping round as the camera moves on, so a place at xz is at xz / span in its layer; r how deep
	// the ground is pressed there, of size.z metres, g how churned up. Put by the game (under FrameId::GroundMarks);
	// without it the ground is unmarked.
	struct GroundMarksFrame {
		TextureHandle map;
		SamplerHandle sampler;
		Math::Vec4 window; // xy the fine window's least x and z, zw the coarse one's, metres
		Math::Vec4 size;   // x the fine window's span, y the coarse one's, z the deepest pressed, metres; w how deep grass lies flat
		Math::Vec4 look;   // x the rims' height of the depth pressed aside within y metres; z how wet churned ground looks; w ruts' darkening per metre below their sides
		Math::Vec4 churn;  // xy the churn the soil starts and wholly shows at, zw the churn grass starts going and is all gone at
		// A square of ground near the camera drawn dense (g_buffer_ground.vs), so that its marks are pressed into it as
		// they are deep; the terrain's own mesh is put out of sight under it (sfeGroundUnderPatch). The terrain's heights,
		// R32F, a texel a vertex of its mesh, as AssetsModule::terrainHeightsTexture makes them.
		TextureHandle heights;
		Math::Vec4 grid;   // xy the world x and z of the terrain mesh's first vertex, z its vertices' spacing, w the terrain's height
		Math::Vec4 patch;  // xy the square's least world x and z, on the mesh's vertices; z its side; w its vertices' spacing. A zero side, none
	};

	// Mud maps (the game's vehicles'): how caked with mud each cell of a box over a model as it rests is, 0 clean to 1
	// wholly (r), a map of kCells (x, y, z) cells of kCell metres a model; a 2D array, x across and z down each layer,
	// a layer for each height (y), a map's layers one after another from its first (MaterialComponent::mudMap). Put by
	// the game (under FrameId::MudMaps); without it nothing is caked by them.
	struct MudMapsFrame {
		static constexpr float kCell = 0.05f;
		static constexpr uint32_t kCells[3] = { 56, 60, 128 };
		TextureHandle map;
		SamplerHandle sampler;
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
		constexpr const char* AreaTint = "areaTint";
		constexpr const char* GroundMarks = "groundMarks";
		constexpr const char* MudMaps = "mudMaps";
	}

}
