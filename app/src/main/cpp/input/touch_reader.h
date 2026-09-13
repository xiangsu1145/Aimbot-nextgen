// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng input — physical touchscreen reader
//
//  Opens the real touch panel, takes it EXCLUSIVELY with EVIOCGRAB and decodes
//  its evdev stream into screen-space pointers. Ported from the previous
//  project's inputmgr_core.cpp / touch_core.cpp (G:\ai\Aimbot-ai\android-client)
//  with the aimbot-specific pieces removed (license gate, detection zones).
//
//  Why grab: while a device is grabbed the kernel stops delivering its events
//  to InputReader, so the physical finger produces no system touch at all. The
//  caller then decides what to do with it — feed it to the ImGui menu and/or
//  mirror it back through /dev/uinput (uinput_mirror_physical) so the app
//  underneath still behaves normally.
//
//  Must run in a process allowed to read /dev/input and ioctl EVIOCGRAB —
//  root, or the shell UID (which sits in the `input` group). That is the shell
//  daemon, not the app process.
//
//  Coordinates handed out are SCREEN coordinates in the current orientation.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define READER_MAX_POINTERS 16

/// Upper bound on the menu rectangles the overlay may publish.
#define READER_MAX_REGIONS 8

/// One finger currently touching the panel.
typedef struct ReaderPointer {
    int   id;  ///< stable tracking id for as long as the finger stays down
    float x;   ///< screen X in pixels (current orientation)
    float y;   ///< screen Y in pixels (current orientation)
} ReaderPointer;

/// Finds the touch panel, opens it and grabs it.
///
/// `screenW`/`screenH` are the display size in the CURRENT orientation
/// (landscape gives e.g. 3000x2120); `rotation` is 0..3 as reported by the
/// display. Returns false when no touch panel could be opened.
bool reader_init(int screenW, int screenH, int rotation);

/// Releases the grab and closes every fd. Safe to call repeatedly. Always call
/// this before the process exits, otherwise the panel stays grabbed.
void reader_close(void);

bool reader_is_ready(void);

/// Re-applies the display geometry (call on rotation / resize).
void reader_set_screen_params(int screenW, int screenH, int rotation);

/// (Re)acquires the exclusive grab. Idempotent.
bool reader_grab(void);

/// Releases the exclusive grab so the panel works normally again.
void reader_ungrab(void);

bool reader_is_grabbed(void);

/// Whether physical fingers should be mirrored back through uinput. Turned off
/// while the menu wants the touch to itself.
void reader_set_sink(bool enabled);

bool reader_get_sink(void);

/// Publishes the rectangles that must SWALLOW a touch, as a flat
/// [x, y, w, h, ...] list in screen pixels (`count` rectangles).
///
/// A gesture that starts inside one of them is not mirrored back through
/// uinput: the finger belongs to the menu, and letting it through would make
/// the app underneath react as well. The daemon cannot ask ImGui itself — it
/// runs in a different process — so the overlay pushes its window geometry
/// here. An empty list (or a null pointer) means "no menu on screen", which
/// restores full pass-through.
void reader_set_regions(const int* rects, int count);

/// Number of rectangles currently published (diagnostics).
int reader_get_region_count(void);

/// Blocking wait for panel events.
/// Returns >0 when the pointer set changed, 0 on timeout, <0 on error.
/// When the sink is enabled, the new pointer set is mirrored to uinput here.
int reader_poll(int timeoutMs);

/// Copies the current pointer set (screen coordinates) into `out`.
/// Returns the number of pointers written.
int reader_read_pointers(ReaderPointer* out, int maxCount);

/// Number of active pointers right now.
int reader_pointer_count(void);

/// Panel coordinate range, for diagnostics.
int reader_get_max_x(void);
int reader_get_max_y(void);

/// Path of the panel that was opened ("" when none). Handed to the uinput
/// module so it clones exactly that device.
const char* reader_get_panel_path(void);

/// One discovered touch panel. The buffers are owned by the reader and stay
/// alive for the lifetime of the process; copy them out (e.g. into a std::string)
/// if you need to keep them past the next [reader_list_panels] call.
typedef struct ReaderPanel {
    const char* path;        ///< e.g. "/dev/input/event4"
    const char* name;        ///< EVIOCGNAME, e.g. "synaptics,s2338"
    /// True when this node is our own uinput injection device — the kernel
    /// hands every uinput file the same inode as /dev/uinput itself, so a
    /// `stat(path).st_ino == stat("/dev/uinput").st_ino` test is what
    /// catches the clone-or-random-named sibling that lives in the panel
    /// list but is not actually a panel to mirror from. The UI surfaces
    /// it greyed-out rather than hiding it, so the user can see why their
    /// finger's panel is "missing one".
    bool        ownVirtual;
} ReaderPanel;

/// Upper bound on the number of panels [reader_list_panels] will write.
#define READER_MAX_PANELS 8

/// Fills `out` with every touch-capable device under /dev/input, in the same
/// order `reader_init` would have picked them. Returns the number written.
/// Aimbot's own virtual device is skipped.
int reader_list_panels(ReaderPanel* out, int max);

/// Re-opens a single, explicitly-chosen panel. Useful when the caller wants a
/// different panel than `reader_init` would have picked first — and as the
/// first half of [reader_select_panel].
bool reader_init_with_path(const char* path, int screenW, int screenH, int rotation);

/// Drops the grab, closes the current panel, re-opens `path`, and re-grabs.
/// No-op (returns false) if the path is the one already open. Keeps the
/// previously held grab if the swap fails, so the panel does not end up in a
/// "nobody owns it" window that the system InputReader would briefly answer to.
bool reader_select_panel(const char* path);

#ifdef __cplusplus
}
#endif
