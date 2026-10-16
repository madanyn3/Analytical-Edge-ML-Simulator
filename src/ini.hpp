#pragma once
// Internal strict-INI lexer shared by the version-1 and version-2 config parsers.
// Not a public header: the dialect is documented in docs/configuration.md.
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace edge::ini {
using U = std::uint64_t;
bool identifier(const std::string&);
std::string trim(std::string);
struct Value { std::string text; std::size_t line; };
struct Section { std::string name; std::size_t line; };
// Values are consumed exactly once. Any remaining entry is a typo/unsupported knob,
// never a silently ignored setting. This also makes schema evolution explicit.
class Fields {
public:
    std::map<std::string, Value> values; // Keyed "section.key".
    std::string source;
    std::string take(const std::string& key);
    U number(const std::string& key);
    void literal(const std::string& key, const std::string& expected);
    void reject_leftovers() const; // Throws on the first unconsumed key.
    static U integer(const std::string& value, const std::string& where);
};
struct Document {
    std::vector<Section> sections; // Declaration order; names are unique.
    Fields fields;
};
// Lexical rules only: size/line limits, control characters, comments, sections,
// key=value syntax and duplicates. Section names are NOT checked here; each schema
// version decides which sections exist.
Document lex(std::string_view text, const std::string& source);
std::string read_file(const std::string& path); // 1 MiB limit, binary read.
}
