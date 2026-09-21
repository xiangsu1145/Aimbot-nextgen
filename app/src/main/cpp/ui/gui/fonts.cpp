// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui — ImGui font loading (implementation)
//
//  The .ttf files are not opened at runtime: CMake runs objcopy over them and
//  links the raw bytes into this library's .rodata. objcopy names the symbols
//  after the file, so "product_sans_medium.ttf" becomes
//  _binary_product_sans_medium_ttf_start / _end.
// ─────────────────────────────────────────────────────────────────────────────
#include "fonts.h"

#include <android/log.h>

#include "imgui.h"

// Raw font bytes, emitted by objcopy (see CMakeLists.txt).
extern "C" {
extern const unsigned char _binary_product_sans_medium_ttf_start[];
extern const unsigned char _binary_product_sans_medium_ttf_end[];
extern const unsigned char _binary_simhei_regular_ttf_start[];
extern const unsigned char _binary_simhei_regular_ttf_end[];
}

#define TAG "AimbotNg"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace aimbotng {
namespace ui {

namespace {

// Reference size for both fonts. With ImGui 1.92+'s dynamic font atlas this
// is no longer "the one baked size": glyphs are rasterised on demand at
// whatever pixel size each AddText request carries, so text stays sharp at
// any scale (hud scale × density boost) without re-baking.
constexpr float kFontSize = 20.0f;

/// Registers one embedded font on the atlas. `len` is derived from the objcopy
/// symbol pair; FontDataOwnedByAtlas stays false because the data lives in
/// .rodata and must never be freed.
ImFont* addEmbedded(ImGuiIO& io,
                    const unsigned char* begin,
                    const unsigned char* end,
                    const ImFontConfig& cfg,
                    const ImWchar* ranges) {
    const int len = static_cast<int>(end - begin);
    if (len <= 0) {
        LOGW("embedded font is empty (%d bytes)", len);
        return nullptr;
    }
    return io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(begin), len, kFontSize, &cfg, ranges);
}

}  // namespace

void loadFonts(ImGuiIO& io) {
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    // Data is baked into .rodata — ImGui must not try to free it.
    cfg.FontDataOwnedByAtlas = false;

    // ── Main (Latin) font ────────────────────────────────────────────────
    ImFont* latin = addEmbedded(io,
                                _binary_product_sans_medium_ttf_start,
                                _binary_product_sans_medium_ttf_end,
                                cfg, nullptr);
    if (latin) {
        io.FontDefault = latin;
    } else {
        LOGW("failed to load product_sans_medium, falling back to ImGui default");
    }

    // ── Chinese fallback, merged into the main font ──────────────────────
    // MergeMode appends SimHei's glyphs to the font above instead of creating a
    // second font, so Latin text keeps the Product Sans look and any CJK
    // codepoint falls through to SimHei.
    //
    // The glyph ranges are mandatory here: without them the atlas only bakes
    // Latin ranges and Chinese renders as blank/tofu. "SimplifiedCommon" covers
    // the ~2500 most used simplified characters plus half-width forms (a couple
    // hundred KB of atlas) rather than the full 21k-glyph CJK set.
    ImFontConfig cfgCjk = cfg;
    cfgCjk.MergeMode = true;
    ImFont* cjk = addEmbedded(io,
                              _binary_simhei_regular_ttf_start,
                              _binary_simhei_regular_ttf_end,
                              cfgCjk,
                              io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (!cjk) {
        LOGW("failed to load simhei_regular, Chinese glyphs will be missing");
    }
}

}  // namespace ui
}  // namespace aimbotng
