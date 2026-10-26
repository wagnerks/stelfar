#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Asset.h"
#include "containersModule/Singleton.h"
#include "rhi/RenderHardwareInterface.h"

namespace AssetsModule {
	// How a texture read from a file is kept on the device. Compressed takes a quarter of Raw's RGBA8 or less: BC4 of
	// the red when the image is grey and opaque, read as grey in r, g and b, and BC7 otherwise. Encoding is slow, so
	// the levels are kept in the cache, by the file's contents (BinaryCache).
	enum class TextureStorage : uint8_t { Raw, Compressed };

	class Texture : public Asset {
	public:
		Texture(const Texture& other) = delete;
		Texture(Texture&& other) noexcept = delete;
		Texture& operator=(const Texture& other) = delete;
		Texture& operator=(Texture&& other) noexcept = delete;

		Texture() = default;

		bool isValid() const;

		SFE::Render::TextureHandle texture;
		SFE::Render::SamplerHandle sampler;
		bool hasAlpha = false;
		bool hasCutoutAlpha = false;
		float fgMean = 0.f;
		// How it was read from its file, to read it again the same way (TextureHandler::reloadChanged).
		int width = 0;
		int height = 0;
		bool flip = false;
		TextureStorage storage = TextureStorage::Raw;
		// What the device holds it as.
		SFE::Render::TextureFormat format = SFE::Render::TextureFormat::RGBA8;
	};

	class TextureHandler : public SFE::Singleton<TextureHandler> {
		friend Singleton;
	public:
		Texture mDefaultTex;

		// Any thread; the texture is on the device when it returns. One that does not read is mDefaultTex, which is
		// not isValid().
		static Texture* loadTexture(const std::string& path, bool flip = false, TextureStorage storage = TextureStorage::Raw);
		// Image files read ahead, in parallel, for loadTexture (unflipped, Compressed) to upload without reading them
		// again. Whatever it did not take is let go with this.
		class Predecoded {
		public:
			Predecoded() = default;
			Predecoded(Predecoded&&) = default;
			Predecoded& operator=(Predecoded&&) = default; // over an empty one only
			Predecoded(const Predecoded&) = delete;
			Predecoded& operator=(const Predecoded&) = delete;
			~Predecoded();
		private:
			friend TextureHandler;
			std::vector<std::string> mPaths; // the ones read ahead here
		};
		[[nodiscard]] static Predecoded predecode(const std::vector<std::string>& paths);
		// A texture array, one layer per image in order, every image the same size and, compressed, the same format (a
		// grey layer among colour ones is not); unflipped, with mips. Kept under `name`.
		static Texture* loadTextureArray(const std::string& name, const std::vector<std::string>& paths);
		// An image file's pixels on the CPU: RGBA8, rows from the top, unflipped; empty when it does not read. Any thread.
		static std::vector<unsigned char> readPixels(const std::string& path, int& width, int& height);
		// Every texture read from a file that has been written since, read again into the same texture: whatever draws
		// with it shows the new picture. One whose size or format changed is left as it was, and named in `unchanged`.
		// From the device thread, between frames. How many were read again.
		static size_t reloadChanged(std::string& unchanged);
		// Levels 1..n of an RGBA8 image, each a 2x2 box average of the one above it. Built here rather than by
		// the driver: OpenGL and Vulkan produce different chains, and the difference shows up at the horizon,
		// where a pixel on the alpha test threshold then flips from one backend to the other.
		static std::vector<std::vector<unsigned char>> buildMipChain(const unsigned char* rgba, int width, int height);
	};
}
