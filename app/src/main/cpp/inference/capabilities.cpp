// ─────────────────────────────────────────────────────────────────────────────
//  aimbotng::infer — implementation of capabilities.h
//
//  Every field here is a *description*, never a decision. The one thing worth
//  deriving is htpArch, because the skel library's name depends on it — and even
//  that is only used to pick which file to try, not to claim a backend works.
// ─────────────────────────────────────────────────────────────────────────────
#include "inference/capabilities.h"

#include "inference/backend.h"
#include "inference/libpath.h"

#include <sys/system_properties.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace aimbotng {
namespace infer {
namespace {

std::string prop(const char* name) {
    char buf[PROP_VALUE_MAX] = {0};
    const int n = __system_property_get(name, buf);
    return (n > 0) ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

int propInt(const char* name) {
    const std::string s = prop(name);
    return s.empty() ? 0 : atoi(s.c_str());
}

/// Hexagon generation from the SoC model. Only the families this project has
/// seen are listed; anything else reports 0 and the caller says so rather than
/// guessing, because a wrong skel name is a dlopen failure two layers deeper.
int htpArchFor(const std::string& soc) {
    struct Row { const char* soc; int arch; };
    // SM8650 is the device this was developed against (HTP v75). The rest are
    // here so the same APK says something sensible on the other affected
    // handsets rather than "unknown Qualcomm".
    static const Row kRows[] = {
        {"SM8450", 68},   // 8 Gen 1
        {"SM8475", 69},   // 8+ Gen 1
        {"SM8550", 73},   // 8 Gen 2
        {"SM8650", 75},   // 8 Gen 3
        {"SM8750", 79},   // 8 Elite
    };
    for (const Row& r : kRows) {
        if (soc.rfind(r.soc, 0) == 0) return r.arch;
    }
    return 0;
}

Capabilities probe() {
    Capabilities c;
    c.socModel = prop("ro.soc.model");
    c.board    = prop("ro.board.platform");
    c.apiLevel = propInt("ro.build.version.sdk");

    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    c.cpuCores = cores > 0 ? static_cast<int>(cores) : 0;

    // Matched on either property: ro.soc.model is the modern one, but plenty of
    // Qualcomm builds still only carry the platform name.
    const std::string haystack = c.socModel + " " + c.board;
    c.isQualcomm = haystack.find("SM8") != std::string::npos ||
                   haystack.find("qcom") != std::string::npos ||
                   haystack.find("pineapple") != std::string::npos ||
                   haystack.find("kalama") != std::string::npos ||
                   haystack.find("waipio") != std::string::npos ||
                   haystack.find("cape") != std::string::npos;
    c.isMediaTek = haystack.find("MT") != std::string::npos ||
                   haystack.find("mediatek") != std::string::npos;

    if (c.isQualcomm) c.htpArch = htpArchFor(c.socModel);

    return c;
}

std::mutex     g_mutex;
Capabilities   g_caps;
bool           g_probed = false;
std::string    g_report;

}  // namespace

const Capabilities& capabilities() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_probed) {
        g_caps = probe();
        g_probed = true;
    }
    return g_caps;
}

void resetCapabilities() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_probed = false;
    g_report.clear();
}

std::string capabilitiesReport() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_report.empty()) return g_report;
    }

    const Capabilities& c = capabilities();

    std::string out;
    out.reserve(1024);

    char line[256];
    snprintf(line, sizeof(line), "device   %s / %s  api=%d  cores=%d\n",
             c.socModel.empty() ? "(unknown)" : c.socModel.c_str(),
             c.board.empty() ? "(unknown)" : c.board.c_str(),
             c.apiLevel, c.cpuCores);
    out += line;

    if (c.isQualcomm) {
        if (c.htpArch > 0) {
            snprintf(line, sizeof(line), "htp      arch v%d\n", c.htpArch);
        } else {
            snprintf(line, sizeof(line), "htp      qualcomm, but no arch mapped\n");
        }
        out += line;
    }
    if (c.isMediaTek) out += "soc      MediaTek\n";
    snprintf(line, sizeof(line), "libdir   %s\n",
             libraryDir()[0] ? libraryDir() : "(unresolved)");
    out += line;

    out += "\nbackends\n";
    int count = 0;
    const Pair* pairs = allPairs(count);
    for (int i = 0; i < count; ++i) {
        const bool ok = epAvailable(pairs[i].runtime, pairs[i].ep);
        const char* name = pairLabel(pairs[i].runtime, pairs[i].ep);
        if (ok) {
            snprintf(line, sizeof(line), "  ok       %s\n", name);
        } else {
            snprintf(line, sizeof(line), "  no       %-34s %s\n", name,
                     epUnavailableReason(pairs[i].runtime, pairs[i].ep));
        }
        out += line;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_report = out;
    }
    return out;
}

bool isSupportedDevice() {
    const Capabilities& c = capabilities();
    return c.isQualcomm && c.htpArch > 0;
}

}  // namespace infer
}  // namespace aimbotng
