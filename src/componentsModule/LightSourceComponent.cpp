#include "LightSourceComponent.h"

#include <algorithm>
#include <cmath>

#include "mathModule/Vector.h"

LightSourceComponent::LightSourceComponent(ecss::SectorId id, eLightType type) : ComponentInterface(id), mType(type) {
}

// How far a flickering light's centre wanders, in metres for each unit of mFlicker: the campfire's 0.2 moves it up to
// 15 cm, enough for the shadows round the fire to tremble at dusk, when the sky still lights most of them.
constexpr float kFlickerSway = 0.75f;

// A few waves of no common period laid over each other, so it never quite repeats.
float LightSourceComponent::flickerGain(float t) const {
	return 1.f + mFlicker * (0.5f * std::sin(7.3f * t) + 0.3f * std::sin(13.1f * t + 1.3f) + 0.2f * std::sin(23.7f * t + 0.4f));
}

// Side to side on waves of its own; up and down with the brightness, higher as the flame flares.
SFE::Math::Vec3 LightSourceComponent::flickerShift(float t) const {
	const float reach = mFlicker * kFlickerSway;
	return SFE::Math::Vec3{
		reach * (0.6f * std::sin(9.7f * t + 0.5f) + 0.4f * std::sin(17.3f * t + 2.2f)),
		reach * 0.5f * (flickerGain(t) - 1.f) / (std::max)(mFlicker, 1e-4f),
		reach * (0.6f * std::sin(8.9f * t + 1.7f) + 0.4f * std::sin(19.1f * t + 0.9f))
	};
}
SFE::ComponentsModule::eLightType LightSourceComponent::getType() const {
	return mType;
}

void LightSourceComponent::setType(eLightType type) {
	mType = type;
}


void LightSourceComponent::setIntensity(float intensity) {
	mIntensity = intensity;
}

float LightSourceComponent::getIntensity() const {
	return mIntensity;
}


void LightSourceComponent::setLightColor(const SFE::Math::Vec3& lightColor) {
	mLightColor = lightColor;
}

const SFE::Math::Vec3& LightSourceComponent::getLightColor() const {
	return mLightColor;
}

void LightSourceComponent::setBias(float bias) {
	mBias = bias;
}

float LightSourceComponent::getBias() const {
	return mBias;
}

void LightSourceComponent::setTexelSize(const SFE::Math::Vec2& texelSize) {
	mTexelSize = texelSize;
}

const SFE::Math::Vec2& LightSourceComponent::getTexelSize() const {
	return mTexelSize;
}

void LightSourceComponent::setSamples(int samples) {
	mSamples = samples;
}

int LightSourceComponent::getSamples() const {
	return mSamples;
}

