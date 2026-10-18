#include "../platform/3ds/source/update_notes.hpp"
#include <cassert>
#include <iostream>
using namespace starfox::platform_3ds;
int main() {
    auto measure=[](const char* value){int n=0; for(;*value;++value) n+=*value=='W'?12:6; return n;};
    auto lines=update_note_lines("<img src=\"qr.png\">\n## Changelog\n- **Initial** [release](https://example.com).\n## Bug reports\nPrivate info",measure,100);
    std::string all;
    for(const auto& line:lines) {assert(measure(line.c_str())<=100); all+=line+' ';}
    assert(all=="- INITIAL RELEASE. ");
    lines=update_note_lines(std::string(200,'W'),measure,100);
    for(const auto& line:lines) assert(measure(line.c_str())<=100);
    assert(lines.size()==25);
    assert(update_note_lines("",measure,100).front()=="NO CHANGELOG AVAILABLE");
    lines=update_note_lines(std::string(16000,'W'),measure,100);
    assert(lines.size()<=768);
    std::cout<<"PASS: native-font wrapping, Markdown links, HTML suppression and bounded notes\n";
}
