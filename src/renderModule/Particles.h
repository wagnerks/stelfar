#pragma once

#include <optional>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "mathModule/Forward.h"
#include "rhi/RenderHardwareInterface.h"

// Particles: bursts of small things let go into the scene, moved on the GPU (particles.cs) -- falling, drifting on the
// wind, bouncing off and settling on whatever the picture shows there (its depth and normals), going on through leaves
// and grass -- and drawn by ParticlePass (chips), ParticleLiquidPass and ParticleClearLiquidPass (drops) and
// ParticleSpritePass (motes, sparks). Drops that stick are caught by what is given as
// a catcher (a vehicle) where they strike its shape, wherever the camera is, and told back (caught). Nothing of them
// reaches the physics or the network: each machine lets go its own at what it sees happen.
namespace SFE::Render::Particles {
	// What a particle is drawn as.
	// - Chip: a small lit piece (a shred of plastic, a splinter, a drop), drawn into the G-buffer and lit as the scene is;
	//   it settles where it lands and shrinks away at its end.
	// - Mote: a soft puff (dust, steam, smoke) laid over the picture, lit by the sun and the sky, growing as it thins out.
	// - Spark: a glowing speck giving off its own light, streaked along its way.
	// - Drop: a blob of a liquid (water, mud), drawn as the liquid it is part of (ParticleLiquidPass): the drops near
	//   each other run together into one body of it, clear or thick as the liquid is (`density`: what stops the light
	//   into the G-buffer, lit as the scene is; what lets it through over the picture), smooth or in lumps as
	//   far round as it gathers (`reach`), mirroring as a liquid's face does or dull (`rough`); in flight drawn out along
	//   its way as far as it holds together; come to rest, spreading out over what it lies on as thin as it goes
	//   (`settle`), those lying by each other a puddle.
	enum class Look : uint32_t { Chip = 0, Mote = 1, Spark = 2, Drop = 3 };

	// A handful of particles let go at once (emit).
	struct Burst {
		Look look = Look::Chip;
		uint32_t count = 1;
		Math::Vec3 at{ 0.f, 0.f, 0.f };       // where they start, each within `spread` metres of it
		float spread = 0.f;
		Math::Vec3 velocity{ 0.f, 0.f, 0.f }; // how they go, each off it by up to `scatter` metres a second, any way
		float scatter = 0.f;
		Math::Vec3 colour{ 1.f, 1.f, 1.f };   // a chip's, a drop's or a mote's albedo, linear; a spark's light
		float shade = 0.f;                    // each's colour darker by up to this share of it
		float tint = 0.f;                     // and each channel of it off by up to this share, apart: of many colours
		float opacity = 1.f;                  // how much of what is behind it a mote hides
		float size = 0.02f;                   // metres across, each off it by up to `sizeSpread` of it
		float sizeSpread = 0.3f;
		float grow = 0.f;                     // metres a second a mote grows across
		float life = 1.f;                     // seconds, each off it by up to `lifeSpread` of it
		float lifeSpread = 0.3f;
		float gravity = 1.f;                  // the share of gravity pulling it down; below nought it rises
		float drag = 0.f;                     // how quickly it takes the wind's speed, a second
		float bounce = 0.3f;                  // what it keeps of its speed into a surface it hits
		float floor = -1e30f;                 // it is gone below this height: a lake's surface for a splash
		float stick = 0.f;                    // a drop's: how much of it sticks to a catcher it strikes, 0 none (water) to 1 (mud)
		float density = 5.f;                  // a drop's: how much of the light through it its liquid stops, of it a metre
		float reach = 2.f;                    // and how far round it gathers with others, of its radius: far, smoothly into one body; just past it, in lumps
		float rough = 0.f;                    // and how rough its face is: 0 a mirror (water), toward 1 dull
		float settle = 0.1f;                  // and how thick it spreads out to lying, of its size: water thin, mud thicker
		// Its colour that of what the picture shows at this point instead, as much darker as `shade` makes it, where it
		// shows it there and not something before it (particles.cs): the ground a wheel threw it up from.
		std::optional<Math::Vec3> colourOf;
	};

	// Any thread: `burst` let go at the next frame. More than a frame takes (kMaxSpawn) wait for the next ones.
	void emit(const Burst& burst);
	// How many were let go since the start, for checks.
	uint64_t emitted();

	// A thing's shape, in its own frame, to catch drops that stick: `cells` (x, y, z) of `cell` metres from `low`, the
	// ones it fills set in `filled`, a bit each, 32 to a word, x fastest, then z, then y (a layer of x and z for each
	// height). A drop striking a filled cell is caught there: gone, and told (caught).
	struct Shape {
		Math::Vec3 low{ 0.f, 0.f, 0.f };
		float cell = 0.05f;
		std::array<uint32_t, 3> cells{};
		std::vector<uint32_t> filled;
	};
	// How many catchers there may be, and the most words of a shape's `filled`.
	constexpr uint32_t kCatchers = 8;
	constexpr uint32_t kShapeWords = 16384;
	// Any thread: catcher `id` (below kCatchers) given `shape`; one with no cells catches nothing.
	void shape(uint32_t id, Shape shape);
	// Any thread: where catcher `id` is now, `toLocal` taking the world into its frame; the next steps catch there.
	void place(uint32_t id, const Math::Mat4& toLocal);
	// A drop caught: by which catcher, where in its frame it struck, and how much it was, litres (its ball's volume
	// times how much of it sticks).
	struct Caught {
		uint32_t catcher = 0;
		Math::Vec3 at{ 0.f, 0.f, 0.f };
		float litres = 0.f;
	};
	// Any thread: what was caught since last asked, some frames late (as the GPU gets to it).
	std::vector<Caught> caught();

	// ---- The passes' -----------------------------------------------------------------------------------------------

	// One particle on the GPU. Mirrors SfeParticle in sfe_particle.glsl, std430.
	struct GpuParticle {
		float posAge[4]{};  // xyz where, w seconds since it was let go
		float velLife[4]{}; // xyz how it goes, w how long it lives: 0 for none
		float colour[4]{};  // rgb albedo or light, a opacity (a drop's: how much of it sticks, Burst::stick)
		float shape[4]{};   // x size, y grow, z look, w a random number of its own in [0, 1)
		float motion[4]{};  // x gravity, y drag, z bounce, w floor
		float liquid[4]{};  // a drop's: x density, y reach, z rough, w settle
		float lying[4]{};   // a drop come to rest (particles.cs): xyz which way what it lies on faces, w its age then; let go
		                    // to take its colour from the picture (Burst::colourOf): xyz where, w minus the share of it it is
	};
	static_assert(sizeof(GpuParticle) == 112);

	// How many live at once at most: the oldest give way to new ones past that.
	constexpr uint32_t kCapacity = 32768;
	// How many a frame lets go at most.
	constexpr uint32_t kMaxSpawn = 512;

	// `burst`'s particles appended to `out`, drawn with `random`.
	void spawn(const Burst& burst, std::mt19937& random, std::vector<GpuParticle>& out);
	// What was let go since last asked, at most kMaxSpawn, taken at `now` (Engine::clockSeconds).
	std::vector<GpuParticle> take(double now);
	// Whether any of what was taken may still live at `now`: nothing to move or draw otherwise.
	bool alive(double now);
	// Whether any drop let go may still be in flight at `now`.
	bool liquid(double now);
	// The particles, ParticlePass's: what ParticleSpritePass draws too. Empty before it made them.
	BufferHandle& buffer();

	// One catcher on the GPU. Mirrors SfeCatcher in particles.cs, std140: the world into its frame (column-major), its
	// low corner (xyz) and its cells' metres (w), its cells across (xyz) and where its shape's words start (w).
	struct GpuCatcher {
		float toLocal[16]{};
		float low[4]{};
		uint32_t cells[4]{};
	};
	static_assert(sizeof(GpuCatcher) == 96);
	// How many caught drops a frame slot holds until they are told back; the oldest give way past that.
	constexpr uint32_t kMaxCaught = 1024;
	// The catchers as placed now into `out` (one with no cells where none is, catcher k's shape at k * kShapeWords),
	// and each one's shape's version (counting up as it is given anew); how many there are, the last given's id + 1.
	uint32_t catchers(std::array<GpuCatcher, kCatchers>& out, std::array<uint64_t, kCatchers>& versions);
	// Catcher `id`'s shape's `filled`, at most kShapeWords.
	std::vector<uint32_t> filledOf(uint32_t id);
	// What the GPU caught, for caught to tell.
	void tell(const std::vector<Caught>& caught);
}
