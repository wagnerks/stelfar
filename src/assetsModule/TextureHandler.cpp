#include "TextureHandler.h"

#include <stb_image.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "AssetsManager.h"
#include "assetsModule/modelModule/ModelRelease.h"
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
		int width = 0;
		int height = 0;
		bool hasAlpha = false;
		bool hasCutoutAlpha = false;
		float fgMean = 0.f;
		// Where the RHI prepares textures on any thread, the texture itself, and no pixels any more.
		std::unique_ptr<SFE::Render::PreparedTexture> prepared;

		bool read() const { return data || prepared; }
	};

	// A texture loaded from a file: RGBA8 unless asked otherwise, with a full mip chain. Without the levels a
	// distant character samples one texel of a full size texture per pixel: the armour sparkles, and TAA averages
	// the sparkle into grey.
	SFE::Render::TextureDesc fileTextureDesc(const int width, const int height, const SFE::Render::TextureFormat format) {
		SFE::Render::TextureDesc desc;
		desc.width = static_cast<uint32_t>(width);
		desc.height = static_cast<uint32_t>(height);
		desc.format = format;
		desc.dimension = SFE::Render::TextureDimension::Tex2D;
		desc.mipLevels = SFE::Render::mipLevelsFor(desc.width, desc.height);
		return desc;
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
		if (image.prepared || !image.data || !rhi->preparesTextures()) {
			return;
		}
		const auto chain = AssetsModule::TextureHandler::buildMipChain(image.data.get(), image.width, image.height);
		std::vector<const void*> levels{ image.data.get() };
		for (const auto& level : chain) {
			levels.push_back(level.data());
		}
		if ((image.prepared = rhi->prepareTexture(desc, levels))) {
			image.data.reset();
		}
	}

	DecodedImage decodeImage(const std::string& path, const bool flip) {
		DecodedImage image;
		int channels = 0;
		// stb's flag for this thread only: loads run on several at once.
		stbi_set_flip_vertically_on_load_thread(flip);
		image.data.reset(stbi_load(SFE::FileSystem::resolve(path).string().c_str(), &image.width, &image.height, &channels, 4));
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

	// Unflipped RGBA8 images predecode read ahead, waiting for loadTexture to take them.
	std::mutex gPredecodedMutex;
	std::unordered_map<std::string, DecodedImage> gPredecoded;

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
	SFE::Render::TextureHandle createWithMips(const SFE::Render::TextureDesc& desc, const unsigned char* data) {
		auto* rhi = SFE::Engine::instance()->rhi();
		const auto tex = rhi->createTexture(desc, nullptr);
		if (!tex || !data) {
			return tex;
		}
		rhi->updateTexture(tex, 0, 0, desc.width, desc.height, desc.format, data, 0);
		const auto levels = AssetsModule::TextureHandler::buildMipChain(data, static_cast<int>(desc.width), static_cast<int>(desc.height));
		for (size_t i = 0; i < levels.size(); ++i) {
			const uint32_t mip = static_cast<uint32_t>(i) + 1;
			rhi->updateTexture(tex, 0, 0, (std::max)(1u, desc.width >> mip), (std::max)(1u, desc.height >> mip),
				desc.format, levels[i].data(), mip);
		}
		return tex;
	}

	// A decoded image made `texture`'s on the device: adopted where the RHI prepared it already, made here otherwise.
	// Waits for the device thread (onDevice).
	void putOnDevice(Texture& texture, const SFE::Render::TextureDesc& desc, DecodedImage&& image) {
		onDevice([&texture, desc, image = std::make_shared<DecodedImage>(std::move(image))] {
			auto* rhi = SFE::Engine::instance()->rhi();
			texture.texture = image->prepared ? rhi->adoptTexture(std::move(image->prepared)) : createWithMips(desc, image->data.get());
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
		auto image = decodeImage(texture.assetPath, texture.flip);
		if (image.read()) {
			const auto desc = fileTextureDesc(image.width, image.height, texture.format);
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
		auto image = decodeImage(ahead.mPaths[i], false);
		if (!image.data) {
			return;
		}
		prepare(image, fileTextureDesc(image.width, image.height, SFE::Render::TextureFormat::RGBA8));
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
	std::vector<std::vector<std::vector<unsigned char>>> chains(paths.size());
	forEachOnWorkers(paths.size(), [&](const size_t i) {
		images[i] = decodeImage(paths[i], false);
		chains[i] = buildMipChain(images[i].data.get(), images[i].width, images[i].height);
	});
	for (size_t i = 0; i < images.size(); ++i) {
		if (!images[i].data || images[i].width != images.front().width || images[i].height != images.front().height) {
			SFE::LogsModule::Logger::LOG_ERROR("TextureHandler::texture array %s: %s is missing or not the size of the first layer", name.c_str(), paths[i].c_str());
			return &TextureHandler::instance()->mDefaultTex;
		}
	}

	auto desc = fileTextureDesc(images.front().width, images.front().height, SFE::Render::TextureFormat::RGBA8);
	desc.dimension = SFE::Render::TextureDimension::Array;
	desc.depth = static_cast<uint32_t>(images.size());
	desc.arrayLayers = desc.depth;
	auto* texture = AssetsManager::instance()->createAsset<Texture>(name);
	onDevice([texture, desc, images = std::make_shared<std::vector<DecodedImage>>(std::move(images)),
		chains = std::make_shared<std::vector<std::vector<std::vector<unsigned char>>>>(std::move(chains))] {
		auto* rhi = SFE::Engine::instance()->rhi();
		texture->texture = rhi->createTexture(desc, nullptr);
		for (uint32_t layer = 0; layer < desc.depth; ++layer) {
			rhi->updateTexture(texture->texture, 0, 0, desc.width, desc.height, desc.format, (*images)[layer].data.get(), 0, layer);
			for (uint32_t level = 1; level < desc.mipLevels && level <= (*chains)[layer].size(); ++level) {
				rhi->updateTexture(texture->texture, 0, 0, (std::max)(1u, desc.width >> level), (std::max)(1u, desc.height >> level), desc.format,
					(*chains)[layer][level - 1].data(), level, layer);
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
		auto image = decodeImage(texture->assetPath, texture->flip);
		if (!image.data) {
			continue;
		}
		if (image.width != texture->width || image.height != texture->height || texture->format != SFE::Render::TextureFormat::RGBA8) {
			unchanged += (unchanged.empty() ? "" : ", ") + texture->assetPath;
			continue;
		}
		if (!waited) {
			rhi->waitIdle();
			waited = true;
		}
		const auto chain = buildMipChain(image.data.get(), image.width, image.height);
		const auto levels = SFE::Render::mipLevelsFor(static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height));
		rhi->updateTexture(texture->texture, 0, 0, image.width, image.height, texture->format, image.data.get(), 0);
		for (uint32_t level = 1; level < levels && level <= chain.size(); ++level) {
			rhi->updateTexture(texture->texture, 0, 0, (std::max)(1, image.width >> level), (std::max)(1, image.height >> level),
				texture->format, chain[level - 1].data(), level);
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

Texture* TextureHandler::loadTexture(const std::string& path, bool flip, SFE::Render::TextureFormat format) {
	if (auto* texture = AssetsManager::instance()->getAsset<Texture>(path)) {
		readBack(*texture);
		return texture;
	}

	DecodedImage image;
	if (!flip && format == SFE::Render::TextureFormat::RGBA8) {
		std::lock_guard lock(gPredecodedMutex);
		if (const auto it = gPredecoded.find(path); it != gPredecoded.end()) {
			image = std::move(it->second);
			gPredecoded.erase(it);
		}
	}
	if (!image.read()) {
		image = decodeImage(path, flip);
	}
	if (!image.read()) {
		SFE::LogsModule::Logger::LOG_ERROR("TextureHandler::can't load texture %s", path.c_str());
		return &TextureHandler::instance()->mDefaultTex;
	}
	const auto desc = fileTextureDesc(image.width, image.height, format);
	prepare(image, desc);

	auto* texture = AssetsManager::instance()->createAsset<Texture>(path);
	texture->hasAlpha = image.hasAlpha;
	texture->hasCutoutAlpha = image.hasCutoutAlpha;
	texture->fgMean = image.fgMean;
	texture->width = image.width;
	texture->height = image.height;
	texture->flip = flip;
	texture->format = format;
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
