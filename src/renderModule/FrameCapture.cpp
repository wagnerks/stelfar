#include "FrameCapture.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "core/Engine.h"
#include "core/FileSystem.h"
#include "logsModule/logger.h"
#include "rhi/RenderHardwareInterface.h"

namespace SFE::Render {
	namespace {
		// A PNG needs a zlib stream, and zlib's "stored" block type lets us emit one without
		// pulling in a compressor. The file is bigger than a deflated PNG but is a plain, valid
		// PNG that any tool reads -- worth it to keep this dependency-free.
		uint32_t crc32Of(const uint8_t* data, const size_t len, uint32_t crc = 0xFFFFFFFFu) {
			static uint32_t table[256];
			static bool ready = false;
			if (!ready) {
				for (uint32_t n = 0; n < 256; ++n) {
					uint32_t c = n;
					for (int k = 0; k < 8; ++k) {
						c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
					}
					table[n] = c;
				}
				ready = true;
			}
			for (size_t i = 0; i < len; ++i) {
				crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
			}
			return crc;
		}

		uint32_t adler32Of(const uint8_t* data, const size_t len) {
			uint32_t a = 1;
			uint32_t b = 0;
			for (size_t i = 0; i < len; ++i) {
				a = (a + data[i]) % 65521u;
				b = (b + a) % 65521u;
			}
			return (b << 16) | a;
		}

		void putBigEndian32(std::vector<uint8_t>& out, const uint32_t value) {
			out.push_back(static_cast<uint8_t>(value >> 24));
			out.push_back(static_cast<uint8_t>(value >> 16));
			out.push_back(static_cast<uint8_t>(value >> 8));
			out.push_back(static_cast<uint8_t>(value));
		}

		void putChunk(std::vector<uint8_t>& out, const char (&type)[5], const std::vector<uint8_t>& data) {
			putBigEndian32(out, static_cast<uint32_t>(data.size()));
			const size_t crcStart = out.size();
			out.insert(out.end(), type, type + 4);
			out.insert(out.end(), data.begin(), data.end());
			putBigEndian32(out, crc32Of(out.data() + crcStart, out.size() - crcStart) ^ 0xFFFFFFFFu);
		}
	}

	bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, const int width, const int height) {
		if (width <= 0 || height <= 0) {
			return false;
		}
		const size_t rowBytes = static_cast<size_t>(width) * 4u;
		if (rgba.size() < rowBytes * static_cast<size_t>(height)) {
			LogsModule::Logger::LOG_ERROR("FrameCapture: pixel buffer too small for %dx%d", width, height);
			return false;
		}

		// Each scanline is prefixed with filter type 0 (None).
		std::vector<uint8_t> raw;
		raw.reserve((rowBytes + 1u) * static_cast<size_t>(height));
		for (int y = 0; y < height; ++y) {
			raw.push_back(0);
			const uint8_t* row = rgba.data() + rowBytes * static_cast<size_t>(y);
			raw.insert(raw.end(), row, row + rowBytes);
		}

		std::vector<uint8_t> zlib;
		zlib.reserve(raw.size() + raw.size() / 65535u * 5u + 16u);
		zlib.push_back(0x78); // CMF: deflate, 32K window
		zlib.push_back(0x01); // FLG: no preset dictionary, fastest
		size_t offset = 0;
		do {
			const size_t blockLen = (std::min)(static_cast<size_t>(65535), raw.size() - offset);
			const bool isLast = (offset + blockLen) >= raw.size();
			zlib.push_back(isLast ? 1 : 0); // BFINAL, BTYPE = 00 (stored)
			zlib.push_back(static_cast<uint8_t>(blockLen & 0xFFu));
			zlib.push_back(static_cast<uint8_t>((blockLen >> 8) & 0xFFu));
			zlib.push_back(static_cast<uint8_t>(~blockLen & 0xFFu));
			zlib.push_back(static_cast<uint8_t>((~blockLen >> 8) & 0xFFu));
			zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + blockLen);
			offset += blockLen;
		} while (offset < raw.size());
		putBigEndian32(zlib, adler32Of(raw.data(), raw.size()));

		std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		{
			std::vector<uint8_t> ihdr;
			putBigEndian32(ihdr, static_cast<uint32_t>(width));
			putBigEndian32(ihdr, static_cast<uint32_t>(height));
			ihdr.push_back(8); // bit depth
			ihdr.push_back(6); // colour type: RGBA
			ihdr.push_back(0); // compression
			ihdr.push_back(0); // filter
			ihdr.push_back(0); // interlace
			putChunk(png, "IHDR", ihdr);
		}
		putChunk(png, "IDAT", zlib);
		putChunk(png, "IEND", {});

		std::error_code ec;
		const std::filesystem::path out(path);
		if (out.has_parent_path()) {
			std::filesystem::create_directories(out.parent_path(), ec);
		}
		std::ofstream file(out, std::ios::binary | std::ios::trunc);
		if (!file) {
			LogsModule::Logger::LOG_ERROR("FrameCapture: cannot open %s", path.c_str());
			return false;
		}
		file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
		if (!file) {
			LogsModule::Logger::LOG_ERROR("FrameCapture: write failed %s", path.c_str());
			return false;
		}
		return true;
	}

	uint32_t crcOfPixels(const std::vector<uint8_t>& rgba) {
		return crc32Of(rgba.data(), rgba.size()) ^ 0xFFFFFFFFu;
	}

	bool captureFrameToPng(const std::string& path) {
		auto* rhi = Engine::instance() ? Engine::instance()->rhi() : nullptr;
		if (!rhi) {
			LogsModule::Logger::LOG_ERROR("FrameCapture: no RHI");
			return false;
		}

		std::vector<uint8_t> rgba;
		int width = 0;
		int height = 0;
		if (!rhi->readbackTexture(rhi->presentSourceTexture(), rgba, width, height)) {
			LogsModule::Logger::LOG_ERROR("FrameCapture: reading the present source failed (%s)",
				graphicsBackendName(rhi->backend()));
			return false;
		}

		// A relative path is under the application's root, as every write is: a picture never lands among the engine's
		// assets, even where the engine's folder has a file of that name.
		const std::filesystem::path requested(path);
		const std::string resolved = requested.is_absolute()
			? requested.string()
			: (FileSystem::root() / requested).lexically_normal().string();

		if (!writePng(resolved, rgba, width, height)) {
			return false;
		}
		LogsModule::Logger::LOG_INFO("FrameCapture: wrote %s (%dx%d, %s)",
			resolved.c_str(), width, height, graphicsBackendName(rhi->backend()));
		return true;
	}
}
