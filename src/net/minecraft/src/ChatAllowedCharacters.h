#pragma once

#include <string>
#include "java/Type.h"

// net.minecraft.src.ChatAllowedCharacters
class ChatAllowedCharacters
{
public:
	// Read from /font.txt on first use rather than in a global constructor:
	// a resource open before main() reaches the console asset locators
	// before their devices are initialised.
	static const std::string &allowedCharacters();
	static const char allowedCharactersArray[15];
	static bool isAllowedCharacter(char_t c);
	// Index of c within allowedCharacters(), decoded to UTF-16 exactly once
	// (lazily, on first call) and cached in a lookup table -- see the .cpp
	// for why this exists: FontRenderer::getCharIndex() calls this once per
	// glyph drawn, every frame, and used to re-decode the whole ~256-
	// character haystack from UTF-8 on every single call.
	static int_t indexOfAllowedCharacter(char_t c);
	static std::string filterAllowedCharacters(const std::string &text);
	static std::string func_52019_a(const std::string &text) { return filterAllowedCharacters(text); }

private:
	static std::string getAllowedCharacters();
};
