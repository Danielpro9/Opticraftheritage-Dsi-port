#include "ChatAllowedCharacters.h"
#include <vector>
#include <unordered_map>

#include "java/Resource.h"
#include "java/String.h"

#include <memory>

const std::string &ChatAllowedCharacters::allowedCharacters()
{
	static const std::string characters = getAllowedCharacters();
	return characters;
}

const char ChatAllowedCharacters::allowedCharactersArray[15] = {
	'/', '\n', '\r', '\t', '\0', '\f', '`', '?', '*', '\\',
	'<', '>', '|', '"', ':'
};

std::string ChatAllowedCharacters::getAllowedCharacters()
{
	std::unique_ptr<std::istream> bufferedreader(Resource::getResource("/font.txt"));
	std::string result;

	std::string line;
	while (std::getline(*bufferedreader, line))
	{
		if (line.empty() || line[0] == '#')
			continue;
		if (line.back() == '\r')
			line.pop_back();
		result += line;
	}
	return result;
}


int_t ChatAllowedCharacters::indexOfAllowedCharacter(char_t c)
{
	// allowedCharacters() never changes after the first call (it's read once
	// from /font.txt and cached), so the UTF-8->UTF-16 decode and the id-to-
	// index mapping only need to happen once, on first use, instead of on
	// every glyph FontRenderer draws.
	static const std::unordered_map<char_t, int_t> lookup = [] {
		std::unordered_map<char_t, int_t> map;
		const std::vector<char_t> units = String::toUtf16(jstring(allowedCharacters()));
		for (std::size_t i = 0; i < units.size(); ++i)
			map.emplace(units[i], static_cast<int_t>(i));
		return map;
	}();
	auto it = lookup.find(c);
	return it != lookup.end() ? it->second : -1;
}

bool ChatAllowedCharacters::isAllowedCharacter(char_t c)
{
	return c != 167 && (indexOfAllowedCharacter(c) >= 0 || c > 32);
}

std::string ChatAllowedCharacters::filterAllowedCharacters(const std::string &text)
{
	std::vector<char_t> filtered;
	for (char_t c : String::toUtf16(jstring(text)))
	{
		if (isAllowedCharacter(c))
			filtered.push_back(c);
	}
	return String::fromUtf16(filtered);
}
