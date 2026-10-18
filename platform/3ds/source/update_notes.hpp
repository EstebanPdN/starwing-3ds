#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace starfox::platform_3ds {
// Normalize metadata to the cartridge font without dropping accented letters.
inline std::string menu_text_ascii(std::string_view input) {
    std::string out;
    for(std::size_t i=0;i<input.size();) {
        const auto first=static_cast<unsigned char>(input[i++]);
        if(first<128U) {out+=static_cast<char>(std::toupper(first));continue;}
        unsigned code=first;unsigned count=0;
        if((first&0xe0U)==0xc0U) {code=first&0x1fU;count=1;}
        else if((first&0xf0U)==0xe0U) {code=first&0x0fU;count=2;}
        else if((first&0xf8U)==0xf0U) {code=first&7U;count=3;}
        else {out+='?';continue;}
        bool valid=i+count<=input.size();
        for(unsigned n=0;n<count&&valid;++n) {
            const auto next=static_cast<unsigned char>(input[i]);
            if((next&0xc0U)!=0x80U) {valid=false;break;}
            code=(code<<6U)|(next&0x3fU);++i;
        }
        if(!valid) {out+='?';continue;}
        if(code>=0x300U&&code<=0x36fU) continue;
        if((code>=0xc0U&&code<=0xc5U)||(code>=0xe0U&&code<=0xe5U)) out+='A';
        else if(code==0xc7U||code==0xe7U) out+='C';
        else if((code>=0xc8U&&code<=0xcbU)||(code>=0xe8U&&code<=0xebU)) out+='E';
        else if((code>=0xccU&&code<=0xcfU)||(code>=0xecU&&code<=0xefU)) out+='I';
        else if(code==0xd1U||code==0xf1U) out+='N';
        else if((code>=0xd2U&&code<=0xd6U)||(code>=0xf2U&&code<=0xf6U)) out+='O';
        else if((code>=0xd9U&&code<=0xdcU)||(code>=0xf9U&&code<=0xfcU)) out+='U';
        else if(code==0xddU||code==0xfdU||code==0xffU) out+='Y';
        else if(code==0x2013U||code==0x2014U||code==0x2022U) out+='-';
        else if(code==0x2018U||code==0x2019U) out+='\'';
        else if(code==0x201cU||code==0x201dU) out+='"';
        else if(code==0x2026U) out+="...";
        else if(code==0xa0U) out+=' ';
        else out+='?';
    }
    return out;
}
// Release Markdown becomes bounded text in the game's own font.
template<class Measure>
std::vector<std::string> update_note_lines(std::string_view body, Measure measure,
    int width) {
    body = body.substr(0, 12288U);
    const auto changelog = body.find("## Changelog");
    if (changelog != std::string_view::npos) body.remove_prefix(changelog + 12U);
    const auto reports = body.find("## Bug reports");
    if (reports != std::string_view::npos) body = body.substr(0, reports);
    std::vector<std::string> lines;
    while (!body.empty() && lines.size() < 768U) {
        const auto end = body.find('\n');
        auto row = body.substr(0, end);
        body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
        while (!row.empty() && (row.front() == '#' || row.front() == ' ' || row.front() == '\r'))
            row.remove_prefix(1);
        if (row.empty() || row.starts_with('<') || row.starts_with("![")) continue;
        const auto normalized=menu_text_ascii(row);
        row=normalized;
        std::string plain;
        for (std::size_t i = 0; i < row.size(); ++i) {
            const auto c = static_cast<unsigned char>(row[i]);
            if (c == '<') { while (i + 1 < row.size() && row[i] != '>') ++i; continue; }
            if (c == '*' || c == '`' || c == '[' || c == ']') continue;
            if (c == '(' && i && row[i - 1] == ']') {
                while (i + 1 < row.size() && row[i] != ')') ++i;
                continue;
            }
            const char letter = c >= 32 && c <= 126 ? static_cast<char>(std::toupper(c)) : ' ';
            if (letter == ' ' && (plain.empty() || plain.back() == ' ')) continue;
            plain += letter >= ' ' && letter <= 'Z' ? letter : ' ';
        }
        while (!plain.empty() && plain.back() == ' ') plain.pop_back();
        while (!plain.empty() && lines.size() < 768U) {
            std::size_t take = std::min<std::size_t>(plain.size(), 80U);
            while (take > 1 && measure(plain.substr(0, take).c_str()) > width) --take;
            if (take < plain.size()) {
                const auto space = plain.rfind(' ', take);
                if (space != std::string::npos && space > 0) take = space;
            }
            lines.push_back(plain.substr(0, take));
            plain.erase(0, take);
            while (!plain.empty() && plain.front() == ' ') plain.erase(0, 1);
        }
    }
    if (lines.empty()) lines.emplace_back("NO CHANGELOG AVAILABLE");
    return lines;
}
} // namespace starfox::platform_3ds
