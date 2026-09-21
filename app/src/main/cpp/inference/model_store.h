// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::model — the model list the menu edits
//
//  A model is a file plus everything needed to run it: which runtime it wants,
//  what shape it expects its input in, and what its output classes are called.
//  This module owns that list and nothing else — it does not load a runtime and
//  does not run inference (that comes later): it is the menu's data, kept in one
//  place so the Model page and whatever eventually consumes a model cannot
//  disagree about it.
//
//  Two decisions worth writing down:
//
//  * The engine list is derived from the file's *extension*, not chosen freely.
//    An .onnx file can only run on ONNX Runtime here; a .tflite file can go to
//    QNN's HTP backend or stay on the CPU. Offering a menu of engines before a
//    file is picked would be offering choices the file cannot honour, so the page
//    asks for the file first and the dropdown is empty until then.
//
//  * The list is persisted. The daemon is restarted constantly while this is
//    being developed — every rebuild, every rotation of the layer — and a model
//    list that vanishes each time is a list nobody will fill in twice. It is
//    written to the shell's scratch directory as one tab-separated line per
//    model: no JSON parser, no escaping rules to get wrong, and it can be read
//    and edited with a text editor when something goes wrong.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string>
#include <vector>

namespace aimbotng {
namespace model {

/// What a model file *is*, decided by its extension.
enum class Kind {
    Unknown = 0,
    Onnx,
    Tflite,
};

/// A runtime that can execute a model. Not every engine can run every kind —
/// see enginesFor().
enum class Engine {
    None = 0,
    OnnxRuntime,   // .onnx — XNNPACK kernels (the fast default)
    QnnHtp,        // .tflite — Qualcomm's NPU (HTP) through QNN (not wired in this build)
    Nnapi,         // .tflite — Android's own delegate, picks whatever it likes
    Cpu,           // .tflite — plain LiteRT CPU path, no extra delegate
    OnnxCpu,       // .onnx — plain ORT/MLAS CPU, no XNNPACK. For FP16 / XNNPACK-incompatible models
    TfliteXnnpack, // .tflite — explicit XNNPACK delegate on top of the default
    TfliteGpu,     // .tflite — GPU delegate (OpenCL)
    LiteRtNpu,     // .tflite — LiteRT 2.x vendor dispatch: MediaTek Neuron /
                   //           Qualcomm HTP. Different runtime from TfliteGpu's,
                   //           same file format.
    NeuroPilot,    // .tflite — MediaTek NeuroPilot (libtflite_mtk.mtk.so), the
                   //           APU path. A third runtime again, and the only
                   //           one whose .so comes from the system image.
    Neuron,        // .tflite — MediaTek APU driven through the Neuron adapter
                   //           (libneuronusdk_adapter.mtk.so) with Google's own
                   //           TFLite interpreter underneath. Not the same thing
                   //           as NeuroPilot: no MediaTek TFLite fork is used,
                   //           only the delegate/APU half of their SDK.
                   //
                   // Appended last on purpose. This enum is persisted as an int
                   // in the model store, so inserting anywhere but the end
                   // would silently re-label every model already saved on
                   // device (a stored 5 would become a different engine).
};

/// One entry in the list. `loaded` is a marker, not a state of the runtime: at
/// most one entry may have it set, and the menu shows that one first.
struct Entry {
    int                      id = 0;
    std::string              name;        // shown in the list; defaults to the file name
    std::string              path;        // absolute path to a .onnx / .tflite file
    Kind                     kind = Kind::Unknown;
    Engine                   engine = Engine::None;
    std::vector<std::string> classes;     // index 0 is class 0
    int                      classCount = 0;    // detection-class count, probed from the graph
    int                      inputSize = 640;   // square input the model wants
    /// ONNX Runtime intra-op thread count, 1–8. Stored per model because the
    /// right value depends on the model (a tiny head is memory-bound and slows
    /// down with more threads; a big one parallelises well) and on how many
    /// cores the user wants to give the detector vs. leave to the game.
    int                      cpuThreads = 1;     // default 1; range enforced in the UI
    /// HTP (QNN) performance-mode vote, raw TfLiteQnnDelegateHtpPerformanceMode
    /// int. 0 = system default; 1 = Sustained High Performance (UI default).
    /// Only meaningful for the QNN HTP engine; harmless for the others.
    int                      htpPerfMode = 1;     // default: Sustained High Performance
    /// Detections below this are thrown away. Per model, because a model
    /// trained on 20 classes and one trained on 2 do not sit at the same
    /// operating point, and a single global number would force a compromise.
    float                    confidence = 0.5f;   // 0..1
    bool                     loaded = false;
    /// Detected tensor element type label: "fp32" / "fp16" / "int8" / … / "".
    /// Empty until the file is peeked (at add-time or on first load). Shown in
    /// the list so FP16 / quantized models are visible at a glance.
    std::string              typeText;
};

// ── Kinds and engines ────────────────────────────────────────────────────────

/// Kind from the file extension, or Unknown for anything else.
Kind kindOfPath(const std::string& path);

/// Short label for the badge on a list row ("ONNX" / "TFLite").
const char* kindLabel(Kind kind);

/// Human name of an engine, for the dropdown and the list row.
const char* engineLabel(Engine engine);

/// The engines that can run `kind`, as labels; `count` is set to how many.
/// Returns null-terminated statics, so the pointers stay valid.
const char* const* enginesFor(Kind kind, int& count);

/// The engine behind the `index`-th label of enginesFor(), or None if `index`
/// is out of range. Kept next to enginesFor() so the two cannot drift apart.
Engine engineAt(Kind kind, int index);

/// The index engineAt() would map back to `engine`, or **-1** when this build
/// has no row for it. Used to place the dropdown on an entry that is being
/// edited.
///
/// Negative is not interchangeable with 0: 0 is a valid index that names a
/// real engine, so a caller that clamped this to 0 would show — and on Save
/// persist — an engine the user never chose. Callers must skip the write when
/// this returns a negative value.
int engineIndex(Kind kind, Engine engine);

/// "file.onnx" -> "file", for pre-filling the name field.
std::string baseName(const std::string& path);

// ── The list ────────────────────────────────────────────────────────────────
// Written from the render thread (the menu) and from whichever thread delivers
// a file-picker / keyboard result, so every call takes the module's lock.

/// Every entry: the loaded one first, then most recently added first.
std::vector<Entry> all();

/// Appends `entry` (its id is assigned) and persists. Returns the new id.
int add(Entry entry);

/// Drops the entry with this id. Returns whether anything was removed.
bool remove(int id);

/// Marks one entry loaded and every other one unloaded; passing the id that is
/// already loaded unloads it instead. Returns whether the list changed.
bool toggleLoaded(int id);

/// The id of the loaded entry, or 0 when none is loaded.
int loadedId();

/// Replaces the classes of an entry. Used by the keyboard result path.
bool setClasses(int id, const std::vector<std::string>& classes);

/// Changes which runtime an entry runs on (its "推理方式"). Returns whether an
/// entry with that id exists — an unknown id leaves the list untouched.
bool setEngine(int id, Engine engine);

/// Changes an entry's confidence threshold. Clamped to 0..1.
bool setConfidence(int id, float confidence);

/// Both of the above in one write, which is what the per-model Settings
/// dialog does: it edits two fields and saves once on confirm. `threads` is the
/// ONNX Runtime intra-op thread count (1–8); pass a value <= 0 to leave the
/// stored count untouched. `htpPerf` is the HTP performance-mode vote; pass a
/// value < 0 to leave it untouched.
bool setInference(int id, Engine engine, float confidence, int threads = -1,
                  int htpPerf = -1);

/// Records the detected tensor element type for an entry ("fp32" / "fp16" / …),
/// as reported by the inference backend after load or peeking the file.
bool setType(int id, const std::string& typeText);

// ── Persistence ─────────────────────────────────────────────────────────────

/**
 * Points the persisted list at `<dir>/aimbot_models.tsv` instead of the legacy
 * /data/local/tmp location.
 *
 * `dir` is the App's shared models directory (/sdcard/Android/data/<pkg>/models),
 * which this shell-uid process can read and write and which the App process can
 * too — the store stops being a file only the daemon can reach. Must be called
 * before the first loadFromDisk()/all()/add() of the process lifetime; a call
 * after the list is already read is ignored. When the new file does not exist
 * yet but the legacy one does, the first load adopts the legacy list and the
 * first save migrates it over.
 */
void setStoreDir(const char* dir);

/**
 * Appends the model file at `path` with the defaults a fresh Add-dialog entry
 * would get: the first engine of the kind's engine table that this build can
 * actually run, confidence 0.5, cpuThreads 1, HTP perf mode 1, input size and
 * class count probed from the graph, tensor type peeked from the file. The
 * display name is the file's base name.
 *
 * Returns the entry id — the existing one when the path is already registered,
 * so importing the same file twice does not duplicate the row — or -1 when the
 * path is not a .onnx/.tflite file.
 */
int addDefault(const char* path);

/// Reads the list back from disk. Safe to call once at start-up; a missing or
/// unreadable file leaves the list empty rather than failing.
void loadFromDisk();

/// Writes the list to disk. Called after every mutation, which is cheap: the
/// file is a few hundred bytes and this happens on a user action, not per frame.
void saveToDisk();

/// Where the list lives. Exposed so a log line can name it.
const char* storePath();

}  // namespace model
}  // namespace aimbotng
