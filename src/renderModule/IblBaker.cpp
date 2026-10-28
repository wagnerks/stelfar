#include "IblBaker.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "renderModule/Atmosphere.h"

namespace SFE::Render {
	namespace {
		constexpr int kEnvSize = 64;
		constexpr int kIrrSize = 16;
		constexpr int kPreSize = 64;
		constexpr int kPreMips = 5;
		constexpr int kLutSize = 128;
		constexpr int kSamples = 32;
		constexpr float kPi = 3.14159265f;

		struct F3 {
			float x, y, z;
		};

		F3 add(F3 a, F3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		F3 mul(F3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
		F3 mul(F3 a, F3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }

		float dot3(F3 a, F3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

		F3 norm(F3 v) {
			const float l = std::sqrt(dot3(v, v));
			if (l < 1e-8f) {
				return { 0.f, 1.f, 0.f };
			}
			return mul(v, 1.f / l);
		}

		F3 faceDir(int face, float u, float v) {
			switch (face) {
			case 0: return norm({ 1.f, -v, -u });
			case 1: return norm({ -1.f, -v, u });
			case 2: return norm({ u, 1.f, v });
			case 3: return norm({ u, -1.f, -v });
			case 4: return norm({ u, -v, 1.f });
			default: return norm({ -u, -v, -1.f });
			}
		}

		void dirToUv(F3 d, int& face, float& u, float& v) {
			const float ax = std::fabs(d.x);
			const float ay = std::fabs(d.y);
			const float az = std::fabs(d.z);
			if (ax >= ay && ax >= az) {
				if (d.x > 0.f) {
					face = 0;
					u = -d.z / ax;
					v = -d.y / ax;
				}
				else {
					face = 1;
					u = d.z / ax;
					v = -d.y / ax;
				}
			}
			else if (ay >= az) {
				if (d.y > 0.f) {
					face = 2;
					u = d.x / ay;
					v = d.z / ay;
				}
				else {
					face = 3;
					u = d.x / ay;
					v = -d.z / ay;
				}
			}
			else if (d.z > 0.f) {
				face = 4;
				u = d.x / az;
				v = -d.y / az;
			}
			else {
				face = 5;
				u = -d.x / az;
				v = -d.y / az;
			}
			u = u * 0.5f + 0.5f;
			v = v * 0.5f + 0.5f;
		}

		F3 skyColor(F3 dir) {
			dir = norm(dir);
			const float up = dir.y * 0.5f + 0.5f;
			F3 col{
				0.16f + 0.08f * up,
				0.16f + 0.09f * up,
				0.17f + 0.11f * up
			};
			if (dir.y < -0.15f) {
				col = { 0.10f, 0.095f, 0.09f };
			}

			auto addArea = [&](F3 lightDir, float sharpness, float intensity, F3 color) {
				const float w = std::pow((std::max)(0.f, dot3(dir, norm(lightDir))), sharpness);
				col = add(col, mul(color, w * intensity));
			};
			// Studio key / fill / rim — bright enough for dielectric catchlights.
			addArea({ 0.42f, 0.78f, 0.48f }, 28.f, 22.f, { 1.f, 0.97f, 0.92f });
			addArea({ -0.62f, 0.38f, 0.35f }, 14.f, 5.5f, { 0.65f, 0.75f, 1.f });
			addArea({ -0.15f, 0.22f, -0.96f }, 22.f, 10.f, { 1.f, 0.94f, 0.86f });
			return col;
		}

		F3 sampleEnv(const std::vector<float>& env, F3 dir) {
			int face = 0;
			float u = 0.f;
			float v = 0.f;
			dirToUv(dir, face, u, v);
			int x = static_cast<int>(u * static_cast<float>(kEnvSize - 1) + 0.5f);
			int y = static_cast<int>(v * static_cast<float>(kEnvSize - 1) + 0.5f);
			if (x < 0) { x = 0; }
			if (y < 0) { y = 0; }
			if (x >= kEnvSize) { x = kEnvSize - 1; }
			if (y >= kEnvSize) { y = kEnvSize - 1; }
			const int i = ((face * kEnvSize + y) * kEnvSize + x) * 4;
			return { env[i], env[i + 1], env[i + 2] };
		}

		float radicalInverseVdC(uint32_t bits) {
			bits = (bits << 16u) | (bits >> 16u);
			bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
			bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
			bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
			bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
			return static_cast<float>(bits) * 2.3283064365386963e-10f;
		}

		void hammersley(uint32_t i, uint32_t n, float& u, float& v) {
			u = static_cast<float>(i) / static_cast<float>(n);
			v = radicalInverseVdC(i);
		}

		F3 importanceSampleGGX(float xi1, float xi2, F3 n, float roughness) {
			const float a = roughness * roughness;
			const float phi = 2.f * kPi * xi1;
			const float cosTheta = std::sqrt((1.f - xi2) / (1.f + (a * a - 1.f) * xi2));
			const float sinTheta = std::sqrt(1.f - cosTheta * cosTheta);
			const F3 h{
				std::cos(phi) * sinTheta,
				std::sin(phi) * sinTheta,
				cosTheta
			};
			const F3 up = std::fabs(n.z) < 0.999f ? F3{ 0.f, 0.f, 1.f } : F3{ 1.f, 0.f, 0.f };
			const F3 tangent = norm({
				up.y * n.z - up.z * n.y,
				up.z * n.x - up.x * n.z,
				up.x * n.y - up.y * n.x
			});
			const F3 bitangent{
				n.y * tangent.z - n.z * tangent.y,
				n.z * tangent.x - n.x * tangent.z,
				n.x * tangent.y - n.y * tangent.x
			};
			return norm(add(add(mul(tangent, h.x), mul(bitangent, h.y)), mul(n, h.z)));
		}

		float geometrySchlickGGX(float nDotV, float roughness) {
			const float k = (roughness * roughness) / 2.f;
			return nDotV / (nDotV * (1.f - k) + k);
		}

		float geometrySmith(float nDotV, float nDotL, float roughness) {
			return geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
		}

		void writeRgba(std::vector<float>& dst, int i, F3 c) {
			dst[i] = c.x;
			dst[i + 1] = c.y;
			dst[i + 2] = c.z;
			dst[i + 3] = 1.f;
		}

		// The scene's own sky and the ground under it, the light an outdoor scene is bathed in. Clouds are not
		// drawn in: they only whiten the sky in proportion to how much of it they cover.
		constexpr float kSkyLightCap = 0.9f;

		F3 outdoorColor(F3 dir, const ComponentsModule::SkyComponent& sky, float sunIntensity) {
			const auto toSun = sky.toSun();
			const auto sky3 = [&](const Math::Vec3& d) {
				auto clear = Atmosphere::radiance(d, toSun) * (Atmosphere::kSkyBrightness * sunIntensity);
				// The glow right around the sun is the sun itself as far as lighting goes, and the direct light already
				// carries it: left in, a low sun washes every slope facing it out to beige.
				const float peak = (std::max)({ clear.x, clear.y, clear.z });
				if (peak > kSkyLightCap) {
					clear = clear * (kSkyLightCap / peak);
				}
				const float grey = (clear.x + clear.y + clear.z) / 3.f * 1.6f;
				const float cover = std::clamp(sky.cloudCoverage, 0.f, 1.f) * 0.55f;
				return F3{ clear.x + (grey - clear.x) * cover, clear.y + (grey - clear.y) * cover, clear.z + (grey - clear.z) * cover };
			};
			dir = norm(dir);
			if (dir.y >= 0.f) {
				return sky3({ dir.x, dir.y, dir.z });
			}
			// The ground (the sky's groundAlbedo) lit by the sun and the sky above it.
			const auto sun = Atmosphere::sunTransmittance(toSun) * (sunIntensity * (std::max)(toSun.y, 0.f));
			const F3 zenith = sky3({ 0.f, 1.f, 0.f });
			const F3 irradiance = add(F3{ sun.x, sun.y, sun.z }, mul(zenith, kPi));
			const F3 ground = mul(mul(F3{ sky.groundAlbedo.x, sky.groundAlbedo.y, sky.groundAlbedo.z }, irradiance), 1.f / kPi);
			const F3 horizon = sky3({ dir.x, 0.f, dir.z });
			const float t = std::clamp(-dir.y / 0.08f, 0.f, 1.f);
			return add(mul(horizon, 1.f - t), mul(ground, t));
		}

		void uploadCube(RenderHardwareInterface* rhi, TextureHandle tex, int size, int mip, const std::vector<float>& faces) {
			const size_t faceBytes = static_cast<size_t>(size) * static_cast<size_t>(size) * 4u * sizeof(float);
			rhi->updateCubeMip(tex, static_cast<uint32_t>(mip), faces.data(), faceBytes);
		}
	}

	IblTexels computeIbl(const std::optional<ComponentsModule::SkyComponent>& sky, const float sunIntensity) {
		IblTexels texels;
		std::vector<float> env(6 * kEnvSize * kEnvSize * 4);
		for (int face = 0; face < 6; ++face) {
			for (int y = 0; y < kEnvSize; ++y) {
				for (int x = 0; x < kEnvSize; ++x) {
					const float u = 2.f * (static_cast<float>(x) + 0.5f) / static_cast<float>(kEnvSize) - 1.f;
					const float v = 2.f * (static_cast<float>(y) + 0.5f) / static_cast<float>(kEnvSize) - 1.f;
					const F3 dir = faceDir(face, u, v);
					writeRgba(env, ((face * kEnvSize + y) * kEnvSize + x) * 4, sky ? outdoorColor(dir, *sky, sunIntensity) : skyColor(dir));
				}
			}
		}

		auto& irr = texels.irradiance;
		irr.resize(6 * kIrrSize * kIrrSize * 4);
		for (int face = 0; face < 6; ++face) {
			for (int y = 0; y < kIrrSize; ++y) {
				for (int x = 0; x < kIrrSize; ++x) {
					const float u = 2.f * (static_cast<float>(x) + 0.5f) / static_cast<float>(kIrrSize) - 1.f;
					const float v = 2.f * (static_cast<float>(y) + 0.5f) / static_cast<float>(kIrrSize) - 1.f;
					const F3 n = faceDir(face, u, v);
					const F3 up = std::fabs(n.z) < 0.999f ? F3{ 0.f, 0.f, 1.f } : F3{ 1.f, 0.f, 0.f };
					const F3 tangent = norm({
						up.y * n.z - up.z * n.y,
						up.z * n.x - up.x * n.z,
						up.x * n.y - up.y * n.x
					});
					const F3 bitangent{
						n.y * tangent.z - n.z * tangent.y,
						n.z * tangent.x - n.x * tangent.z,
						n.x * tangent.y - n.y * tangent.x
					};

					F3 acc{ 0.f, 0.f, 0.f };
					float weight = 0.f;
					const int phiSteps = 16;
					const int thetaSteps = 8;
					for (int ip = 0; ip < phiSteps; ++ip) {
						const float phi = (static_cast<float>(ip) + 0.5f) / static_cast<float>(phiSteps) * 2.f * kPi;
						for (int it = 0; it < thetaSteps; ++it) {
							const float theta = (static_cast<float>(it) + 0.5f) / static_cast<float>(thetaSteps) * 0.5f * kPi;
							const float sinT = std::sin(theta);
							const float cosT = std::cos(theta);
							const F3 l = norm(add(add(mul(tangent, std::cos(phi) * sinT), mul(bitangent, std::sin(phi) * sinT)), mul(n, cosT)));
							acc = add(acc, mul(sampleEnv(env, l), cosT * sinT));
							weight += cosT * sinT;
						}
					}
					writeRgba(irr, ((face * kIrrSize + y) * kIrrSize + x) * 4, mul(acc, kPi / weight));
				}
			}
		}

		for (int mip = 0; mip < kPreMips; ++mip) {
			int size = kPreSize >> mip;
			if (size < 1) {
				size = 1;
			}
			const float roughness = static_cast<float>(mip) / static_cast<float>(kPreMips - 1);
			auto& pre = texels.prefilter.emplace_back(static_cast<size_t>(6 * size * size * 4));
			for (int face = 0; face < 6; ++face) {
				for (int y = 0; y < size; ++y) {
					for (int x = 0; x < size; ++x) {
						const float u = 2.f * (static_cast<float>(x) + 0.5f) / static_cast<float>(size) - 1.f;
						const float v = 2.f * (static_cast<float>(y) + 0.5f) / static_cast<float>(size) - 1.f;
						const F3 n = faceDir(face, u, v);
						const F3 vDir = n;

						F3 acc{ 0.f, 0.f, 0.f };
						float weight = 0.f;
						for (int i = 0; i < kSamples; ++i) {
							float xi1 = 0.f;
							float xi2 = 0.f;
							hammersley(static_cast<uint32_t>(i), static_cast<uint32_t>(kSamples), xi1, xi2);
							const F3 h = importanceSampleGGX(xi1, xi2, n, roughness < 0.04f ? 0.04f : roughness);
							F3 l = add(mul(h, 2.f * dot3(vDir, h)), mul(vDir, -1.f));
							l = norm(l);
							const float nDotL = dot3(n, l);
							if (nDotL > 0.f) {
								acc = add(acc, mul(sampleEnv(env, l), nDotL));
								weight += nDotL;
							}
						}
						if (weight > 0.f) {
							acc = mul(acc, 1.f / weight);
						}
						writeRgba(pre, ((face * size + y) * size + x) * 4, acc);
					}
				}
			}
		}

		auto& lut = texels.brdfLut;
		lut.resize(kLutSize * kLutSize * 4);
		for (int y = 0; y < kLutSize; ++y) {
			for (int x = 0; x < kLutSize; ++x) {
				const float nDotV = (static_cast<float>(x) + 0.5f) / static_cast<float>(kLutSize);
				const float roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(kLutSize);
				const F3 vDir{ std::sqrt(1.f - nDotV * nDotV), 0.f, nDotV };
				const F3 n{ 0.f, 0.f, 1.f };

				float a = 0.f;
				float b = 0.f;
				for (int i = 0; i < kSamples; ++i) {
					float xi1 = 0.f;
					float xi2 = 0.f;
					hammersley(static_cast<uint32_t>(i), static_cast<uint32_t>(kSamples), xi1, xi2);
					const F3 h = importanceSampleGGX(xi1, xi2, n, roughness < 0.04f ? 0.04f : roughness);
					F3 l = add(mul(h, 2.f * dot3(vDir, h)), mul(vDir, -1.f));
					l = norm(l);
					const float nDotL = l.z > 0.f ? l.z : 0.f;
					const float nDotH = h.z > 0.f ? h.z : 0.f;
					const float vDotH = dot3(vDir, h);
					const float vDotHClamped = vDotH > 0.f ? vDotH : 0.f;
					if (nDotL > 0.f) {
						const float g = geometrySmith(nDotV, nDotL, roughness < 0.04f ? 0.04f : roughness);
						const float gVis = (g * vDotHClamped) / (nDotH * nDotV + 1e-5f);
						const float fc = std::pow(1.f - vDotHClamped, 5.f);
						a += (1.f - fc) * gVis;
						b += fc * gVis;
					}
				}
				a /= static_cast<float>(kSamples);
				b /= static_cast<float>(kSamples);
				const int i = (y * kLutSize + x) * 4;
				lut[i] = a;
				lut[i + 1] = b;
				lut[i + 2] = 0.f;
				lut[i + 3] = 1.f;
			}
		}

		return texels;
	}

	void uploadIbl(RenderHardwareInterface* rhi, const IblTexels& texels, IblMaps& maps) {
		// The maps made the first time, the BRDF table in them then, as it is the same whatever the sky.
		if (!maps.irradiance) {
			TextureDesc irrDesc;
			irrDesc.width = kIrrSize;
			irrDesc.height = kIrrSize;
			irrDesc.dimension = TextureDimension::Cube;
			irrDesc.format = TextureFormat::RGBA32F;
			irrDesc.mipLevels = 1;
			maps.irradiance = rhi->createTexture(irrDesc, nullptr);

			TextureDesc preDesc;
			preDesc.width = kPreSize;
			preDesc.height = kPreSize;
			preDesc.dimension = TextureDimension::Cube;
			preDesc.format = TextureFormat::RGBA32F;
			preDesc.mipLevels = kPreMips;
			maps.prefilter = rhi->createTexture(preDesc, nullptr);

			TextureDesc lutDesc;
			lutDesc.width = kLutSize;
			lutDesc.height = kLutSize;
			lutDesc.format = TextureFormat::RGBA32F;
			maps.brdfLut = rhi->createTexture(lutDesc, texels.brdfLut.data());

			SamplerDesc cubeSamp;
			cubeSamp.minFilter = FilterMode::Linear;
			cubeSamp.magFilter = FilterMode::Linear;
			cubeSamp.addressU = AddressMode::ClampToEdge;
			cubeSamp.addressV = AddressMode::ClampToEdge;
			cubeSamp.addressW = AddressMode::ClampToEdge;
			maps.irradianceSampler = rhi->createSampler(cubeSamp);

			SamplerDesc mipSamp = cubeSamp;
			mipSamp.minFilter = FilterMode::LinearMipmapLinear;
			maps.prefilterSampler = rhi->createSampler(mipSamp);

			maps.lutSampler = rhi->createSampler(cubeSamp);
		}
		uploadCube(rhi, maps.irradiance, kIrrSize, 0, texels.irradiance);
		for (int mip = 0; mip < kPreMips; ++mip) {
			uploadCube(rhi, maps.prefilter, (std::max)(kPreSize >> mip, 1), mip, texels.prefilter[mip]);
		}
	}

	IblMaps bakeIbl(RenderHardwareInterface* rhi, const float sunIntensity) {
		IblMaps maps{};
		if (rhi) {
			uploadIbl(rhi, computeIbl(Atmosphere::sceneSky(), sunIntensity), maps);
		}
		return maps;
	}
}
