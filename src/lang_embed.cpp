// D10: the texts of lang/*.txt built into the DLL (see lang.h, Embedded).

#include <cstddef>

#include "lang.h"

namespace lang {
namespace {

struct EmbeddedLang {
    const char* code;
    const unsigned char* bytes;
    size_t size;
};

#include "lang_texts.inc"  // kEmbeddedLangs (build directory, tools/embed_lang.cmake)

}  // namespace

std::string Embedded(const std::string& code) {
    for (const EmbeddedLang& l : kEmbeddedLangs)
        if (code == l.code) return std::string(reinterpret_cast<const char*>(l.bytes), l.size);
    return std::string();
}

std::vector<std::string> EmbeddedCodes() {
    std::vector<std::string> out;
    for (const EmbeddedLang& l : kEmbeddedLangs) out.push_back(l.code);
    return out;
}

}  // namespace lang
