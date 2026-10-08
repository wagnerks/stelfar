#include "rhi/GraphicsBackend.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace SFE::Render {
	namespace {
		// Vulkan unless asked otherwise: it draws the same picture as OpenGL for about a quarter less GPU time and half
		// the main thread's.
		GraphicsBackend gBackend = GraphicsBackend::Vulkan;

		bool equalsIgnoreCase(std::string_view a, std::string_view b) {
			if (a.size() != b.size()) {
				return false;
			}
			const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
			for (size_t i = 0; i < a.size(); ++i) {
				if (lower(a[i]) != lower(b[i])) {
					return false;
				}
			}
			return true;
		}

		// An unknown name used to mean OpenGL, so a typo -- or a backend this build does not know -- ran the
		// wrong one without a word.
		std::optional<GraphicsBackend> parseName(std::string_view name) {
			if (equalsIgnoreCase(name, "opengl") || equalsIgnoreCase(name, "gl")) {
				return GraphicsBackend::OpenGL;
			}
			if (equalsIgnoreCase(name, "vulkan") || equalsIgnoreCase(name, "vk")) {
				return GraphicsBackend::Vulkan;
			}
			if (equalsIgnoreCase(name, "none") || equalsIgnoreCase(name, "null")) {
				return GraphicsBackend::None;
			}
			std::fprintf(stderr, "unknown graphics backend '%.*s' (expected opengl, vulkan or none)\n",
				static_cast<int>(name.size()), name.data());
			return std::nullopt;
		}
	}

	bool configureGraphicsBackend(int argc, char** argv) {
		bool ok = true;
		const auto apply = [&](std::string_view name) {
			if (const auto parsed = parseName(name)) {
				gBackend = *parsed;
			}
			else {
				ok = false;
			}
		};

		if (const char* env = std::getenv("STELFORGE_BACKEND")) {
			apply(env);
		}

		for (int i = 1; i < argc; ++i) {
			if (!argv[i]) {
				continue;
			}
			const std::string_view arg{ argv[i] };
			if (arg == "--backend" && i + 1 < argc && argv[i + 1]) {
				apply(argv[i + 1]);
				++i;
			}
			else if (arg.starts_with("--backend=")) {
				apply(arg.substr(std::string_view("--backend=").size()));
			}
		}
		return ok;
	}

	GraphicsBackend activeGraphicsBackend() {
		return gBackend;
	}

	const char* graphicsBackendName(const GraphicsBackend backend) {
		switch (backend) {
		case GraphicsBackend::OpenGL: return "OpenGL";
		case GraphicsBackend::Vulkan: return "Vulkan";
		case GraphicsBackend::None: return "None";
		}
		return "unknown";
	}
}
