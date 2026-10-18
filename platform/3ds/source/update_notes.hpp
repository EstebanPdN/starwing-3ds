#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace starfox::platform_3ds {
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
