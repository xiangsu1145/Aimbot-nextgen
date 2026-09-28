// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::ime — implementation. See ime.h for the rationale.
// ─────────────────────────────────────────────────────────────────────────────
#include "ui/gui/ime.h"

#include <android/log.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "ui/gui/theme.h"
#include "pinyin/include/pinyinime.h"

using namespace ime_pinyin;

#define IMTAG "AimbotIme"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, IMTAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, IMTAG, __VA_ARGS__)

#ifdef HAS_IMGUI
#include "imgui.h"

namespace aimbotng {
namespace ui {
namespace ime {

namespace {

using namespace theme;

constexpr int kMaxKeys = 48;

// ── Key actions ─────────────────────────────────────────────────────────────
enum {
    A_Ins = 0,   ///< append `insert` (letters / digits / symbols)
    A_Back,      ///< delete the character before the caret
    A_Space,     ///< a space in Latin mode; commits candidate 0 in Chinese
    A_Caps,      ///< toggle upper case
    A_Layer,     ///< letters <-> numbers
    A_Mode,      ///< Latin <-> pinyin
    A_Done,      ///< close the keyboard
    A_Left,      ///< caret left one character
    A_Right,     ///< caret right one character
    A_Cand = 64, ///< A_Cand + i commits candidate i
};

struct KeyDef {
    const char* label;
    const char* insert;
    int         action;
    float       weight;
};

using KeyRow = std::vector<KeyDef>;

// ── Layout metrics, in design px against a 1200 px board ────────────────────
constexpr float kPad      = 10.0f;
constexpr float kGap      = 7.0f;
constexpr float kKeyH     = 54.0f;
constexpr float kRowGap   = 8.0f;
constexpr float kCandH    = 52.0f;
constexpr float kRoundKey = 11.0f;
constexpr float kRoundPad = 18.0f;
constexpr float kTextSize = 26.0f;
constexpr float kCandSize = 28.0f;

/// Press-down travel: the key is drawn this much lower while held, and its
/// shadow shrinks to match, which is the whole "the key went down" cue.
constexpr float kKeyTravel = 3.0f;

/// How long the delete key has to be held before it starts repeating, and how
/// often it repeats after that.
constexpr float kRepeatDelaySec = 0.40f;
constexpr float kRepeatRateSec  = 0.075f;

// ── Pinyin engine ───────────────────────────────────────────────────────────
//
// The candidates come from the AOSP Google Pinyin decoder (libgooglepinyin):
// a real sentence engine over a 65k-lemma dictionary, so typing a whole
// sentence's pinyin produces whole-phrase candidates — proper 连打 — instead of
// per-syllable single characters from a hand-written table. The binary
// dictionary lives on the device at kDictPath; the user dictionary grows as
// commits are made. Nothing here needs the network.
constexpr const char* kDictPath = "/data/local/tmp/aimbotng/ime/dict_pinyin.dat";
constexpr const char* kUserDictPath = "/data/local/tmp/aimbotng/ime/usr.dat";

bool g_engineReady = false;
bool g_engineFailed = false;

/// UTF-16 -> UTF-8 for a single string of `n` code units.
std::string utf16To8(const ime_pinyin::char16* s, size_t n) {
    std::string out;
    out.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) {
        unsigned int c = static_cast<unsigned char>(s[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            const unsigned int lo = static_cast<unsigned char>(s[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000u + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if      (c < 0x80)        out += static_cast<char>(c);
        else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

/// Loads the decoder on first use. Failure is sticky and non-fatal: the raw
/// pinyin chip at the end of the strip always remains, so nothing typed is
/// ever unreachable even without a dictionary.
bool ensureEngine() {
    if (g_engineReady) return true;
    if (g_engineFailed) return false;
    // Dict on first run: the shell pushes dict_pinyin.dat next to the app's
    // config; if it is missing, say so once in the log and fall back.
    if (!im_open_decoder(kDictPath, kUserDictPath)) {
        g_engineFailed = true;
        LOGW("ime: decoder open FAILED dict=%s", kDictPath);
        return false;
    }
    // A name field never needs more than this; the engine defaults are for
    // whole-sentence chat input.
    im_set_max_lens(40, 30);
    g_engineReady = true;
    return true;
}


/// Length in bytes of the UTF-8 character ENDING at `i`.
int prevCharLen(const char* buf, int i) {
    int n = 1;
    while (i - n > 0 && (static_cast<unsigned char>(buf[i - n]) & 0xC0) == 0x80) ++n;
    return n;
}

/// Length in bytes of the UTF-8 character STARTING at `i`.
int nextCharLen(const char* buf, int len, int i) {
    if (i >= len) return 0;
    int n = 1;
    while (i + n < len && (static_cast<unsigned char>(buf[i + n]) & 0xC0) == 0x80) ++n;
    return n;
}

// ── State ───────────────────────────────────────────────────────────────────

constexpr int kMaxCands = 6;

struct Store {
    bool                  open = false;
    widgets::DialogField* f = nullptr;

    bool  cn    = false;   ///< pinyin mode
    bool  caps  = false;
    int   layer = 0;       ///< 0 letters, 1 numbers

    char  py[24] = {};
    int   pyLen  = 0;

    float anim = 0.0f;                 ///< open/close ease
    float key[kMaxKeys] = {};          ///< per-key press ease (1 = held)
    float backHeld = 0.0f;             ///< seconds the delete key has been down
    float backAcc  = 0.0f;             ///< repeat accumulator

    std::vector<std::string> cands;
    std::vector<const char*> candPtr;
    int   candHit = -1;                ///< which chip consumed this frame's tap

    widgets::Rect panel{};
    bool  panelValid = false;
};

Store g;

void rebuildCandidates() {
    g.cands.clear();
    g.candPtr.clear();
    if (!g.cn || g.pyLen == 0) {
        return;
    }
    if (!ensureEngine()) {
        // No dictionary on this device: the raw pinyin is still offered, so
        // the keyboard degrades to a Latin keyboard with Chinese ambitions.
        g.cands.push_back(std::string(g.py, static_cast<size_t>(g.pyLen)));
        g.candPtr.push_back(g.cands[0].c_str());
        return;
    }

    const std::string py(g.py, static_cast<size_t>(g.pyLen));
    im_reset_search();
    const size_t n = im_search(py.c_str(), py.size());
    static bool s_loggedOnce = false;
    if (!s_loggedOnce) {
        s_loggedOnce = true;
        LOGI("ime: first search '%s' -> %zu candidates (engineReady=%d)",
             py.c_str(), n, (int)g_engineReady);
    }

    ime_pinyin::char16 buf[64];
    for (size_t i = 0; i < 10 && g.cands.size() < static_cast<size_t>(kMaxCands); ++i) {
        const char16* r = im_get_candidate(i, buf, 63);
        if (r == nullptr) break;
        buf[63] = u'\0';
        size_t len = 0;
        while (len < 63 && buf[len] != 0) ++len;
        std::string s = utf16To8(buf, len);
        if (s.empty()) break;
        g.cands.push_back(std::move(s));
    }

    // The raw pinyin rides at the END: committing it is "this is a Latin name",
    // which is a real answer for a profile, but never the default (space would
    // then produce Latin text out of a Chinese keyboard).
    g.cands.push_back(py);
    g.candPtr.reserve(g.cands.size());
    for (const std::string& s : g.cands) g.candPtr.push_back(s.c_str());
}

// ── Editing ─────────────────────────────────────────────────────────────────

void insertRaw(const char* t, int n) {
    if (g.f == nullptr || n <= 0) return;
    if (g.f->len + n > static_cast<int>(sizeof(g.f->buf)) - 1) return;
    std::memmove(&g.f->buf[g.f->caret + n], &g.f->buf[g.f->caret],
                 static_cast<size_t>(g.f->len - g.f->caret));
    std::memcpy(&g.f->buf[g.f->caret], t, static_cast<size_t>(n));
    g.f->len += n;
    g.f->caret += n;
    g.f->buf[g.f->len] = '\0';
}

void insertChar(const char* ch) { insertRaw(ch, static_cast<int>(std::strlen(ch))); }

void backspace() {
    if (g.f == nullptr || g.f->caret <= 0) return;
    const int n = prevCharLen(g.f->buf, g.f->caret);
    std::memmove(&g.f->buf[g.f->caret - n], &g.f->buf[g.f->caret],
                 static_cast<size_t>(g.f->len - g.f->caret));
    g.f->len -= n;
    g.f->caret -= n;
    g.f->buf[g.f->len] = '\0';
}

void commitCandidate(int i) {
    if (i < 0 || i >= static_cast<int>(g.cands.size())) return;
    insertRaw(g.cands[static_cast<size_t>(i)].c_str(),
              static_cast<int>(g.cands[static_cast<size_t>(i)].size()));
    g.pyLen = 0;
    g.py[0] = '\0';
    if (g_engineReady) {
        // The decoder holds the aborted composition; clear it so the next
        // syllable starts fresh, and flush so the user dictionary learns.
        im_reset_search();
        im_flush_cache();
    }
    rebuildCandidates();
}

/// Types one letter. In pinyin mode it grows the composition buffer; otherwise
/// it goes straight into the field.
void typeLetter(const char* letter) {
    if (g.cn) {
        const int n = static_cast<int>(std::strlen(letter));
        if (g.pyLen + n >= static_cast<int>(sizeof(g.py)) - 1) return;
        std::memcpy(&g.py[g.pyLen], letter, static_cast<size_t>(n));
        g.pyLen += n;
        g.py[g.pyLen] = '\0';
        rebuildCandidates();
    } else {
        insertChar(letter);
    }
}

// ── Rows ────────────────────────────────────────────────────────────────────

KeyRow lettersRow(int r, bool caps) {
    const char* const r0 = "qwertyuiop";
    const char* const r1 = "asdfghjkl";
    const char* const r2 = "zxcvbnm";

    KeyRow row;
    if (r == 0) {
        for (int i = 0; i < 10; ++i) {
            static std::string labels[10][2];
            labels[i][0] = static_cast<char>(std::toupper(r0[i]));
            labels[i][1] = r0[i];
            row.push_back(KeyDef{labels[i][caps ? 0 : 1].c_str(),
                                 labels[i][caps ? 0 : 1].c_str(), A_Ins, 1.0f});
        }
    } else if (r == 1) {
        for (int i = 0; i < 9; ++i) {
            static std::string labels[9][2];
            labels[i][0] = static_cast<char>(std::toupper(r1[i]));
            labels[i][1] = r1[i];
            row.push_back(KeyDef{labels[i][caps ? 0 : 1].c_str(),
                                 labels[i][caps ? 0 : 1].c_str(), A_Ins, 1.0f});
        }
    } else if (r == 2) {
        row.push_back(KeyDef{caps ? "小写" : "大写", nullptr, A_Caps, 1.6f});
        for (int i = 0; i < 7; ++i) {
            static std::string labels[7][2];
            labels[i][0] = static_cast<char>(std::toupper(r2[i]));
            labels[i][1] = r2[i];
            row.push_back(KeyDef{labels[i][caps ? 0 : 1].c_str(),
                                 labels[i][caps ? 0 : 1].c_str(), A_Ins, 1.0f});
        }
        row.push_back(KeyDef{"删除", nullptr, A_Back, 1.6f});
    } else {
        row.push_back(KeyDef{"123", nullptr, A_Layer, 1.5f});
        row.push_back(KeyDef{g.cn ? "中" : "英", nullptr, A_Mode, 1.5f});
        row.push_back(KeyDef{"←", nullptr, A_Left, 1.0f});
        row.push_back(KeyDef{"空格", nullptr, A_Space, 4.0f});
        row.push_back(KeyDef{"→", nullptr, A_Right, 1.0f});
        row.push_back(KeyDef{"完成", nullptr, A_Done, 2.0f});
    }
    return row;
}

KeyRow numbersRow(int r) {
    static const char* const r0 = "1234567890";
    static const char* const r1[] = {"-", "_", "=", "+", "/", "\\", ":", ";", "(", ")"};
    static const char* const r2[] = {".", ",", "?", "!", "'", "\"", "@", "#", "*"};

    KeyRow row;
    if (r == 0) {
        for (int i = 0; i < 10; ++i) {
            static std::string one[10];
            one[i] = r0[i];
            row.push_back(KeyDef{one[i].c_str(), one[i].c_str(), A_Ins, 1.0f});
        }
    } else if (r == 1) {
        for (int i = 0; i < 10; ++i) row.push_back(KeyDef{r1[i], r1[i], A_Ins, 1.0f});
    } else if (r == 2) {
        for (int i = 0; i < 9; ++i) row.push_back(KeyDef{r2[i], r2[i], A_Ins, 1.0f});
        row.push_back(KeyDef{"删除", nullptr, A_Back, 1.6f});
    } else {
        row.push_back(KeyDef{"字母", nullptr, A_Layer, 1.5f});
        row.push_back(KeyDef{g.cn ? "中" : "英", nullptr, A_Mode, 1.5f});
        row.push_back(KeyDef{"←", nullptr, A_Left, 1.0f});
        row.push_back(KeyDef{"空格", nullptr, A_Space, 4.0f});
        row.push_back(KeyDef{"→", nullptr, A_Right, 1.0f});
        row.push_back(KeyDef{"完成", nullptr, A_Done, 2.0f});
    }
    return row;
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────────

float height(float s) {
    // The strip is reserved whenever the pinyin mode is on — including with an
    // empty composition — so the panel's height never changes under the keys
    // while the user is typing. (A panel that grew by 60 px the moment the
    // first letter was pressed would move every key out from under the finger.)
    float h = kPad * 2.0f + kRowGap * 3.0f + kKeyH * 4.0f;
    if (g.cn) h += kCandH + kRowGap;
    return h * s;
}

widgets::Rect panelRect(const HudRect& board, float s) {
    const float h = height(s);
    const float m = kPad * 3.0f * s;
    widgets::Rect r;
    r.x = board.x + m;
    r.y = board.y + board.h - h - m;
    r.w = board.w - m * 2.0f;
    r.h = h;
    return r;
}

void open(widgets::DialogField* field) {
    g.f = field;
    if (field != nullptr) {
        field->focused = true;
        field->caret = field->len;
    }
    g.open = true;
    rebuildCandidates();
}

void close() {
    if (g.f != nullptr) g.f->focused = false;
    g.f = nullptr;
    g.open = false;
    g.anim = 0.0f;
    g.pyLen = 0;
    g.py[0] = '\0';
    g.cands.clear();
    g.candPtr.clear();
    if (g_engineReady) im_reset_search();
}

bool isOpen() { return g.open; }
widgets::DialogField* field() { return g.f; }
const char* composition() { return g.cn ? g.py : ""; }

bool hitPanel(const ImVec2& p) {
    if (!g.panelValid) return false;
    return p.x >= g.panel.x && p.x < g.panel.x + g.panel.w &&
           p.y >= g.panel.y && p.y < g.panel.y + g.panel.h;
}

bool draw(ImDrawList* dl, const widgets::Rect& r, float s, const Xf& xf) {
    // The keyboard owns the pointer for as long as it is up. Set every frame so
    // it survives a page switch and is never left latched on.
    widgets::setModalOwner(g.open);

    const ImGuiIO& io = ImGui::GetIO();
    bool changed = false;

    // Ease the open/close so the panel slides rather than blinks.
    {
        float dt = io.DeltaTime;
        if (!(dt > 0.0f)) dt = 1.0f / 60.0f;
        const float target = g.open ? 1.0f : 0.0f;
        const float d = target - g.anim;
        if (std::fabs(d) > 0.002f) g.anim += d * (1.0f - std::exp(-16.0f * dt));
        else g.anim = target;
    }
    if (g.anim <= 0.002f) { g.panelValid = false; return false; }

    ImFont* font = ImGui::GetFont();
    const float pad    = kPad * s;
    const float gap    = kGap * s;
    const float keyH   = kKeyH * s;
    const float rowGap = kRowGap * s;
    const float candH  = kCandH * s;

    // Slide up from below by (1 - anim) of the panel's own height.
    const float slide = (1.0f - g.anim) * r.h;
    const widgets::Rect p{r.x, r.y + slide, r.w, r.h};
    g.panel = p;
    g.panelValid = true;

    const ImVec2 pmin = xf.pt(ImVec2(p.x, p.y));
    const ImVec2 pmax = xf.pt(ImVec2(p.x + p.w, p.y + p.h));

    // Soft drop shadow, so the panel reads as floating over the page rather
    // than as another row of it.
    dl->AddRectFilled(ImVec2(pmin.x, pmin.y + xf.s(6.0f * s)),
                      ImVec2(pmax.x, pmax.y + xf.s(6.0f * s)),
                      xf.col(IM_COL32(0, 0, 0, 110)), xf.s(20.0f * s));
    dl->AddRectFilled(pmin, pmax, xf.col(ListBg), xf.s(20.0f * s));
    dl->AddRect(pmin, pmax, xf.col(Edge), xf.s(20.0f * s), 0, xf.s(1.0f * s));

    const bool pressed = io.MouseDown[ImGuiMouseButton_Left];
    const bool fresh   = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const float innerX = p.x + pad;
    const float innerW = p.w - pad * 2.0f;

    float y = p.y + pad;

    // ── Candidate strip ─────────────────────────────────────────────────────
    const bool showCand = g.cn;
    g.candHit = -1;
    if (showCand) {
        const widgets::Rect bar{innerX, y, innerW, candH};
        dl->AddRectFilled(xf.pt(ImVec2(bar.x, bar.y)),
                          xf.pt(ImVec2(bar.x + bar.w, bar.y + bar.h)),
                          xf.col(ControlBg), xf.s(kRoundKey * s));

        float cx = bar.x + 14.0f * s;
        const float cy = bar.y + bar.h * 0.5f;

        if (g.pyLen > 0) {
            char comp[32];
            std::snprintf(comp, sizeof(comp), "[%s]", g.py);
            const float cs = tsize(kCandSize * 0.85f * s) * xf.scale;
            dl->AddText(font, cs, xf.pt(ImVec2(cx, cy - cs * 0.6f)),
                        xf.col(TextMuted), comp);
            cx += font->CalcTextSizeA(cs, FLT_MAX, 0.0f, comp).x + 16.0f * s;
        }

        const float cs = tsize(kCandSize * s) * xf.scale;
        for (int i = 0; i < static_cast<int>(g.candPtr.size()); ++i) {
            const char* label = g.candPtr[static_cast<size_t>(i)];
            const float tw = font->CalcTextSizeA(cs, FLT_MAX, 0.0f, label).x;
            const float chipW = tw + 26.0f * s;
            if (cx + chipW > bar.x + bar.w - 8.0f * s) break;

            const widgets::Rect chip{cx, bar.y + 6.0f * s, chipW, bar.h - 12.0f * s};
            const bool isRaw = (i == static_cast<int>(g.candPtr.size()) - 1);
            // The first candidate is the one 空格 will take, so it wears the
            // accent. The raw-pinyin chip at the end is deliberately muted: it
            // is there to be reachable, not to be the default.
            const ImU32 bg = isRaw ? Track : (i == 0 ? Accent : ControlBgHi);
            dl->AddRectFilled(xf.pt(ImVec2(chip.x, chip.y)),
                              xf.pt(ImVec2(chip.x + chip.w, chip.y + chip.h)),
                              xf.col(bg), xf.s(kRoundKey * s));

            textCentred(dl, xf.pt(ImVec2(chip.x, chip.y)),
                        xf.pt(ImVec2(chip.x + chip.w, chip.y + chip.h)),
                        xf.s(kCandSize * s),
                        xf.col(isRaw ? TextMuted : TextPrimary), label);

            if (fresh &&
                io.MousePos.x >= chip.x && io.MousePos.x < chip.x + chip.w &&
                io.MousePos.y >= chip.y && io.MousePos.y < chip.y + chip.h) {
                g.candHit = i;
            }
            cx += chipW + gap;
        }
        y += candH + rowGap;
    }

    // ── Keys ────────────────────────────────────────────────────────────────
    // Two passes: work out every rect and consume the tap, then paint. Painting
    // last means a key pressed this frame is already drawn in its pressed
    // state, which is the difference between a keyboard that feels connected
    // and one that lags a frame.
    struct Placed {
        widgets::Rect r;
        KeyDef        k;    ///< BY VALUE: `defs` is a local that dies each row
        int           idx;
    };
    std::vector<Placed> placed;
    placed.reserve(kMaxKeys);

    int keyIdx = 0;
    int fired = -1;
    // BY VALUE, like Placed::k above: `defs` is re-created (and destroyed) on
    // every row iteration, so a pointer into it dangles by the time the Act
    // pass below runs — the finger taps "j" and the freed memory answers "n".
    KeyDef firedKey{};
    bool hasFired = false;

    for (int row = 0; row < 4; ++row) {
        KeyRow defs = (g.layer == 0) ? lettersRow(row, g.caps) : numbersRow(row);

        float total = 0.0f;
        for (const KeyDef& k : defs) total += k.weight;
        const float unit = (innerW - gap * static_cast<float>(defs.size() - 1)) / total;

        float x = innerX;
        for (const KeyDef& k : defs) {
            const float w = unit * k.weight;
            const widgets::Rect kr{x, y, w, keyH};
            placed.push_back(Placed{kr, k, keyIdx});

            if (fresh && io.MousePos.x >= kr.x && io.MousePos.x < kr.x + kr.w &&
                io.MousePos.y >= kr.y && io.MousePos.y < kr.y + kr.h) {
                fired = keyIdx;
                firedKey = k;
                hasFired = true;
                g.key[keyIdx] = 1.0f;
            }
            x += w + gap;
            ++keyIdx;
            if (keyIdx >= kMaxKeys) break;
        }
        y += keyH + rowGap;
        if (keyIdx >= kMaxKeys) break;
    }

    // A key already held stays lit until the finger lifts.
    if (!pressed) {
        for (int i = 0; i < kMaxKeys; ++i) g.key[i] = 0.0f;
        g.backHeld = 0.0f;
    } else if (hasFired && firedKey.action == A_Back) {
        g.backHeld += io.DeltaTime;
        g.backAcc += io.DeltaTime;
    } else {
        g.backHeld = 0.0f;
    }

    // ── Act ─────────────────────────────────────────────────────────────────
    if (g.candHit >= 0) {
        commitCandidate(g.candHit);
        changed = true;
    } else if (hasFired) {
        switch (firedKey.action) {
            case A_Ins:
                if (g.layer == 0) typeLetter(firedKey.insert);
                else { insertChar(firedKey.insert); changed = true; }
                if (g.layer == 0 && !g.cn) changed = true;
                break;
            case A_Back:
                if (g.cn && g.pyLen > 0) {
                    const int n = prevCharLen(g.py, g.pyLen);
                    g.pyLen -= n;
                    g.py[g.pyLen] = '\0';
                    rebuildCandidates();
                } else {
                    backspace();
                    changed = true;
                }
                g.backAcc = 0.0f;
                break;
            case A_Space:
                if (g.cn && !g.cands.empty()) {
                    // Space takes the first candidate — the convention every
                    // Chinese IME on the planet uses, and the reason the first
                    // chip is the one wearing the accent.
                    commitCandidate(0);
                    changed = true;
                } else if (!g.cn) {
                    insertChar(" ");
                    changed = true;
                }
                break;
            case A_Caps:
                g.caps = !g.caps;
                break;
            case A_Layer:
                g.layer = (g.layer == 0) ? 1 : 0;
                break;
            case A_Mode:
                // Leaving pinyin mode commits whatever was composed: dropping a
                // half-typed syllable on the floor is how a keyboard loses text
                // the user can see on screen.
                if (g.cn && g.pyLen > 0) {
                    commitCandidate(static_cast<int>(g.cands.size()) - 1);
                    changed = true;
                }
                g.cn = !g.cn;
                g.pyLen = 0;
                g.py[0] = '\0';
                rebuildCandidates();
                break;
            case A_Left:
                if (g.f != nullptr && g.f->caret > 0)
                    g.f->caret -= prevCharLen(g.f->buf, g.f->caret);
                break;
            case A_Right:
                if (g.f != nullptr && g.f->caret < g.f->len)
                    g.f->caret += nextCharLen(g.f->buf, g.f->len, g.f->caret);
                break;
            case A_Done:
                close();
                break;
            default:
                break;
        }
    }

    // Held delete: one immediate character, then a repeat after the delay.
    if (pressed && hasFired && firedKey.action == A_Back &&
        g.backHeld > kRepeatDelaySec && g.backAcc > kRepeatRateSec) {
        g.backAcc = 0.0f;
        if (g.cn && g.pyLen > 0) {
            const int n = prevCharLen(g.py, g.pyLen);
            g.pyLen -= n;
            g.py[g.pyLen] = '\0';
            rebuildCandidates();
        } else {
            backspace();
            changed = true;
        }
    }

    // ── Paint keys ──────────────────────────────────────────────────────────
    for (const Placed& q : placed) {
        const float k = g.key[q.idx];
        const bool accent = (q.k.action == A_Done) ||
                            (q.k.action == A_Mode && g.cn) ||
                            (q.k.action == A_Caps && g.caps);
        const bool special = (q.k.action == A_Back || q.k.action == A_Caps ||
                              q.k.action == A_Layer || q.k.action == A_Left ||
                              q.k.action == A_Right);

        const float top = q.r.y + kKeyTravel * s * k;
        const widgets::Rect kd{q.r.x, top, q.r.w, q.r.h};

        // Shadow: full depth when released, gone when pressed.
        const float sh = kKeyTravel * s * (1.0f - k);
        if (sh > 0.4f) {
            dl->AddRectFilled(xf.pt(ImVec2(q.r.x, q.r.y + kKeyTravel * s)),
                              xf.pt(ImVec2(q.r.x + q.r.w, q.r.y + keyH + kKeyTravel * s)),
                              xf.col(IM_COL32(0, 0, 0, 70)), xf.s(kRoundKey * s));
        }
        const ImU32 base = accent ? Accent : (special ? Track : ControlBg);
        dl->AddRectFilled(xf.pt(ImVec2(kd.x, kd.y)),
                          xf.pt(ImVec2(kd.x + kd.w, kd.y + kd.h)),
                          xf.col(lerpColor(base, Accent, accent ? k * 0.3f : k * 0.55f)),
                          xf.s(kRoundKey * s));
        dl->AddRect(xf.pt(ImVec2(kd.x, kd.y)),
                    xf.pt(ImVec2(kd.x + kd.w, kd.y + kd.h)),
                    xf.col(Edge), xf.s(kRoundKey * s), 0, xf.s(1.0f * s));

        textCentred(dl, xf.pt(ImVec2(kd.x, kd.y)),
                    xf.pt(ImVec2(kd.x + kd.w, kd.y + kd.h)),
                    xf.s(kTextSize * s),
                    xf.col(accent ? TextOnAccent : TextPrimary), q.k.label);
    }

    return changed;
}

}  // namespace ime
}  // namespace ui
}  // namespace aimbotng

#endif  // HAS_IMGUI
