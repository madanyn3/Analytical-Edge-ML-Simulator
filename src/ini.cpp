#include "ini.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace edge::ini {
std::string trim(std::string s) {
    const auto first=s.find_first_not_of(" \t\r");
    if (first==std::string::npos) return {};
    return s.substr(first,s.find_last_not_of(" \t\r")-first+1);
}
bool identifier(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(),s.end(),[](char c) {
        return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='_';
    });
}
std::string Fields::take(const std::string& key) {
    auto it=values.find(key);
    if (it==values.end()) throw std::invalid_argument(source+": missing required key "+key);
    auto v=it->second.text;
    values.erase(it);
    return v;
}
U Fields::number(const std::string& key) {
    const auto it=values.find(key);
    const auto line=it==values.end()?0:it->second.line;
    auto v=take(key);
    return integer(v,source+":"+std::to_string(line)+": "+key);
}
U Fields::integer(const std::string& v,const std::string& where) {
    U result=0;
    const bool hex=v.starts_with("0x");
    const auto* begin=v.data()+(hex?2:0);
    const auto* end=v.data()+v.size();
    const auto parsed=std::from_chars(begin,end,result,hex?16:10);
    if (begin==end || parsed.ec!=std::errc{} || parsed.ptr!=end)
        throw std::invalid_argument(where+": expected unsigned decimal or 0x integer");
    return result;
}
void Fields::literal(const std::string& key,const std::string& expected) {
    const auto value=take(key);
    if (value!=expected) throw std::invalid_argument(source+": "+key+": supported value is "+expected);
}
void Fields::reject_leftovers() const {
    if (values.empty()) return;
    const auto& [key,value]=*values.begin();
    throw std::invalid_argument(source+":"+std::to_string(value.line)+": unknown key "+key);
}
Document lex(std::string_view text,const std::string& source) {
    if (text.size()>1024*1024) throw std::length_error(source+": config exceeds 1 MiB");
    Document d;
    d.fields.source=source;
    std::istringstream input{std::string(text)};
    std::string raw,section;
    std::set<std::string> seen;
    std::size_t line=0;
    while (std::getline(input,raw)) {
        ++line;
        const std::string where=source+":"+std::to_string(line)+": ";
        if (raw.size()>4096) throw std::invalid_argument(where+"line exceeds 4096 bytes");
        if (std::any_of(raw.begin(),raw.end(),[](unsigned char c){return c<32 && c!='\t' && c!='\r';}))
            throw std::invalid_argument(where+"control character");
        const auto comment=raw.find('#');
        if (comment!=std::string::npos) raw.resize(comment);
        raw=trim(raw);
        if (raw.empty()) continue;
        if (raw.front()=='[') {
            if (raw.back()!=']') throw std::invalid_argument(where+"malformed section");
            section=raw.substr(1,raw.size()-2);
            if (!seen.insert(section).second) throw std::invalid_argument(where+"duplicate section "+section);
            d.sections.push_back({section,line});
            continue;
        }
        const auto eq=raw.find('=');
        if (section.empty() || eq==std::string::npos) throw std::invalid_argument(where+"expected section and key=value");
        const auto key=trim(raw.substr(0,eq));
        const auto value=trim(raw.substr(eq+1));
        if (!identifier(key) || value.empty() || value.find('=')!=std::string::npos)
            throw std::invalid_argument(where+"invalid key/value");
        if (!d.fields.values.emplace(section+"."+key,Value{value,line}).second)
            throw std::invalid_argument(where+"duplicate key "+key);
    }
    return d;
}
std::string read_file(const std::string& path) {
    std::ifstream in(path,std::ios::binary);
    if (!in) throw std::invalid_argument("cannot open config: "+path);
    std::string text;
    char ch;
    while (in.get(ch)) {
        if (text.size()==1024*1024) throw std::length_error("config exceeds 1 MiB: "+path);
        text+=ch;
    }
    if (!in.eof()) throw std::runtime_error("cannot read config: "+path);
    return text;
}
}
