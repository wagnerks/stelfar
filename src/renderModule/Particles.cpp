#include "Particles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace SFE::Render::Particles {
	namespace {
		std::mutex gMutex;
		std::vector<GpuParticle> gPending;
		std::mt19937 gRandom{ 0x5eedu };
		BufferHandle gBuffer;
		double gAliveUntil = -1.0;
		uint64_t gEmitted = 0;
		// The catchers: each one's shape and its version, where it is, whether it was ever placed; what they caught.
		struct Held {
			Shape shape;
			uint64_t version = 0;
			Math::Mat4 toLocal{ 1.f };
			bool placed = false;
		};
		std::array<Held, kCatchers> gCatchers;
		static_assert(sizeof(Math::Mat4) == sizeof(GpuCatcher::toLocal));
		std::vector<Caught> gCaught;

		// A point evenly anywhere in the ball of radius 1.
		Math::Vec3 inBall(std::mt19937& random) {
			std::uniform_real_distribution<float> any(-1.f, 1.f);
			for (;;) {
				const Math::Vec3 p{ any(random), any(random), any(random) };
				if (p.x * p.x + p.y * p.y + p.z * p.z <= 1.f) {
					return p;
				}
			}
		}
	}

	void spawn(const Burst& burst, std::mt19937& random, std::vector<GpuParticle>& out) {
		std::uniform_real_distribution<float> unit(0.f, 1.f);
		const auto off = [&](float value, float share) { return value * (1.f + share * (2.f * unit(random) - 1.f)); };
		for (uint32_t i = 0; i < burst.count; ++i) {
			GpuParticle p;
			const auto at = burst.at + inBall(random) * burst.spread;
			const auto velocity = burst.velocity + inBall(random) * burst.scatter;
			const float shade = 1.f - burst.shade * unit(random);
			p.posAge[0] = at.x;
			p.posAge[1] = at.y;
			p.posAge[2] = at.z;
			p.velLife[0] = velocity.x;
			p.velLife[1] = velocity.y;
			p.velLife[2] = velocity.z;
			p.velLife[3] = (std::max)(off(burst.life, burst.lifeSpread), 0.01f);
			p.colour[0] = (std::max)(off(burst.colour.x, burst.tint), 0.f) * shade;
			p.colour[1] = (std::max)(off(burst.colour.y, burst.tint), 0.f) * shade;
			p.colour[2] = (std::max)(off(burst.colour.z, burst.tint), 0.f) * shade;
			p.colour[3] = burst.look == Look::Drop ? burst.stick : burst.opacity;
			p.shape[0] = (std::max)(off(burst.size, burst.sizeSpread), 0.f);
			p.shape[1] = burst.grow;
			p.shape[2] = static_cast<float>(burst.look);
			p.shape[3] = unit(random);
			p.motion[0] = burst.gravity;
			p.motion[1] = burst.drag;
			p.motion[2] = burst.bounce;
			p.motion[3] = burst.floor;
			out.push_back(p);
		}
	}

	void emit(const Burst& burst) {
		std::lock_guard lock(gMutex);
		spawn(burst, gRandom, gPending);
		gEmitted += burst.count;
		// Nothing takes them without a picture drawn (a game without a backend): the oldest go.
		if (gPending.size() > 4 * kMaxSpawn) {
			gPending.erase(gPending.begin(), gPending.end() - 4 * kMaxSpawn);
		}
	}

	std::vector<GpuParticle> take(double now) {
		std::vector<GpuParticle> taken;
		std::lock_guard lock(gMutex);
		if (gPending.size() <= kMaxSpawn) {
			taken.swap(gPending);
		}
		else {
			taken.assign(gPending.begin(), gPending.begin() + kMaxSpawn);
			gPending.erase(gPending.begin(), gPending.begin() + kMaxSpawn);
		}
		for (const auto& p : taken) {
			gAliveUntil = (std::max)(gAliveUntil, now + static_cast<double>(p.velLife[3]) + 0.1);
		}
		return taken;
	}

	uint64_t emitted() {
		std::lock_guard lock(gMutex);
		return gEmitted;
	}

	bool alive(double now) {
		std::lock_guard lock(gMutex);
		return now <= gAliveUntil;
	}

	BufferHandle& buffer() {
		return gBuffer;
	}

	void shape(const uint32_t id, Shape shape) {
		if (id >= kCatchers) {
			return;
		}
		if (shape.filled.size() > kShapeWords) {
			shape = {};
		}
		std::lock_guard lock(gMutex);
		auto& held = gCatchers[id];
		held.shape = std::move(shape);
		++held.version;
	}

	void place(const uint32_t id, const Math::Mat4& toLocal) {
		if (id >= kCatchers) {
			return;
		}
		std::lock_guard lock(gMutex);
		gCatchers[id].toLocal = toLocal;
		gCatchers[id].placed = true;
	}

	std::vector<Caught> caught() {
		std::vector<Caught> told;
		std::lock_guard lock(gMutex);
		told.swap(gCaught);
		return told;
	}

	uint32_t catchers(std::array<GpuCatcher, kCatchers>& out, std::array<uint64_t, kCatchers>& versions) {
		std::lock_guard lock(gMutex);
		uint32_t count = 0;
		for (uint32_t id = 0; id < kCatchers; ++id) {
			const auto& held = gCatchers[id];
			auto& gpu = out[id];
			gpu = {};
			versions[id] = held.version;
			if (!held.placed || held.shape.filled.empty()) {
				continue;
			}
			std::memcpy(gpu.toLocal, &held.toLocal, sizeof(gpu.toLocal));
			gpu.low[0] = held.shape.low.x;
			gpu.low[1] = held.shape.low.y;
			gpu.low[2] = held.shape.low.z;
			gpu.low[3] = held.shape.cell;
			for (size_t k = 0; k < 3; ++k) {
				gpu.cells[k] = held.shape.cells[k];
			}
			gpu.cells[3] = id * kShapeWords;
			count = id + 1;
		}
		return count;
	}

	std::vector<uint32_t> filledOf(const uint32_t id) {
		std::lock_guard lock(gMutex);
		return id < kCatchers ? gCatchers[id].shape.filled : std::vector<uint32_t>{};
	}

	void tell(const std::vector<Caught>& caught) {
		std::lock_guard lock(gMutex);
		gCaught.insert(gCaught.end(), caught.begin(), caught.end());
		// Nobody asking (no game asks): the oldest go.
		if (gCaught.size() > 4 * kMaxCaught) {
			gCaught.erase(gCaught.begin(), gCaught.end() - 4 * kMaxCaught);
		}
	}
}
