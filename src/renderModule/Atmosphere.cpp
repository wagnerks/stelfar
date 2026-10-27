#include "Atmosphere.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <execution>
#include <numeric>
#include <vector>

#include "assetsModule/TextureHandler.h"
#include "core/Clocks.h"
#include "core/ECSHandler.h"
#include "core/Engine.h"
#include "mathModule/MatrixOperations.h"
#include "systemsModule/systems/RenderSystem.h"

namespace SFE::Render::Atmosphere {
	namespace {
		constexpr double kPlanetRadius = 6371e3;
		constexpr double kAtmosphereRadius = 6471e3;
		constexpr double kRayleigh[3] = { 5.5e-6, 13.0e-6, 22.4e-6 };
		constexpr double kMie = 21e-6;
		constexpr double kRayleighHeight = 8e3;
		constexpr double kMieHeight = 1.2e3;
		constexpr double kMieG = 0.758;
		constexpr double kPi = 3.14159265358979;

		struct D3 {
			double x, y, z;
		};
		D3 add(D3 a, D3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		D3 mul(D3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
		double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		double length(D3 a) { return std::sqrt(dot(a, a)); }
		D3 normalized(D3 a) {
			const double l = length(a);
			return l > 1e-12 ? mul(a, 1.0 / l) : D3{ 0.0, 1.0, 0.0 };
		}

		double sphereExit(D3 origin, D3 dir, double radius) {
			const double b = dot(origin, dir);
			const double c = dot(origin, origin) - radius * radius;
			return -b + std::sqrt((std::max)(b * b - c, 0.0));
		}

		// Optical depths from p to the top of the atmosphere towards dir.
		void depthsTowards(D3 p, D3 dir, int steps, double& rayleigh, double& mie) {
			const double len = sphereExit(p, dir, kAtmosphereRadius) / steps;
			rayleigh = 0.0;
			mie = 0.0;
			for (int j = 0; j < steps; ++j) {
				const D3 q = add(p, mul(dir, len * (j + 0.5)));
				const double h = (std::max)(length(q) - kPlanetRadius, 0.0);
				rayleigh += std::exp(-h / kRayleighHeight) * len;
				mie += std::exp(-h / kMieHeight) * len;
			}
		}

		constexpr D3 kOrigin{ 0.0, kPlanetRadius + 2.0, 0.0 };
	}

	Math::Vec3 radiance(const Math::Vec3& viewDir, const Math::Vec3& sun) {
		constexpr int steps = 10;
		constexpr int lightSteps = 4;
		const D3 toSun = normalized({ sun.x, sun.y, sun.z });
		const D3 dir = normalized({ viewDir.x, (std::max)(viewDir.y, 0.f), viewDir.z });
		const double stepLen = sphereExit(kOrigin, dir, kAtmosphereRadius) / steps;

		double rayleigh[3]{};
		double mie[3]{};
		double depthR = 0.0;
		double depthM = 0.0;
		for (int i = 0; i < steps; ++i) {
			const D3 p = add(kOrigin, mul(dir, stepLen * (i + 0.5)));
			const double h = length(p) - kPlanetRadius;
			const double dR = std::exp(-h / kRayleighHeight) * stepLen;
			const double dM = std::exp(-h / kMieHeight) * stepLen;
			depthR += dR;
			depthM += dM;
			double lightR = 0.0;
			double lightM = 0.0;
			depthsTowards(p, toSun, lightSteps, lightR, lightM);
			for (int c = 0; c < 3; ++c) {
				const double attenuation = std::exp(-(kMie * (depthM + lightM) + kRayleigh[c] * (depthR + lightR)));
				rayleigh[c] += dR * attenuation;
				mie[c] += dM * attenuation;
			}
		}

		const double mu = dot(dir, toSun);
		const double g2 = kMieG * kMieG;
		const double phaseR = 3.0 / (16.0 * kPi) * (1.0 + mu * mu);
		const double phaseM = 3.0 / (8.0 * kPi) * ((1.0 - g2) * (1.0 + mu * mu))
			/ (std::pow(1.0 + g2 - 2.0 * mu * kMieG, 1.5) * (2.0 + g2));
		const auto channel = [&](int c) { return static_cast<float>(rayleigh[c] * phaseR * kRayleigh[c] + mie[c] * phaseM * kMie); };
		// The moon lights the night sky the way the sun lights the day one, only far fainter and colder: without it
		// the sky goes to nothing once the sun is down, and everything it lights goes black with it.
		const double night = std::clamp((0.06 - sun.y) / 0.16, 0.0, 1.0);
		const Math::Vec3 moonlit{ 0.55f, 0.72f, 1.f };
		const float up = std::clamp(static_cast<float>(dir.y), 0.f, 1.f);
		const float glow = kMoonlight * static_cast<float>(night * night) * (0.55f + 0.45f * (1.f - up));
		return { channel(0) + moonlit.x * glow, channel(1) + moonlit.y * glow, channel(2) + moonlit.z * glow };
	}

	Math::Vec3 sunTransmittance(const Math::Vec3& sun) {
		// Just above the horizon at worst: below it the terrain would shadow everything anyway.
		const D3 toSun = normalized({ sun.x, (std::max)(sun.y, 0.02f), sun.z });
		double depthR = 0.0;
		double depthM = 0.0;
		depthsTowards(kOrigin, toSun, 32, depthR, depthM);
		// Once the sun is under the horizon, its light fades out over the twilight instead of shining through the ground.
		const double set = std::clamp((sun.y + 0.1) / 0.13, 0.0, 1.0);
		const double twilight = set * set * (3.0 - 2.0 * set);
		const auto channel = [&](int c) { return static_cast<float>(std::exp(-(kRayleigh[c] * depthR + kMie * depthM)) * twilight); };
		return { channel(0), channel(1), channel(2) };
	}

	std::optional<ComponentsModule::SkyComponent> sceneSky(const bool withSettings) {
		for (const auto& [id, sky] : ECSHandler::registry().view<const ComponentsModule::SkyComponent>()) {
			if (!sky) {
				continue;
			}
			auto result = *sky;
			auto* renderSystem = withSettings ? ECSHandler::getSystem<SystemsModule::RenderSystem>() : nullptr;
			if (renderSystem && renderSystem->getRenderData().graphics.timeOfDay >= 0.f) {
				result.setTimeOfDay(renderSystem->getRenderData().graphics.timeOfDay);
			}
			return result;
		}
		return std::nullopt;
	}

	std::optional<ComponentsModule::WindComponent> sceneWind(const bool withSettings) {
		for (const auto& [id, wind] : ECSHandler::registry().view<const ComponentsModule::WindComponent>()) {
			if (!wind) {
				continue;
			}
			auto result = *wind;
			auto* renderSystem = withSettings ? ECSHandler::getSystem<SystemsModule::RenderSystem>() : nullptr;
			if (renderSystem && renderSystem->getRenderData().graphics.windStrength >= 0.f) {
				result.strength = renderSystem->getRenderData().graphics.windStrength;
			}
			return result;
		}
		return std::nullopt;
	}

	bool skyParams(const SystemsModule::RenderData& renderData, SkyParams& out) {
		const auto sky = sceneSky();
		if (!sky) {
			return false;
		}
		const auto toSun = sky->toSun();
		const auto sun = sunTransmittance(toSun) * renderData.graphics.sunIntensity;
		out.invViewProj = Math::inverse(renderData.current.PV); // unjittered on the CPU side
		out.toSun[0] = toSun.x;
		out.toSun[1] = toSun.y;
		out.toSun[2] = toSun.z;
		out.toSun[3] = sky->cloudCoverage;
		out.sunColor[0] = sun.x;
		out.sunColor[1] = sun.y;
		out.sunColor[2] = sun.z;
		// Wrapped so the float keeps its precision in a long session; the drift jumps once a day.
		out.sunColor[3] = static_cast<float>(std::fmod(Engine::instance()->animationClock(), 3600.0)); // as RenderSystem wraps the wind clock: the cloud drift must match between the sky and the shadows it casts
		out.cameraPos[0] = renderData.mCameraPos.x;
		out.cameraPos[1] = renderData.mCameraPos.y;
		out.cameraPos[2] = renderData.mCameraPos.z;
		out.cameraPos[3] = kSkyBrightness * renderData.graphics.sunIntensity;
		out.wind[0] = renderData.current.wind.x;
		out.wind[1] = renderData.current.wind.y;
		out.wind[2] = renderData.current.wind.z;
		out.wind[3] = renderData.graphics.stylise;
		return true;
	}

	SkyTexture cloudTexture() {
		static SkyTexture clouds;
		if (!clouds.texture) {
			if (const auto* texture = AssetsModule::TextureHandler::instance()->loadTexture("textures/sky/clouds.png")) {
				clouds.texture = texture->texture;
				clouds.sampler = texture->sampler;
			}
		}
		return clouds;
	}

	SkyTexture skyTexture() {
		constexpr uint32_t kWidth = 128;
		constexpr uint32_t kHeight = 32;
		static SkyTexture sky;
		// Three written in turn, each by then read by no frame still in flight: made anew each time (and the old freed),
		// or written over while frames still read it, it held the driver up for seconds at a time.
		static std::array<TextureHandle, 3> ring;
		static size_t written = 0;
		static Math::Vec3 bakedFor{ 0.f, -2.f, 0.f };
		// When it was last baked, on the simulation's clock: a sun on the move is baked again at most every 0.1 s of the
		// world's time, so a frame stepped shows the same sky on every run.
		static double bakedAt = 0.0;

		auto* rhi = Engine::instance()->rhi();
		const double now = Clocks::simulation();
		if (!sky.sampler) {
			SamplerDesc desc;
			desc.addressU = AddressMode::Repeat;
			sky.sampler = rhi->createSampler(desc);
		}

		const auto scene = sceneSky();
		const Math::Vec3 toSun = scene ? scene->toSun() : Math::Vec3{ 0.f, 1.f, 0.f };
		const bool moved = toSun.x != bakedFor.x || toSun.y != bakedFor.y || toSun.z != bakedFor.z;
		if (sky.texture && (!moved || (now >= bakedAt && now - bakedAt < 0.1))) {
			return sky;
		}

		std::vector<float> texels(static_cast<size_t>(kWidth) * kHeight * 4);
		std::vector<uint32_t> rows(kHeight);
		std::iota(rows.begin(), rows.end(), 0u);
		std::for_each(std::execution::par, rows.begin(), rows.end(), [&](const uint32_t row) {
			const float v = (static_cast<float>(row) + 0.5f) / kHeight;
			const float height = v * v;
			const float across = std::sqrt((std::max)(1.f - height * height, 0.f));
			for (uint32_t column = 0; column < kWidth; ++column) {
				const float azimuth = ((static_cast<float>(column) + 0.5f) / kWidth - 0.5f) * 6.2831853f;
				const auto color = radiance({ std::cos(azimuth) * across, height, std::sin(azimuth) * across }, toSun);
				const size_t at = (static_cast<size_t>(row) * kWidth + column) * 4;
				texels[at] = color.x;
				texels[at + 1] = color.y;
				texels[at + 2] = color.z;
				texels[at + 3] = 1.f;
			}
		});
		written = (written + 1) % ring.size();
		if (ring[written]) {
			rhi->updateTexture(ring[written], 0, 0, kWidth, kHeight, TextureFormat::RGBA32F, texels.data());
		}
		else {
			TextureDesc desc;
			desc.width = kWidth;
			desc.height = kHeight;
			desc.format = TextureFormat::RGBA32F;
			ring[written] = rhi->createTexture(desc, texels.data());
		}
		sky.texture = ring[written];
		bakedFor = toSun;
		bakedAt = now;
		return sky;
	}
}
