#include "Terrain.h"

#include <algorithm>
#include <cmath>
#include <execution>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <unordered_map>

#include "assetsModule/AssetsManager.h"
#include "assetsModule/modelModule/MeshLod.h"
#include "assetsModule/TextureHandler.h"
#include "assetsModule/modelModule/Model.h"
#include "core/BinaryCache.h"
#include "core/Engine.h"

namespace AssetsModule {
	namespace {
		// sfeCellHash, sfeValueNoise and sfeFbm of sfe.glsl, bit for bit where the integer part is concerned.
		uint32_t shaderCellHash(int x, int y) {
			const uint32_t vx = static_cast<uint32_t>(x) * 1664525u;
			const uint32_t vy = static_cast<uint32_t>(y) * 1013904223u;
			uint32_t h = vx ^ (vy * 747796405u) ^ 2891336453u;
			h ^= h >> 16u;
			h *= 2246822519u;
			h ^= h >> 13u;
			h *= 3266489917u;
			h ^= h >> 16u;
			return h;
		}

		float shaderValueNoise(float x, float y) {
			const float cx = std::floor(x);
			const float cy = std::floor(y);
			const auto quintic = [](float f) { return f * f * f * (f * (f * 6.f - 15.f) + 10.f); };
			const float fx = quintic(x - cx);
			const float fy = quintic(y - cy);
			const int ix = static_cast<int>(cx);
			const int iy = static_cast<int>(cy);
			const auto corner = [](int i, int j) { return static_cast<float>(shaderCellHash(i, j) >> 8u) / 16777216.f; };
			return std::lerp(std::lerp(corner(ix, iy), corner(ix + 1, iy), fx), std::lerp(corner(ix, iy + 1), corner(ix + 1, iy + 1), fx), fy);
		}

		float shaderFbm(float x, float y, int octaves) {
			float sum = 0.f;
			float amp = 0.5f;
			float norm = 0.f;
			for (int i = 0; i < octaves; ++i) {
				sum += shaderValueNoise(x, y) * amp;
				norm += amp;
				const float nx = 1.6f * x - 1.2f * y + 3.1f;
				const float ny = 1.2f * x + 1.6f * y + 7.7f;
				x = nx;
				y = ny;
				amp *= 0.5f;
			}
			return sum / norm;
		}

		float hash(int x, int z, int seed) {
			uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u + static_cast<uint32_t>(seed) * 2246822519u;
			h = (h ^ (h >> 13)) * 1274126177u;
			h ^= h >> 16;
			return static_cast<float>(h & 0xFFFFFFu) / 16777215.f;
		}

		// Value noise in [0, 1].
		float noise(float x, float z, int seed) {
			const int ix = static_cast<int>(std::floor(x));
			const int iz = static_cast<int>(std::floor(z));
			float fx = x - static_cast<float>(ix);
			float fz = z - static_cast<float>(iz);
			fx = fx * fx * fx * (fx * (fx * 6.f - 15.f) + 10.f);
			fz = fz * fz * fz * (fz * (fz * 6.f - 15.f) + 10.f);
			const float a = hash(ix, iz, seed);
			const float b = hash(ix + 1, iz, seed);
			const float c = hash(ix, iz + 1, seed);
			const float d = hash(ix + 1, iz + 1, seed);
			return a + (b - a) * fx + (c - a) * fz + (a - b - c + d) * fx * fz;
		}

		// Fractal sum in [0, 1].
		float fbm(float x, float z, int seed, int octaves) {
			float sum = 0.f;
			float amp = 0.5f;
			float norm = 0.f;
			for (int i = 0; i < octaves; ++i) {
				sum += noise(x, z, seed + i * 31) * amp;
				norm += amp;
				// Rotated between octaves so the grid does not line up.
				const float nx = 1.6f * x + 1.2f * z;
				const float nz = -1.2f * x + 1.6f * z;
				x = nx;
				z = nz;
				amp *= 0.5f;
			}
			return sum / norm;
		}

		float smoothstep(float e0, float e1, float x) {
			const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
			return t * t * (3.f - 2.f * t);
		}

		// The terrain's shape before the road is levelled into it.
		float naturalHeightAt(const SFE::ComponentsModule::TerrainComponent& t, float x, float z) {
			const int seed = t.seed;
			// The shoreline: an oval, long along z, measured through a warped domain. A slow warp bends the whole
			// outline, a quicker one cuts coves into it and leaves points between them, and a fine one keeps the bank
			// from running smooth between those. All follow the lake's width, so a bigger lake keeps the same kind of
			// outline instead of a busier one.
			const float halfWidth = t.lakeWidth * 0.5f;
			const float halfLength = t.lakeLength * 0.5f;
			const float bendFrequency = 0.9f / t.lakeWidth;
			const float coveFrequency = 3.5f / t.lakeWidth;
			const float fineFrequency = 10.f / t.lakeWidth;
			const float warpX = (fbm(x * bendFrequency, z * bendFrequency, seed + 100, 3) - 0.5f) * halfWidth * 1.3f
				+ (fbm(x * coveFrequency + 31.f, z * coveFrequency - 7.f, seed + 500, 3) - 0.5f) * halfWidth * 1.3f
				+ (fbm(x * fineFrequency + 71.f, z * fineFrequency + 3.f, seed + 700, 2) - 0.5f) * halfWidth * 0.35f;
			const float warpZ = (fbm(x * bendFrequency + 17.f, z * bendFrequency - 9.f, seed + 200, 3) - 0.5f) * halfWidth * 1.3f
				+ (fbm(x * coveFrequency - 13.f, z * coveFrequency + 23.f, seed + 600, 3) - 0.5f) * halfWidth * 1.3f
				+ (fbm(x * fineFrequency - 41.f, z * fineFrequency - 19.f, seed + 800, 2) - 0.5f) * halfWidth * 0.35f;
			const float across = (x + warpX) / halfWidth;
			const float along = (z + warpZ) / halfLength;
			const float d = std::sqrt(across * across + along * along);

			// A bowl under the water and a bank that levels off, with the same slope where they meet at d = 1.
			const float bankHeight = t.lakeDepth * 0.5f;
			float h = d < 1.f
				? -t.lakeDepth * (1.f - d * d)
				: bankHeight * (1.f - std::exp(-(d - 1.f) * 4.f));

			// Rolling ground once clear of the bank, higher towards the edge so the horizon is hills, not a cliff.
			const float away = smoothstep(1.15f, 2.4f, d);
			const float hills = fbm(x * 0.006f, z * 0.006f, seed + 300, 5);
			h += t.hillHeight * away * hills * hills * 1.6f;
			const float edge = (std::max)(std::fabs(x), std::fabs(z)) / (t.size * 0.5f);
			h += t.hillHeight * 0.8f * smoothstep(0.55f, 1.f, edge);

			// Small bumps everywhere, weaker on the lake bed, which silt keeps smooth.
			const float bumps = fbm(x * 0.07f, z * 0.07f, seed + 400, 3) - 0.5f;
			h += bumps * (d < 1.f ? 0.25f : 0.7f) * smoothstep(0.85f, 1.1f, d) + bumps * 0.15f;
			return h;
		}
	}

	namespace {
		// The road as a polyline a metre a point, with its segments bucketed in square cells so a query only looks
		// at the few near it.
		struct RoadLine {
			static constexpr float kCell = 16.f;
			std::vector<SFE::Math::Vec2> points;
			std::vector<float> along; // metres along the line to each point
			float originX = 0.f;
			float originZ = 0.f;
			int cellsX = 0;
			int cellsZ = 0;
			std::vector<std::vector<uint32_t>> cells; // segment i runs from points[i] to points[i + 1]

			// The nearest point of the line to (x, z), and the signed distance to it: positive to the right of the
			// direction the road runs in.
			float distance(float x, float z, SFE::Math::Vec2* nearest = nullptr) const {
				if (cells.empty()) {
					return kRoadFar;
				}
				const int cx = static_cast<int>(std::floor((x - originX) / kCell));
				const int cz = static_cast<int>(std::floor((z - originZ) / kCell));
				if (cx < 0 || cz < 0 || cx >= cellsX || cz >= cellsZ) {
					return kRoadFar;
				}
				float best = kRoadFar;
				float bestSign = 1.f;
				for (const uint32_t i : cells[static_cast<size_t>(cz) * cellsX + cx]) {
					const auto& a = points[i];
					const auto& b = points[i + 1];
					const float abx = b.x - a.x;
					const float abz = b.y - a.y;
					const float lengthSq = (std::max)(abx * abx + abz * abz, 1e-6f);
					const float u = std::clamp(((x - a.x) * abx + (z - a.y) * abz) / lengthSq, 0.f, 1.f);
					const float px = a.x + abx * u;
					const float pz = a.y + abz * u;
					const float d = std::sqrt((x - px) * (x - px) + (z - pz) * (z - pz));
					if (d < best) {
						best = d;
						bestSign = abx * (z - a.y) - abz * (x - a.x) >= 0.f ? 1.f : -1.f;
						if (nearest) {
							*nearest = { px, pz };
						}
					}
				}
				return best * bestSign;
			}

			// Metres across the line (signed as `distance`) and along it from its first point, by the nearest point of
			// the line going on straight past both its ends: a clearing at an end lies in its frame too.
			SFE::Math::Vec2 frame(float x, float z) const {
				SFE::Math::Vec2 best{ kRoadFar, 0.f };
				if (points.size() < 2) {
					return best;
				}
				float bestDistance = kRoadFar;
				const uint32_t last = static_cast<uint32_t>(points.size()) - 2;
				const auto consider = [&](const uint32_t i) {
					const auto& a = points[i];
					const auto& b = points[i + 1];
					const float abx = b.x - a.x;
					const float abz = b.y - a.y;
					const float length = std::sqrt(abx * abx + abz * abz);
					if (length < 1e-4f) {
						return;
					}
					const float u = std::clamp(((x - a.x) * abx + (z - a.y) * abz) / (length * length), i == 0 ? -kRoadFar : 0.f,
						i == last ? kRoadFar : 1.f);
					const float d = std::hypot(x - (a.x + abx * u), z - (a.y + abz * u));
					if (d < bestDistance) {
						bestDistance = d;
						best = { abx * (z - a.y) - abz * (x - a.x) >= 0.f ? d : -d, along[i] + u * length };
					}
				};
				consider(0);
				consider(last);
				const int cx = static_cast<int>(std::floor((x - originX) / kCell));
				const int cz = static_cast<int>(std::floor((z - originZ) / kCell));
				if (cx >= 0 && cz >= 0 && cx < cellsX && cz < cellsZ) {
					for (const uint32_t i : cells[static_cast<size_t>(cz) * cellsX + cx]) {
						consider(i);
					}
				}
				return best;
			}
		};

		// How far from the line a segment is still bucketed: past it the distance does not matter.
		constexpr float kRoadReach = 10.f;

		// A road or a footpath through `control` on a terrain `size` metres a side.
		RoadLine buildLine(const std::vector<SFE::Math::Vec2>& control, const float size) {
			RoadLine line;
			if (control.size() < 2) {
				return line;
			}
			// Catmull-Rom through the points, the ends repeated.
			for (size_t i = 0; i + 1 < control.size(); ++i) {
				const auto& p0 = control[i == 0 ? 0 : i - 1];
				const auto& p1 = control[i];
				const auto& p2 = control[i + 1];
				const auto& p3 = control[(std::min)(i + 2, control.size() - 1)];
				const float span = std::sqrt((p2.x - p1.x) * (p2.x - p1.x) + (p2.y - p1.y) * (p2.y - p1.y));
				const int steps = (std::max)(1, static_cast<int>(std::ceil(span)));
				for (int s = 0; s < steps; ++s) {
					const float u = static_cast<float>(s) / static_cast<float>(steps);
					const auto curve = [&](float a, float b, float c, float d) {
						return 0.5f * (2.f * b + (c - a) * u + (2.f * a - 5.f * b + 4.f * c - d) * u * u + (3.f * b - a - 3.f * c + d) * u * u * u);
					};
					line.points.push_back({ curve(p0.x, p1.x, p2.x, p3.x), curve(p0.y, p1.y, p2.y, p3.y) });
				}
			}
			line.points.push_back(control.back());
			line.along.assign(line.points.size(), 0.f);
			for (size_t i = 1; i < line.points.size(); ++i) {
				const auto& a = line.points[i - 1];
				const auto& b = line.points[i];
				line.along[i] = line.along[i - 1] + std::hypot(b.x - a.x, b.y - a.y);
			}

			const float half = size * 0.5f;
			line.originX = -half - kRoadReach;
			line.originZ = -half - kRoadReach;
			line.cellsX = line.cellsZ = static_cast<int>(std::ceil((size + 2.f * kRoadReach) / RoadLine::kCell));
			line.cells.resize(static_cast<size_t>(line.cellsX) * line.cellsZ);
			for (uint32_t i = 0; i + 1 < line.points.size(); ++i) {
				const auto& a = line.points[i];
				const auto& b = line.points[i + 1];
				const int x0 = std::clamp(static_cast<int>(std::floor(((std::min)(a.x, b.x) - kRoadReach - line.originX) / RoadLine::kCell)), 0, line.cellsX - 1);
				const int x1 = std::clamp(static_cast<int>(std::floor(((std::max)(a.x, b.x) + kRoadReach - line.originX) / RoadLine::kCell)), 0, line.cellsX - 1);
				const int z0 = std::clamp(static_cast<int>(std::floor(((std::min)(a.y, b.y) - kRoadReach - line.originZ) / RoadLine::kCell)), 0, line.cellsZ - 1);
				const int z1 = std::clamp(static_cast<int>(std::floor(((std::max)(a.y, b.y) + kRoadReach - line.originZ) / RoadLine::kCell)), 0, line.cellsZ - 1);
				for (int cz = z0; cz <= z1; ++cz) {
					for (int cx = x0; cx <= x1; ++cx) {
						line.cells[static_cast<size_t>(cz) * line.cellsX + cx].push_back(i);
					}
				}
			}
			return line;
		}

		// The road and the clearings, for the keys of what is built from them.
		std::string roadText(const SFE::ComponentsModule::TerrainComponent& t) {
			std::string text;
			for (const auto& point : t.road) {
				text += std::to_string(point.x) + "," + std::to_string(point.y) + ";";
			}
			for (const auto& clearing : t.clearings) {
				text += "o" + std::to_string(clearing.x) + "," + std::to_string(clearing.y) + "," + std::to_string(clearing.z) + ";";
			}
			return text;
		}

		// The footpaths and bare patches, for the keys of what is built from them. They leave the ground's shape as it
		// is, so the heights and the mesh's levels do not key on them.
		std::string pathText(const SFE::ComponentsModule::TerrainComponent& t) {
			std::string text;
			for (const auto& path : t.paths) {
				text += "p" + std::to_string(path.width) + "," + std::to_string(path.wear) + ":";
				for (const auto& point : path.points) {
					text += std::to_string(point.x) + "," + std::to_string(point.y) + ";";
				}
			}
			for (const auto& patch : t.patches) {
				text += "o" + std::to_string(patch.x) + "," + std::to_string(patch.y) + "," + std::to_string(patch.z) + ","
					+ std::to_string(patch.w) + ";";
			}
			return text;
		}

		const RoadLine& roadOf(const SFE::ComponentsModule::TerrainComponent& t) {
			static std::mutex mutex;
			// Never erased, so a reference handed out stays good.
			static std::unordered_map<std::string, std::unique_ptr<RoadLine>> known;
			const auto key = std::to_string(t.size) + "|" + roadText(t);
			std::lock_guard lock(mutex);
			auto& line = known[key];
			if (!line) {
				line = std::make_unique<RoadLine>(buildLine(t.road, t.size));
			}
			return *line;
		}

		// The natural height with the road levelled in: across the road the ground takes the height of the line
		// under its middle, a few centimetres down, and blends back into the slope beside it. A clearing is flat at the
		// height its middle had, and blends back over kClearingBlend past its edge.
		constexpr float kClearingBlend = 6.f;
		float heightAt(const SFE::ComponentsModule::TerrainComponent& t, const RoadLine& road, float x, float z) {
			float height = naturalHeightAt(t, x, z);
			SFE::Math::Vec2 nearest;
			const float distance = std::fabs(road.distance(x, z, &nearest));
			if (distance < kRoadHalfWidth + 4.f) {
				const float bed = naturalHeightAt(t, nearest.x, nearest.y) - 0.08f;
				height += (bed - height) * (1.f - smoothstep(kRoadHalfWidth + 0.5f, kRoadHalfWidth + 4.f, distance));
			}
			for (const auto& clearing : t.clearings) {
				const float fromMiddle = std::sqrt((x - clearing.x) * (x - clearing.x) + (z - clearing.y) * (z - clearing.y));
				if (fromMiddle < clearing.z + kClearingBlend) {
					const float flat = naturalHeightAt(t, clearing.x, clearing.y) - 0.05f;
					height += (flat - height) * (1.f - smoothstep(clearing.z, clearing.z + kClearingBlend, fromMiddle));
				}
			}
			return height;
		}

		// What a patch's radius is scaled by at x/z: never quite round, it is worn further out on one side than on another.
		float patchLopsided(float x, float z) {
			return 1.f + 0.15f * (shaderFbm(x * 0.25f + 17.f, z * 0.25f + 17.f, 2) * 2.f - 1.f);
		}
	}

	float terrainClearingDistance(const SFE::ComponentsModule::TerrainComponent& t, float x, float z) {
		float distance = kRoadFar;
		for (const auto& clearing : t.clearings) {
			const float fromMiddle = std::sqrt((x - clearing.x) * (x - clearing.x) + (z - clearing.y) * (z - clearing.y));
			distance = (std::min)(distance, fromMiddle - clearing.z);
		}
		return distance;
	}

	float terrainRoadDistance(const SFE::ComponentsModule::TerrainComponent& t, float x, float z) {
		const float distance = roadOf(t).distance(x, z);
		const float trodden = (std::max)(terrainClearingDistance(t, x, z), 0.f) + kClearingDistance;
		return trodden < std::fabs(distance) ? trodden : distance;
	}

	// The footpaths as lines and the patches as they are, and the box round them all past which nothing is looked at.
	struct TerrainPathSet {
		std::vector<RoadLine> lines;
		std::vector<float> halfWidths;
		std::vector<float> wears;
		std::vector<SFE::Math::Vec4> patches;
		float minX = kRoadFar;
		float minZ = kRoadFar;
		float maxX = -kRoadFar;
		float maxZ = -kRoadFar;
	};

	const TerrainPathSet* terrainPathSet(const SFE::ComponentsModule::TerrainComponent& t) {
		if (t.paths.empty() && t.patches.empty()) {
			return nullptr;
		}
		static std::mutex mutex;
		// Never erased, so a pointer handed out stays good.
		static std::unordered_map<std::string, std::unique_ptr<TerrainPathSet>> known;
		const auto key = std::to_string(t.size) + "|" + pathText(t);
		std::lock_guard lock(mutex);
		auto& set = known[key];
		if (set) {
			return set.get();
		}
		set = std::make_unique<TerrainPathSet>();
		const auto reach = [&](const float x, const float z, const float radius) {
			set->minX = (std::min)(set->minX, x - radius - kPathFar);
			set->minZ = (std::min)(set->minZ, z - radius - kPathFar);
			set->maxX = (std::max)(set->maxX, x + radius + kPathFar);
			set->maxZ = (std::max)(set->maxZ, z + radius + kPathFar);
		};
		for (const auto& path : t.paths) {
			set->lines.push_back(buildLine(path.points, t.size));
			set->halfWidths.push_back(path.width * 0.5f);
			set->wears.push_back(path.wear);
			for (const auto& point : path.points) {
				reach(point.x, point.y, path.width * 0.5f);
			}
		}
		set->patches = t.patches;
		for (const auto& patch : t.patches) {
			reach(patch.x, patch.y, patch.z);
		}
		return set.get();
	}

	float terrainPathDistance(const TerrainPathSet* paths, float x, float z) {
		if (!paths || x < paths->minX || x > paths->maxX || z < paths->minZ || z > paths->maxZ) {
			return kPathFar;
		}
		// Run into each other in a curve where they meet, as feet cut the corner.
		constexpr float kJoin = 0.5f;
		float distance = kRoadFar;
		const auto join = [&](const float other) {
			const float h = (std::max)(kJoin - std::fabs(distance - other), 0.f) / kJoin;
			distance = (std::min)(distance, other) - h * h * kJoin * 0.25f;
		};
		for (size_t i = 0; i < paths->lines.size(); ++i) {
			join(std::fabs(paths->lines[i].distance(x, z)) - paths->halfWidths[i]);
		}
		const float lopsided = patchLopsided(x, z);
		for (const auto& patch : paths->patches) {
			join(std::sqrt((x - patch.x) * (x - patch.x) + (z - patch.y) * (z - patch.y)) - patch.z * lopsided);
		}
		// Ragged: the grass creeps in here and is worn back there.
		distance += 0.3f * (shaderFbm(x * 0.45f, z * 0.45f, 3) * 2.f - 1.f);
		return (std::min)(distance, kPathFar);
	}

	float terrainPathDistance(const SFE::ComponentsModule::TerrainComponent& t, float x, float z) {
		return terrainPathDistance(terrainPathSet(t), x, z);
	}

	float terrainPathWear(const TerrainPathSet* paths, float x, float z, float* core) {
		if (core) {
			*core = -kPathFar;
		}
		if (!paths || x < paths->minX || x > paths->maxX || z < paths->minZ || z > paths->maxZ) {
			return 0.f;
		}
		// The worn ground's edge wanders in and out by a noise of its own, so it never runs in a clean curve.
		const float ragged = shaderFbm(x * 0.6f + 5.f, z * 0.6f + 5.f, 3) * 2.f - 1.f;
		float wear = 0.f;
		// A path is a line of grass walked short and thin across its width, thinning out half a metre past it, with a
		// strip trodden to the earth down its middle: unbroken from end to end, a third to two fifths of its width the
		// more it is worn, wider and narrower along it. The line feet keep to wanders a little from side to side.
		const float wander = shaderFbm(x * 0.3f + 9.f, z * 0.3f + 9.f, 2) * 2.f - 1.f;
		const float frayed = 0.06f * (shaderFbm(x * 2.5f + 11.f, z * 2.5f + 11.f, 2) * 2.f - 1.f);
		for (size_t i = 0; i < paths->lines.size(); ++i) {
			const float half = paths->halfWidths[i];
			SFE::Math::Vec2 nearest{ 0.f, 0.f };
			const float centre = std::fabs(paths->lines[i].distance(x, z, &nearest) + 0.6f * half * wander);
			const float across = centre + 0.35f * half * ragged;
			wear = (std::max)(wear, 0.7f * paths->wears[i] * (1.f - smoothstep(0.5f * half, half + 0.5f, across)));
			// Past a metre and a half off its middle the strip matters to nothing.
			if (core && centre < 1.5f) {
				// By a noise at the nearest point of its middle, the same all across it, so it changes only along it.
				const float along = shaderFbm(nearest.x * 0.7f + 3.f, nearest.y * 0.7f + 3.f, 2);
				*core = (std::max)(*core, 0.7f * half * paths->wears[i] * (0.2f + 1.6f * along) + frayed - centre);
			}
		}
		// A patch, worn most in its middle and less and less out to a metre and a half past its edge.
		const float lopsided = patchLopsided(x, z);
		for (const auto& patch : paths->patches) {
			const float edge = patch.z * lopsided;
			const float from = std::sqrt((x - patch.x) * (x - patch.x) + (z - patch.y) * (z - patch.y)) + 0.6f * ragged;
			wear = (std::max)(wear, patch.w * (1.f - smoothstep(edge - 1.5f, edge + 1.5f, from)));
		}
		return wear;
	}

	float terrainBareEarth(float wear, float core, float x, float z) {
		// Spots that spread and join as the wear rises, through the noise's own range: none below a third, two fifths of
		// the ground at a half, two thirds at three quarters, all of it at 1; and a path's strip. Their edges broken by a
		// finer noise.
		const float reach = 0.47f * wear + 0.2f * smoothstep(0.2f, 0.5f, wear) + 0.3f * smoothstep(0.8f, 1.f, wear);
		return (std::max)(reach - shaderFbm(x * 0.5f + 23.f, z * 0.5f + 23.f, 3), 1.5f * core)
			- 0.1f * (shaderFbm(x * 3.f + 7.f, z * 3.f + 7.f, 2) - 0.5f);
	}

	namespace {
		// Bump when heightAt, the mesh built from the heights or its levels of detail change: the cache keys on this too.
		constexpr uint32_t kCacheVersion = 3;

		// Everything the terrain's shape is built from.
		std::string cacheText(const SFE::ComponentsModule::TerrainComponent& t) {
			return std::to_string(kCacheVersion) + ":" + std::to_string(t.size) + ":" + std::to_string(t.resolution) + ":"
				+ std::to_string(t.lakeWidth) + ":" + std::to_string(t.lakeLength) + ":" + std::to_string(t.lakeDepth) + ":" + std::to_string(t.hillHeight)
			+ ":" + std::to_string(t.seed) + ":" + roadText(t);
		}

		uint64_t cacheKey(const SFE::ComponentsModule::TerrainComponent& t) {
			return SFE::BinaryCache::hash(cacheText(t));
		}
	}

	uint64_t terrainCacheKey(const SFE::ComponentsModule::TerrainComponent& t) {
		return SFE::BinaryCache::hash(cacheText(t) + "|" + pathText(t));
	}

	const std::vector<float>& terrainHeights(const SFE::ComponentsModule::TerrainComponent& t) {
		static std::mutex mutex;
		// Never erased, so a reference handed out stays good.
		static std::unordered_map<uint64_t, std::unique_ptr<std::vector<float>>> known;
		const uint64_t key = cacheKey(t);
		std::lock_guard lock(mutex);
		auto& heights = known[key];
		if (heights) {
			return *heights;
		}
		heights = std::make_unique<std::vector<float>>();

		const int n = std::clamp(t.resolution, 2, 2049);
		const size_t count = static_cast<size_t>(n) * static_cast<size_t>(n);
		const auto file = SFE::BinaryCache::path("terrain_heights", key);
		if (SFE::BinaryCache::Reader reader(file); reader) {
			reader.vector(*heights);
			if (reader && heights->size() == count) {
				return *heights;
			}
		}

		const float step = t.size / static_cast<float>(n - 1);
		const float half = t.size * 0.5f;
		heights->resize(count);
		std::vector<int> rows(n);
		std::iota(rows.begin(), rows.end(), 0);
		const auto& road = roadOf(t);
		std::for_each(std::execution::par, rows.begin(), rows.end(), [&](const int j) {
			for (int i = 0; i < n; ++i) {
				(*heights)[static_cast<size_t>(j) * n + i] = heightAt(t, road, -half + i * step, -half + j * step);
			}
		});
		SFE::BinaryCache::Writer writer(file);
		writer.vector(*heights);
		writer.commit();
		return *heights;
	}

	SFE::Render::TextureHandle terrainNoiseTexture(const SFE::ComponentsModule::TerrainComponent& t) {
		const std::string name = "builtin:terrain:noise:" + std::to_string(cacheKey(t));
		if (auto* texture = AssetsManager::instance()->getAsset<Texture>(name)) {
			return texture->texture;
		}
		const int side = (std::max)(2, static_cast<int>(std::ceil(t.size / kTerrainNoiseTexelMetres)));
		std::vector<uint8_t> texels(static_cast<size_t>(side) * side * 4);
		const auto& road = roadOf(t);
		std::vector<int> rows(side);
		std::iota(rows.begin(), rows.end(), 0);
		std::for_each(std::execution::par, rows.begin(), rows.end(), [&](const int j) {
			const float z = (static_cast<float>(j) + 0.5f - side * 0.5f) * kTerrainNoiseTexelMetres;
			for (int i = 0; i < side; ++i) {
				const float x = (static_cast<float>(i) + 0.5f - side * 0.5f) * kTerrainNoiseTexelMetres;
				const auto byte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
				const size_t at = (static_cast<size_t>(j) * side + i) * 4;
				// The same expressions sfe_terrain.glsl and g_buffer_terrain.fs used per pixel.
				texels[at] = byte(shaderFbm(x * 0.018f, z * 0.018f, 4));
				texels[at + 1] = byte(shaderFbm(x * 0.006f + 41.f, z * 0.006f + 41.f, 3));
				texels[at + 2] = byte(shaderFbm(x * 0.045f + 7.f, z * 0.045f + 7.f, 2));
				texels[at + 3] = byte(road.distance(x, z) / kRoadTextureRange + 0.5f);
			}
		});
		SFE::Render::TextureDesc desc;
		desc.width = static_cast<uint32_t>(side);
		desc.height = desc.width;
		desc.format = SFE::Render::TextureFormat::RGBA8;
		auto* texture = AssetsManager::instance()->createAsset<Texture>(name);
		texture->texture = SFE::Engine::instance()->rhi()->createTexture(desc, texels.data());
		return texture->texture;
	}

	SFE::Render::TextureHandle terrainRoadFrameTexture(const SFE::ComponentsModule::TerrainComponent& t) {
		const std::string name = "builtin:terrain:road:" + std::to_string(cacheKey(t));
		if (auto* texture = AssetsManager::instance()->getAsset<Texture>(name)) {
			return texture->texture;
		}
		const int side = (std::max)(2, static_cast<int>(std::ceil(t.size / kTerrainNoiseTexelMetres)));
		std::vector<float> texels(static_cast<size_t>(side) * side * 2);
		const auto& road = roadOf(t);
		std::vector<int> rows(side);
		std::iota(rows.begin(), rows.end(), 0);
		std::for_each(std::execution::par, rows.begin(), rows.end(), [&](const int j) {
			const float z = (static_cast<float>(j) + 0.5f - side * 0.5f) * kTerrainNoiseTexelMetres;
			for (int i = 0; i < side; ++i) {
				const float x = (static_cast<float>(i) + 0.5f - side * 0.5f) * kTerrainNoiseTexelMetres;
				const auto frame = road.frame(x, z);
				const size_t at = (static_cast<size_t>(j) * side + i) * 2;
				texels[at] = frame.x;
				texels[at + 1] = frame.y;
			}
		});
		SFE::Render::TextureDesc desc;
		desc.width = static_cast<uint32_t>(side);
		desc.height = desc.width;
		desc.format = SFE::Render::TextureFormat::RG32F;
		auto* texture = AssetsManager::instance()->createAsset<Texture>(name);
		texture->texture = SFE::Engine::instance()->rhi()->createTexture(desc, texels.data());
		return texture->texture;
	}

	SFE::Render::TextureHandle terrainPathTexture(const SFE::ComponentsModule::TerrainComponent& t) {
		const std::string name = "builtin:terrain:paths:" + std::to_string(terrainCacheKey(t));
		if (auto* texture = AssetsManager::instance()->getAsset<Texture>(name)) {
			return texture->texture;
		}
		// The box round the paths and patches, out to round any clearing that reaches into it, so the clearing's own
		// distance does not stop short in a seam. With none, nothing but the first row, saying no metres a texel.
		const auto* paths = terrainPathSet(t);
		float minX = 0.f;
		float minZ = 0.f;
		float metres = 0.f;
		int width = 2;
		int height = 1;
		if (paths) {
			minX = paths->minX;
			minZ = paths->minZ;
			float maxX = paths->maxX;
			float maxZ = paths->maxZ;
			for (const auto& clearing : t.clearings) {
				const float reach = clearing.z + kPathFar;
				if (clearing.x + reach > minX && clearing.x - reach < maxX && clearing.y + reach > minZ && clearing.y - reach < maxZ) {
					minX = (std::min)(minX, clearing.x - reach);
					minZ = (std::min)(minZ, clearing.y - reach);
					maxX = (std::max)(maxX, clearing.x + reach);
					maxZ = (std::max)(maxZ, clearing.y + reach);
				}
			}
			// Coarser only where the box would not fit a texture otherwise; a path is lost between texels much past this.
			metres = (std::max)(kPathTexelMetres, (std::max)(maxX - minX, maxZ - minZ) / 2047.f);
			width = (std::max)(2, static_cast<int>(std::ceil((maxX - minX) / metres)) + 1);
			height = static_cast<int>(std::ceil((maxZ - minZ) / metres)) + 2;
		}
		std::vector<float> texels(static_cast<size_t>(width) * height * 4, 0.f);
		texels[0] = minX;
		texels[1] = minZ;
		texels[4] = metres;
		std::vector<int> rows(height - 1);
		std::iota(rows.begin(), rows.end(), 0);
		std::for_each(std::execution::par, rows.begin(), rows.end(), [&](const int j) {
			const float z = minZ + static_cast<float>(j) * metres;
			for (int i = 0; i < width; ++i) {
				const float x = minX + static_cast<float>(i) * metres;
				const size_t at = (static_cast<size_t>(j + 1) * width + i) * 4;
				texels[at] = std::clamp(terrainPathDistance(paths, x, z), -kPathFar, kPathFar);
				texels[at + 1] = std::clamp(terrainClearingDistance(t, x, z), -kPathFar, kPathFar);
				float core = 0.f;
				texels[at + 2] = terrainPathWear(paths, x, z, &core);
				texels[at + 3] = core;
			}
		});
		SFE::Render::TextureDesc desc;
		desc.width = static_cast<uint32_t>(width);
		desc.height = static_cast<uint32_t>(height);
		desc.format = SFE::Render::TextureFormat::RGBA32F;
		auto* texture = AssetsManager::instance()->createAsset<Texture>(name);
		texture->texture = SFE::Engine::instance()->rhi()->createTexture(desc, texels.data());
		return texture->texture;
	}

	float terrainNoise(float x, float z, int seed, int octaves) {
		return fbm(x, z, seed, octaves);
	}

	float terrainSurfaceHeight(const SFE::ComponentsModule::TerrainComponent& t, const std::vector<float>& heights, float x, float z, float* slopeX, float* slopeZ) {
		const int n = std::clamp(t.resolution, 2, 2049);
		const float step = t.size / static_cast<float>(n - 1);
		const float half = t.size * 0.5f;
		const float gx = std::clamp((x + half) / step, 0.f, static_cast<float>(n - 1) - 1e-3f);
		const float gz = std::clamp((z + half) / step, 0.f, static_cast<float>(n - 1) - 1e-3f);
		const int i = static_cast<int>(gx);
		const int j = static_cast<int>(gz);
		const float fx = gx - static_cast<float>(i);
		const float fz = gz - static_cast<float>(j);
		const auto corner = [&](int ci, int cj) { return heights[static_cast<size_t>(cj) * n + ci]; };
		const float a = corner(i, j);
		const float b = corner(i + 1, j);
		const float c = corner(i, j + 1);
		const float d = corner(i + 1, j + 1);
		// The cell is split along its b-c diagonal, as the mesh indexes it.
		float h = 0.f;
		float dx = 0.f;
		float dz = 0.f;
		if (fx + fz <= 1.f) {
			h = a + (b - a) * fx + (c - a) * fz;
			dx = b - a;
			dz = c - a;
		}
		else {
			h = d + (c - d) * (1.f - fx) + (b - d) * (1.f - fz);
			dx = d - c;
			dz = d - b;
		}
		if (slopeX) {
			*slopeX = dx / step;
		}
		if (slopeZ) {
			*slopeZ = dz / step;
		}
		return h;
	}

	Model* getOrCreateTerrain(const SFE::ComponentsModule::TerrainComponent& t) {
		const std::string path = "builtin:terrain:" + std::to_string(t.size) + ":" + std::to_string(t.resolution) + ":"
			+ std::to_string(t.lakeWidth) + ":" + std::to_string(t.lakeLength) + ":" + std::to_string(t.lakeDepth) + ":" + std::to_string(t.hillHeight)
			+ ":" + std::to_string(t.seed) + ":" + std::to_string(terrainCacheKey(t));
		auto* assets = AssetsManager::instance();
		if (auto* existing = assets->getAsset<Model>(path)) {
			return existing;
		}

		const int n = std::clamp(t.resolution, 2, 2049);
		const float step = t.size / static_cast<float>(n - 1);
		const float half = t.size * 0.5f;
		const auto& heights = terrainHeights(t);
		const auto heightOf = [&](int i, int j) {
			return heights[static_cast<size_t>(std::clamp(j, 0, n - 1)) * n + std::clamp(i, 0, n - 1)];
		};

		// Square chunks of kTerrainChunkCells cells, each a mesh of its own with its own levels of detail: the scene
		// culls them one by one and picks each one's level by its own distance. One mesh for the whole terrain was
		// always drawn whole, at full detail, into the G-buffer and every shadow cascade.
		const int chunkCells = kTerrainChunkCells;
		const int chunks = (n - 1 + chunkCells - 1) / chunkCells;
		std::vector<SFE::MeshObject3D> chunkMeshes(static_cast<size_t>(chunks) * chunks);
		for (int cz = 0; cz < chunks; ++cz) {
			for (int cx = 0; cx < chunks; ++cx) {
				auto& chunk = chunkMeshes[static_cast<size_t>(cz) * chunks + cx].mesh;
				const int i0 = cx * chunkCells;
				const int j0 = cz * chunkCells;
				const int i1 = (std::min)(i0 + chunkCells, n - 1);
				const int j1 = (std::min)(j0 + chunkCells, n - 1);
				const int side = i1 - i0 + 1;
				for (int j = j0; j <= j1; ++j) {
					for (int i = i0; i <= i1; ++i) {
						const float x = -half + i * step;
						const float z = -half + j * step;
						const float y = heightOf(i, j);
						SFE::Vertex3D v{};
						v.position = { x, y, z };
						const float dx = (heightOf(i + 1, j) - heightOf(i - 1, j)) / (2.f * step);
						const float dz = (heightOf(i, j + 1) - heightOf(i, j - 1)) / (2.f * step);
						v.normal = SFE::Math::normalize(SFE::Math::Vec3{ -dx, 1.f, -dz });
						v.tangent = SFE::Math::normalize(SFE::Math::Vec3{ 1.f, dx, 0.f });
						v.biTangent = SFE::Math::cross(v.normal, v.tangent);
						v.texCoords = { x, z };
						v.texCoords1 = { y, 0.f };
						chunk.vertices.push_back(v);
					}
				}
				for (int j = 0; j < j1 - j0; ++j) {
					for (int i = 0; i < i1 - i0; ++i) {
						const unsigned a = static_cast<unsigned>(j * side + i);
						const unsigned b = a + 1;
						const unsigned c = a + static_cast<unsigned>(side);
						const unsigned d = c + 1;
						// Counter-clockwise seen from above.
						for (const unsigned idx : { a, c, b, b, c, d }) {
							chunk.indices.push_back(idx);
						}
					}
				}
			}
		}
		// The vertices are all distinct, so there is nothing to weld. Every chunk's levels together take a while, so
		// they come from the cache when they can.
		const auto lodFile = SFE::BinaryCache::path("terrain_chunk_lods", cacheKey(t) ^ static_cast<uint64_t>(chunkCells));
		bool cached = false;
		if (SFE::BinaryCache::Reader reader(lodFile); reader) {
			for (auto& chunk : chunkMeshes) {
				uint64_t levels = 0;
				reader.value(levels);
				chunk.mesh.lodIndices.resize(static_cast<size_t>((std::min)(levels, uint64_t{ 8 })));
				for (auto& level : chunk.mesh.lodIndices) {
					reader.vector(level);
				}
			}
			cached = static_cast<bool>(reader);
		}
		if (!cached) {
			// Edges locked, so neighbouring chunks at different levels still meet; and a small error, since a chunk
			// is large on screen long after an object of its size would have dropped a level.
			const SFE::MeshLodOptions options{ .errorPerLevel = 0.005f, .lockBorder = true };
			std::for_each(std::execution::par, chunkMeshes.begin(), chunkMeshes.end(), [&](SFE::MeshObject3D& chunk) {
				SFE::buildMeshLods(chunk.mesh, options);
			});
			SFE::BinaryCache::Writer writer(lodFile);
			for (const auto& chunk : chunkMeshes) {
				writer.value(static_cast<uint64_t>(chunk.mesh.lodIndices.size()));
				for (const auto& level : chunk.mesh.lodIndices) {
					writer.vector(level);
				}
			}
			writer.commit();
		}

		SFE::MeshObject3D meshObj;
		meshObj.material.roughness = 0.9f;
		// The ground materials, one layer each in the order g_buffer_terrain.fs names them.
		std::vector<std::string> albedo;
		std::vector<std::string> normal;
		for (const char* name : { "grass", "moss", "mud", "sand", "rocks", "litter", "track", "pebbles" }) {
			albedo.push_back(std::string("textures/ground/") + name + "_albedo_height.png");
			normal.push_back(std::string("textures/ground/") + name + "_normal_rough_ao.png");
		}
		if (auto* texture = TextureHandler::loadTextureArray("builtin:terrain:albedo", albedo); texture && texture->isValid()) {
			meshObj.material[SFE::DIFFUSE] = SFE::MaterialTexture{ texture->texture, SFE::DIFFUSE, SFE::DIFFUSE };
		}
		if (auto* texture = TextureHandler::loadTextureArray("builtin:terrain:normal", normal); texture && texture->isValid()) {
			meshObj.material[SFE::NORMALS] = SFE::MaterialTexture{ texture->texture, SFE::NORMALS, SFE::NORMALS };
		}
		meshObj.material[SFE::ROUGHNESS] = SFE::MaterialTexture{ terrainNoiseTexture(t), SFE::ROUGHNESS, SFE::ROUGHNESS };
		meshObj.material[SFE::METALLIC] = SFE::MaterialTexture{ terrainRoadFrameTexture(t), SFE::METALLIC, SFE::METALLIC };
		meshObj.material[SFE::SUBSURFACE] = SFE::MaterialTexture{ terrainPathTexture(t), SFE::SUBSURFACE, SFE::SUBSURFACE };

		// The root holds nothing; every chunk is a child with the ground's material and its own bounds. They are drawn in
		// this order, which runs from the middle out: the lake is there and the world is lived in round it, so the ground
		// nearer the camera mostly goes first and the depth test turns away what it hides before it is shaded.
		std::vector<size_t> order(chunkMeshes.size());
		std::iota(order.begin(), order.end(), size_t{ 0 });
		const auto fromMiddle = [&](const size_t c) {
			// Twice the chunk's middle, from its first and last vertices, its opposite corners.
			const auto& vertices = chunkMeshes[c].mesh.vertices;
			const float x = vertices.front().position.x + vertices.back().position.x;
			const float z = vertices.front().position.z + vertices.back().position.z;
			return x * x + z * z;
		};
		std::stable_sort(order.begin(), order.end(), [&](const size_t a, const size_t b) { return fromMiddle(a) < fromMiddle(b); });
		SFE::Tree<SFE::MeshObject3D> tree;
		for (const size_t c : order) {
			auto& chunk = chunkMeshes[c];
			SFE::Math::Vec3 lo = chunk.mesh.vertices.front().position;
			SFE::Math::Vec3 hi = lo;
			for (const auto& v : chunk.mesh.vertices) {
				for (int axis = 0; axis < 3; ++axis) {
					lo[axis] = (std::min)(lo[axis], v.position[axis]);
					hi[axis] = (std::max)(hi[axis], v.position[axis]);
				}
			}
			chunk.material = meshObj.material;
			chunk.aabb = SFE::FrustumModule::AABB(lo, hi);
			tree.addChild(std::move(chunk));
		}
		return assets->createAsset<Model>(path, std::move(tree), Armature{}, std::vector<Animation>{});
	}
}
