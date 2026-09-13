// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::ui — ImGui font loading
//
//  Two fonts are embedded straight into libaimbotng.so (see CMakeLists.txt,
//  objcopy turns the .ttf files into a .rodata blob) and registered on the
//  ImGui atlas at startup:
//
//    * product_sans_medium.ttf — Latin/main font, set as io.FontDefault
//    * simhei_regular.ttf      — Chinese, merged in as a fallback so any CJK
//                                codepoint missing from the main font resolves
//                                to glyphs from SimHei
//
//  Nothing here is Vulkan-specific: it only feeds glyphs into the font atlas.
//  With ImGui 1.93's dynamic atlas the GPU texture is uploaded lazily by
//  ImGui_ImplVulkan_NewFrame() on the first frame, so there is no
//  backend-side font-texture step to do.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

struct ImGuiIO;

namespace aimbotng {
namespace ui {

/// Registers the embedded fonts on `io`'s atlas. Call once, after
/// ImGui::CreateContext() and before the first frame is rendered — i.e. before
/// ImGui_ImplVulkan_Init() has had a chance to build the atlas.
void loadFonts(ImGuiIO& io);

}  // namespace ui
}  // namespace aimbotng
