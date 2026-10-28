#include <cstddef>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "EngineTestFixture.h"
#include "core/FileSystem.h"
#include "systemsModule/systems/RenderSystem.h"

// The camera block the renderer writes every frame (RenderMatrices) and the one every shader reads it as
// (SFE_SHARED_MATRICES in sfe.glsl) are one block: member for member, at the same offsets, of the same size. A slip
// between them shows on screen only, in every lit view.
namespace SFE::EngineTests {
	namespace {
		struct Member {
			std::string name;
			size_t offset = 0;
		};

		// The members of `#define SFE_SHARED_MATRICES` as std140 lays them out: a mat4 64 bytes and a vec4 16, both on
		// 16. Its lines are joined (each but the last ends in a backslash) and their comments taken out first.
		std::vector<Member> sharedMatrices(const std::string& glsl) {
			std::istringstream lines(glsl);
			std::string line;
			std::string body;
			bool inside = false;
			while (std::getline(lines, line)) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (!inside) {
					inside = line.rfind("#define SFE_SHARED_MATRICES", 0) == 0;
					if (!inside) {
						continue;
					}
					line = line.substr(std::string("#define SFE_SHARED_MATRICES").size());
				}
				const bool more = !line.empty() && line.back() == '\\';
				body += (more ? line.substr(0, line.size() - 1) : line) + " ";
				if (!more) {
					break;
				}
			}
			body = std::regex_replace(body, std::regex(R"(/\*[\s\S]*?\*/)"), " ");
			std::vector<Member> members;
			size_t offset = 0;
			const std::regex member(R"((mat4|vec4)\s+(\w+)\s*;)");
			for (auto it = std::sregex_iterator(body.begin(), body.end(), member); it != std::sregex_iterator(); ++it) {
				const size_t size = (*it)[1] == "mat4" ? 64 : 16;
				offset = (offset + 15) / 16 * 16;
				members.push_back({ (*it)[2].str(), offset });
				offset += size;
			}
			members.push_back({ "(end)", offset });
			return members;
		}
	}

	TEST(SharedMatrices, MirrorTheShader) {
		using SystemsModule::RenderMatrices;
		const auto path = FileSystem::root() / "shaders" / "sfe.glsl";
		std::ifstream in(path, std::ios::binary);
		ASSERT_TRUE(in) << "no " << path.string();
		const std::string glsl((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

		const std::vector<Member> expected = {
			{ "projection", offsetof(RenderMatrices, projection) },
			{ "view", offsetof(RenderMatrices, view) },
			{ "PV", offsetof(RenderMatrices, PV) },
			{ "PVNoJitter", offsetof(RenderMatrices, PVNoJitter) },
			{ "prevPVNoJitter", offsetof(RenderMatrices, prevPVNoJitter) },
			{ "wind", offsetof(RenderMatrices, wind) },
			{ "windBefore", offsetof(RenderMatrices, windBefore) },
			{ "invProjection", offsetof(RenderMatrices, invProjection) },
			{ "invView", offsetof(RenderMatrices, invView) },
			{ "style", offsetof(RenderMatrices, style) },
			{ "patchDeepest", offsetof(RenderMatrices, patchDeepest) },
			{ "patchGrid", offsetof(RenderMatrices, patchGrid) },
			{ "patchSquare", offsetof(RenderMatrices, patchSquare) },
			{ "(end)", sizeof(RenderMatrices) },
		};
		const auto shader = sharedMatrices(glsl);
		ASSERT_EQ(shader.size(), expected.size()) << "the shader's block has other members than RenderMatrices";
		for (size_t i = 0; i < expected.size(); ++i) {
			EXPECT_EQ(shader[i].name, expected[i].name) << "member " << i;
			EXPECT_EQ(shader[i].offset, expected[i].offset) << expected[i].name;
		}
		// As the frame's writes have them: the style, then the ground drawn dense in the last three lanes.
		EXPECT_EQ(offsetof(RenderMatrices, style), 480u);
		EXPECT_EQ(offsetof(RenderMatrices, patchDeepest), 496u);
		EXPECT_EQ(offsetof(RenderMatrices, patchGrid), 512u);
		EXPECT_EQ(offsetof(RenderMatrices, patchSquare), 528u);
		EXPECT_EQ(sizeof(RenderMatrices), 544u);
	}
}
