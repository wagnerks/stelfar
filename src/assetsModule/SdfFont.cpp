#include "SdfFont.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <numeric>
#include <unordered_map>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_OUTLINE_H

#include "core/BinaryCache.h"
#include "core/FileSystem.h"
#include "logsModule/logger.h"
#include "renderModule/FrameCapture.h"

namespace AssetsModule {
	namespace {
		// Each glyph drawn kEm pixels to the em, its field reaching kSpread pixels each side of its outline, packed kGap
		// apart (so the filter reading one never reads the next) into a picture kWidth wide. kVersion goes into the
		// cache's key: up it as what is built here changes.
		constexpr int kEm = 64;
		constexpr int kSpread = 8;
		constexpr int kGap = 2;
		constexpr int kWidth = 2048;
		constexpr uint32_t kVersion = 1;

		// What every font is built with besides what it is asked for: printable ASCII, the Cyrillic of Russian and its
		// neighbours, and a few signs.
		std::u32string everyday() {
			std::u32string codes;
			for (char32_t c = 0x20; c < 0x7F; ++c) {
				codes += c;
			}
			for (char32_t c = 0x400; c < 0x460; ++c) {
				codes += c;
			}
			// Guillemets, the numero, the dashes, degrees, plus-minus and times.
			for (const char32_t c : { 0xAB, 0xBB, 0x2116, 0x2013, 0x2014, 0xB0, 0xB1, 0xD7 }) {
				codes += c;
			}
			return codes;
		}

		std::mutex gMutex;
		// The newest of each font, by path; and every one ever made, kept so one handed out stays whole while a newer one
		// with more glyphs takes its place.
		std::unordered_map<std::string, SdfFont*> gNewest;
		std::vector<std::unique_ptr<SdfFont>> gMade;
	}

	struct SdfFontBuild {
		// `font`'s glyphs and line height read from `cached`, its picture there too.
		static bool read(const std::string& cached, SdfFont& font) {
			SFE::BinaryCache::Reader in(cached);
			uint32_t version = 0;
			in.value(version);
			in.value(font.mLineHeight);
			in.vector(font.mGlyphs);
			std::error_code ec;
			return in && version == kVersion && std::filesystem::exists(font.mPicture, ec);
		}

		// `font` drawn from the font file's `bytes`, each of `codes` it has, its picture written and the rest cached.
		static bool build(const std::string& bytes, const std::u32string& codes, const std::string& cached, SdfFont& font) {
			FT_Library library = nullptr;
			if (FT_Init_FreeType(&library)) {
				return false;
			}
			const FT_Int spread = kSpread;
			FT_Property_Set(library, "bsdf", "spread", &spread);
			FT_Face face = nullptr;
			if (FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(bytes.data()), static_cast<FT_Long>(bytes.size()), 0, &face)
				|| FT_Set_Pixel_Sizes(face, 0, kEm)) {
				FT_Done_FreeType(library);
				return false;
			}
			const float em = static_cast<float>(kEm);
			font.mLineHeight = static_cast<float>(face->size->metrics.height) / 64.f / em;

			// Each glyph's field, drawn from its coverage (FreeType's bsdf: steadier than its outline renderer): drawn
			// plain first, then as a field, kSpread more pixels round it each way.
			struct Drawn {
				SdfFont::Glyph glyph;
				int width = 0;
				int rows = 0;
				std::vector<uint8_t> field;
			};
			std::vector<Drawn> drawn;
			for (const char32_t code : codes) {
				const FT_UInt index = FT_Get_Char_Index(face, code);
				if (index == 0 || FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING)) {
					continue;
				}
				FT_GlyphSlot slot = face->glyph;
				Drawn made;
				made.glyph.code = code;
				made.glyph.advance = static_cast<float>(slot->advance.x) / 64.f / em;
				made.glyph.inkLow = { 1.f, 1.f };
				made.glyph.inkHigh = { 0.f, 0.f };
				if (slot->format == FT_GLYPH_FORMAT_OUTLINE && slot->outline.n_contours > 0) {
					FT_BBox box;
					FT_Outline_Get_CBox(&slot->outline, &box);
					made.glyph.inkLow = { static_cast<float>(box.xMin) / 64.f / em, static_cast<float>(box.yMin) / 64.f / em };
					made.glyph.inkHigh = { static_cast<float>(box.xMax) / 64.f / em, static_cast<float>(box.yMax) / 64.f / em };
					if (!FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) && !FT_Render_Glyph(slot, FT_RENDER_MODE_SDF) && slot->bitmap.pitch > 0) {
						const auto& bitmap = slot->bitmap;
						made.width = static_cast<int>(bitmap.width);
						made.rows = static_cast<int>(bitmap.rows);
						made.field.resize(static_cast<size_t>(made.width) * made.rows);
						for (int y = 0; y < made.rows; ++y) {
							std::memcpy(made.field.data() + static_cast<size_t>(y) * made.width, bitmap.buffer + static_cast<size_t>(y) * bitmap.pitch,
								static_cast<size_t>(made.width));
						}
						made.glyph.low = { static_cast<float>(slot->bitmap_left) / em, static_cast<float>(slot->bitmap_top - made.rows) / em };
						made.glyph.high = { static_cast<float>(slot->bitmap_left + made.width) / em, static_cast<float>(slot->bitmap_top) / em };
					}
				}
				drawn.push_back(std::move(made));
			}
			FT_Done_Face(face);
			FT_Done_FreeType(library);

			// Packed tallest first, in rows across the picture, its height a power of two.
			std::vector<size_t> order(drawn.size());
			std::iota(order.begin(), order.end(), size_t{ 0 });
			std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return drawn[a].rows > drawn[b].rows; });
			std::vector<std::pair<int, int>> at(drawn.size());
			int x = kGap, y = kGap, rowHeight = 0;
			for (const size_t i : order) {
				if (drawn[i].width == 0) {
					continue;
				}
				if (x + drawn[i].width + kGap > kWidth) {
					x = kGap;
					y += rowHeight + kGap;
					rowHeight = 0;
				}
				at[i] = { x, y };
				x += drawn[i].width + kGap;
				rowHeight = (std::max)(rowHeight, drawn[i].rows);
			}
			int height = 4;
			while (height < y + rowHeight + kGap) {
				height *= 2;
			}
			// The field in all three colours, the picture opaque: nothing of a material's own reading of alpha applies.
			std::vector<uint8_t> rgba(static_cast<size_t>(kWidth) * height * 4, 0);
			for (size_t p = 3; p < rgba.size(); p += 4) {
				rgba[p] = 255;
			}
			font.mGlyphs.clear();
			for (size_t i = 0; i < drawn.size(); ++i) {
				auto glyph = drawn[i].glyph;
				if (drawn[i].width > 0) {
					const auto [gx, gy] = at[i];
					for (int row = 0; row < drawn[i].rows; ++row) {
						for (int col = 0; col < drawn[i].width; ++col) {
							auto* texel = &rgba[(static_cast<size_t>(gy + row) * kWidth + gx + col) * 4];
							texel[0] = texel[1] = texel[2] = drawn[i].field[static_cast<size_t>(row) * drawn[i].width + col];
						}
					}
					glyph.uvLow = { static_cast<float>(gx) / kWidth, static_cast<float>(gy + drawn[i].rows) / height };
					glyph.uvHigh = { static_cast<float>(gx + drawn[i].width) / kWidth, static_cast<float>(gy) / height };
				}
				font.mGlyphs.push_back(glyph);
			}
			std::sort(font.mGlyphs.begin(), font.mGlyphs.end(), [](const auto& a, const auto& b) { return a.code < b.code; });
			if (!SFE::Render::writePng(font.mPicture, rgba, kWidth, height)) {
				return false;
			}
			SFE::BinaryCache::Writer out(cached);
			out.value(kVersion);
			out.value(font.mLineHeight);
			out.vector(font.mGlyphs);
			SFE::LogsModule::Logger::LOG_INFO("SdfFont: %zu glyphs drawn into %s (%dx%d)", font.mGlyphs.size(), font.mPicture.c_str(), kWidth, height);
			return out.commit();
		}
	};

	const SdfFont* SdfFont::get(const std::string& path, const std::u32string& needed) {
		std::lock_guard lock(gMutex);
		SdfFont* newest = gNewest.contains(path) ? gNewest[path] : nullptr;
		const auto asked = [&](const SdfFont& font, char32_t code) { return std::binary_search(font.mAsked.begin(), font.mAsked.end(), code); };
		if (newest && std::all_of(needed.begin(), needed.end(), [&](char32_t c) { return c < 0x20 || asked(*newest, c); })) {
			return newest;
		}
		// What it is built for: every day's glyphs, all it was asked for before and now; named in cache/ by the font's
		// bytes, those and how it is built.
		std::u32string codes = everyday() + (newest ? newest->mAsked : std::u32string()) + needed;
		std::erase_if(codes, [](char32_t c) { return c < 0x20; });
		std::sort(codes.begin(), codes.end());
		codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
		const auto file = SFE::FileSystem::resolve(path);
		std::ifstream in(file, std::ios::binary);
		const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		if (bytes.empty()) {
			SFE::LogsModule::Logger::LOG_ERROR("SdfFont: no font at %s", file.string().c_str());
			return newest;
		}
		uint64_t key = SFE::BinaryCache::hash(bytes);
		key = SFE::BinaryCache::hash(std::string_view(reinterpret_cast<const char*>(codes.data()), codes.size() * sizeof(char32_t)), key);
		key = SFE::BinaryCache::hash(std::to_string(kVersion) + " " + std::to_string(kEm) + " " + std::to_string(kSpread), key);
		const auto cached = SFE::BinaryCache::path("sdf_" + file.stem().string(), key);
		auto made = std::make_unique<SdfFont>();
		made->mAsked = codes;
		made->mPicture = std::filesystem::path(cached).replace_extension(".png").string();
		if (!SdfFontBuild::read(cached, *made)) {
			made->mGlyphs.clear();
			if (!SdfFontBuild::build(bytes, codes, cached, *made)) {
				SFE::LogsModule::Logger::LOG_ERROR("SdfFont: could not draw %s", file.string().c_str());
				return newest;
			}
		}
		gNewest[path] = made.get();
		gMade.push_back(std::move(made));
		return gNewest[path];
	}

	const SdfFont::Glyph* SdfFont::glyph(char32_t code) const {
		const auto found = std::lower_bound(mGlyphs.begin(), mGlyphs.end(), code, [](const Glyph& g, char32_t c) { return g.code < c; });
		return found != mGlyphs.end() && found->code == code ? &*found : nullptr;
	}
}
