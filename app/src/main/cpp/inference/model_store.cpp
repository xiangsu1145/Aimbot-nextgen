// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::model — implementation. See model_store.h for the rationale.
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/model_store.h"

#include "inference/model_type.h"

#include <android/log.h>

#include "inference/model_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

#define TAG "AimbotModel"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace aimbotng {
namespace model {
namespace {

// Legacy location, kept as the migration fallback. It was chosen because the
// daemon (app_process, shell UID) could write it and the App could not — but
// that split is exactly what made importing a downloaded model impossible from
// the App side. The store now lives in the App's shared models directory
// (/sdcard/Android/data/<pkg>/models/, handed over via setStoreDir()), which
// BOTH sides can read and write, so a download and an import touch the same
// tree. A store that predates the move is adopted on first load and migrated
// by the first save.
constexpr const char* kLegacyStorePath = "/data/local/tmp/aimbot_models.tsv";
std::string g_storePath = kLegacyStorePath;

// Field separators. Tab between fields, 0x1f between class names — neither can
// appear in a file name the picker will hand back, so no escaping is needed and
// the file stays readable in an editor.
constexpr char kFieldSep = '\t';
constexpr char kClassSep = '\x1f';

std::mutex            g_mutex;
std::vector<Entry>    g_entries;
int                   g_nextId = 1;
bool                  g_loadedFromDisk = false;

// Forward declaration: ensureLoadedLocked() re-probes model shapes below and
// may need to persist the corrected values, but saveLocked() is defined later.
void saveLocked();

// Two explicit choices for ONNX: XNNPACK (fast) and plain CPU. The plain-CPU
// entry exists because some models — notably FP16 ones — misbehave under
// XNNPACK on this build (dense false detections). The user picks, we never
// auto-decide. Index 0 stays XNNPACK so older entries that serialised as
// engine=1 still mean "XNNPACK".
const char* kOnnxEngines[]   = {"ONNX Runtime · XNNPACK", "ONNX Runtime · 普通 CPU"};
const Engine kOnnxEngineIds[] = {Engine::OnnxRuntime, Engine::OnnxCpu};

// QNN HTP first: it is the reason to run a .tflite model on this device. NNAPI
// and CPU are the fallbacks for when the delegate refuses a graph — which it
// does for a surprising share of exported models — so they stay on the list
// rather than being hidden behind a failure.
//
// CPU is the literal LiteRT default path; XNNPACK is the same default plus
// an explicit XNNPack delegate. The two are kept separate so the user can
// see what they have selected, not so the menu has twice as many entries.
//
// GPU is the OpenCL delegate. It is widely available on Adreno / Mali.
// QNN HTP is shown but not wired (this build has no QNN EP); the engine
// table marks it "not implemented" so the row appears grey rather than gone.
// ⚠️ These two arrays are the single source of truth for the tflite engine
// dropdown, and enginesFor() reports their length as a literal. Adding an
// engine means editing BOTH arrays AND the count — miss one and the dropdown
// renders a garbage `const char*` taken from whatever the linker put after
// this array, which is a SIGSEGV inside ImFontCalcTextSizeEx (strlen) the
// first time the list is drawn. That crash looks nothing like its cause.
// Neuron (APU) is gated independently of NeuroPilot (AIMBOTNG_HAVE_NEURON):
// the delegate source is private and not in the repo. Previously Neuron was
// folded into the NeuroPilot branch, which wrongly hid it whenever NeuroPilot
// was off. Keep the two flags orthogonal.
// ── NPU (LiteRT) is temporarily commented out: LiteRT 2.x vendor dispatch
//    crashes on this MediaTek SKU and is not being fixed in this branch.
//    The arrays below still hold the same length, so engineAt/engineIndex
//    keep returning a meaningful value for entries written by an older build;
//    those entries fall through to (LiteRT, Cpu) in resolvePair() so the
//    model still loads, just on the CPU backend.
// {"NPU (LiteRT)",
#if AIMBOTNG_HAVE_NEUROPILOT
const char* kTfliteEngines[]   = {"NeuroPilotTFLite",
#if AIMBOTNG_HAVE_NEURON
                                  "Neuron (APU)",
#endif
                                  "QNN HTP", "NNAPI", "CPU", "XNNPACK", "GPU"};
const Engine kTfliteEngineIds[] = {Engine::NeuroPilot,
#if AIMBOTNG_HAVE_NEURON
                                   Engine::Neuron,
#endif
                                   Engine::QnnHtp, Engine::Nnapi, Engine::Cpu,
                                   Engine::TfliteXnnpack, Engine::TfliteGpu};
#elif AIMBOTNG_HAVE_NEURON
const char* kTfliteEngines[]   = {"Neuron (APU)",
                                  "QNN HTP", "NNAPI", "CPU", "XNNPACK", "GPU"};
const Engine kTfliteEngineIds[] = {Engine::Neuron,
                                   Engine::QnnHtp, Engine::Nnapi, Engine::Cpu,
                                   Engine::TfliteXnnpack, Engine::TfliteGpu};
#else
const char* kTfliteEngines[]   = {"QNN HTP", "NNAPI", "CPU", "XNNPACK", "GPU"};
const Engine kTfliteEngineIds[] = {Engine::QnnHtp, Engine::Nnapi, Engine::Cpu,
                                   Engine::TfliteXnnpack, Engine::TfliteGpu};
#endif

/** Loads the list on first use, so a caller cannot read an empty list by
 *  simply asking before start-up has read the file. */
void ensureLoadedLocked() {
    if (g_loadedFromDisk) return;
    g_loadedFromDisk = true;

    bool changed = false;   // a re-probe corrected a stored value -> persist it

    FILE* f = fopen(g_storePath.c_str(), "rb");
    if (f == nullptr && g_storePath != kLegacyStorePath) {
        // First run against the shared location. Adopt the legacy
        // /data/local/tmp list so an upgrade does not silently empty the
        // user's model list; `changed` makes the first save write the new
        // path, which is the migration itself.
        f = fopen(kLegacyStorePath, "rb");
        if (f != nullptr) {
            changed = true;
            LOGI("migrating model list from %s", kLegacyStorePath);
        }
    }
    if (f == nullptr) return;  // first run — an empty list is a correct answer

    std::string line;
    int ch = 0;
    while ((ch = fgetc(f)) != EOF) {
        if (ch != '\n') {
            line.push_back(static_cast<char>(ch));
            continue;
        }
        std::vector<std::string> fields;
        std::string field;
        std::istringstream in(line);
        while (std::getline(in, field, kFieldSep)) fields.push_back(field);
        line.clear();
        if (fields.size() < 7) continue;

        Entry e;
        e.id        = atoi(fields[0].c_str());
        e.name      = fields[1];
        e.path      = fields[2];
        e.kind      = static_cast<Kind>(atoi(fields[3].c_str()));
        e.engine    = static_cast<Engine>(atoi(fields[4].c_str()));
        e.inputSize = atoi(fields[5].c_str());
        e.loaded    = fields[6] != "0";
        // Confidence was added after the file format was already on device, so
        // it is optional: an older line (or a hand-edited one) keeps the
        // default rather than reading a threshold of 0 and matching nothing.
        e.confidence = (fields.size() > 8) ? atof(fields[8].c_str()) : 0.5f;
        if (e.confidence < 0.0f) e.confidence = 0.0f;
        if (e.confidence > 1.0f) e.confidence = 1.0f;
        // typeText is the 10th field (index 9); older lines that stop at the
        // confidence field simply keep an empty label until they are re-saved.
        if (fields.size() > 9) e.typeText = fields[9];
        // cpuThreads is the 11th field (index 10); missing on older lines, which
        // keep the struct default of 1.
        if (fields.size() > 10) {
            int t = atoi(fields[10].c_str());
            if (t < 1) t = 1;
            if (t > 8) t = 8;
            e.cpuThreads = t;
        }
        // htpPerfMode is the 12th field (index 11); missing on older lines,
        // which keep the struct default of Sustained High Performance (1).
        if (fields.size() > 11) {
            int p = atoi(fields[11].c_str());
            if (p < 0) p = 0;
            if (p > 9) p = 9;   // top of TfLiteQnnDelegateHtpPerformanceMode
            e.htpPerfMode = p;
        }
        // classCount is the 13th field (index 12); missing on older lines,
        // which fall back to a re-probe from the file below.
        if (fields.size() > 12) {
            int c = atoi(fields[12].c_str());
            if (c < 0) c = 0;
            e.classCount = c;
        }
        if (fields.size() > 7 && !fields[7].empty()) {
            std::istringstream cls(fields[7]);
            std::string name;
            while (std::getline(cls, name, kClassSep)) {
                if (!name.empty()) e.classes.push_back(name);
            }
        }
        if (e.kind == Kind::Unknown) e.kind = kindOfPath(e.path);

        // The resolution and class count are best read from the model file
        // itself — the file on disk is the source of truth. Lines written by
        // older builds pre-dated these fields (inputSize left at the 640
        // default, classCount at 0), and a hand-edited line may carry a wrong
        // value, so we override with an authoritative probe whenever it
        // succeeds.
        {
            int pw = 0, ph = 0, pc = 0;
            if (aimbotng::infer::detectModelShape(e.path, pw, ph, pc)) {
                if (pw > 0) e.inputSize = pw;   // square: W == H
                e.classCount = pc;
                changed = true;
            } else if (e.inputSize <= 0) {
                e.inputSize = 640;
            }
        }

        if (e.id <= 0) continue;
        g_entries.push_back(e);
        if (e.id >= g_nextId) g_nextId = e.id + 1;
    }
    fclose(f);
    if (changed) saveLocked();   // persist any values the re-probe corrected
    LOGI("loaded %zu model(s) from %s", g_entries.size(), g_storePath.c_str());
}

/** Writes the whole list. Called with the lock held. */
void saveLocked() {
    // The shared location sits one level deep under a directory that always
    // exists by the time anything imports a model, but a first-run save can
    // race the App's own directory setup — one mkdir is cheaper than losing
    // the list to a missing parent.
    {
        std::string parent = g_storePath;
        const size_t slash = parent.find_last_of('/');
        if (slash != std::string::npos && slash > 0) {
            parent.resize(slash);
            mkdir(parent.c_str(), 0755);   // EEXIST is fine
        }
    }
    FILE* f = fopen(g_storePath.c_str(), "wb");
    if (f == nullptr) {
        LOGE("cannot write %s", g_storePath.c_str());
        return;
    }
    for (const Entry& e : g_entries) {
        fprintf(f, "%d%c%s%c%s%c%d%c%d%c%d%c%d%c",
                e.id, kFieldSep,
                e.name.c_str(), kFieldSep,
                e.path.c_str(), kFieldSep,
                static_cast<int>(e.kind), kFieldSep,
                static_cast<int>(e.engine), kFieldSep,
                e.inputSize, kFieldSep,
                e.loaded ? 1 : 0, kFieldSep);
        for (size_t i = 0; i < e.classes.size(); ++i) {
            if (i != 0) fputc(kClassSep, f);
            fputs(e.classes[i].c_str(), f);
        }
        // Confidence comes *after* the class list: the class list is
        // variable-length, so putting the new field before it would renumber
        // field 7 and leave every line already on device misread.
        fputc(kFieldSep, f);
        fprintf(f, "%.3f", e.confidence);
        // typeText is last for the same reason — a variable-length field must not
        // sit before anything that older lines already parsed by position.
        fputc(kFieldSep, f);
        fputs(e.typeText.c_str(), f);
        // cpuThreads appended last for the same positional-compatibility reason:
        // lines already on device stop at typeText and simply default to 1.
        fputc(kFieldSep, f);
        fprintf(f, "%d", e.cpuThreads);
        // htpPerfMode after that: also a fixed single int, so appending keeps
        // every existing line on device readable (older lines lack it and
        // default to 1 on load).
        fputc(kFieldSep, f);
        fprintf(f, "%d", e.htpPerfMode);
        // classCount is the 13th field (index 12): the detection-class count
        // probed from the graph. Fixed single int, appended last for the same
        // positional-compatibility reason as the fields above it.
        fputc(kFieldSep, f);
        fprintf(f, "%d", e.classCount);
        fputc('\n', f);
    }
    fclose(f);
}

/** Loaded entries first, then newest first. */
void sortLocked() {
    std::stable_sort(g_entries.begin(), g_entries.end(),
                     [](const Entry& a, const Entry& b) {
                         if (a.loaded != b.loaded) return a.loaded;
                         return a.id > b.id;
                     });
}

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::string(suffix).size();
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace

// ── Kinds and engines ────────────────────────────────────────────────────────

Kind kindOfPath(const std::string& path) {
    const std::string p = lower(path);
    if (endsWith(p, ".onnx")) return Kind::Onnx;
    if (endsWith(p, ".tflite")) return Kind::Tflite;
    return Kind::Unknown;
}

const char* kindLabel(Kind kind) {
    switch (kind) {
        case Kind::Onnx:   return "ONNX";
        case Kind::Tflite: return "TFLite";
        default:           return "?";
    }
}

const char* engineLabel(Engine engine) {
    switch (engine) {
        case Engine::OnnxRuntime:   return "ONNX Runtime · XNNPACK";
        case Engine::OnnxCpu:       return "普通 CPU";
        case Engine::QnnHtp:        return "QNN HTP";
        case Engine::Nnapi:         return "NNAPI";
        case Engine::Cpu:           return "CPU";
        case Engine::TfliteXnnpack: return "XNNPACK";
        case Engine::TfliteGpu:     return "GPU";
        case Engine::LiteRtNpu:     return "NPU (LiteRT)";
#if AIMBOTNG_HAVE_NEUROPILOT
        case Engine::NeuroPilot:    return "NeuroPilotTFLite";
#endif
#if AIMBOTNG_HAVE_NEURON
        case Engine::Neuron:        return "Neuron (APU)";
#endif
        default:                    return "未选择";
    }
}

/// Array length, so the counts below cannot drift away from the arrays they
/// describe. See the warning on kTfliteEngines for why that matters.
template <class T, size_t N>
constexpr int arrayLen(T (&)[N]) { return static_cast<int>(N); }

const char* const* enginesFor(Kind kind, int& count) {
    switch (kind) {
        case Kind::Onnx:   count = arrayLen(kOnnxEngines);   return kOnnxEngines;
        case Kind::Tflite: count = arrayLen(kTfliteEngines); return kTfliteEngines;
        default:           count = 0; return nullptr;
    }
}

Engine engineAt(Kind kind, int index) {
    if (kind == Kind::Onnx) {
        return (index >= 0 && index < arrayLen(kOnnxEngineIds))
                   ? kOnnxEngineIds[index] : Engine::None;
    }
    if (kind == Kind::Tflite) {
        return (index >= 0 && index < arrayLen(kTfliteEngineIds))
                   ? kTfliteEngineIds[index] : Engine::None;
    }
    return Engine::None;
}

/// The dropdown index for `engine`, or -1 when this build has no row for it.
///
/// -1 rather than 0 is load-bearing. 0 is a real index (it is "NPU (LiteRT)"
/// in the tflite list and "ONNX Runtime · XNNPACK" in the onnx one), so
/// returning it for "not found" does not mean "no selection" — it means a
/// concrete engine the user never picked. A stored engine that this build
/// cannot name (NeuroPilot saved by a NeuroPilot build, then opened by one
/// built without it) would come back as index 0, the Settings dialog would
/// show "NPU (LiteRT)", and pressing Save would write LiteRtNpu over the
/// user's model. Callers must treat negative as "leave the selection alone".
int engineIndex(Kind kind, Engine engine) {
    if (kind == Kind::Onnx) {
        for (int i = 0; i < arrayLen(kOnnxEngineIds); ++i)
            if (kOnnxEngineIds[i] == engine) return i;
    } else if (kind == Kind::Tflite) {
        for (int i = 0; i < arrayLen(kTfliteEngineIds); ++i)
            if (kTfliteEngineIds[i] == engine) return i;
    }
    return -1;
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

// ── The list ────────────────────────────────────────────────────────────────

std::vector<Entry> all() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    sortLocked();
    return g_entries;
}

int add(Entry entry) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    entry.id = g_nextId++;
    g_entries.push_back(entry);
    sortLocked();
    saveLocked();
    LOGI("added model #%d %s (%s)", entry.id, entry.name.c_str(), entry.path.c_str());
    return entry.id;
}

bool remove(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
        if (it->id != id) continue;
        g_entries.erase(it);
        saveLocked();
        LOGI("removed model #%d", id);
        return true;
    }
    return false;
}

bool toggleLoaded(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    bool changed = false;
    for (Entry& e : g_entries) {
        if (e.id == id) {
            e.loaded = !e.loaded;
            changed = true;
        } else if (e.loaded) {
            e.loaded = false;  // only one model is ever loaded
        }
    }
    if (changed) {
        sortLocked();
        saveLocked();
    }
    return changed;
}

int loadedId() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Entry& e : g_entries) {
        if (e.loaded) return e.id;
    }
    return 0;
}

bool setClasses(int id, const std::vector<std::string>& classes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Entry& e : g_entries) {
        if (e.id != id) continue;
        e.classes = classes;
        saveLocked();
        return true;
    }
    return false;
}

bool setEngine(int id, Engine engine) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    for (Entry& e : g_entries) {
        if (e.id != id) continue;
        e.engine = engine;
        saveLocked();
        LOGI("model #%d engine -> %s", id, engineLabel(engine));
        return true;
    }
    return false;
}

bool setConfidence(int id, float confidence) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    if (confidence < 0.0f) confidence = 0.0f;
    if (confidence > 1.0f) confidence = 1.0f;
    for (Entry& e : g_entries) {
        if (e.id != id) continue;
        e.confidence = confidence;
        saveLocked();
        LOGI("model #%d confidence -> %.2f", id, confidence);
        return true;
    }
    return false;
}

bool setInference(int id, Engine engine, float confidence, int threads,
                   int htpPerf) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    if (confidence < 0.0f) confidence = 0.0f;
    if (confidence > 1.0f) confidence = 1.0f;
    for (Entry& e : g_entries) {
        if (e.id != id) continue;
        // Engine::None means "no runtime chosen", which is not a state a saved
        // model may be left in: prepare() would fail with "this model has no
        // runtime selected" and the entry would be dead weight in the list.
        // Callers that have no real selection pass None by accident (an empty
        // dropdown, an index that had no row); keeping the existing engine is
        // the only answer that does not destroy a working model.
        if (engine != Engine::None) e.engine = engine;
        e.confidence = confidence;
        if (threads > 0) {
            int t = threads;
            if (t < 1) t = 1;
            if (t > 8) t = 8;
            e.cpuThreads = t;
        }
        if (htpPerf >= 0) {
            int p = htpPerf;
            if (p < 0) p = 0;
            if (p > 9) p = 9;
            e.htpPerfMode = p;
        }
        saveLocked();
        // Log the engine that was actually stored, not the argument: when the
        // caller passes None the write above is skipped, and a log line naming
        // "未选择" for a model that still has a working engine is exactly the
        // kind of misleading entry this project has been bitten by before.
        LOGI("model #%d inference -> %s @ %.2f, threads=%d, htpPerf=%d",
             id, engineLabel(e.engine), confidence, e.cpuThreads, e.htpPerfMode);
        return true;
    }
    return false;
}

bool setType(int id, const std::string& typeText) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Entry& e : g_entries) {
        if (e.id != id) continue;
        if (e.typeText == typeText) return true;  // nothing to persist
        e.typeText = typeText;
        saveLocked();
        LOGI("model #%d type -> %s", id, typeText.c_str());
        return true;
    }
    return false;
}

/// The engine a freshly imported model gets: the first row of the kind's
/// engine table that this build can actually run. The table order IS the
/// default order (the same one the Add dialog's dropdown shows); the only
/// deviation is skipping NeuroPilot / Neuron when their vendor .so is absent
/// — the same check that greys those rows in the dropdown — because an
/// import that defaults onto an engine the device cannot run would fail at
/// Load every single time.
Engine defaultEngineFor(Kind kind) {
    const Engine* ids = nullptr;
    int count = 0;
    if (kind == Kind::Onnx) {
        ids = kOnnxEngineIds;  count = arrayLen(kOnnxEngineIds);
    } else if (kind == Kind::Tflite) {
        ids = kTfliteEngineIds;  count = arrayLen(kTfliteEngineIds);
    }
    for (int i = 0; ids != nullptr && i < count; ++i) {
        const Engine e = ids[i];
        if ((e == Engine::NeuroPilot || e == Engine::Neuron) &&
            !infer::runtime::engineAvailable(e)) {
            continue;
        }
        return e;
    }
    return (ids != nullptr && count > 0) ? ids[0] : Engine::None;
}

void setStoreDir(const char* dir) {
    if (dir == nullptr || dir[0] == '\0') return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_loadedFromDisk) {
        LOGE("setStoreDir after load — ignored (%s)", dir);
        return;
    }
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    g_storePath = d + "/aimbot_models.tsv";
    LOGI("model store: %s", g_storePath.c_str());
}

int addDefault(const char* path) {
    if (path == nullptr || path[0] == '\0') return -1;
    const Kind k = kindOfPath(path);
    if (k == Kind::Unknown) return -1;

    Entry e;
    e.path  = path;
    e.name  = baseName(path);
    e.kind  = k;
    e.engine = defaultEngineFor(k);
    // The rest are the struct defaults — confidence 0.5, cpuThreads 1,
    // htpPerfMode 1 (Sustained High Performance) — the same values the Add
    // dialog seeds its controls with. The two probed fields are the only
    // ones that need the file itself.
    {
        int w = 0, h = 0, c = 0;
        if (aimbotng::infer::detectModelShape(path, w, h, c)) {
            if (w > 0) e.inputSize = w;   // square: W == H
            e.classCount = c;
        }
        e.typeText = aimbotng::infer::detectModelType(path);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    // Idempotent: the same file imported twice is one entry, not two.
    for (const Entry& old : g_entries) {
        if (old.path == e.path) return old.id;
    }
    e.id = g_nextId++;
    g_entries.push_back(e);
    sortLocked();
    saveLocked();
    LOGI("added default model #%d %s (%s, engine %s)", e.id, e.name.c_str(),
         e.path.c_str(), engineLabel(e.engine));
    return e.id;
}

void loadFromDisk() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
}

void saveToDisk() {
    std::lock_guard<std::mutex> lock(g_mutex);
    saveLocked();
}

const char* storePath() { return g_storePath.c_str(); }

}  // namespace model
}  // namespace aimbotng
