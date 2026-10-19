#include "TerrainCollider.h"

#include <algorithm>
#include <cmath>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

namespace SFE::SystemsModule::TerrainCollider {
	namespace {
		constexpr JPH::uint kBlock = 2;
		constexpr JPH::uint kBits = 8;
		// A sample given anew is moved if it is this much higher or lower, metres: more than the field rounds a rut's
		// wall to (its eight bits over a block's heights).
		constexpr float kMoved = 0.002f;
		constexpr float kHole = JPH::HeightFieldShapeConstants::cNoCollisionValue;

		// `heights` written into `field` over `width` x `height` samples from (u, v): a whole number of its blocks at a
		// time, as it keeps them, what lies round them in those blocks read back first. Where the samples it moved are,
		// from the lowest they were or are to the highest.
		JPH::AABox write(JPH::HeightFieldShape& field, const int u, const int v, const int width, const int height, const float* heights,
			JPH::TempAllocator& allocator) {
			const int block = static_cast<int>(field.GetBlockSize());
			const int count = static_cast<int>(field.GetSampleCount());
			const int fromU = u / block * block;
			const int fromV = v / block * block;
			const int toU = (std::min)(((u + width - 1) / block + 1) * block, count);
			const int toV = (std::min)(((v + height - 1) / block + 1) * block, count);
			const int blocksWide = toU - fromU;
			const int blocksHigh = toV - fromV;
			std::vector<float> samples(static_cast<size_t>(blocksWide) * static_cast<size_t>(blocksHigh));
			field.GetHeights(static_cast<JPH::uint>(fromU), static_cast<JPH::uint>(fromV), static_cast<JPH::uint>(blocksWide),
				static_cast<JPH::uint>(blocksHigh), samples.data(), static_cast<JPH::uint>(blocksWide));
			JPH::AABox moved;
			for (int b = 0; b < height; ++b) {
				for (int a = 0; a < width; ++a) {
					float& was = samples[static_cast<size_t>(v + b - fromV) * blocksWide + (u + a - fromU)];
					const float now = heights[static_cast<size_t>(b) * width + a];
					if ((was == kHole) != (now == kHole) || std::fabs(now - was) > kMoved) {
						const auto at = field.GetPosition(static_cast<JPH::uint>(u + a), static_cast<JPH::uint>(v + b));
						for (const float y : { was, now }) {
							if (y != kHole) {
								moved.Encapsulate(JPH::Vec3(at.GetX(), y, at.GetZ()));
							}
						}
					}
					was = now;
				}
			}
			field.SetHeights(static_cast<JPH::uint>(fromU), static_cast<JPH::uint>(fromV), static_cast<JPH::uint>(blocksWide),
				static_cast<JPH::uint>(blocksHigh), samples.data(), static_cast<JPH::uint>(blocksWide), allocator);
			return moved;
		}
	}

	JPH::ShapeRefC shape(const std::vector<float>& heights, const int n, const float step, const Math::Vec3& corner) {
		if (n < 2 || heights.size() < static_cast<size_t>(n) * static_cast<size_t>(n)) {
			return {};
		}
		// The field's sample (u, v) is the terrain's (n - 1 - v, u): along the field's x the terrain's z, along its z
		// back down the terrain's x. A cell's main diagonal there is the terrain's other one.
		std::vector<float> samples(static_cast<size_t>(n) * static_cast<size_t>(n));
		for (int v = 0; v < n; ++v) {
			for (int u = 0; u < n; ++u) {
				samples[static_cast<size_t>(v) * n + u] = heights[static_cast<size_t>(u) * n + (n - 1 - v)];
			}
		}
		JPH::HeightFieldShapeSettings settings(samples.data(), JPH::Vec3::sZero(), JPH::Vec3(step, 1.f, step), static_cast<JPH::uint32>(n));
		settings.mBlockSize = kBlock;
		settings.mBitsPerSample = kBits;
		const auto field = settings.Create();
		if (field.HasError()) {
			return {};
		}
		field.Get()->SetUserData(static_cast<JPH::uint64>(n));
		// Turned so its x runs along the world's +z and its z along the world's -x, its first sample on the terrain's
		// last along x.
		const JPH::Vec3 at(corner.x + static_cast<float>(n - 1) * step, corner.y, corner.z);
		const auto laid = JPH::RotatedTranslatedShapeSettings(at, JPH::Quat::sRotation(JPH::Vec3::sAxisY(), -0.5f * JPH::JPH_PI), field.Get()).Create();
		return laid.HasError() ? JPH::ShapeRefC{} : JPH::ShapeRefC(laid.Get());
	}

	JPH::HeightFieldShape* field(const JPH::Shape& shape) {
		if (shape.GetSubType() != JPH::EShapeSubType::RotatedTranslated) {
			return nullptr;
		}
		const auto* inner = static_cast<const JPH::RotatedTranslatedShape&>(shape).GetInnerShape();
		if (inner->GetSubType() != JPH::EShapeSubType::HeightField || inner->GetUserData() < 2) {
			return nullptr;
		}
		// Each world's own (PhysicsSystem builds one for each), so cutting it in place cuts no other.
		return const_cast<JPH::HeightFieldShape*>(static_cast<const JPH::HeightFieldShape*>(inner));
	}

	bool isGround(const JPH::Shape& shape) {
		return shape.GetSubType() == JPH::EShapeSubType::HeightField || field(shape) != nullptr;
	}

	Grid gridOf(const JPH::Shape& shape) {
		const auto* inner = field(shape);
		if (!inner) {
			return {};
		}
		Grid grid;
		grid.n = static_cast<int>(inner->GetUserData());
		grid.step = inner->GetPosition(1, 0).GetX() - inner->GetPosition(0, 0).GetX();
		const auto at = static_cast<const JPH::RotatedTranslatedShape&>(shape).GetPosition();
		grid.corner = { at.GetX() - static_cast<float>(grid.n - 1) * grid.step, at.GetY(), at.GetZ() };
		return grid;
	}

	void cut(JPH::HeightFieldShape& field, const Grid& grid, const int x, const int z, const int cells, JPH::TempAllocator& allocator) {
		if (cells < 1 || x < 0 || z < 0 || x + cells > grid.n - 1 || z + cells > grid.n - 1) {
			return;
		}
		// Its samples, i in x .. x + cells and j in z .. z + cells, are the field's u = j and v = n - 1 - i.
		const int side = cells + 1;
		const std::vector<float> holes(static_cast<size_t>(side) * static_cast<size_t>(side), kHole);
		write(field, z, grid.n - 1 - (x + cells), side, side, holes.data(), allocator);
	}

	int samples(const int cells, const int per) {
		return (cells + 2) * per + 1;
	}

	JPH::ShapeRefC fine(const Grid& grid, const int x, const int z, const int cells, const int per, const std::vector<float>& heights, const float sink) {
		const int count = samples(cells, per);
		if (cells < 1 || per < 1 || grid.step <= 0.f || heights.size() != static_cast<size_t>(count) * static_cast<size_t>(count)) {
			return {};
		}
		const float spacing = grid.step / static_cast<float>(per);
		const JPH::Vec3 at(grid.corner.x + static_cast<float>(x - 1) * grid.step, 0.f, grid.corner.z + static_cast<float>(z - 1) * grid.step);
		JPH::HeightFieldShapeSettings settings(heights.data(), at, JPH::Vec3(spacing, 1.f, spacing), static_cast<JPH::uint32>(count));
		settings.mBlockSize = kBlock;
		settings.mBitsPerSample = kBits;
		// A field keeps no height outside the range it is made over: given lower later, it would stay where it was.
		settings.mMinHeightValue = *std::min_element(heights.begin(), heights.end()) - (std::max)(sink, 0.f);
		const auto made = settings.Create();
		return made.HasError() ? JPH::ShapeRefC{} : JPH::ShapeRefC(made.Get());
	}

	JPH::AABox setHeights(const JPH::Shape& fine, const int u, const int v, const int width, const int height, const std::vector<float>& heights,
		JPH::TempAllocator& allocator) {
		if (fine.GetSubType() != JPH::EShapeSubType::HeightField || width < 1 || height < 1
			|| heights.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
			return {};
		}
		// Its body's alone: changed in place.
		auto& field = const_cast<JPH::HeightFieldShape&>(static_cast<const JPH::HeightFieldShape&>(fine));
		const int count = static_cast<int>(field.GetSampleCount());
		if (u < 0 || v < 0 || u + width > count || v + height > count) {
			return {};
		}
		return write(field, u, v, width, height, heights.data(), allocator);
	}
}
