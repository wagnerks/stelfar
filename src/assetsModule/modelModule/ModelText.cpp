#include "ModelText.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "assetsModule/SdfFont.h"
#include "assetsModule/TextureHandler.h"
#include "logsModule/logger.h"

namespace AssetsModule::ModelText {
	namespace {
		using SFE::Math::Vec2;
		using SFE::Math::Vec3;

		// `utf8` as code points; a broken sequence as U+FFFD.
		std::u32string decoded(const std::string& utf8) {
			std::u32string codes;
			for (size_t i = 0; i < utf8.size();) {
				const auto lead = static_cast<unsigned char>(utf8[i]);
				const int more = lead < 0x80 ? 0 : (lead >> 5) == 0x6 ? 1 : (lead >> 4) == 0xE ? 2 : (lead >> 3) == 0x1E ? 3 : -1;
				char32_t code = more == 0 ? lead : more == 1 ? lead & 0x1F : more == 2 ? lead & 0x0F : lead & 0x07;
				bool whole = more >= 0 && i + static_cast<size_t>(more) < utf8.size();
				for (int k = 1; whole && k <= more; ++k) {
					const auto next = static_cast<unsigned char>(utf8[i + static_cast<size_t>(k)]);
					whole = (next & 0xC0) == 0x80;
					code = (code << 6) | (next & 0x3F);
				}
				codes += whole ? code : char32_t(0xFFFD);
				i += whole ? static_cast<size_t>(more) + 1 : 1;
			}
			return codes;
		}

		// The numbers of `extras`' `key`, none if it has none.
		std::vector<float> numbersOf(const std::map<std::string, std::string>& extras, const char* key) {
			std::vector<float> numbers;
			if (const auto found = extras.find(key); found != extras.end()) {
				std::istringstream text(found->second);
				for (float number; text >> number;) {
					numbers.push_back(number);
				}
			}
			return numbers;
		}

		float linear(float srgb) {
			return srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
		}

		// `node`'s text as a mesh in its own space, false if there is nothing to draw.
		bool written(const SFE::MeshObject3D& node, SFE::MeshObject3D& out) {
			const auto& extras = node.extras;
			const auto size = numbersOf(extras, "size");
			if (size.size() != 2 || size[0] <= 0.f || size[1] <= 0.f) {
				SFE::LogsModule::Logger::LOG_ERROR("ModelText: %s has a text but no `size` \"w h\"", node.nodeName.c_str());
				return false;
			}
			const auto codes = decoded(extras.at("text"));
			const auto fontPath = extras.contains("font") ? extras.at("font") : std::string(kDefaultFont);
			const auto* font = SdfFont::get(fontPath, codes);
			if (!font) {
				return false;
			}
			auto* picture = TextureHandler::loadTexture(font->picture());
			if (!picture || !picture->isValid()) {
				return false;
			}

			// Laid out in ems, a line under another, each centred on its ink; then the whole ink fitted into `size`.
			struct Quad {
				Vec2 low, high, uvLow, uvHigh;
			};
			std::vector<Quad> quads;
			Vec2 inkLow{ std::numeric_limits<float>::max() };
			Vec2 inkHigh{ std::numeric_limits<float>::lowest() };
			size_t lineStart = 0;
			float lineInkLow = std::numeric_limits<float>::max(), lineInkHigh = std::numeric_limits<float>::lowest();
			float pen = 0.f, baseline = 0.f;
			const auto endLine = [&] {
				if (lineInkHigh >= lineInkLow) {
					const float shift = -0.5f * (lineInkLow + lineInkHigh);
					for (size_t q = lineStart; q < quads.size(); ++q) {
						quads[q].low.x += shift;
						quads[q].high.x += shift;
					}
					inkLow.x = (std::min)(inkLow.x, lineInkLow + shift);
					inkHigh.x = (std::max)(inkHigh.x, lineInkHigh + shift);
				}
				lineStart = quads.size();
				lineInkLow = std::numeric_limits<float>::max();
				lineInkHigh = std::numeric_limits<float>::lowest();
				pen = 0.f;
				baseline -= font->lineHeight();
			};
			for (const char32_t code : codes) {
				if (code == U'\n') {
					endLine();
					continue;
				}
				const auto* glyph = font->glyph(code);
				if (!glyph) {
					glyph = font->glyph(U'?');
				}
				if (!glyph) {
					continue;
				}
				if (glyph->high.x > glyph->low.x) {
					quads.push_back({ { pen + glyph->low.x, baseline + glyph->low.y }, { pen + glyph->high.x, baseline + glyph->high.y },
						glyph->uvLow, glyph->uvHigh });
				}
				if (glyph->inkHigh.x >= glyph->inkLow.x) {
					lineInkLow = (std::min)(lineInkLow, pen + glyph->inkLow.x);
					lineInkHigh = (std::max)(lineInkHigh, pen + glyph->inkHigh.x);
					inkLow.y = (std::min)(inkLow.y, baseline + glyph->inkLow.y);
					inkHigh.y = (std::max)(inkHigh.y, baseline + glyph->inkHigh.y);
				}
				pen += glyph->advance;
			}
			endLine();
			if (quads.empty() || inkHigh.x <= inkLow.x || inkHigh.y <= inkLow.y) {
				return false;
			}
			const float scale = (std::min)(size[0] / (inkHigh.x - inkLow.x), size[1] / (inkHigh.y - inkLow.y));
			const Vec2 middle = (inkLow + inkHigh) * 0.5f;

			auto& mesh = out.mesh;
			for (const auto& quad : quads) {
				const auto base = static_cast<unsigned>(mesh.vertices.size());
				const Vec2 corners[4] = { quad.low, { quad.high.x, quad.low.y }, quad.high, { quad.low.x, quad.high.y } };
				const Vec2 uvs[4] = { quad.uvLow, { quad.uvHigh.x, quad.uvLow.y }, quad.uvHigh, { quad.uvLow.x, quad.uvHigh.y } };
				for (int k = 0; k < 4; ++k) {
					SFE::Vertex3D vertex;
					const Vec2 at = (corners[k] - middle) * scale;
					vertex.position = { at.x, at.y, kLift };
					vertex.texCoords = uvs[k];
					// Where on the text it is, metres: what its wear is laid over.
					vertex.texCoords1 = at;
					vertex.normal = { 0.f, 0.f, 1.f };
					vertex.tangent = { 1.f, 0.f, 0.f };
					vertex.biTangent = { 0.f, 1.f, 0.f };
					vertex.color = { 1.f, 1.f, 1.f, 1.f };
					mesh.vertices.push_back(vertex);
				}
				for (const unsigned k : { 0u, 1u, 2u, 0u, 2u, 3u }) {
					mesh.indices.push_back(base + k);
				}
			}

			// Its ink's box, and a margin for the field round it.
			const Vec2 margin = Vec2{ size[0], size[1] } * 0.1f;
			const Vec2 low = (inkLow - middle) * scale - margin, high = (inkHigh - middle) * scale + margin;
			out.aabb = SFE::FrustumModule::AABB(Vec3{ low.x, low.y, 0.f }, Vec3{ high.x, high.y, 2.f * kLift });

			auto& material = out.material;
			material.name = "text";
			material[SFE::DIFFUSE] = SFE::MaterialTexture{ picture->texture, SFE::DIFFUSE, SFE::DIFFUSE, 0 };
			material[SFE::DIFFUSE].path = picture->assetPath;
			const auto colour = numbersOf(extras, "colour");
			material.baseColor = colour.size() == 3 ? Vec3{ linear(colour[0]), linear(colour[1]), linear(colour[2]) } : Vec3{ 1.f };
			const auto wear = numbersOf(extras, "wear");
			// Its field cut at half, at its glyphs' own edges; worn, further in (kTextFS).
			material.alphaCutoff = 0.5f + 0.5f * std::clamp(wear.empty() ? 0.f : wear[0], 0.f, 0.9f);
			material.roughness = 0.6f;
			material.metallic = 0.f;
			material.fragmentShader = kTextFS;
			material.castsShadow = false;
			return true;
		}
	}

	void add(SFE::Tree<SFE::MeshObject3D>& meshes) {
		std::vector<SFE::Tree<SFE::MeshObject3D>*> marked;
		for (auto& node : meshes) {
			if (node.value.extras.contains("text")) {
				marked.push_back(&node);
			}
		}
		for (auto* node : marked) {
			SFE::MeshObject3D text;
			if (!written(node->value, text)) {
				continue;
			}
			text.nodeName = node->value.nodeName;
			text.transform = node->value.transform;
			node->addChild(std::move(text));
		}
	}
}
