#ifndef SFE_WORDS_GLSL
#define SFE_WORDS_GLSL
// An entity's words (EntityUser.h): eight u32 an application keeps for each entity, word 7 the kind of whoever wrote
// them (0: nobody). g_buffer.vs reads them by the entity's id from binding 37 of set 0 and hands them on flat; a
// fragment shader drawn after it reads them here. A vertex shader that stands in for g_buffer.vs writes the same two
// outputs at the same locations under the same names (0s for an entity without words).
layout(location = 13) flat in uvec4 vWords;     // words 0-3
layout(location = 16) flat in uvec4 vWordsMore; // words 4-7

// The kind of whoever wrote this entity's words, 0 for none.
uint sfeWordsKind() {
    return vWordsMore.w;
}
#endif
