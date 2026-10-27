#pragma once
#include <array>
#include <future>
#include <utility>
#include <vector>

#include "renderModule/renderPasses/RenderPass.h"
#include "renderModule/IblBaker.h"

namespace SFE::Render {
	struct CsmFrame;
}

namespace SFE::Render::RenderPasses {
	class LightingPass : public RenderPass {
	public:
		struct CascadData {
			// x 1/resolution, y world size of one texel, z depth bias, w samples
			Math::Vec4 cascade;
			// x light-space depth extent of the cascade, so the shader can turn a depth difference
			// into a world distance; the rest is spare. std140 gives the array a 32 byte stride
			// with two vec4s, which is what this struct is.
			Math::Vec4 cascadeExtra;
		};
		struct CascadedShadowBlock {
			Math::Vec4 direction_intensity; // xyz direction, w intensity
			Math::Vec4 color_and_misc;
			CascadData data[6];
			Math::Vec4 cascadeSplits;
			Math::Vec4 cascadeSplits2;
		};
		// The sun shadow block (sfe_sun_shadow.glsl) of this frame's cascades, for every pass that looks the shadow up:
		// this one, and SunShadowPass before it.
		static void fillCascadedShadows(const SystemsModule::RenderData& renderDataHandle, const CsmFrame& csm,
			CascadedShadowBlock& block);

		LightingPass();
		void init(SystemsModule::RenderData& renderDataHandle) override;
		void render(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd) override;
		void resize(SystemsModule::RenderData& renderDataHandle) override;
		// This frame's sun shadow block (sfe_sun_shadow.glsl), for the passes after this one that look the shadow up too.
		const auto& cascadedShadows() const { return cascadedShadowData; }

	private:
		// This frame's blocks for the dynamic bindings of mUboSet.
		struct UboBlocks {
			TransientAllocation pointMatrices;   // 3
			TransientAllocation cascadedShadows; // 9
			TransientAllocation pointLights;     // 10
			TransientAllocation lightMatrices;   // 11
			TransientAllocation drawData;        // 15
		};
		void updateTextures(SystemsModule::RenderData& renderDataHandle, Render::CommandListHandle cmd, const UboBlocks& blocks);

		struct DrawData
		{
			float viewPosX = 0.f, viewPosY = 0.f, viewPosZ = 0.f;
			float shadowIntensity = 1.f;
			float fogStart = 0.f;
			float fogEnd = 0.f;
			int   pointLightsSize = 0;
			int   cascadeCount = 0;
			float ambientX = 0.f, ambientY = 0.f, ambientZ = 0.f;
			float ssaoAmount = 1.f;
			float viewDirX = 0.f, viewDirY = 0.f, viewDirZ = 1.f;
			float iblStrength = 1.f;
			float shadowAmbient = 0.42f;
			float padLook0 = 0.f; // SSS wrap
			float padLook1 = 0.f; // SSS strength
			// Backend-parity debugging: non-zero makes deferred_shading.fs output one isolated
			// term instead of the lit result, so each term can be diffed on its own. Reuses the
			// last free pad slot, so the UBO layout does not change.
			float debugTerm = 0.f;
			// The soft sheen and rim light on rough dielectrics; 0 with GraphicsSettings::sheen off.
			float sheenStrength = 0.f;
			float rimStrength = 0.f;
			// The TAA frame for noise that moves every frame, or -1 without TAA to resolve it.
			float taaFrame = -1.f;
			// The scene sky's cloud coverage, for the shadows the clouds cast on the ground.
			float cloudCoverage = 0.f;
			// See deferred_shading.fs: 0 leaves the lighting as it is, 1 paints it.
			float stylise = 0.f;
			// GraphicsSettings::ssaoSun.
			float ssaoSun = 0.f;
			float shelterCount = 0.f;
			// ShelterComponent::kSoft: the metres from a shelter's walls and roof over which its inside comes in.
			float shelterSoft = 0.f;
			// The area tint (AreaTintFrame); a zero area, no tint.
			Math::Vec4 tintArea{};
			Math::Vec4 tintFoliage{};
			Math::Vec4 tintAir{};
			// The ShelterComponents in the scene, the first kMostShelters.
			static constexpr int kMostShelters = 4;
			struct Shelter {
				Math::Mat4 toLocal{};  // world to the shelter's own space
				Math::Vec4 box{};      // x width, y the ridge's height, z length, w the eaves' height
				Math::Vec4 sky{};      // x the share of the sky's light that comes in
			} shelters[kMostShelters];
		};

		CascadedShadowBlock cascadedShadowData;
		DrawData drawData;

		Render::DescriptorSetLayoutHandle mTexSetLayout{};
		Render::DescriptorSetLayoutHandle mUboSetLayout{};
		bool                              mTexSetInitialized = false;

		Render::PipelineHandle            mLightingPipeline{};    // предполагаем, что ты его будешь создавать по RHI
		Render::RenderPassHandle          mLightingRenderPass{};  // если будешь делать отдельный RenderPass

		PipelineHandle pipeline;
		RenderPassHandle renderPass;

		struct PointLight {
			Math::Vec3 Position;
			float pad0 = 0.f;
			Math::Vec2 texelSize;
			float bias = 0.f;
			int samples = 0;
			float radius = 0.f;
			int offset = 0;
			float pad1 = 0.f;
			float pad2 = 0.f;
			Math::Vec3 Color;
			float Linear = 0.f;
			float Quadratic = 0.f;
			int Type = 0; // 0 a point light, 1 a spot
			int Layers = 0;
			float cosInner = 1.f;
			Math::Vec3 Direction;
			float cosOuter = 0.f;
		};
		static_assert(sizeof(PointLight) == 96);

		PointLight pointLight[12];

		TextureHandle mDummyPointShadow;
		SamplerHandle mDummyPointShadowSampler;

		TextureHandle mPresentTex[2];
		SamplerHandle mPresentSampler{};
		FramebufferHandle mPresentFbo[2];
		int mPresentShow = 0;

		IblMaps mIbl;
		IblMaps mIblSpare; // what the next bake is written into, then swapped with mIbl
		// What the sky light was baked from, and the bake under way on a worker.
		std::array<float, 5> mIblKey{};
		std::future<IblTexels> mIblBaking;
		void bakeSkyLight(SystemsModule::RenderData& renderDataHandle);
		// mIbl bound for the frames to come.
		void useSkyLight(SystemsModule::RenderData& renderDataHandle);
		void updateSkyLight(SystemsModule::RenderData& renderDataHandle);
		static std::array<float, 5> skyLightKey(const SystemsModule::RenderData& renderDataHandle);

		struct BoundImage {
			TextureHandle texture;
			SamplerHandle sampler;
		};
		// One set per frame slot: the G-buffer and the lit target alternate per slot, so a shared set would be
		// rewritten every frame -- and writing a set the previous frame still binds costs a copy of it.
		struct Slot {
			Render::DescriptorSetHandle texSet{};
			Render::DescriptorSetHandle uboSet{};
			BoundImage gMaterial{};
			BoundImage sceneDepth{};
			BoundImage gNormal{};
			BoundImage gAlbedo{};
			BoundImage gVelocity{};
			BoundImage gEmissive{};
			BoundImage ssao{};
			BoundImage sunShadow{};
			BoundImage pointShadow{};
			BoundImage outlines{};
			BoundImage clouds{};
			BoundImage areaTint{};
			BoundImage csm{};
			BoundImage csmDepth{};
			BoundImage iblIrr{};
			BoundImage iblPref{};
			BoundImage iblLut{};
			BufferHandle lightMat{};
			BufferHandle pointMat{};
			BufferHandle pc{};
			BufferHandle shadows{};
			BufferHandle pointLights{};
			BufferHandle cam{};
		};
		std::vector<Slot> mSlots;
	};
}
