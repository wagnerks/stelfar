#include <algorithm>
#include <cstdint>
#include <cstring>

#include "EngineTestFixture.h"
#include "rhi/RenderHardwareInterface.h"

// What a buffer made without data holds before anything writes it (RenderHardwareInterface::createBuffer): zeros,
// never what its memory held before. Pinned here on the None backend, the one these tests run, which keeps a buffer's
// bytes on the CPU: it proves nothing of OpenGL's or Vulkan's (rhi_tests' BufferMadeWithoutDataReadsAsZeros holds the
// two to it on a GPU, with SFE_BUILD_RHI_TESTS).
namespace SFE::EngineTests {
	using namespace Render;

	// Made, filled and let go of, then made again at the size: the new one reads as zeros, as a readback ring read before
	// the GPU's first pass over it must (the particles' caught ring read a freed shader source on OpenGL once).
	TEST(RhiBuffers, OneMadeWithoutDataReadsAsZeros) {
		auto* rhi = Engine::instance()->rhi();
		BufferDesc desc;
		desc.size = 256;
		desc.usage = BufferUsage::Storage;
		desc.cpuAccess = CpuAccess::Read;
		const auto first = rhi->createBuffer(desc);
		ASSERT_TRUE(first);
		auto* written = static_cast<uint8_t*>(rhi->mapBuffer(first, MapMode::Write));
		ASSERT_NE(written, nullptr);
		std::memset(written, 0xAB, desc.size);
		rhi->unmapBuffer(first);
		rhi->destroyBuffer(first);

		const auto buffer = rhi->createBuffer(desc);
		ASSERT_TRUE(buffer);
		const auto* read = static_cast<const uint8_t*>(rhi->mapBuffer(buffer, MapMode::Read));
		ASSERT_NE(read, nullptr);
		EXPECT_EQ(std::count_if(read, read + desc.size, [](uint8_t b) { return b != 0; }), 0)
			<< "a buffer made without data reads what was there before";
		rhi->unmapBuffer(buffer);
		rhi->destroyBuffer(buffer);
	}
}
