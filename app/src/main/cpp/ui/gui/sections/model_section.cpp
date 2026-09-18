// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui::sections::model — implementation. See model_section.h.
// ─────────────────────────────────────────────────────────────────────────────
#include "model_section.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "inference/model_runtime.h"
#include "inference/model_store.h"
#include "inference/model_type.h"
#include "ui/gui/hud.h"
#include "ui/gui/notify.h"
#include "ui/gui/sections/settings_section.h"
#include "ui/gui/theme.h"

#define TAG "AimbotInfer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

namespace aimbotng {
namespace ui {
namespace sections {

PageModel g_pageModel;

namespace {
using namespace theme;

constexpr float kModelRowH     = 96.0f;
constexpr float kModelRowGap   = 12.0f;
constexpr float kModelAddBtnH  = 76.0f;

// ── File browser ─────────────────────────────────────────────────────────────
//
// The system file picker (ACTION_OPEN_DOCUMENT) was replaced by this: a modal
// card painted with the same ImDrawList primitives as the rest of the board.
// It lists the directory under `dir` (default /sdcard), a tap navigates into
// folders, and only .onnx / .tflite files may be picked — every other file is
// shown greyed and ignores taps. The daemon runs under shell UID 2000, the
// same user `adb shell` runs as, so opendir/readdir over /sdcard work directly.

struct FsEntry {
    std::string name;      // leaf name
    std::string path;      // absolute path
    bool        isDir   = false;
    bool        pickable = false;   // .onnx or .tflite
};

struct FileBrowser {
    bool        open = false;
    std::string dir  = "/sdcard";
    std::vector<FsEntry> entries;
    bool        loaded = false;
    std::string error;

    // List scrolling (px). The list is drag-scrolled: touch down inside it,
    // move to scroll, release without moving to tap the row under the finger.
    float scroll        = 0.0f;
    bool  dragScroll    = false;
    float dragStartY    = 0.0f;
    float dragStartScroll = 0.0f;
    bool  moved         = false;
};

FileBrowser g_fileBrowser;

bool hitRect(const ImVec2& p, const widgets::Rect& r) {
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

/** Cuts `text` back until it fits `maxW` px at `size`, ending in "..".
 *  Model paths are long and the row's buttons are not going to move, so
 *  something has to give — better the tail of a file name than a name
 *  painted halfway underneath a Delete button. */
std::string ellipsize(ImFont* font, float size, const std::string& text, float maxW) {
    if (maxW <= 0.0f) return "";
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= maxW) return text;
    std::string out = text;
    while (out.size() > 1) {
        out.pop_back();
        const std::string probe = out + "..";
        if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, probe.c_str()).x <= maxW) {
            return probe;
        }
    }
    return "";
}

bool rectTapped(const widgets::Rect& r) {
    const ImGuiIO& io = ImGui::GetIO();
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hitRect(io.MousePos, r);
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == "/") return "/" + name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string parentOf(const std::string& dir) {
    if (dir.empty() || dir == "/") return "/";
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    const size_t slash = d.find_last_of('/');
    if (slash == std::string::npos) return "/";
    if (slash == 0) return "/";
    return d.substr(0, slash);
}

bool isPickableName(const std::string& name) {
    const model::Kind k = model::kindOfPath(name);
    return k == model::Kind::Onnx || k == model::Kind::Tflite;
}

/** Which engines actually honour the "CPU Threads" slider — i.e. the ones
 *  that run on CPU cores and take a thread count. QNN HTP, NNAPI and GPU do
 *  not, so the slider is hidden for them (it used to show for every engine,
 *  which was misleading on the HTP path). */
bool engineUsesCpuThreads(model::Engine e) {
    return e == model::Engine::OnnxRuntime ||
           e == model::Engine::OnnxCpu ||
           e == model::Engine::Cpu ||
           e == model::Engine::TfliteXnnpack;
}

/** Whether an engine row in the 推理引擎 dropdown should be dimmed and
 *  un-pickable.
 *
 *  Only the backends whose library we dlopen at menu time can answer "no" —
 *  NeuroPilot (libtflite_mtk.mtk.so, from the system image) and Neuron (the
 *  APU adapter, libneuronusdk_adapter.mtk.so, also from the system image).
 *  Both are simply absent on a Snapdragon, and both would otherwise let the
 *  user pick a row that fails only once a model is loaded. Everything else
 *  either links into this binary or is loaded when the model loads, and dimming
 *  those here would hide a real error message behind a grey row.
 *
 *  The check costs one dlopen the first time the Model page draws and nothing
 *  afterwards (epAvailable() caches), so it is safe to call per row per frame.
 */
bool engineRowDisabled(model::Engine e) {
    // "No engine" is not a choice. engineAt() answers Engine::None for an
    // out-of-range index, and the row that maps to None has nothing behind it
    // — resolvePair() would fail with "this model has no runtime selected" —
    // so it must not be selectable. Without this it fell through to the
    // `return false` below and looked like a perfectly good option.
    bool disabled = false;
    if (e == model::Engine::None) {
        disabled = true;
    } else if (e == model::Engine::Neuron) {
        disabled = !infer::runtime::engineAvailable(e);
    } else if (e == model::Engine::NeuroPilot) {
        disabled = !infer::runtime::engineAvailable(e);
    }
    LOGD("engineRowDisabled: engine=%d (%s) → disabled=%d",
         static_cast<int>(e), model::engineLabel(e), disabled ? 1 : 0);
    return disabled;
}

/** Fills the parallel `disabled` array widgets::dropdown() takes. `out` must
 *  hold `count` entries; anything past the engine count is left untouched. */
void fillEngineDisabled(model::Kind kind, bool* out, int count) {
    for (int i = 0; i < count; ++i) {
        out[i] = engineRowDisabled(model::engineAt(kind, i));
    }
}

// ── HTP performance-mode picker ────────────────────────────────────────────────
//
// The dropdown index is the raw TfLiteQnnDelegateHtpPerformanceMode value, so
// the picker's selection maps 1:1 onto what the engine writes into
// htp_options.performance_mode. Order and values MUST stay in lock-step with
// that enum (see QnnTFLiteDelegate.h). The list is returned by pointer so it
// can be handed straight to widgets::dropdown().
const char* const* htpPerfLabels(int& count) {
    static const char* labels[] = {
        "默认 Default",                                  // 0 kHtpDefault
        "持续高性能 Sustained High Performance",          // 1 kHtpSustainedHighPerformance
        "突发 Burst",                                     // 2 kHtpBurst
        "高性能 High Performance",                        // 3 kHtpHighPerformance
        "省电 Power Saver",                               // 4 kHtpPowerSaver
        "低省电 Low Power Saver",                         // 5 kHtpLowPowerSaver
        "高省电 High Power Saver",                        // 6 kHtpHighPowerSaver
        "低均衡 Low Balanced",                            // 7 kHtpLowBalanced
        "均衡 Balanced",                                  // 8 kHtpBalanced
        "极致省电 Extreme Power Saver",                   // 9 kHtpExtremePowerSaver
    };
    static_assert(sizeof(labels) / sizeof(labels[0]) == 10,
                  "HTP perf label count must match the enum");
    count = static_cast<int>(sizeof(labels) / sizeof(labels[0]));
    return labels;
}

/// Clamps a stored perf-mode int to the valid [0,9] range.
int clampHtpPerf(int v) {
    if (v < 0) return 0;
    if (v > 9) return 9;
    return v;
}

void loadDir(FileBrowser& fb, const std::string& dir) {
    fb.dir = dir;
    fb.entries.clear();
    fb.loaded = true;
    fb.scroll = 0.0f;
    fb.error.clear();

    DIR* d = opendir(dir.c_str());
    if (d == nullptr) {
        fb.error = "cannot open directory";
        return;
    }

    std::vector<FsEntry> dirs;
    std::vector<FsEntry> files;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        const char* nm = ent->d_name;
        if (nm == nullptr || nm[0] == '\0') continue;
        std::string name = nm;
        if (name == "." || name == "..") continue;

        const std::string full = joinPath(dir, name);
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;   // inaccessible: skip

        FsEntry e;
        e.name = name;
        e.path = full;
        if (S_ISDIR(st.st_mode)) {
            e.isDir = true;
            dirs.push_back(std::move(e));
        } else if (S_ISREG(st.st_mode)) {
            e.isDir = false;
            e.pickable = isPickableName(name);
            files.push_back(std::move(e));
        }
    }
    closedir(d);

    std::sort(dirs.begin(), dirs.end(), [](const FsEntry& a, const FsEntry& b) {
        return a.name < b.name;
    });
    // Pickable files first, so the models float to the top of a long listing.
    std::sort(files.begin(), files.end(), [](const FsEntry& a, const FsEntry& b) {
        if (a.pickable != b.pickable) return a.pickable;
        return a.name < b.name;
    });

    fb.entries.reserve(dirs.size() + files.size());
    for (FsEntry& e : dirs) fb.entries.push_back(std::move(e));
    for (FsEntry& e : files) fb.entries.push_back(std::move(e));
}

void openFileBrowser() {
    g_fileBrowser.open = true;
    g_fileBrowser.dragScroll = false;
    g_fileBrowser.moved = false;
    loadDir(g_fileBrowser, "/sdcard");
    if (!g_fileBrowser.error.empty()) {
        // /sdcard is normally a symlink to this; fall back if it is absent on
        // a particular build.
        loadDir(g_fileBrowser, "/storage/emulated/0");
    }
}

void selectFile(const std::string& path) {
    PageModel& pg = g_pageModel;
    const size_t n = std::min(path.size(), sizeof(pg.pathBuf) - 1);
    memcpy(pg.pathBuf, path.data(), n);
    pg.pathBuf[n] = '\0';
    pg.pathLen = static_cast<int>(n);
    // -1, not 0. The old engine index may well not fit the new file's kind —
    // that is what this line is for — but 0 is a *valid* index and it maps to
    // "NPU (LiteRT)" in the tflite list, so resetting to 0 does not mean
    // "nothing chosen", it means "LiteRtNpu chosen". The Add button already
    // gates on `pg.engine.value >= 0` and the dropdown already treats -1 as
    // "no selection" (see the Unknown-kind branch below), so -1 is the value
    // this screen was written against. Resetting to 0 silently enrolled every
    // newly added .tflite on the LiteRT 2.x NPU path, which is the one that
    // traps in __loader_android_link_namespaces and takes the process down.
    pg.engine.value = -1;
    pg.pathPicking = false;
    pg.pathFailed = false;
    pg.pathErr[0] = '\0';
    g_fileBrowser.open = false;
    LOGI("AddDialog: file selected path='%s'", pg.pathBuf);
    g_fileBrowser.dragScroll = false;
    // Peek the model's tensor element type so the XNNPACK caution (and the list
    // label) can react to FP16 without loading the whole graph yet.
    pg.pathType = aimbotng::infer::detectModelType(std::string(pg.pathBuf, pg.pathLen));
}

struct QuickFolder { const char* label; const char* path; };
const QuickFolder kQuickFolders[] = {
    {"sdcard",    "/sdcard"},
    {"Download",  "/sdcard/Download"},
    {"Documents", "/sdcard/Documents"},
};

// ── File browser dialog card ─────────────────────────────────────────────────
void drawFileBrowser(ImDrawList* dl, const HudRect& board, float s, float es, const Xf& xf) {
    FileBrowser& fb = g_fileBrowser;
    const ImGuiIO& io = ImGui::GetIO();
    ImFont* font = ImGui::GetFont();

    // Scrim over the whole board.
    dl->AddRectFilled(xf.pt(ImVec2(board.x, board.y)),
                      xf.pt(ImVec2(board.x + board.w, board.y + board.h)),
                      xf.col(IM_COL32(0, 0, 0, 200)));

    widgets::Rect card = widgets::dialogRect(board, es);

    // Outside-tap closes the browser (and consumes the tap, so the release
    // never reaches the Add-Model dialog or the rows underneath).
    if (widgets::clickedOutside(card)) {
        widgets::consumeTap();
        fb.open = false;
        fb.dragScroll = false;
        return;
    }

    const ImVec2 cardMin(card.x, card.y);
    const ImVec2 cardMax(card.x + card.w, card.y + card.h);
    dl->AddRectFilled(cardMin, cardMax, xf.col(PaneRight), xf.s(20.0f * s));
    dl->AddRect(cardMin, cardMax, xf.col(Edge), xf.s(20.0f * s), 0, xf.s(1.0f * s));

    // Header: title + X close.
    const float titleH = widgets::dialogHeaderHeight(es);
    const float padX = widgets::dialogInnerPad(es);
    dl->AddText(font, 36.0f * s,
                ImVec2(card.x + padX, card.y + (titleH - 36.0f * s) * 0.5f),
                xf.col(TextPrimary), "Select Model");
    const float xBtn = 50.0f * s;
    widgets::Rect xRect{card.x + card.w - padX - xBtn,
                        card.y + (titleH - xBtn) * 0.5f, xBtn, xBtn};
    if (widgets::closeButton(dl, xRect, es)) {
        fb.open = false;
        fb.dragScroll = false;
        return;
    }

    float yc = card.y + titleH + 18.0f * s;

    // Current path.
    dl->AddText(font, 20.0f * s, ImVec2(card.x + padX, yc),
                xf.col(TextMuted), fb.dir.c_str());
    yc += 32.0f * s;

    // A row of quick folder shortcuts.
    const float chipH = 46.0f * s;
    const float chipGap = 10.0f * s;
    float cx = card.x + padX;
    for (const QuickFolder& q : kQuickFolders) {
        const ImVec2 ts = font->CalcTextSizeA(22.0f * s, FLT_MAX, 0.0f, q.label);
        const float chipW = ts.x + 40.0f * s;
        widgets::Rect chip{cx, yc, chipW, chipH};
        const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left) && hitRect(io.MousePos, chip);
        dl->AddRectFilled(ImVec2(chip.x, chip.y), ImVec2(chip.x + chip.w, chip.y + chip.h),
                          xf.col(held ? ControlBgHi : ControlBg), xf.s(10.0f * s));
        dl->AddText(font, 22.0f * s,
                    ImVec2(chip.x + (chip.w - ts.x) * 0.5f,
                           chip.y + (chip.h - ts.y) * 0.5f),
                    xf.col(TextPrimary), q.label);
        if (rectTapped(chip)) { loadDir(fb, q.path); return; }
        cx += chipW + chipGap;
    }
    yc += chipH + 16.0f * s;

    // ── Scrollable list ───────────────────────────────────────────────────────
    const float listTop = yc;
    const float listBottom = card.y + card.h - 16.0f * s;
    const float listH = listBottom - listTop;
    const float rowH = 46.0f * s;
    const widgets::Rect listRect{card.x + padX, listTop, card.w - 2.0f * padX, listH};

    const bool hasParent = (parentOf(fb.dir) != fb.dir);
    const int totalRows = (hasParent ? 1 : 0) + static_cast<int>(fb.entries.size());
    const float maxScroll = std::max(0.0f, totalRows * rowH - listH);

    // Drag to scroll; a tap (down + up with no movement) picks the row under it.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hitRect(io.MousePos, listRect)) {
        fb.dragScroll = true;
        fb.dragStartY = io.MousePos.y;
        fb.dragStartScroll = fb.scroll;
        fb.moved = false;
    }
    if (fb.dragScroll && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float dy = io.MousePos.y - fb.dragStartY;
        if (fabsf(dy) > 4.0f * s) fb.moved = true;
        fb.scroll = std::max(0.0f, std::min(maxScroll, fb.dragStartScroll - dy));
    }
    int tapIndex = -1;
    bool tappedRow = false;
    if (fb.dragScroll && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (!fb.moved) {
            const float relY = io.MousePos.y - listRect.y + fb.scroll;
            const int idx = static_cast<int>(floorf(relY / rowH));
            if (idx >= 0 && idx < totalRows) { tapIndex = idx; tappedRow = true; }
        }
        fb.dragScroll = false;
        fb.moved = false;
    }

    // Resolve what a tap means once, then apply after drawing.
    std::string navigateTo;   // non-empty -> navigate into this directory
    std::string select;       // non-empty -> select this file
    if (tappedRow) {
        int idx = tapIndex;
        if (hasParent) {
            if (idx == 0) { navigateTo = parentOf(fb.dir); idx = -1; }
            else idx -= 1;
        }
        if (idx >= 0 && idx < static_cast<int>(fb.entries.size())) {
            const FsEntry& e = fb.entries[idx];
            if (e.isDir) navigateTo = e.path;
            else if (e.pickable) select = e.path;
            // else: an unsupported file — tap does nothing.
        }
    }

    // ── Draw the rows, clipped to the list ───────────────────────────────────
    dl->PushClipRect(ImVec2(listRect.x, listRect.y),
                     ImVec2(listRect.x + listRect.w, listRect.y + listRect.h), true);

    const float rowSz = 24.0f * s;
    auto paintRow = [&](const char* label, ImU32 col, int index) {
        const float rowY = listRect.y - fb.scroll + index * rowH;
        const widgets::Rect row{listRect.x, rowY, listRect.w, rowH};
        const bool held = fb.dragScroll && !fb.moved &&
                          ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                          hitRect(io.MousePos, row);
        if (held) {
            dl->AddRectFilled(ImVec2(row.x, row.y),
                              ImVec2(row.x + row.w, row.y + row.h),
                              xf.col(ControlBgHi), xf.s(8.0f * s));
        }
        dl->AddText(font, rowSz,
                    ImVec2(row.x + 8.0f * s, row.y + (row.h - rowSz) * 0.5f),
                    xf.col(col), label);
    };

    int index = 0;
    if (hasParent) {
        paintRow("..", TextMuted, index);
        ++index;
    }
    for (const FsEntry& e : fb.entries) {
        ImU32 col;
        std::string label = e.name;
        if (e.isDir) {
            col = TextPrimary;
            label += "/";
        } else if (e.pickable) {
            col = TextPrimary;
        } else {
            col = IM_COL32(96, 100, 114, 255);   // greyed out, not selectable
        }
        paintRow(label.c_str(), col, index);
        ++index;
    }
    dl->PopClipRect();

    // A thin scrollbar so a long directory does not read as "it just ends".
    if (maxScroll > 0.0f) {
        const float trackX = listRect.x + listRect.w - 6.0f * s;
        const float trackH = listRect.h;
        const float thumbH = std::max(24.0f * s, trackH * (listH / (totalRows * rowH)));
        const float thumbY = listRect.y + (trackH - thumbH) * (fb.scroll / maxScroll);
        dl->AddRectFilled(ImVec2(trackX, listRect.y),
                          ImVec2(trackX + 4.0f * s, listRect.y + trackH),
                          xf.col(ControlBg), xf.s(2.0f * s));
        dl->AddRectFilled(ImVec2(trackX, thumbY),
                          ImVec2(trackX + 4.0f * s, thumbY + thumbH),
                          xf.col(TextMuted), xf.s(2.0f * s));
    }

    if (!fb.error.empty()) {
        dl->AddText(font, 20.0f * s, ImVec2(listRect.x, listRect.y + 8.0f * s),
                    xf.col(IM_COL32(255, 96, 96, 255)), fb.error.c_str());
    }

    // Apply the tap action last, after the current directory has been drawn.
    if (!navigateTo.empty()) {
        loadDir(fb, navigateTo);
    } else if (!select.empty()) {
        selectFile(select);
    }
}

// ── Add-Model dialog scrim ────────────────────────────────────────────────────
void drawDialogScrim(ImDrawList* dl, const HudRect& board, const Xf& xf) {
    // Full-board scrim, no alpha underneath. The scrim's own alpha is left
    // alone (it is already a translucent overlay), so it just rides on top
    // of whatever fade the board itself is doing.
    dl->AddRectFilled(xf.pt(ImVec2(board.x, board.y)),
                      xf.pt(ImVec2(board.x + board.w, board.y + board.h)),
                      xf.col(IM_COL32(0, 0, 0, 180)));
}

/**
 * A tap fully outside the dialog rect (press AND release outside) closes it
 * and consumes the gesture, so the release never reaches the rows underneath.
 * Press-only used to close, which handed the same gesture's release to
 * whatever row sat below the finger.
 */
bool closeOnOutsideTap(const widgets::Rect& card) {
    if (!widgets::clickedOutside(card)) return false;
    widgets::consumeTap();
    return true;
}

void drawAddModelDialog(ImDrawList* dl, const HudRect& board, float s, float es, const Xf& xf) {
    PageModel& pg = g_pageModel;
    widgets::Rect card = widgets::dialogRect(board, es);

    // Outside-tap closes. Page owns the dialog state, so the test runs here
    // once per frame regardless of which control inside the dialog fires.
    if (closeOnOutsideTap(card)) {
        pg.dialog = ModelDialogState::Closed;
        pg.pathPicking = false;
        return;
    }

    // Frame card.
    const ImVec2 cardMin(card.x, card.y);
    const ImVec2 cardMax(card.x + card.w, card.y + card.h);
    dl->AddRectFilled(cardMin, cardMax, xf.col(PaneRight), xf.s(20.0f * s));
    dl->AddRect(cardMin, cardMax, xf.col(Edge), xf.s(20.0f * s), 0, xf.s(1.0f * s));

    // Title strip + X close.
    const float titleH = widgets::dialogHeaderHeight(es);
    const float titlePadX = widgets::dialogInnerPad(es);
    ImFont* font = ImGui::GetFont();
    dl->AddText(font, 36.0f * s,
                ImVec2(card.x + titlePadX, card.y + (titleH - 36.0f * s) * 0.5f),
                xf.col(TextPrimary), "添加模型");
    const float xBtn = 50.0f * s;
    widgets::Rect xRect{card.x + card.w - titlePadX - xBtn,
                        card.y + (titleH - xBtn) * 0.5f, xBtn, xBtn};
    if (widgets::closeButton(dl, xRect, es)) {
        pg.dialog = ModelDialogState::Closed;
        pg.pathPicking = false;
        return;
    }

    // Body — stack: path picker, engine, confirm/cancel. The display name is
    // taken from the file's basename (see Add button below), so there is no
    // text field on this dialog — and therefore no route through the
    // `requestText` / system-IME relay that used to fire `AimModelTextActivity`
    // on every stale residual JSON.
    const float padX     = widgets::dialogInnerPad(es);
    const float padY     = 24.0f * s;
    float yc = card.y + titleH + padY;

    // 1. Path picker.
    if (widgets::pathPill(dl, {card.x + padX, yc,
                               card.w - 2.0f * padX, 78.0f * s},
                          pg.pathBuf, pg.pathLen, false, es)) {
        openFileBrowser();
        pg.pathFailed = false;
    }
    yc += 78.0f * s + padY;
    if (pg.pathFailed && pg.pathErr[0] != '\0') {
        dl->AddText(font, 18.0f * s, ImVec2(card.x + padX, yc),
                    xf.col(IM_COL32(255, 96, 96, 255)), pg.pathErr);
        yc += 28.0f * s;
    }

    // 2. Engine dropdown — gated when no path has been picked yet.
    const model::Kind kind = (pg.pathLen > 0)
                              ? model::kindOfPath(std::string(pg.pathBuf, pg.pathLen))
                              : model::Kind::Unknown;
    const char* label = (kind == model::Kind::Unknown)
                            ? "请先选择模型"
                            : "推理引擎";
    int  enginesCount = 0;
    const char* const* engines = model::enginesFor(kind, enginesCount);
    const char* engineDisplay[8] = {nullptr};
    if (kind == model::Kind::Unknown) {
        engineDisplay[0] = "（无）";
        engines = engineDisplay;
        enginesCount = 1;
        pg.engine.value = -1;
        pg.engine.open  = false;
    }
    bool engineDisabled[8] = {};
    fillEngineDisabled(kind, engineDisabled, enginesCount);
    widgets::Rect engineRect{card.x + padX, yc, card.w - 2.0f * padX, 58.0f * s};
    const bool engineChanged = widgets::dropdown(dl, engineRect, pg.engine, label,
                      kind == model::Kind::Unknown ? engineDisplay : engines,
                      enginesCount, es, false, engineDisabled);
    yc += 58.0f * s + padY;

    // 3. Confidence — the threshold detections are filtered by. Lives here as
    //    well as in the per-model dialog so a model is usable the moment it
    //    is added, without a second trip through Settings.
    widgets::Rect confRect{card.x + padX, yc, card.w - 2.0f * padX, 80.0f * s};
    widgets::sliderFloat(dl, confRect, pg.conf, "置信度", 2, es);
    yc += 80.0f * s + padY;

    // Which engine is currently selected drives which extra controls show.
    const model::Engine selEngine = (kind != model::Kind::Unknown && pg.engine.value >= 0)
                                     ? model::engineAt(kind, pg.engine.value)
                                     : model::Engine::None;

    // 4. "CPU Threads" — only for engines that actually run on CPU cores and
    //    take a thread count (ONNX XNNPACK / plain CPU, LiteRT CPU / XNNPACK).
    //    QNN HTP, NNAPI and GPU ignore it, so it is hidden there.
    if (engineUsesCpuThreads(selEngine)) {
        widgets::Rect threadsRect{card.x + padX, yc, card.w - 2.0f * padX, 80.0f * s};
        widgets::sliderFloat(dl, threadsRect, pg.threadsSlider, "CPU 线程数", 0, es);
        yc += 80.0f * s + padY;
    }

    // 4b. HTP performance-mode picker — for the QNN HTP engine, and for the
    //     LiteRT 2.x NPU engine because on a Snapdragon that one ends up on the
    //     same Hexagon and takes the same vote. On MediaTek it is accepted and
    //     ignored, which is harmless: the MediaTek path has its own options,
    //     set unconditionally by the engine.
    widgets::Rect htpRect{};
    int htpCount = 0;
    const char* const* htpLabels = nullptr;
    if (selEngine == model::Engine::QnnHtp || selEngine == model::Engine::LiteRtNpu) {
        htpLabels = htpPerfLabels(htpCount);
        htpRect = widgets::Rect{card.x + padX, yc, card.w - 2.0f * padX, 58.0f * s};
        widgets::dropdown(dl, htpRect, pg.htpPerf,
                          "HTP 调度 (性能档位)", htpLabels, htpCount, es, false);
        yc += 58.0f * s + padY;
    }

    // 5. Confirm / Cancel. These stay at their normal fixed position — the
    //    dropdown popover is painted LAST on the foreground draw list (see the
    //    dropdownList() calls below), so an open Inference-Engine / HTP list
    //    simply floats *over* this row instead of shoving it downward. The
    //    buttons never move; they are momentarily covered by the open popover,
    //    which is the expected behaviour for a floating menu.
    const float btnW = (card.w - 2.0f * padX - 12.0f * s) * 0.5f;
    const float btnH = 76.0f * s;
    widgets::Rect cancelRect{card.x + padX, yc, btnW, btnH};
    widgets::Rect confirmRect{cancelRect.x + btnW + 12.0f * s, yc, btnW, btnH};
    if (widgets::button(dl, cancelRect, "取消", widgets::ButtonVariant::Ghost, es)) {
        pg.dialog = ModelDialogState::Closed;
        pg.pathPicking = false;
        return;
    }
    const bool canConfirm = (pg.pathLen > 0 && pg.engine.value >= 0);
    if (widgets::button(dl, confirmRect, "添加",
                        canConfirm ? widgets::ButtonVariant::Primary
                                    : widgets::ButtonVariant::Disabled, es)) {
        model::Entry e;
        e.path.assign(pg.pathBuf, pg.pathLen);
        // Display name is just the file's basename — no manual rename.
        e.name       = model::baseName(e.path);
        e.kind       = kind;
        e.engine     = model::engineAt(kind, pg.engine.value);
        LOGI("AddDialog: CONFIRM path='%s', kind=%d, engine=%d (%s), "
             "dropdown index=%d",
             e.path.c_str(), static_cast<int>(e.kind),
             static_cast<int>(e.engine), model::engineLabel(e.engine),
             pg.engine.value);
        // Peek the model's real input edge and class count so the list can
        // show "N classes • WxH" and so preprocessing feeds the right size.
        // Falls back to 640 when the size is not statically known.
        int mw = 0, mh = 0, mcls = 0;
        if (aimbotng::infer::detectModelShape(e.path, mw, mh, mcls)) {
            e.inputSize = (mw > 0) ? mw : 640;
            e.classCount = mcls;
        } else {
            e.inputSize = 640;
        }
        e.confidence = pg.conf.value;
        e.typeText   = pg.pathType;   // detected at file pick time
        e.cpuThreads = static_cast<int>(pg.threadsSlider.value + 0.5f);
        e.htpPerfMode = clampHtpPerf(pg.htpPerf.value);  // HTP scheduler vote
        model::add(e);
        pg.dialog = ModelDialogState::Closed;
        pg.pathPicking = false;
    }

    // Popovers are painted LAST, on the foreground draw list, so an open
    // Inference-Engine or HTP list floats above every other control in the
    // dialog — the sliders and the Cancel/Add buttons — instead of being
    // painted over by them. (Painting them inline, before the buttons, is
    // exactly what made the list read as "under" the other controls.) The
    // Cancel/Add buttons keep their fixed position and are simply covered by
    // the floating popover while it is open — standard menu behaviour.
    widgets::dropdownList(dl, engineRect, pg.engine, engines, enginesCount, es,
                          engineDisabled);
    if (selEngine == model::Engine::QnnHtp && htpRect.w > 0.0f) {
        widgets::dropdownList(dl, htpRect, pg.htpPerf, htpLabels, htpCount, es);
    }
}

// ── Per-model Settings dialog ────────────────────────────────────────────────
//
// Opened by the "Setting" button on a list row. It edits the two things that
// are a property of *how a model is run* rather than of the file: which
// runtime executes it, and the confidence its detections must clear. Both are
// copied out of the entry on open and written back on Save — Cancel leaves the
// store alone, so abandoning an edit is free.
void drawModelSettingsDialog(ImDrawList* dl, const HudRect& board, float s, float es,
                            const Xf& xf) {
    PageModel& pg = g_pageModel;
    widgets::Rect card = widgets::dialogRect(board, es);

    if (closeOnOutsideTap(card)) {
        pg.dialog = ModelDialogState::Closed;
        return;
    }

    const ImVec2 cardMin(card.x, card.y);
    const ImVec2 cardMax(card.x + card.w, card.y + card.h);
    dl->AddRectFilled(cardMin, cardMax, xf.col(PaneRight), xf.s(20.0f * s));
    dl->AddRect(cardMin, cardMax, xf.col(Edge), xf.s(20.0f * s), 0, xf.s(1.0f * s));

    const float titleH = widgets::dialogHeaderHeight(es);
    const float padX = widgets::dialogInnerPad(es);
    ImFont* font = ImGui::GetFont();
    dl->AddText(font, 36.0f * s,
                ImVec2(card.x + padX, card.y + (titleH - 36.0f * s) * 0.5f),
                xf.col(TextPrimary), "模型设置");
    const float xBtn = 50.0f * s;
    widgets::Rect xRect{card.x + card.w - padX - xBtn,
                        card.y + (titleH - xBtn) * 0.5f, xBtn, xBtn};
    if (widgets::closeButton(dl, xRect, es)) {
        pg.dialog = ModelDialogState::Closed;
        return;
    }

    // Which entry this dialog is editing. It can vanish between frames (the
    // App side can remove a model while the menu is open), so look it up
    // every frame instead of caching a copy.
    model::Entry current;
    bool found = false;
    for (const model::Entry& e : model::all()) {
        if (e.id == pg.settingsId) { current = e; found = true; break; }
    }
    if (!found) {
        dl->AddText(font, 22.0f * s,
                    ImVec2(card.x + padX, card.y + titleH + 30.0f * s),
                    xf.col(TextMuted), "该模型已不存在");
        if (widgets::button(dl, {card.x + padX, card.y + titleH + 80.0f * s,
                                 card.w - 2.0f * padX, 76.0f * s},
                            "关闭", widgets::ButtonVariant::Ghost, es)) {
            pg.dialog = ModelDialogState::Closed;
        }
        return;
    }

    LOGD("SettingsDialog: editing model #%d '%s' (current engine=%d %s, "
         "stored index=%d)",
         current.id, current.name.c_str(),
         static_cast<int>(current.engine), model::engineLabel(current.engine),
         pg.settingsEngine.value);

    const float padY = 24.0f * s;
    float yc = card.y + titleH + padY;

    // The file, for orientation: two dialogs deep, "which model is this?"
    // is a fair question, and the path is the only thing that answers it.
    dl->AddText(font, 18.0f * s, ImVec2(card.x + padX, yc),
                xf.col(TextMuted), current.path.c_str());
    yc += 32.0f * s + padY;

    // 1. Inference engine (推理方式) — only the runtimes this file's kind can
    //    actually use, same list the Add dialog offers.
    int enginesCount = 0;
    const char* const* engines = model::enginesFor(current.kind, enginesCount);
    const char* engineDisplay[8] = {nullptr};
    if (engines == nullptr || enginesCount <= 0) {
        engineDisplay[0] = "（无）";
        engines = engineDisplay;
        enginesCount = 1;
    }
    bool settingsEngineDisabled[8] = {};
    fillEngineDisabled(current.kind, settingsEngineDisabled, enginesCount);
    widgets::Rect engineRect{card.x + padX, yc, card.w - 2.0f * padX, 58.0f * s};
    const bool settingsEngineChanged = widgets::dropdown(dl, engineRect, pg.settingsEngine,
                                                      "推理引擎", engines, enginesCount, es, false,
                                                      settingsEngineDisabled);
    yc += 58.0f * s + padY;

    // 2. Confidence.
    widgets::Rect confRect{card.x + padX, yc, card.w - 2.0f * padX, 80.0f * s};
    widgets::sliderFloat(dl, confRect, pg.settingsConf, "置信度", 2, es);
    yc += 80.0f * s + padY;

    // Which engine is currently selected drives which extra controls show.
    const model::Engine selEngine = (pg.settingsEngine.value >= 0)
                                     ? model::engineAt(current.kind, pg.settingsEngine.value)
                                     : model::Engine::None;

    // 3. "CPU Threads" — only for engines that actually run on CPU cores and
    //    take a thread count. QNN HTP, NNAPI and GPU ignore it, so it is
    //    hidden there (the HTP path gets the scheduler dropdown instead).
    if (engineUsesCpuThreads(selEngine)) {
        widgets::Rect threadsRect{card.x + padX, yc, card.w - 2.0f * padX, 80.0f * s};
        widgets::sliderFloat(dl, threadsRect, pg.settingsThreadsSlider, "CPU 线程数", 0, es);
        yc += 80.0f * s + padY;
    }

    // 3b. HTP performance-mode picker — QNN HTP, and LiteRT 2.x NPU on a
    //     Snapdragon (same Hexagon, same vote).
    widgets::Rect htpRect{};
    int htpCount = 0;
    const char* const* htpLabels = nullptr;
    if (selEngine == model::Engine::QnnHtp || selEngine == model::Engine::LiteRtNpu) {
        htpLabels = htpPerfLabels(htpCount);
        htpRect = widgets::Rect{card.x + padX, yc, card.w - 2.0f * padX, 58.0f * s};
        widgets::dropdown(dl, htpRect, pg.settingsHtpPerf,
                          "HTP 调度 (性能档位)", htpLabels, htpCount, es, false);
        yc += 58.0f * s + padY;
    }

    // 4. Cancel / Save. Buttons stay at their fixed position; the open
    //    Inference/HTP popover floats over this row (painted last, see below)
    //    and simply covers them while open — expected menu behaviour.
    const float btnW = (card.w - 2.0f * padX - 12.0f * s) * 0.5f;
    const float btnH = 76.0f * s;
    widgets::Rect cancelRect{card.x + padX, yc, btnW, btnH};
    widgets::Rect saveRect{cancelRect.x + btnW + 12.0f * s, yc, btnW, btnH};
    if (widgets::button(dl, cancelRect, "取消", widgets::ButtonVariant::Ghost, es)) {
        pg.dialog = ModelDialogState::Closed;
        return;
    }
    if (widgets::button(dl, saveRect, "保存", widgets::ButtonVariant::Primary, es)) {
        const int threads = static_cast<int>(pg.settingsThreadsSlider.value + 0.5f);
        // A negative index means the dropdown has no selection — either the
        // entry's stored engine has no row in this build, or a file was picked
        // but no engine chosen yet. engineAt() would answer Engine::None for
        // it, and passing None to setInference() would persist "no engine" and
        // wipe a working selection. Keep the entry's engine in that case and
        // save only the fields the dialog can speak for.
        const bool haveEngine = pg.settingsEngine.value >= 0;
        const model::Engine newEngine =
            haveEngine ? model::engineAt(current.kind, pg.settingsEngine.value)
                       : current.engine;
        const float newConf   = pg.settingsConf.value;
        const int   newHtp    = clampHtpPerf(pg.settingsHtpPerf.value);

        // Did anything that the *compiled graph* depends on change? Engine,
        // thread count and HTP vote are all baked in at delegate-build time, so
        // a change to any of them leaves the live interpreter holding the old
        // one — and prepare()'s fast path (`engineReady && same id`) cannot see
        // it. Read the previous values before the write below clobbers them.
        const bool engineChanged =
            current.engine != newEngine || current.cpuThreads != threads ||
            current.htpPerfMode != newHtp;

        LOGI("SettingsDialog: SAVE model #%d '%s' — old engine=%d (%s), "
             "new engine=%d (%s), threads=%d, htpPerf=%d, conf=%.2f, "
             "engineChanged=%d",
             current.id, current.name.c_str(),
             static_cast<int>(current.engine), model::engineLabel(current.engine),
             static_cast<int>(newEngine),    model::engineLabel(newEngine),
             threads, newHtp, newConf, engineChanged ? 1 : 0);

        model::setInference(pg.settingsId, newEngine, newConf, threads, newHtp);

        // Confidence, by contrast, is applied per frame by the session's
        // post-processing, so it is picked up on the next inference and must NOT
        // trigger a recompile — that would turn a slider drag into a 20-second
        // stall the next time the user held the area.
        if (engineChanged) infer::runtime::invalidateEngine();

        pg.dialog = ModelDialogState::Closed;
    }

    // Popovers last, same as the Add dialog, so an open Inference/HTP list
    // floats above the Cancel/Save buttons instead of being painted over.
    // The buttons keep their fixed position and are just covered by the
    // floating popover while it is open.
    widgets::dropdownList(dl, engineRect, pg.settingsEngine, engines, enginesCount, es,
                          settingsEngineDisabled);
    if (selEngine == model::Engine::QnnHtp && htpRect.w > 0.0f) {
        widgets::dropdownList(dl, htpRect, pg.settingsHtpPerf, htpLabels, htpCount, es);
    }
}

}  // namespace

bool anyDialogOpen() {
    return g_pageModel.dialog != ModelDialogState::Closed || g_fileBrowser.open;
}

void drawModelSection(ImDrawList* dl, float x, float& y, float w,
                      float bottomY, float s, float es, const Xf& xf,
                      Scroll& sc) {
    PageModel& pg = g_pageModel;
    const float gap = kModelRowGap * s;
    ImFont* font = ImGui::GetFont();

    auto wRect = [&](float wx, float wy, float ww, float wh) {
        // Shift by the page's scroll offset so paint and hit-test agree on the
        // visual y. `y` keeps advancing in the natural coordinate space.
        const ImVec2 p = xf.pt(wx, wy - sc.offset);
        return widgets::Rect{p.x, p.y, xf.s(ww), xf.s(wh)};
    };

    // A dialog covers almost the whole board but the rows underneath are still
    // drawn and still hit-tested — the scrim is only paint. Without this guard
    // a tap that opens or dismisses a dialog would also fire whatever row
    // button happens to sit under the finger.
    const bool dialogOpen = (pg.dialog != ModelDialogState::Closed);

    // Add-button: full row width.
    const float addH = kModelAddBtnH * s;
    if (widgets::button(dl, wRect(x, y, w, addH), "Add Model",
                        widgets::ButtonVariant::Primary, es) && !dialogOpen) {
        pg.dialog = ModelDialogState::Adding;
        // Swallow the opening tap: the release lands outside the dialog card,
        // where the dialog's own outside-tap-close would otherwise read it
        // and shut the dialog in the same frame it opened.
        widgets::consumeTap();
        // Reset dialog state every open — abandoning a half-filled path
        // is not a workflow anyone will follow by accident and clearing
        // it on entry keeps an open dialog from lying about its state.
        pg.pathBuf[0]        = '\0';
        pg.pathLen           = 0;
        pg.engine.value      = -1;       // gated until a file lands
        pg.engine.open       = false;
        pg.conf.value        = 0.5f;
        pg.conf.placed       = false;    // re-snap the thumb to the new value
        pg.conf.dragging     = false;
        pg.threadsSlider.value  = 1.0f;  // 1..8 default
        pg.threadsSlider.placed = false;
        pg.threadsSlider.dragging = false;
        pg.htpPerf.value  = 1;   // default: Sustained High Performance
        pg.htpPerf.open   = false;
        pg.pathPicking       = false;
        pg.pathFailed        = false;
        pg.pathErr[0]        = '\0';
        g_fileBrowser.open   = false;
    }
    y += addH + gap;

    // The list: every entry is a fixed-height row with the load / delete
    // buttons on the right. The model file name (or path tail) sits
    // left, with a `[LOADED]` badge for the active one and a kind/engine
    // tag below. Layout is driven by hand so a row's height does not
    // depend on its content.
    const float rowH = kModelRowH * s;
    auto entries = model::all();
    if (entries.empty()) {
        dl->AddText(font, 22.0f * s, xf.pt(ImVec2(x, y + 24.0f * s)),
                    xf.col(TextMuted), "No models yet — tap Add Model above.");
    }
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (y + rowH > bottomY) break;
        const model::Entry& e = entries[i];
        widgets::Rect row{x, y, w, rowH};
        const ImVec2 rowMin(row.x, row.y);
        const ImVec2 rowMax(row.x + row.w, row.y + row.h);
        ImU32 fill = e.loaded ? lerpColor(PaneRail, Accent, 0.25f) : PaneRail;
        dl->AddRectFilled(xf.pt(rowMin), xf.pt(rowMax), xf.col(fill), xf.s(12.0f * s));
        dl->AddRect(xf.pt(rowMin), xf.pt(rowMax), xf.col(Edge),
                    xf.s(12.0f * s), 0, xf.s(1.0f * s));

        // Right side: Setting / Load / Delete. Load toggles; the menu sorts so
        // the loaded row sits at the top automatically. Setting opens the
        // per-model dialog (engine + confidence).
        const float btnH = 40.0f * s;
        const float btnGap = 10.0f * s;
        const float wDel = 104.0f * s;
        const float wLdr = 108.0f * s;
        const float wSet = 120.0f * s;
        const float rx = row.x + row.w - 18.0f * s;
        const float btnY = row.y + (row.h - btnH) * 0.5f;
        widgets::Rect delRect{rx - wDel, btnY, wDel, btnH};
        widgets::Rect ldrRect{delRect.x - btnGap - wLdr, btnY, wLdr, btnH};
        widgets::Rect setRect{ldrRect.x - btnGap - wSet, btnY, wSet, btnH};

        // Two short lines, kind tag and the name tail. Both are cut to the
        // space the buttons leave: a long file name must not run under them.
        const float textMaxW = setRect.x - 18.0f * s - (row.x + 18.0f * s);
        char line1[96], line2[96];
        snprintf(line1, sizeof(line1), "[%s] %s",
                 model::kindLabel(e.kind),
                 e.name.empty() ? model::baseName(e.path).c_str() : e.name.c_str());
        const char* typeTxt = e.typeText.empty() ? "—" : e.typeText.c_str();
        // The third detail line reflects what the engine actually consumes:
        // CPU-family engines show the thread count; QNN HTP shows the
        // performance-mode vote; NNAPI / GPU show neither.
        char detail[48];
        if (engineUsesCpuThreads(e.engine)) {
            snprintf(detail, sizeof(detail), "threads %d", e.cpuThreads);
        } else if (e.engine == model::Engine::QnnHtp ||
                   e.engine == model::Engine::LiteRtNpu) {
            snprintf(detail, sizeof(detail), "htp %d", e.htpPerfMode);
        } else {
            detail[0] = '\0';
        }
        snprintf(line2, sizeof(line2),
                 "%s  •  %s  •  conf %.2f  •  %d classes  •  %dx%d%s%s%s",
                 model::engineLabel(e.engine),
                 typeTxt,
                 e.confidence,
                 static_cast<int>(e.classCount),
                 e.inputSize,
                 e.inputSize,
                 detail[0] ? "  •  " : "",
                 detail,
                 e.loaded ? "  •  LOADED" : "");
        const std::string cut1 = ellipsize(font, 24.0f * s, line1, textMaxW);
        const std::string cut2 = ellipsize(font, 18.0f * s, line2, textMaxW);
        dl->AddText(font, 24.0f * s,
                    xf.pt(ImVec2(row.x + 18.0f * s, row.y + 12.0f * s)),
                    xf.col(TextPrimary), cut1.c_str());
        dl->AddText(font, 18.0f * s,
                    xf.pt(ImVec2(row.x + 18.0f * s, row.y + row.h - 30.0f * s)),
                    xf.col(TextMuted), cut2.c_str());

        if (widgets::button(dl, wRect(setRect.x, setRect.y, setRect.w, setRect.h),
                            "Setting",
                             widgets::ButtonVariant::Ghost, es) && !dialogOpen) {
            pg.settingsId = e.id;
            LOGI("ModelList: tap Setting on model #%d '%s' (engine=%d %s)",
                 e.id, e.name.c_str(),
                 static_cast<int>(e.engine), model::engineLabel(e.engine));
            // engineIndex() returns -1 when this build has no row for the
            // entry's engine — the honest answer is "nothing selected", and
            // the dropdown and the Save guard below both understand -1. The
            // value must NOT be coerced to 0 here: 0 is "NPU (LiteRT)", so a
            // stored NeuroPilot/absent engine would open this dialog showing
            // LiteRtNpu and Save would persist that over the user's choice.
            pg.settingsEngine.value = model::engineIndex(e.kind, e.engine);
            pg.settingsEngine.open  = false;
            pg.settingsConf.value   = e.confidence;
            pg.settingsConf.placed  = false;    // re-snap: this is a new model
            pg.settingsConf.dragging = false;
            pg.settingsThreadsSlider.value = static_cast<float>(e.cpuThreads);
            pg.settingsThreadsSlider.placed = false;
            pg.settingsThreadsSlider.dragging = false;
            pg.settingsHtpPerf.value = clampHtpPerf(e.htpPerfMode);
            pg.settingsHtpPerf.open  = false;
            pg.dialog = ModelDialogState::Settings;
            // Same opening-tap swallow as the Add Model button above.
            widgets::consumeTap();
        }
        if (widgets::button(dl, wRect(ldrRect.x, ldrRect.y, ldrRect.w, ldrRect.h),
                            e.loaded ? "Unload" : "Load",
                            widgets::ButtonVariant::Primary, es) && !dialogOpen) {
            model::toggleLoaded(e.id);
        }
        if (widgets::button(dl, wRect(delRect.x, delRect.y, delRect.w, delRect.h),
                            "Delete",
                            widgets::ButtonVariant::Danger, es) && !dialogOpen) {
            model::remove(e.id);
        }
        y += rowH + gap;
    }
}

/// Spins up a background thread that runs the (potentially multi-second)
/// graph compile, and puts the page into the Compiling state so the overlay
/// can show progress. A successful prepare() leaves the engine ready (no
/// frames flowing yet); a failed one keeps the switch where it is and
/// surfaces the reason through `compileError`.
///
/// `killed` is set to true on the way out when the call is being made
/// redundant (the user turned the switch off again, or swapped to
/// another model while a compile was in flight). The background thread
/// still runs to completion, but its result is dropped — the page only
/// cares about the latest request.
void startCompileAsync(int modelId, std::atomic<bool>* killed) {
    PageModel& pg = g_pageModel;

    // Replace any in-flight compile: the previous thread either has not
    // returned yet (rare) or has already returned (common). Joining here
    // is what keeps the unique_ptr from leaking the old thread.
    if (pg.compileThread && pg.compileThread->joinable()) {
        pg.compileThread->join();
    }
    pg.compileThread.reset();

    pg.compileId    = modelId;
    pg.compileOk    = false;
    pg.compileError.clear();
    pg.compileActive.store(true);
    pg.compileDone.store(false);
    pg.dialog       = ModelDialogState::Compiling;

    pg.compileThread = std::make_unique<std::thread>(
        [modelId]() {
            // Make this thread reflect the model the user actually wants to
            // load right now. Doing the lookup on the worker side avoids
            // racing with a Settings-driven model mutation on the UI thread.
            model::Entry entry;
            bool found = false;
            for (const model::Entry& e : model::all()) {
                if (e.id == modelId) { entry = e; found = true; break; }
            }

            infer::Status s;
            if (!found) {
                s = infer::Status::bad("the model is no longer in the list");
            } else {
                // Load-time compile, not load-time inference. The user no
                // longer pays the 1–3 s cold start the moment they want a
                // detection — they pay it once at Load, and later arm() is
                // a flag flip. If the master switch is already on, arm now
                // so they do not need a second tap; if not, arm() will be
                // called by syncModelPage() on the turnedOn edge and the
                // engine will be hot by then anyway.
                //
                // The toast and the modal card are raised together, on purpose.
                // The card is this page's own surface and is right for a user
                // who is watching the Model page; the toast is the one that
                // survives the menu being closed or the page being switched,
                // which is what happens when the user flips the switch and
                // immediately goes back to the Settings page to hold the area.
                notify::post(notify::kCompileTag, notify::Kind::Info,
                                 "模型编译中  正在准备引擎", 0.0f, 0.05f);
                notify::publishProgress(notify::kCompileTag, 0.20f);

                s = infer::runtime::prepare();

                if (s.ok) {
                    notify::publishProgress(notify::kCompileTag, 1.0f);
                    notify::post(notify::kCompileTag, notify::Kind::Success,
                                     "模型编译完成", 2.5f, 1.0f);
                    if (g_pageModel.enabled.value) {
                        infer::runtime::arm();
                    }
                } else {
                    notify::publishProgress(notify::kCompileTag, -1.0f);
                    notify::post(notify::kCompileTag, notify::Kind::Error,
                                     ("模型编译失败: " + s.message).c_str(),
                                     8.0f, -1.0f);
                }
            }

            if (!s.ok) {
                g_pageModel.compileError = s.message;
            }
            g_pageModel.compileOk    = s.ok;
            g_pageModel.compileDone.store(true);
            g_pageModel.compileActive.store(false);
        });
}

void syncModelPage() {
    // Two inputs decide what the runtime should be doing: the switch, and which
    // entry carries the loaded flag. Watching both is what makes Load/Unload in
    // the list take effect while inference is already running — otherwise the
    // user marks a different model, the list says LOADED, and the engine quietly
    // carries on with the old one.
    static bool lastOn      = false;
    static int  lastLoadedId = -1;

    const bool on = g_pageModel.enabled.value;
    const int  id = model::loadedId();

    // ── Triggered mode: this page has no authority ──────────────────────────
    // With continuous inference OFF, the Settings page's inference-area circle
    // owns arm/disarm, and hud.cpp forces this page's switch off and grey
    // precisely so the user cannot reach it. But that forcing is itself an
    // edge: the switch reads ON for one frame (it was left on when the user
    // visited Settings), then hud.cpp writes it off, and this function — which
    // runs before hud.cpp's write, on the very next frame — sees ON → OFF and
    // treats it as the user turning inference off.
    //
    // The result was a race over `disarm()` against the held-finger handler:
    // whichever ran second won, so a finger already down could be disarmed by
    // a switch the user cannot even see, and holding the area would work or not
    // depending on the frame order.
    //
    // So: in triggered mode this function only tracks the two values it watches,
    // and touches nothing. The track is still updated (rather than returning
    // early before it) so that leaving triggered mode does not look like a
    // spontaneous edge to the code below.
    if (!g_pageSettings.continuousInference.value) {
        if (on != lastOn || id != lastLoadedId) {
            lastOn = on;
            lastLoadedId = id;
        }
        return;
    }

    if (on == lastOn && id == lastLoadedId) return;

    const bool turnedOn  = on && !lastOn;
    const bool turnedOff = !on && lastOn;
    const bool swapped   = on && lastOn && id != lastLoadedId;

    lastOn = on;
    lastLoadedId = id;

    if (turnedOff) {
        // Master switch flipped off. Disarm only — the engine stays loaded
        // so flipping the switch back on is instant. A full stop() (which
        // unloads the engine and pays the next-load cost) only happens when
        // the loaded model itself goes away, see the !hasModel branch below.
        infer::runtime::disarm();
        return;
    }
    if (!on) return;

    // On the flip, and again when the loaded model changes underneath it.
    // Both cases run the same path: a background load + a Compiling overlay
    // so the user sees why the menu freezes for a second or two when an ONNX
    // model graph compiles, and so a model that fails to load shows the
    // reason instead of a silent switch flip back.
    if (turnedOn || swapped) {
        // No entry carries the loaded flag (or the flagged entry vanished):
        // there is nothing to compile. Do NOT enter the Compiling overlay
        // with a made-up "(model)" name — show the Chinese hint instead and
        // flip the switch back off so it never reads ON with nothing running.
        bool hasModel = false;
        if (id > 0) {
            for (const model::Entry& e : model::all()) {
                if (e.id == id) { hasModel = true; break; }
            }
        }
        if (!hasModel) {
            infer::runtime::stop();
            g_pageModel.enabled.value = false;
            g_pageModel.dialog = ModelDialogState::NoModel;
            lastOn = false;
            lastLoadedId = id;
            return;
        }
        static std::atomic<bool> killed{false};
        killed.store(false);
        startCompileAsync(id, &killed);
    }
}

void setModelSwitch(bool on) {
    g_pageModel.enabled.value = on;
    // The switch's own animation state is left alone: the ease is cosmetic, and
    // snapping it would make a scripted flip look different from a tapped one in
    // the one place a person is watching.
}

bool modelSwitch() { return g_pageModel.enabled.value; }

void drawCompilingDialog(ImDrawList* dl, const HudRect& board, float s, float es,
                          const Xf& xf) {
    PageModel& pg = g_pageModel;

    // Reap the worker once it has finished, every frame. Joining here — not
    // at the bottom of startCompileAsync — is what lets the overlay close
    // promptly when the model is ready; a join inside the helper would
    // freeze the UI for the compile duration.
    if (pg.compileDone.load() && pg.compileThread &&
        pg.compileThread->joinable()) {
        pg.compileThread->join();
        pg.compileThread.reset();
        if (pg.compileOk) {
            pg.dialog = ModelDialogState::Closed;
            return;  // next frame will draw the list, not the overlay
        }
    }

    const ImVec2 min(board.x, board.y);
    const ImVec2 max(board.x + board.w, board.y + board.h);
    dl->AddRectFilled(xf.pt(min), xf.pt(max),
                      xf.col(IM_COL32(0, 0, 0, 160)));

    // Smaller card than the Add/Settings dialogs — this one is a status
    // surface, not an editor. The ".." / "..." animation is the entire
    // reason it is here, so the card is sized to that line plus a title.
    const float w = 560.0f * s;
    const float h = 220.0f * s;
    const float cx = board.x + board.w * 0.5f;
    const float cy = board.y + board.h * 0.5f;
    const ImVec2 cmin(cx - w * 0.5f, cy - h * 0.5f);
    const ImVec2 cmax(cx + w * 0.5f, cy + h * 0.5f);
    dl->AddRectFilled(cmin, cmax, xf.col(PaneRight), xf.s(20.0f * s));
    dl->AddRect(cmin, cmax, xf.col(Edge), xf.s(20.0f * s), 0, xf.s(1.0f * s));

    ImFont* font = ImGui::GetFont();
    const float padX = 40.0f * s;

    // The title doubles as the busy line — the model name so the user
    // knows what is being compiled, not just "something".
    char title[160];
    model::Entry e;
    bool found = false;
    for (const model::Entry& ent : model::all()) {
        if (ent.id == pg.compileId) { e = ent; found = true; break; }
    }
    // Pull the display name out into a std::string with a lifetime that
    // spans the snprintf below. The ternary that returns a pointer into a
    // temporary would otherwise dangle by the time the format string runs.
    std::string displayName;
    if (found) {
        displayName = e.name.empty() ? model::baseName(e.path) : e.name;
    } else {
        displayName = "(未知模型)";
    }
    snprintf(title, sizeof(title), "正在加载 %s", displayName.c_str());

    // The three dots that march across. They grow in opacity at 1-second
    // intervals (so the cycle is . .. ... . .. ... over three seconds),
    // which is the kind of low-effort animation that keeps the overlay
    // from looking frozen on a long HTP compile.
    const double t = ImGui::GetTime();
    const int phase = static_cast<int>(t * 1.0) % 3;   // 0/1/2 — steps per second
    char dots[8] = {0};
    for (int i = 0; i <= phase; ++i) dots[i] = '.';
    dots[phase + 1] = '\0';

    char line1[192];
    snprintf(line1, sizeof(line1), "%s%s", title, dots);

    dl->AddText(font, 28.0f * s,
                ImVec2(cmin.x + padX, cmin.y + 36.0f * s),
                xf.col(TextPrimary), line1);

    // Subtitle: the backend being prepared, so a long compile is not
    // mysterious — the user knows which graph is in flight.
    if (found) {
        char sub[128];
        snprintf(sub, sizeof(sub), "%s  •  %s",
                 model::kindLabel(e.kind),
                 model::engineLabel(e.engine));
        dl->AddText(font, 18.0f * s,
                    ImVec2(cmin.x + padX, cmin.y + 86.0f * s),
                    xf.col(TextMuted), sub);
    }

    // If the worker has failed, drop the dots and show the reason. The
    // switch stays where the user put it; the only way out is a tap on
    // "关闭" (or the page-level retry of toggling the switch off and on).
    if (pg.compileDone.load() && !pg.compileOk) {
        const float btnW = 200.0f * s;
        const float btnH = 64.0f * s;
        widgets::Rect btn{cx - btnW * 0.5f, cmax.y - btnH - 24.0f * s, btnW, btnH};
        if (widgets::button(dl, btn, "关闭",
                             widgets::ButtonVariant::Primary, es)) {
            pg.dialog = ModelDialogState::Closed;
        }

        char msg[256];
        snprintf(msg, sizeof(msg), "加载失败: %s",
                 pg.compileError.empty() ? "(无详细信息)" : pg.compileError.c_str());
        const std::string cut = ellipsize(
            font, 18.0f * s, msg, w - 2.0f * padX - 16.0f * s);
        dl->AddText(font, 18.0f * s,
                    ImVec2(cmin.x + padX, cmax.y - btnH - 24.0f * s - 36.0f * s),
                    xf.col(IM_COL32(255, 96, 96, 255)), cut.c_str());
    }
}

/// Chinese hint shown when the inference switch is flipped on while no model
/// is loaded. There is nothing to compile, so this replaces the Compiling
/// overlay entirely: same small centered card style, outside-tap or the
/// "知道了" button dismisses it. The switch has already been flipped back
/// off by syncModelPage() before this is shown.
void drawNoModelDialog(ImDrawList* dl, const HudRect& board, float s, float es,
                       const Xf& xf) {
    PageModel& pg = g_pageModel;

    const ImVec2 min(board.x, board.y);
    const ImVec2 max(board.x + board.w, board.y + board.h);
    dl->AddRectFilled(xf.pt(min), xf.pt(max),
                      xf.col(IM_COL32(0, 0, 0, 160)));

    const float w = 560.0f * s;
    const float h = 260.0f * s;
    const float cx = board.x + board.w * 0.5f;
    const float cy = board.y + board.h * 0.5f;
    const ImVec2 cmin(cx - w * 0.5f, cy - h * 0.5f);
    const ImVec2 cmax(cx + w * 0.5f, cy + h * 0.5f);

    widgets::Rect card{cmin.x, cmin.y, w, h};
    if (closeOnOutsideTap(card)) {
        pg.dialog = ModelDialogState::Closed;
        return;
    }

    dl->AddRectFilled(cmin, cmax, xf.col(PaneRight), xf.s(20.0f * s));
    dl->AddRect(cmin, cmax, xf.col(Edge), xf.s(20.0f * s), 0, xf.s(1.0f * s));

    ImFont* font = ImGui::GetFont();
    const float padX = 40.0f * s;
    dl->AddText(font, 30.0f * s,
                ImVec2(cmin.x + padX, cmin.y + 36.0f * s),
                xf.col(TextPrimary), "未选择模型");
    dl->AddText(font, 20.0f * s,
                ImVec2(cmin.x + padX, cmin.y + 92.0f * s),
                xf.col(TextMuted), "请先在模型列表中点击 Load");
    dl->AddText(font, 20.0f * s,
                ImVec2(cmin.x + padX, cmin.y + 122.0f * s),
                xf.col(TextMuted), "选择一个模型，再打开推理开关。");

    const float btnW = 200.0f * s;
    const float btnH = 64.0f * s;
    widgets::Rect btn{cx - btnW * 0.5f, cmax.y - btnH - 24.0f * s, btnW, btnH};
    if (widgets::button(dl, btn, "知道了",
                        widgets::ButtonVariant::Primary, es)) {
        pg.dialog = ModelDialogState::Closed;
    }
}

void drawModelOverlays(ImDrawList* dl, float s, float es, const Xf& xf) {
    if (g_pageModel.dialog == ModelDialogState::Closed) return;
    const HudRect board = hudRect();

    // The file browser is its own card and paints its own scrim.
    if (g_fileBrowser.open) {
        drawFileBrowser(dl, board, s, es, xf);
        return;
    }

    if (g_pageModel.dialog == ModelDialogState::Compiling) {
        drawCompilingDialog(dl, board, s, es, xf);
        return;
    }

    if (g_pageModel.dialog == ModelDialogState::NoModel) {
        drawNoModelDialog(dl, board, s, es, xf);
        return;
    }

    drawDialogScrim(dl, board, xf);
    if (g_pageModel.dialog == ModelDialogState::Settings) {
        drawModelSettingsDialog(dl, board, s, es, xf);
    } else {
        drawAddModelDialog(dl, board, s, es, xf);
    }
}

}  // namespace sections
}  // namespace ui
}  // namespace aimbotng
