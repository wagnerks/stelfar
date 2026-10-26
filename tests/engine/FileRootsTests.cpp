#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "EngineTestFixture.h"
#include "assetsModule/shaderModule/ShaderBase.h"
#include "core/BinaryCache.h"
#include "core/FileSystem.h"

namespace SFE::EngineTests {
	namespace {
		// An application's root and an engine's behind it, each a folder of its own outside every real one, set as
		// FileSystem's roots for one test and put back as they were when it ends. Nothing else reads the roots meanwhile:
		// no frame runs inside these tests, and the workers are idle before the roots change and before they go back.
		class TempRoots {
		public:
			TempRoots() : mWasRoot(FileSystem::root()), mWasEngine(FileSystem::engineRoot()),
				mDir(std::filesystem::temp_directory_path()
					/ ("sfe_file_roots_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
				ThreadPool::instance()->waitIdle();
				std::filesystem::create_directories(content());
				std::filesystem::create_directories(engine());
				FileSystem::setRoot(content().string());
				FileSystem::setEngineRoot(engine().string());
			}

			~TempRoots() {
				ThreadPool::instance()->waitIdle();
				FileSystem::setRoot(mWasRoot.string());
				FileSystem::setEngineRoot(mWasEngine.string());
				std::error_code ec;
				std::filesystem::remove_all(mDir, ec);
			}

			TempRoots(const TempRoots&) = delete;
			TempRoots& operator=(const TempRoots&) = delete;

			std::filesystem::path content() const { return mDir / "content"; }
			std::filesystem::path engine() const { return mDir / "engine"; }

		private:
			std::filesystem::path mWasRoot;
			std::filesystem::path mWasEngine;
			std::filesystem::path mDir;
		};

		// A file holding just this text, its folder made.
		void put(const std::filesystem::path& file, const std::string& text) {
			std::filesystem::create_directories(file.parent_path());
			std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
		}

		std::string textOf(const std::filesystem::path& file) {
			std::ifstream in(file); // as text, as FileSystem::writeFile writes it
			return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}

		// Two spellings of one path compare equal.
		std::string normal(const std::filesystem::path& path) {
			return path.lexically_normal().generic_string();
		}

		std::string expanded(std::string_view path) {
			return ShaderModule::ShaderBase::loadShaderCode(path);
		}
	}

	// A content file stands in for an engine include of the same name, even for an engine shader with the engine's copy
	// beside it; and the RHI's host, which both backends compile from, reads the shader so too.
	TEST(ShaderInclude, AContentFileStandsInForAnEngineInclude) {
		const TempRoots roots;
		put(roots.engine() / "shaders/lit.fs", "#include \"sfe_hook.glsl\"\nlit\n");
		put(roots.engine() / "shaders/sfe_hook.glsl", "engine hook\n");
		put(roots.content() / "shaders/sfe_hook.glsl", "game hook\n");
		EXPECT_EQ(expanded("shaders/lit.fs"), "game hook\nlit\n");
		const auto* host = Engine::instance()->rhi()->host();
		ASSERT_TRUE(host);
		EXPECT_EQ(host->loadShaderSource("shaders/lit.fs"), "game hook\nlit\n");
	}

	// With no content file of its name, an engine include is the engine's, for an engine shader and a content one alike.
	TEST(ShaderInclude, AnEngineIncludeServesWhereNoContentFileStandsIn) {
		const TempRoots roots;
		put(roots.engine() / "shaders/sfe_hook.glsl", "engine hook\n");
		put(roots.engine() / "shaders/lit.fs", "#include \"sfe_hook.glsl\"\nlit\n");
		put(roots.content() / "shaders/own.fs", "#include \"sfe_hook.glsl\"\nown\n");
		EXPECT_EQ(expanded("shaders/lit.fs"), "engine hook\nlit\n");
		EXPECT_EQ(expanded("shaders/own.fs"), "engine hook\nown\n");
	}

	// Every includer of a name gets the one file, wherever the includer lives, and gets it once: a content shader and
	// the engine library it includes both name the hook, and only the game's copy comes in.
	TEST(ShaderInclude, OneNameIsOneFileForEveryIncluder) {
		const TempRoots roots;
		put(roots.engine() / "shaders/sfe_lib.glsl", "#include \"sfe_hook.glsl\"\nlib\n");
		put(roots.engine() / "shaders/sfe_hook.glsl", "engine hook\n");
		put(roots.content() / "shaders/sfe_hook.glsl", "game hook\n");
		put(roots.content() / "shaders/own.fs", "#include \"sfe_hook.glsl\"\n#include \"sfe_lib.glsl\"\nown\n");
		EXPECT_EQ(expanded("shaders/own.fs"), "game hook\nlib\nown\n");
	}

	// The includer's own folder is the last place looked: what is only beside a shader outside shaders/ is found there,
	// and a file of that name under shaders/, in either root, comes first.
	TEST(ShaderInclude, TheIncludersOwnFolderIsTheLastResort) {
		const TempRoots roots;
		put(roots.content() / "looks/odd.fs", "#include \"odd_part.glsl\"\nodd\n");
		put(roots.content() / "looks/odd_part.glsl", "beside\n");
		EXPECT_EQ(expanded("looks/odd.fs"), "beside\nodd\n");
		put(roots.engine() / "shaders/odd_part.glsl", "shared\n");
		EXPECT_EQ(expanded("looks/odd.fs"), "shared\nodd\n");
	}

	// A write lands under the application's root even where only the engine has the path, its folder made; the engine's
	// file stays as it was, and the next read finds the application's copy in front of it.
	TEST(FileSystem, AWriteLandsUnderTheRootWhereOnlyTheEngineHasThePath) {
		const TempRoots roots;
		put(roots.engine() / "notes/said.txt", "engine\n");
		ASSERT_EQ(normal(FileSystem::resolve("notes/said.txt")), normal(roots.engine() / "notes/said.txt"))
			<< "a read does not find the engine's file behind the root";
		std::string text = "game\n";
		ASSERT_TRUE(FileSystem::writeFile("notes/said.txt", text));
		EXPECT_EQ(textOf(roots.content() / "notes/said.txt"), "game\n");
		EXPECT_EQ(textOf(roots.engine() / "notes/said.txt"), "engine\n") << "the engine's file was written over";
		std::string read;
		ASSERT_TRUE(FileSystem::readFile("notes/said.txt", read));
		EXPECT_EQ(read, "game\n");
	}

	// A JSON file as any other: under the root, the engine's left as it was.
	TEST(FileSystem, JsonIsWrittenUnderTheRoot) {
		const TempRoots roots;
		put(roots.engine() / "notes/said.json", "{\"by\": \"engine\"}\n");
		Json::Value said;
		said["by"] = "game";
		ASSERT_TRUE(FileSystem::writeJson("notes/said.json", said));
		EXPECT_EQ(FileSystem::readJson((roots.content() / "notes/said.json").string())["by"].asString(), "game");
		EXPECT_EQ(FileSystem::readJson((roots.engine() / "notes/said.json").string())["by"].asString(), "engine");
	}

	// A folder is made under the root, not found among the engine's and taken as made.
	TEST(FileSystem, AFolderIsMadeUnderTheRoot) {
		const TempRoots roots;
		std::filesystem::create_directories(roots.engine() / "made/here");
		ASSERT_TRUE(FileSystem::createDirectories("made/here"));
		EXPECT_TRUE(std::filesystem::is_directory(roots.content() / "made/here"));
	}

	// Reads still look behind the root: a file only the engine has is read from the engine's folder, and nothing is made
	// under the root for it.
	TEST(FileSystem, AReadFallsBackToTheEngine) {
		const TempRoots roots;
		put(roots.engine() / "only.txt", "engine\n");
		std::string read;
		ASSERT_TRUE(FileSystem::readFile("only.txt", read));
		EXPECT_EQ(read, "engine\n");
		EXPECT_FALSE(std::filesystem::exists(roots.content() / "only.txt"));
	}

	// A cache is the application's own: its file is named under the root, written and read there, though the engine's
	// folder holds one of that name, which is left as it was.
	TEST(FileSystem, TheCacheIsUnderTheRoot) {
		const TempRoots roots;
		put(roots.engine() / "cache/test_0000000000001234.bin", "engine");
		const auto file = BinaryCache::path("test", 0x1234);
		ASSERT_EQ(normal(file), normal(roots.content() / "cache/test_0000000000001234.bin"));
		{
			BinaryCache::Writer writer(file);
			writer.value(uint32_t{ 7 });
			ASSERT_TRUE(writer.commit());
		}
		uint32_t read = 0;
		{
			BinaryCache::Reader reader(file);
			reader.value(read);
			EXPECT_TRUE(static_cast<bool>(reader));
		}
		EXPECT_EQ(read, 7u);
		EXPECT_EQ(textOf(roots.engine() / "cache/test_0000000000001234.bin"), "engine");
	}

	// Compiled SPIR-V is kept with the application's other caches, under its root, not among the engine's shaders.
	TEST(FileSystem, CompiledShadersAreCachedUnderTheRoot) {
		const TempRoots roots;
		const auto* host = Engine::instance()->rhi()->host();
		ASSERT_TRUE(host);
		EXPECT_EQ(normal(host->shaderCacheDir()), normal(roots.content() / "cache/spirv"));
	}
}
