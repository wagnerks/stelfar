#pragma once

#include <vector>

#include "ComponentBase.h"
#include "assetsModule/shaderModule/ShaderBase.h"
#include "assetsModule/modelModule/BoundingVolume.h"
#include "mathModule/Projection.h"
#include "rhi/RenderHardwareInterface.h"


namespace SFE::ComponentsModule {
	struct ShadowCascade {
		SFE::FrustumModule::Frustum frustum = {};
		SFE::MathModule::PerspectiveProjection viewProjection {};
		float bias = 0.f; // in texels of this cascade, grown on grazing surfaces; deferred_shading.fs turns it into depth
		int samples = 64;

		Math::Vec2 zMult = { 1.f,1.f };
		Math::Vec2 texelSize = {};

		// Padded light-space depth extent and the world size of one shadow-map texel. A cascade
		// fitted to a distant frustum slice spans thousands of units while a near one spans
		// dozens, so a bias expressed directly in depth units means wildly different things per
		// cascade. Keeping both lets the upload express bias in texels -- the quantity acne
		// actually scales with -- and convert it to depth units per cascade.
		float depthRange = 1.f;
		float worldPerTexel = 1.f;
	};

	class CascadeShadowComponent : public ecss::ComponentInterface {
	public:
		CascadeShadowComponent(ecss::SectorId id) : ComponentInterface(id) {};

		void updateCascades(const MathModule::PerspectiveProjection& cameraProjection);
		// The first cascade round `focus` (xyz its centre, w its radius) when it has one (RenderData::shadowFocus).
		void updateLightSpaceMatrices(const Math::Mat4& cameraView, const Math::Vec4& focus = {});

		// Returns the matrix the shadow pass renders with; its depth convention follows the
		// backend. `cullingMatrix`, when given, receives the same volume in the [-1,1] convention
		// that frustum-plane extraction expects, so culling stays backend-independent.
		static SFE::Math::Mat4 getLightSpaceMatrix(const std::vector<SFE::Math::Vec4>& corners, const SFE::Math::Mat4& lightView, float nearMultiplier = 1.f, float farMultiplier = 1.f, float mapSize = 1024.f, SFE::Math::Mat4* cullingMatrix = nullptr, float* depthRange = nullptr, float* worldPerTexelOut = nullptr);
		static std::vector<Math::Vec4> getFrustumCornersWorldSpace(const Math::Mat4& proj, const Math::Mat4& view);
		static std::vector<Math::Vec4> getFrustumCornersWorldSpace(const Math::Mat4& projView);

		std::vector<ShadowCascade> cascades;
		std::vector<float> shadowCascadeLevels;
		Math::Vec2 resolution = {};

		MathModule::PerspectiveProjection mCameraProjection = {};


		void markDirty();
		void calculateLightSpaceMatrices(const MathModule::PerspectiveProjection& projection, const Math::Mat4& view, const Math::Vec4& focus = {});
		const std::vector<Math::Mat4>& getLightSpaceMatrices() const;

		float shadowIntensity = 1.f;

		void cacheMatrices();

		static const std::vector<Math::Mat4>& getCacheLightSpaceMatrices();
		static void debugDraw(const std::vector<SFE::Math::Mat4>& lightSpaceMatrices, const Math::Mat4& cameraProjection, const Math::Mat4& cameraView);
		static void clearCacheMatrices();
	private:
		static void drawCascadeVolumeVisualizers(const std::vector<SFE::Math::Mat4>& lightMatrices);
		static inline std::vector<Math::Mat4> mLightMatricesCache;


		std::vector<Math::Mat4> mLightSpaceMatrices;
		bool mDirty = true;

		static inline bool mInited = false;

		static inline Render::PipelineHandle debugPipeline;
		static inline Render::BufferHandle debugMatricesUBO;
		static inline Render::BufferHandle debugMatricesEBO;
	};
}

template<> struct ecss::AllowNonTrivial<SFE::ComponentsModule::CascadeShadowComponent> : std::true_type {};

using SFE::ComponentsModule::CascadeShadowComponent;