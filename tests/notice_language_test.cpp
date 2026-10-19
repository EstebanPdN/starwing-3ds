#include "../platform/3ds/source/notice_language.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace starfox::platform_3ds;
int main() {
    const std::array expected{NoticeLanguage::en,NoticeLanguage::en,
        NoticeLanguage::fr,NoticeLanguage::de,NoticeLanguage::it,
        NoticeLanguage::es,NoticeLanguage::en,NoticeLanguage::en,
        NoticeLanguage::en,NoticeLanguage::pt,NoticeLanguage::en,NoticeLanguage::en};
    for(unsigned value=0;value<256;++value) {
        if(notice_language(value)!=(value<expected.size()?expected[value]:NoticeLanguage::en))
            throw std::runtime_error("Incorrect console-language selection/fallback");
        if(notice_language(value,false)!=NoticeLanguage::en)
            throw std::runtime_error("Failed language query must fall back to English");
    }
    std::cout<<"PASS: all 12 console languages, unknown byte values and failed-query fallback\n";
}
