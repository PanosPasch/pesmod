// render_config.cpp
#include "render_config.h"
#include "../utils/config.h"
#include "../utils/logger.h"

#include <string>

namespace
{
    bool        g_loaded         = false;
    bool        g_enabled        = false;
    bool        g_capture        = false;
    int         g_captureKey     = 0x78;              // VK_F9
    int         g_captureAtFrame = 0;                 // 0 = no auto capture
    bool        g_captureFirst3D = false;
    int         g_captureMin3D   = 100;
    std::string g_captureDir     = "pesmod_capture";
    std::string g_reportPath     = "pesmod_render_report.md";
}

namespace RenderConfig
{

void Load()
{
    g_enabled        = Config::GetBool("render", "enabled", false);
    g_capture        = Config::GetBool("render", "capture", false);
    g_captureKey     = Config::GetInt ("render", "capture_key", 0x78);
    g_captureAtFrame = Config::GetInt ("render", "capture_at_frame", 0);
    g_captureFirst3D = Config::GetBool("render", "capture_on_first_3d", false);
    g_captureMin3D   = Config::GetInt ("render", "capture_min_3d_draws", 100);

    const std::string dir = Config::GetString("render", "capture_dir",
                                              "pesmod_capture");
    if (!dir.empty()) g_captureDir = dir;

    const std::string report = Config::GetString("render", "session_report",
                                                 "pesmod_render_report.md");
    if (!report.empty()) g_reportPath = report;

    g_loaded = true;

    if (g_enabled)
        Logger::Log("[Render] Config: capture=%d key=0x%02X atFrame=%d "
                    "onFirst3D=%d min3D=%d dir='%s' report='%s'",
                    g_capture ? 1 : 0, g_captureKey, g_captureAtFrame,
                    g_captureFirst3D ? 1 : 0, g_captureMin3D,
                    g_captureDir.c_str(), g_reportPath.c_str());
}

bool        Enabled()           { return g_loaded && g_enabled; }
bool        CaptureEnabled()    { return g_loaded && g_enabled && g_capture; }
const char* CaptureDir()        { return g_captureDir.c_str(); }
const char* SessionReportPath() { return g_reportPath.c_str(); }
int         CaptureHotkey()     { return g_captureKey; }
int         CaptureAtFrame()    { return g_captureAtFrame; }
bool        CaptureOnFirst3D()  { return g_captureFirst3D; }
int         CaptureMin3DDraws() { return g_captureMin3D < 1 ? 1 : g_captureMin3D; }

} // namespace RenderConfig
