#include "TextureHandler.h"

#include <stb_image.h>
#include <bc7enc.h>
#include <rgbcx.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <execution>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "AssetsManager.h"
#include "assetsModule/modelModule/ModelRelease.h"
#include "core/BinaryCache.h"
#include "core/FileSystem.h"
#include "rhi/GraphicsBackend.h"
#include "core/Engine.h"
#include "multithreading/ThreadPool.h"
#include "logsModule/logger.h"

using namespace AssetsModule;

bool Texture::isValid() const {
	return texture;
}

namespace {
	struct StbFree {
		void operator()(unsigned char* data) const { stbi_image_free(data); }
	};

	// An image read from its file, with what the loaders learn from its pixels.
	struct DecodedImage {
		std::unique_ptr<unsigned char, StbFree> data; // RGBA8
		// A compressed one's instead of `data`: every level, the first one included, in `format`.
		std::vector<std::vector<unsigned char>> blocks;
		SFE::Render::TextureFormat format = SFE::Render::TextureFormat::RGBA8;
		SFE::Render::TextureSwizzle swizzle = SFE::Render::TextureSwizzle::None;
		int width = 0;
		int height = 0;
		bool hasAlpha = false;
		bool hasCutoutAlpha = false;
		float fgMean = 0.f;
		// Where the RHI prepares textures on any thread, the texture itself, and no pixels any more.
		std::unique_ptr<SFE::Render::PreparedTexture> prepared;

		bool read() const { return data || !blocks.empty() || prepared; }
	};

	// A texture loaded from a file, in the image's format, with a full mip chain. Without the levels a distant
	// character samples one texel of a full size texture per pixel: the armour sparkles, and TAA averages the sparkle
	// into grey.
	SFE::Render::TextureDesc fileTextureDesc(const DecodedImage& image) {
		SFE::Render::TextureDesc desc;
		desc.width = static_cast<uint32_t>(image.width);
		desc.height = static_cast<uint32_t>(image.height);
		desc.format = image.format;
		desc.swizzle = image.swizzle;
		desc.dimension = SFE::Render::TextureDimension::Tex2D;
		desc.mipLevels = SFE::Render::mipLevelsFor(desc.width, desc.height);
		return desc;
	}

	// Every level of the image, the first one included, as the device takes them. An RGBA8 image's chain is built into
	// `chain`, which has to outlive them.
	std::vector<const void*> levelsOf(const DecodedImage& image, std::vector<std::vector<unsigned char>>& chain) {
		std::vector<const void*> levels;
		if (!image.blocks.empty()) {
			for (const auto& level : image.blocks) {
				levels.push_back(level.data());
			}
			return levels;
		}
		chain = AssetsModule::TextureHandler::buildMipChain(image.data.get(), image.width, image.height);
		levels.push_back(image.data.get());
		for (const auto& level : chain) {
			levels.push_back(level.data());
		}
		return levels;
	}

	// Repeating, trilinear and anisotropic.
	SFE::Render::SamplerDesc fileSamplerDesc() {
		SFE::Render::SamplerDesc samplerDesc;
		samplerDesc.minFilter = SFE::Render::FilterMode::LinearMipmapLinear;
		samplerDesc.magFilter = SFE::Render::FilterMode::Linear;
		samplerDesc.addressU = SFE::Render::AddressMode::Repeat;
		samplerDesc.addressV = SFE::Render::AddressMode::Repeat;
		samplerDesc.addressW = SFE::Render::AddressMode::Repeat;
		// Without it a surface seen at a glancing angle picks the mip for the axis it is stretched along and
		// loses the detail on the other one, which looks like a smeared, low resolution texture. Both backends
		// clamp this to what the device allows. It is also the one piece of texture filtering the two implement
		// differently, so the frame comparison shows about 6% of pixels differing at the horizon rather than 1%.
		samplerDesc.maxAnisotropy = 16.f;
		return samplerDesc;
	}

	// Where the RHI can, the texture with its whole chain uploaded right here, on whatever thread this is (see
	// RenderHardwareInterface::prepareTexture), and the pixels let go; otherwise nothing changes.
	void prepare(DecodedImage& image, const SFE::Render::TextureDesc& desc) {
		auto* rhi = SFE::Engine::instance()->rhi();
		if (image.prepared || !image.read() || !rhi->preparesTextures()) {
			return;
		}
		std::vector<std::vector<unsigned char>> chain;
		if ((image.prepared = rhi->prepareTexture(desc, levelsOf(image, chain)))) {
			image.data.reset();
			image.blocks = {};
		}
	}

	// An image file's bytes; empty when it does not read.
	std::string readBytes(const std::string& path) {
		std::ifstream in(SFE::FileSystem::resolve(path), std::ios::binary | std::ios::ate);
		std::string bytes(in ? static_cast<size_t>(in.tellg()) : 0u, '\0');
		in.seekg(0);
		in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		return in ? bytes : std::string{};
	}

	DecodedImage decodePixels(const std::string& file, const bool flip) {
		DecodedImage image;
		int channels = 0;
		// stb's flag for this thread only: loads run on several at once.
		stbi_set_flip_vertically_on_load_thread(flip);
		image.data.reset(stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(file.data()), static_cast<int>(file.size()),
			&image.width, &image.height, &channels, 4));
		if (!image.data) {
			return image;
		}
		const int n = image.width * image.height;
		double fgSum = 0.0;
		int fgCount = 0;
		int partialAlpha = 0;
		for (int i = 0; i < n; ++i) {
			const unsigned char* px = image.data.get() + i * 4;
			if (px[3] < 250) {
				image.hasAlpha = true;
			}
			if (px[3] > 8 && px[3] < 250) {
				++partialAlpha;
			}
			const float m = (std::max)(px[0], (std::max)(px[1], px[2])) / 255.f;
			if (m > 0.04f) {
				fgSum += m;
				++fgCount;
			}
		}
		image.hasCutoutAlpha = partialAlpha > static_cast<int>(n * 0.015);
		image.fgMean = fgCount > 0 ? static_cast<float>(fgSum / static_cast<double>(fgCount)) : 0.f;
		return image;
	}

	// The image and its chain block compressed, in place of its pixels: BC4 of the red when it is grey and opaque, read
	// as grey (TextureSwizzle::Grey), BC7 otherwise. Grey allows two levels between the channels, which a grey picture
	// saved as colour -- a JPEG above all -- has.
	void compress(DecodedImage& image) {
		static std::once_flag once;
		std::call_once(once, [] {
			bc7enc_compress_block_init();
			rgbcx::init();
		});
		const size_t texels = static_cast<size_t>(image.width) * image.height;
		bool grey = !image.hasAlpha;
		for (size_t i = 0; grey && i < texels; ++i) {
			const unsigned char* px = image.data.get() + i * 4;
			grey = std::abs(px[0] - px[1]) <= 2 && std::abs(px[0] - px[2]) <= 2;
		}
		image.format = grey ? SFE::Render::TextureFormat::BC4_UNORM : SFE::Render::TextureFormat::BC7_UNORM;
		image.swizzle = grey ? SFE::Render::TextureSwizzle::Grey : SFE::Render::TextureSwizzle::None;

		bc7enc_compress_block_params params;
		bc7enc_compress_block_params_init(&params);
		// Normal maps and packed masks are data rather than colour: the error is counted in RGB, not in luma.
		bc7enc_compress_block_params_init_linear_weights(&params);
		std::vector<std::vector<unsigned char>> chain;
		const auto levels = levelsOf(image, chain);
		const size_t blockBytes = grey ? 8u : 16u;
		image.blocks.resize(levels.size());
		for (size_t level = 0; level < levels.size(); ++level) {
			const int w = (std::max)(1, image.width >> level);
			const int h = (std::max)(1, image.height >> level);
			const int blocksWide = (w + 3) / 4;
			auto& out = image.blocks[level];
			out.resize(static_cast<size_t>(blocksWide) * ((h + 3) / 4) * blockBytes);
			const auto* src = static_cast<const unsigned char*>(levels[level]);
			std::vector<int> rows((h + 3) / 4);
			std::iota(rows.begin(), rows.end(), 0);
			std::for_each(std::execution::par, rows.begin(), rows.end(), [&](const int by) {
				unsigned char pixels[16 * 4];
				for (int bx = 0; bx < blocksWide; ++bx) {
					for (int y = 0; y < 4; ++y) {
						for (int x = 0; x < 4; ++x) {
							// Past the edge of a side that is not a multiple of 4: the last texel again.
							const int sx = (std::min)(bx * 4 + x, w - 1);
							const int sy = (std::min)(by * 4 + y, h - 1);
							std::memcpy(pixels + (y * 4 + x) * 4, src + (static_cast<size_t>(sy) * w + sx) * 4, 4);
						}
					}
					unsigned char* block = out.data() + (static_cast<size_t>(by) * blocksWide + bx) * blockBytes;
					if (grey) {
						rgbcx::encode_bc4_hq(block, pixels);
					}
					else {
						bc7enc_compress_block(block, pixels, &params);
					}
				}
			});
		}
		image.data.reset();
	}

	// What the cache holds ahead of a compressed image's levels. Bump kCompressedVersion when it, the encoders or their
	// settings change.
	constexpr int kCompressedVersion = 1;
	struct CompressedHeader {
		uint32_t width;
		uint32_t height;
		uint8_t format;
		uint8_t swizzle;
		uint8_t hasAlpha;
		uint8_t hasCutoutAlpha;
		float fgMean;
	};

	// A compressed image as the cache keeps one, every level: false when there is none whole there.
	bool readCached(const std::string& cached, DecodedImage& image) {
		SFE::BinaryCache::Reader reader(cached);
		if (!reader) {
			return false;
		}
		CompressedHeader header{};
		reader.value(header);
		image.format = static_cast<SFE::Render::TextureFormat>(header.format);
		image.blocks.resize(SFE::Render::mipLevelsFor(header.width, header.height));
		bool whole = SFE::Render::isBlockCompressed(image.format);
		for (size_t level = 0; whole && level < image.blocks.size(); ++level) {
			reader.vector(image.blocks[level]);
			whole = reader && image.blocks[level].size() == SFE::Render::imageBytes(image.format,
				(std::max)(1u, header.width >> level), (std::max)(1u, header.height >> level));
		}
		if (!whole) {
			image.blocks.clear();
			return false;
		}
		image.width = static_cast<int>(header.width);
		image.height = static_cast<int>(header.height);
		image.swizzle = static_cast<SFE::Render::TextureSwizzle>(header.swizzle);
		image.hasAlpha = header.hasAlpha != 0;
		image.hasCutoutAlpha = header.hasCutoutAlpha != 0;
		image.fgMean = header.fgMean;
		return true;
	}

	void writeCached(const std::string& cached, const DecodedImage& image) {
		SFE::BinaryCache::Writer writer(cached);
		writer.value(CompressedHeader{ static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height),
			static_cast<uint8_t>(image.format), static_cast<uint8_t>(image.swizzle), image.hasAlpha, image.hasCutoutAlpha, image.fgMean });
		for (const auto& level : image.blocks) {
			writer.vector(level);
		}
		writer.commit();
	}

	// An image file's bytes compressed (compress): from the cache, which knows the file by its contents, or encoded
	// and kept there.
	DecodedImage readCompressed(const std::string& file, const bool flip) {
		const uint64_t key = SFE::BinaryCache::hash(file, SFE::BinaryCache::hash(std::format("texture v{} flip {}", kCompressedVersion, flip)));
		const auto cached = SFE::BinaryCache::path("texture", key);
		if (DecodedImage image; readCached(cached, image)) {
			return image;
		}
		auto image = decodePixels(file, flip);
		if (!image.data) {
			return image;
		}
		compress(image);
		writeCached(cached, image);
		return image;
	}

	DecodedImage decodeImage(const std::string& path, const bool flip, const TextureStorage storage = TextureStorage::Raw) {
		const auto file = readBytes(path);
		if (file.empty()) {
			return {};
		}
		return storage == TextureStorage::Compressed ? readCompressed(file, flip) : decodePixels(file, flip);
	}

	// Compressed images predecode read ahead, unflipped, waiting for loadTexture to take them.
	std::mutex gPredecodedMutex;
	std::unordered_map<std::string, DecodedImage> gPredecoded;

	// A texture is cached by its path alone, so a later request gets what the first one asked for. Asked for with another
	// flip or storage, the request is not what it gets: said loudly, once a path. A hold re-takes and asks nothing: it
	// passes the cached texture's own (ModelLoader's hold).
	std::mutex gMismatchMutex;
	std::unordered_set<std::string> gMismatched;

	void sayMismatch(const std::string& path, const Texture& cached, bool flip, TextureStorage storage) {
		if (cached.flip == flip && cached.storage == storage) {
			return;
		}
		{
			std::lock_guard lock(gMismatchMutex);
			if (!gMismatched.insert(path).second) {
				return;
			}
		}
		const auto name = [](TextureStorage s) { return s == TextureStorage::Compressed ? "Compressed" : "Raw"; };
		SFE::LogsModule::Logger::LOG_ERROR("TextureHandler::loadTexture %s asked for with flip %d, %s, but it is cached with flip %d, %s (the first request's), which it gets",
			path.c_str(), flip ? 1 : 0, name(storage), cached.flip ? 1 : 0, name(cached.storage));
	}

	// Runs fn(i) for i in [0, count) on threads of their own.
	void forEachOnWorkers(const size_t count, const std::function<void(size_t)>& fn) {
		std::atomic<size_t> next{ 0 };
		std::vector<std::jthread> workers((std::min)(count, static_cast<size_t>((std::max)(1u, std::thread::hardware_concurrency()))));
		for (auto& worker : workers) {
			worker = std::jthread([&] {
				for (size_t i = next++; i < count; i = next++) {
					fn(i);
				}
			});
		}
	}

	// Runs `work` where the device may be used, and waits for it: right here on the device (main) thread; from any
	// other, on the main thread between frames -- or, on OpenGL, on the loading worker's shared context. At shutdown
	// it may never run, so `work` owns whatever it needs.
	void onDevice(std::function<void()> work) {
		if (SFE::Engine::isMainThread()) {
			work();
			return;
		}
		auto* pool = SFE::ThreadPool::instance();
		const auto done = SFE::Render::activeGraphicsBackend() == SFE::Render::GraphicsBackend::OpenGL
			? pool->addTask<SFE::WorkerType::RESOURCE_LOADING>([work] { SFE::Engine::instance()->rhi()->runOnWorkerThread(work); }, "Texture upload")
			: pool->addTask<SFE::WorkerType::SYNC>(std::move(work), "Texture upload");
		if (!done.valid()) {
			return;
		}
		while (done.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) {
			if (!SFE::ThreadPool::isAlive() || pool->isShuttingDown()) {
				return;
			}
		}
	}

	// The whole chain, ours rather than the driver's. createTexture is given no pixels on purpose: with data it
	// would build the levels itself, and that is the part the two backends disagree on.
	SFE::Render::TextureHandle createWithLevels(const SFE::Render::TextureDesc& desc, const std::vector<const void*>& levels) {
		auto* rhi = SFE::Engine::instance()->rhi();
		const auto tex = rhi->createTexture(desc, nullptr);
		if (!tex) {
			return tex;
		}
		for (uint32_t mip = 0; mip < levels.size(); ++mip) {
			rhi->updateTexture(tex, 0, 0, (std::max)(1u, desc.width >> mip), (std::max)(1u, desc.height >> mip),
				desc.format, levels[mip], mip);
		}
		return tex;
	}

	// A decoded image made `texture`'s on the device: adopted where the RHI prepared it already, made here otherwise.
	// Waits for the device thread (onDevice).
	void putOnDevice(Texture& texture, const SFE::Render::TextureDesc& desc, DecodedImage&& image) {
		onDevice([&texture, desc, image = std::make_shared<DecodedImage>(std::move(image))] {
			auto* rhi = SFE::Engine::instance()->rhi();
			std::vector<std::vector<unsigned char>> chain;
			texture.texture = image->prepared ? rhi->adoptTexture(std::move(image->prepared)) : createWithLevels(desc, levelsOf(*image, chain));
			if (!texture.sampler) {
				texture.sampler = rhi->createSampler(fileSamplerDesc());
			}
		});
	}

	// Textures releaseTexture let go of, each with whether a thread is reading it back now.
	std::mutex gReleasedMutex;
	std::condition_variable gReadBack;
	std::unordered_map<Texture*, bool> gReleased;

	// A texture let go of read from its file and put on the device again, as it was read the first time, by the first
	// thread to ask for it; another asking meanwhile waits for that, the device thread doing the device's work as it
	// waits (as ModelLoader::load does).
	void readBack(Texture& texture) {
		std::unique_lock lock(gReleasedMutex);
		auto it = gReleased.find(&texture);
		if (it == gReleased.end()) {
			return;
		}
		if (it->second) {
			const bool main = SFE::Engine::isMainThread();
			while (gReleased.contains(&texture)) {
				if (!main) {
					gReadBack.wait(lock);
					continue;
				}
				gReadBack.wait_for(lock, std::chrono::milliseconds(1));
				lock.unlock();
				SFE::ThreadPool::instance()->syncUpdate();
				lock.lock();
			}
			return;
		}
		it->second = true;
		lock.unlock();
		auto image = decodeImage(texture.assetPath, texture.flip, texture.storage);
		if (image.read()) {
			const auto desc = fileTextureDesc(image);
			prepare(image, desc);
			putOnDevice(texture, desc, std::move(image));
		}
		lock.lock();
		gReleased.erase(&texture);
		gReadBack.notify_all();
	}
}

TextureHandler::Predecoded::~Predecoded() {
	std::lock_guard lock(gPredecodedMutex);
	for (const auto& path : mPaths) {
		gPredecoded.erase(path);
	}
}

TextureHandler::Predecoded TextureHandler::predecode(const std::vector<std::string>& paths) {
	Predecoded ahead;
	{
		std::lock_guard lock(gPredecodedMutex);
		for (const auto& path : paths) {
			if (!AssetsManager::instance()->getAsset<Texture>(path) && !gPredecoded.contains(path)
				&& std::find(ahead.mPaths.begin(), ahead.mPaths.end(), path) == ahead.mPaths.end()) {
				ahead.mPaths.push_back(path);
			}
		}
	}
	forEachOnWorkers(ahead.mPaths.size(), [&](const size_t i) {
		auto image = decodeImage(ahead.mPaths[i], false, TextureStorage::Compressed);
		if (!image.read()) {
			return;
		}
		prepare(image, fileTextureDesc(image));
		std::lock_guard lock(gPredecodedMutex);
		gPredecoded.emplace(ahead.mPaths[i], std::move(image));
	});
	return ahead;
}

Texture* TextureHandler::loadTextureArray(const std::string& name, const std::vector<std::string>& paths) {
	if (auto* texture = AssetsManager::instance()->getAsset<Texture>(name)) {
		return texture;
	}
	std::vector<DecodedImage> images(paths.size());
	forEachOnWorkers(paths.size(), [&](const size_t i) {
		images[i] = decodeImage(paths[i], false, TextureStorage::Compressed);
	});
	for (size_t i = 0; i < images.size(); ++i) {
		if (!images[i].read() || images[i].width != images.front().width || images[i].height != images.front().height
			|| images[i].format != images.front().format) {
			SFE::LogsModule::Logger::LOG_ERROR("TextureHandler::texture array %s: %s is missing or not the size or format of the first layer",
				name.c_str(), paths[i].c_str());
			return &TextureHandler::instance()->mDefaultTex;
		}
	}

	auto desc = fileTextureDesc(images.front());
	desc.dimension = SFE::Render::TextureDimension::Array;
	desc.depth = static_cast<uint32_t>(images.size());
	desc.arrayLayers = desc.depth;
	auto* texture = AssetsManager::instance()->createAsset<Texture>(name);
	onDevice([texture, desc, images = std::make_shared<std::vector<DecodedImage>>(std::move(images))] {
		auto* rhi = SFE::Engine::instance()->rhi();
		texture->texture = rhi->createTexture(desc, nullptr);
		for (uint32_t layer = 0; layer < desc.depth; ++layer) {
			std::vector<std::vector<unsigned char>> chain;
			const auto levels = levelsOf((*images)[layer], chain);
			for (uint32_t level = 0; level < desc.mipLevels && level < levels.size(); ++level) {
				rhi->updateTexture(texture->texture, 0, 0, (std::max)(1u, desc.width >> level), (std::max)(1u, desc.height >> level), desc.format,
					levels[level], level, layer);
			}
		}
		texture->sampler = rhi->createSampler(fileSamplerDesc());
	});
	return texture;
}

std::vector<unsigned char> TextureHandler::readPixels(const std::string& path, int& width, int& height) {
	const auto image = decodeImage(path, false);
	if (!image.data) {
		width = height = 0;
		return {};
	}
	width = image.width;
	height = image.height;
	return { image.data.get(), image.data.get() + static_cast<size_t>(width) * height * 4 };
}

size_t TextureHandler::reloadChanged(std::string& unchanged) {
	auto* rhi = SFE::Engine::instance()->rhi();
	size_t made = 0;
	bool waited = false;
	for (auto* texture : AssetsManager::instance()->changedOnDisk<Texture>()) {
		// Only one read from a single image file (loadTexture) is read again.
		if (!texture->texture || !texture->width) {
			continue;
		}
		AssetsManager::stamp(*texture);
		auto image = decodeImage(texture->assetPath, texture->flip, texture->storage);
		if (!image.read()) {
			continue;
		}
		if (image.width != texture->width || image.height != texture->height || image.format != texture->format) {
			unchanged += (unchanged.empty() ? "" : ", ") + texture->assetPath;
			continue;
		}
		if (!waited) {
			rhi->waitIdle();
			waited = true;
		}
		std::vector<std::vector<unsigned char>> chain;
		const auto levels = levelsOf(image, chain);
		for (uint32_t level = 0; level < levels.size(); ++level) {
			rhi->updateTexture(texture->texture, 0, 0, (std::max)(1, image.width >> level), (std::max)(1, image.height >> level),
				texture->format, levels[level], level);
		}
		texture->hasAlpha = image.hasAlpha;
		texture->hasCutoutAlpha = image.hasCutoutAlpha;
		texture->fgMean = image.fgMean;
		++made;
	}
	return made;
}

std::vector<std::vector<unsigned char>> TextureHandler::buildMipChain(const unsigned char* rgba, int width, int height) {
	std::vector<std::vector<unsigned char>> levels;
	if (!rgba || width <= 0 || height <= 0) {
		return levels;
	}
	// A cut-out's coverage: the share of texels at or over the alpha test's 0.5. Averaging thins it level by level --
	// a pine needle a few texels wide is under the threshold after two -- until a tree far away is bare twigs. So
	// each level's alpha is scaled up until it covers as much as the image does, as the alpha test will see it.
	const auto histogram = [](const unsigned char* pixels, const size_t bytes) {
		std::array<size_t, 256> counts{};
		for (size_t i = 3; i < bytes; i += 4) {
			++counts[pixels[i]];
		}
		return counts;
	};
	const auto keepCoverage = [&](std::vector<unsigned char>& pixels, float target) {
		const auto counts = histogram(pixels.data(), pixels.size());
		const size_t want = static_cast<size_t>(target * static_cast<float>(pixels.size() / 4));
		// The lowest alpha that, lifted to 0.5, covers no more than wanted.
		size_t covered = 0;
		int threshold = 128;
		for (int a = 255; a > 0; --a) {
			if (covered + counts[a] > want) {
				break;
			}
			covered += counts[a];
			threshold = a;
		}
		if (threshold >= 128) {
			return;
		}
		const float scale = 128.f / static_cast<float>(threshold);
		for (size_t i = 3; i < pixels.size(); i += 4) {
			pixels[i] = static_cast<unsigned char>((std::min)(255.f, static_cast<float>(pixels[i]) * scale + 0.5f));
		}
	};
	float coverage = 0.f;
	{
		const auto counts = histogram(rgba, static_cast<size_t>(width) * height * 4);
		size_t over = 0;
		for (int a = 128; a < 256; ++a) {
			over += counts[a];
		}
		const size_t texels = static_cast<size_t>(width) * height;
		// Only an image with a real cut-out: at least 1% of it out, and something in.
		if (texels - over > texels / 100 && over > 0) {
			coverage = static_cast<float>(over) / static_cast<float>(texels);
		}
	}
	const unsigned char* src = rgba;
	int sw = width;
	int sh = height;
	while (sw > 1 || sh > 1) {
		const int dw = (std::max)(1, sw / 2);
		const int dh = (std::max)(1, sh / 2);
		std::vector<unsigned char> level(static_cast<size_t>(dw) * dh * 4);
		for (int y = 0; y < dh; ++y) {
			// An odd source side leaves a row or column over; the last destination texel takes it, which is
			// what both drivers' box filters do and keeps the edge from being dropped.
			const int y0 = (std::min)(y * 2, sh - 1);
			const int y1 = (std::min)(y0 + 1, sh - 1);
			for (int x = 0; x < dw; ++x) {
				const int x0 = (std::min)(x * 2, sw - 1);
				const int x1 = (std::min)(x0 + 1, sw - 1);
				const unsigned char* a = src + (static_cast<size_t>(y0) * sw + x0) * 4;
				const unsigned char* b = src + (static_cast<size_t>(y0) * sw + x1) * 4;
				const unsigned char* c = src + (static_cast<size_t>(y1) * sw + x0) * 4;
				const unsigned char* d = src + (static_cast<size_t>(y1) * sw + x1) * 4;
				unsigned char* out = level.data() + (static_cast<size_t>(y) * dw + x) * 4;
				for (int ch = 0; ch < 4; ++ch) {
					// +2 rounds to nearest, so a flat area keeps its exact value down the whole chain.
					out[ch] = static_cast<unsigned char>((a[ch] + b[ch] + c[ch] + d[ch] + 2) / 4);
				}
			}
		}
		if (coverage > 0.f) {
			keepCoverage(level, coverage);
		}
		levels.push_back(std::move(level));
		src = levels.back().data();
		sw = dw;
		sh = dh;
	}
	return levels;
}

Texture* TextureHandler::loadTexture(const std::string& path, bool flip, TextureStorage storage) {
	if (auto* texture = AssetsManager::instance()->getAsset<Texture>(path)) {
		sayMismatch(path, *texture, flip, storage);
		readBack(*texture);
		return texture;
	}

	DecodedImage image;
	if (!flip && storage == TextureStorage::Compressed) {
		std::lock_guard lock(gPredecodedMutex);
		if (const auto it = gPredecoded.find(path); it != gPredecoded.end()) {
			image = std::move(it->second);
			gPredecoded.erase(it);
		}
	}
	if (!image.read()) {
		image = decodeImage(path, flip, storage);
	}
	if (!image.read()) {
		SFE::LogsModule::Logger::LOG_ERROR("TextureHandler::can't load texture %s", path.c_str());
		return &TextureHandler::instance()->mDefaultTex;
	}
	const auto desc = fileTextureDesc(image);
	prepare(image, desc);

	auto* texture = AssetsManager::instance()->createAsset<Texture>(path);
	texture->hasAlpha = image.hasAlpha;
	texture->hasCutoutAlpha = image.hasCutoutAlpha;
	texture->fgMean = image.fgMean;
	texture->width = image.width;
	texture->height = image.height;
	texture->flip = flip;
	texture->storage = storage;
	texture->format = image.format;
	putOnDevice(*texture, desc, std::move(image));
	return texture;
}

void AssetsModule::releaseTexture(Texture& texture) {
	// Only one read from a single image file: loadTexture reads it again.
	if (!texture.texture || !texture.width) {
		return;
	}
	std::lock_guard lock(gReleasedMutex);
	if (gReleased.contains(&texture)) {
		return;
	}
	SFE::Engine::instance()->rhi()->destroyTexture(texture.texture);
	texture.texture = {};
	gReleased.emplace(&texture, false);
}

Texture* AssetsModule::distantTexture(const std::string& path, const int most) {
	const auto name = std::format("{}@{}", path, most);
	if (auto* texture = AssetsManager::instance()->getAsset<Texture>(name)) {
		return texture;
	}
	// Kept in the cache by the file's size and time, as ModelCache knows a model's files, rather than by its contents:
	// knowing those would read the whole image, the very thing a copy of its small levels saves.
	const auto file = SFE::FileSystem::resolve(path);
	std::error_code error;
	const auto stamp = std::format("distant texture v{} {} {} {} {}", kCompressedVersion, file.string(), std::filesystem::file_size(file, error),
		std::filesystem::last_write_time(file, error).time_since_epoch().count(), most);
	const auto cached = SFE::BinaryCache::path("texture", SFE::BinaryCache::hash(stamp));
	DecodedImage image;
	if (!readCached(cached, image)) {
		image = decodeImage(path, false, TextureStorage::Compressed);
		if (image.blocks.empty()) {
			SFE::LogsModule::Logger::LOG_ERROR("distantTexture: can't load texture %s", path.c_str());
			return &TextureHandler::instance()->mDefaultTex;
		}
		// The levels wider or higher than `most` left out: the next one down is the first.
		size_t skip = 0;
		while (skip + 1 < image.blocks.size() && (std::max)(image.width >> skip, image.height >> skip) > most) {
			++skip;
		}
		image.blocks.erase(image.blocks.begin(), image.blocks.begin() + static_cast<std::ptrdiff_t>(skip));
		image.width = (std::max)(1, image.width >> skip);
		image.height = (std::max)(1, image.height >> skip);
		writeCached(cached, image);
	}
	const auto desc = fileTextureDesc(image);
	prepare(image, desc);
	auto* texture = AssetsManager::instance()->createAsset<Texture>(name);
	texture->hasAlpha = image.hasAlpha;
	texture->hasCutoutAlpha = image.hasCutoutAlpha;
	texture->fgMean = image.fgMean;
	texture->format = image.format;
	// No width: not one read from a file as it is, so neither releaseTexture nor reloadChanged takes it.
	putOnDevice(*texture, desc, std::move(image));
	return texture;
}
