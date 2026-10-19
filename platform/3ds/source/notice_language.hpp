#pragma once
namespace starfox::platform_3ds {
enum class NoticeLanguage { en, es, pt, it, de, fr };
// libctru CFG_Language values. Unknown languages and failed queries use English.
constexpr NoticeLanguage notice_language(unsigned system_language, bool read_ok=true) {
    if (!read_ok) return NoticeLanguage::en;
    switch(system_language) {
    case 1: return NoticeLanguage::en;
    case 2: return NoticeLanguage::fr;
    case 3: return NoticeLanguage::de;
    case 4: return NoticeLanguage::it;
    case 5: return NoticeLanguage::es;
    case 9: return NoticeLanguage::pt;
    default: return NoticeLanguage::en;
    }
}
} // namespace starfox::platform_3ds
