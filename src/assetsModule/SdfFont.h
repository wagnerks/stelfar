#pragma once

#include <string>
#include <vector>

#include "mathModule/Forward.h"

namespace AssetsModule {
	// A font's glyphs as signed distance fields (FreeType's), packed into one picture: text drawn from it is sharp however
	// near it is seen and smooth however far, its edge cut where the field reads half. Built for the glyphs asked for and
	// kept in cache/ (the picture a PNG, read as any texture is), built again only for another font or a glyph it has not
	// had. Sizes are in ems, x to the right and y up from the pen on the baseline.
	class SdfFont {
	public:
		struct Glyph {
			char32_t code = 0;
			SFE::Math::Vec2 low;    // its quad, the field round the outline and all
			SFE::Math::Vec2 high;
			SFE::Math::Vec2 uvLow;  // where those corners are in the picture
			SFE::Math::Vec2 uvHigh;
			SFE::Math::Vec2 inkLow; // its outline's box (none for a space: low over high)
			SFE::Math::Vec2 inkHigh;
			float advance = 0.f;
		};

		// Any thread: the font of `path` (a .ttf or .otf, as content paths go) with at least the glyphs of `needed` it has;
		// nullptr if it does not read.
		static const SdfFont* get(const std::string& path, const std::u32string& needed);

		// The glyph of `code`, nullptr if the font has none.
		const Glyph* glyph(char32_t code) const;
		// The picture's file, for TextureHandler::loadTexture.
		const std::string& picture() const { return mPicture; }
		// From one line's baseline to the next's.
		float lineHeight() const { return mLineHeight; }

	private:
		friend struct SdfFontBuild;
		std::u32string mAsked; // what it was built for, sorted: the glyphs it has and those the font lacks
		std::vector<Glyph> mGlyphs; // by code
		std::string mPicture;
		float mLineHeight = 1.2f;
	};
}
