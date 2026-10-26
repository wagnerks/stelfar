#include "FileSystem.h"

#include <string>
#include <json/reader.h>
#include <json/writer.h>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "logsModule/logger.h"

namespace SFE {
	namespace {
		// Where a write goes: under the application's root, whichever folder resolve() would read the path from. A read
		// finds the engine's own file behind the application's, but nothing a run makes goes among the engine's assets:
		// they stay as the engine shipped them, and an application that saves a file the engine provides puts its own
		// copy in front of it.
		std::filesystem::path underRoot(std::string_view path) {
			const auto p = std::filesystem::path(std::string(path));
			if (p.empty() || p.is_absolute()) {
				return p;
			}
			return (FileSystem::root() / p).lexically_normal();
		}

		// A file about to be written, its folder made first: so far that folder may be only the engine's, which a read
		// finds and a write does not go into.
		std::filesystem::path fileToWrite(std::string_view path) {
			auto file = underRoot(path);
			if (file.has_parent_path()) {
				std::error_code ec;
				std::filesystem::create_directories(file.parent_path(), ec);
			}
			return file;
		}
	}

	void FileSystem::setRoot(std::string_view path) {
		if (path.empty()) {
			sRoot.clear();
			return;
		}
		std::error_code ec;
		auto p = std::filesystem::path(std::string(path));
		if (!p.is_absolute()) {
			p = std::filesystem::absolute(p, ec);
		}
		sRoot = p.lexically_normal();
	}

	const std::filesystem::path& FileSystem::root() {
		return sRoot;
	}

	void FileSystem::setEngineRoot(std::string_view path) {
		if (path.empty()) {
			sEngineRoot.clear();
			return;
		}
		std::error_code ec;
		auto p = std::filesystem::path(std::string(path));
		if (!p.is_absolute()) {
			p = std::filesystem::absolute(p, ec);
		}
		sEngineRoot = p.lexically_normal();
	}

	const std::filesystem::path& FileSystem::engineRoot() {
		return sEngineRoot;
	}

	std::filesystem::path FileSystem::resolve(std::string_view path) {
		const auto p = std::filesystem::path(std::string(path));
		if (p.empty() || p.is_absolute() || sRoot.empty()) {
			return p;
		}
		auto inContent = (sRoot / p).lexically_normal();
		// The game's content first, the engine's own assets behind it: a game ships its scene and models,
		// the engine its shaders and fonts, and either may stand in for the other by name.
		if (sEngineRoot.empty() || sEngineRoot == sRoot) {
			return inContent;
		}
		std::error_code ec;
		if (std::filesystem::exists(inContent, ec)) {
			return inContent;
		}
		auto inEngine = (sEngineRoot / p).lexically_normal();
		return std::filesystem::exists(inEngine, ec) ? inEngine : inContent;
	}

	std::filesystem::path FileSystem::executableDir() {
#ifdef _WIN32
		char buf[MAX_PATH]{};
		GetModuleFileNameA(nullptr, buf, MAX_PATH);
		return std::filesystem::path(buf).parent_path();
#else
		return std::filesystem::current_path();
#endif
	}

	std::filesystem::path FileSystem::guessContentRoot(std::string_view exeDir) {
		namespace fs = std::filesystem;
		const fs::path dir = exeDir.empty() ? executableDir() : fs::path(std::string(exeDir));
		auto hasAssets = [](const fs::path& p) {
			std::error_code ec;
			return fs::exists(p / "shaders", ec) || fs::exists(p / "models", ec);
		};
		auto probe = [&](const fs::path& p) -> fs::path {
			if (hasAssets(p)) {
				return p;
			}
			// A game keeps its own in "content" and the engine keeps its in "bin"; a game's comes first, since a
			// game's executable may have a "bin" of its own beside it holding what a run left behind.
			if (hasAssets(p / "content")) {
				return p / "content";
			}
			if (hasAssets(p / "bin")) {
				return p / "bin";
			}
			if (hasAssets(p / "engine" / "bin")) {
				return p / "engine" / "bin";
			}
			return {};
		};
		if (const auto found = probe(dir); !found.empty()) {
			return found;
		}
		auto p = dir;
		for (int i = 0; i < 6; ++i) {
			const auto parent = p.parent_path();
			if (parent == p) {
				break;
			}
			p = parent;
			if (const auto found = probe(p); !found.empty()) {
				return found;
			}
		}
		return dir / "bin";
	}

	bool FileSystem::isFileExists(std::string_view path) {
		return std::filesystem::exists(resolve(path));
	}

	bool FileSystem::createDirectories(std::string_view path) {
		std::error_code ec;
		std::filesystem::create_directories(underRoot(path), ec);
		return !ec;
	}

	bool FileSystem::readFile(std::string_view path, std::string& file) {
		const auto resolved = resolve(path).string();
		std::ifstream inputFile(resolved);
		if (!inputFile.is_open()) {
			LogsModule::Logger::LOG_ERROR("FileSystem::FILE_NOT_SUCCESSFULLY_READ: %s", resolved.c_str());
			return false;
		}

		std::string line;
		while (std::getline(inputFile, line)) {
			file += line + "\n";
		}

		return true;
	}

	bool FileSystem::writeFile(std::string_view path, std::string& file) {
		const auto written = fileToWrite(path).string();
		std::ofstream outputFile(written);
		if (!outputFile.is_open()) {
			LogsModule::Logger::LOG_ERROR("FileSystem::FILE_NOT_SUCCESSFULLY_WRITE: %s", written.c_str());
			return false;
		}

		outputFile.write(file.c_str(), static_cast<std::streamsize>(file.size()));
		return true;
	}

	bool FileSystem::readJson(std::string_view path, Json::Value& root, bool withComments) {
		const auto resolved = resolve(path).string();
		std::ifstream ifs(resolved);
		if (!ifs.is_open()) {
			LogsModule::Logger::LOG_ERROR("FileSystem::readJson can not open file: %s", resolved.c_str());
			return false;
		}

		static Json::CharReaderBuilder builder;
		builder["collectComments"] = withComments;

		static JSONCPP_STRING errs;
		const bool success = parseFromStream(builder, ifs, &root, &errs);

		if (!success) {
			LogsModule::Logger::LOG_ERROR("FileSystem::readJson %s", errs.c_str());
			return false;
		}

		return true;
	}

	bool FileSystem::writeJson(std::string_view path, const Json::Value& root) {
		const auto written = fileToWrite(path).string();
		std::ofstream ofs(written);
		if (!ofs.is_open()) {
			LogsModule::Logger::LOG_ERROR("FileSystem::writeJson can not open file: %s", written.c_str());
			return false;
		}

		static Json::StreamWriterBuilder builder;
		static const std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());

		writer->write(root, &ofs);
		return true;
	}

	Json::Value FileSystem::readJson(std::string_view path, bool withComments) {
		Json::Value json;
		readJson(path, json, withComments);

		return json;
	}
}
