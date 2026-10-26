#include "BinaryCache.h"

#include <cstdio>
#include <filesystem>
#include <thread>

#include "core/FileSystem.h"

namespace SFE::BinaryCache {
	uint64_t hash(const std::string_view text, uint64_t seed) {
		for (const char c : text) {
			seed = (seed ^ static_cast<uint8_t>(c)) * 1099511628211ull;
		}
		return seed;
	}

	std::string path(const std::string_view name, const uint64_t key) {
		char hex[17];
		std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(key));
		// The folder the file resolves to, not "cache" on its own: with a game's content in front of the engine's, that
		// resolved to the engine's folder while a new file went to the game's, which was never made -- and nothing was
		// cached at all.
		const auto file = FileSystem::resolve("cache/" + std::string(name) + "_" + hex + ".bin");
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		return file.string();
	}

	// The temporary file is this thread's own: two threads building the same file at once would otherwise write into one.
	Writer::Writer(std::string path) : mPath(std::move(path)),
		mTemp(mPath + "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".tmp"),
		mOut(mTemp, std::ios::binary | std::ios::trunc) {}

	Writer::~Writer() {
		if (!mCommitted) {
			mOut.close();
			std::error_code ec;
			std::filesystem::remove(mTemp, ec);
		}
	}

	bool Writer::commit() {
		mOut.close();
		if (!mOut) {
			return false;
		}
		std::error_code ec;
		std::filesystem::rename(mTemp, mPath, ec);
		mCommitted = !ec;
		return mCommitted;
	}

	Reader::Reader(const std::string& path) : mIn(path, std::ios::binary) {
		std::error_code ec;
		const auto size = std::filesystem::file_size(path, ec);
		mOk = mIn.is_open() && !ec;
		mRemaining = mOk ? size : 0;
	}

	void Reader::read(void* data, const size_t bytes) {
		if (!mOk || bytes > mRemaining) {
			mOk = false;
			return;
		}
		mIn.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes));
		mOk = static_cast<bool>(mIn);
		mRemaining -= bytes;
	}
}
