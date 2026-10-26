#include "ShaderBase.h"

#include "core/FileSystem.h"

#include <filesystem>
#include <regex>
#include <string_view>
#include <unordered_set>

using namespace SFE::ShaderModule;

namespace {
	// The file an #include names. A relative one is looked up as every other asset is -- under shaders/ in the
	// application's content first, then among the engine's own -- and beside the including file only when neither has
	// it. So an application stands in for an engine include by putting a file of that name in its own shaders, and every
	// includer of one name gets the one file, an engine shader as much as the application's.
	std::filesystem::path includedFile(const std::filesystem::path& inc, const std::filesystem::path& includerDir) {
		if (inc.is_absolute()) {
			return inc;
		}
		std::error_code ec;
		auto found = SFE::FileSystem::resolve((std::filesystem::path("shaders") / inc).generic_string());
		if (std::filesystem::exists(found, ec)) {
			return found;
		}
		auto beside = includerDir / inc;
		return std::filesystem::exists(beside, ec) ? beside : found;
	}

	std::string loadResolved(const std::filesystem::path& path, std::unordered_set<std::string>& seen) {
		const auto key = path.lexically_normal().generic_string();
		if (!seen.insert(key).second) {
			return {};
		}

		std::string code;
		SFE::FileSystem::readFile(path.generic_string(), code);
		if (code.empty()) {
			return {};
		}

		static const std::regex includeRe(R"-(^\s*#include\s+"([^"]+)")-");
		std::string out;
		out.reserve(code.size() + 256);
		const auto dir = path.parent_path();
		std::string_view rest = code;
		while (!rest.empty()) {
			const auto nl = rest.find('\n');
			const auto line = std::string(rest.substr(0, nl == std::string_view::npos ? rest.size() : nl + 1));
			rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);

			std::smatch m;
			if (std::regex_search(line, m, includeRe)) {
				out += loadResolved(includedFile(m[1].str(), dir), seen);
			}
			else {
				out += line;
			}
		}
		return out;
	}
}

std::string ShaderBase::loadShaderCode(std::string_view path) {
	std::unordered_set<std::string> seen;
	return loadResolved(SFE::FileSystem::resolve(path), seen);
}
