#pragma once


#include "componentsModule/ComponentBase.h"
#include "mathModule/Forward.h"


namespace SFE::ComponentsModule {
	enum class eLightType {
		NONE = 0,
		DIRECTIONAL,
		POINT,
		// A cone of light down the entity's -Z (glTF's way), as a lamp's beam: mSpotInner, mSpotOuter. Its shadow is one
		// face, a point light's six.
		SPOT,
		WORLD
	};
	
	class LightSourceComponent : public ecss::ComponentInterface {
	public:
		LightSourceComponent(ecss::SectorId id , eLightType type);
		eLightType getType() const;
		void setType(eLightType type);

		void setIntensity(float intensity);
		float getIntensity() const;

		void setLightColor(const SFE::Math::Vec3& lightColor);
		const SFE::Math::Vec3& getLightColor() const;

		void setBias(float bias);
		float getBias() const;

		void setTexelSize(const SFE::Math::Vec2& texelSize);
		const SFE::Math::Vec2& getTexelSize() const;

		void setSamples(int samples);
		int getSamples() const;

		float mLinear = 0.1f;
		float mQuadratic = 0.0001f;
		float mRadius = 100.f;
		float mNear = 0.1f;
		bool mWithShadows = true;
		// A spot's cone, half its angle in degrees: lit fully inside mSpotInner, fading out to nothing at mSpotOuter
		// (at most PointLightPass kMaxSpotOuter).
		float mSpotInner = 20.f;
		float mSpotOuter = 30.f;
		// How much a flame's light wavers, 0 steady: its brightness goes up and down by up to this share, and the light's
		// centre wanders round the entity (LightSourceComponent.cpp kFlickerSway), so the shadows it throws tremble.
		float mFlicker = 0.f;
		// At time t: the brightness as a share of the intensity, and where the light is from the entity. LightingPass and
		// PointLightPass ask the same time, so the light falls from where its shadows were drawn.
		float flickerGain(float t) const;
		SFE::Math::Vec3 flickerShift(float t) const;
	private:
		float mBias = 0.0002f;
		SFE::Math::Vec2 mTexelSize = { 0.004f, 0.004f };
		int mSamples = 32;

		eLightType mType = eLightType::NONE;

		float mIntensity = 1.f;
		SFE::Math::Vec3 mLightColor = Math::Vec3{ 1.f };


	};
}

using SFE::ComponentsModule::LightSourceComponent;