// render_config.h
//
// Settings for the renderer subsystem, read once from PESMod.ini's [render]
// section at startup.
//
// Everything here defaults to off. A stock PESMod.ini with no [render] section
// leaves the D3D8 interception layer completely uninstalled, so an existing
// install keeps behaving exactly as it does today.
#pragma once

namespace RenderConfig
{
    // Reads the [render] section. Call after Config::Load().
    void Load();

    // Master switch: install the D3D8 interception layer at all.
    // [render] enabled = 0|1   (default 0)
    bool Enabled();

    // Record per-frame statistics and allow single-frame captures.
    // Without this the proxy is installed but records nothing, which is the
    // cheapest way to verify the proxy itself is stable.
    // [render] capture = 0|1   (default 0)
    bool CaptureEnabled();

    // Directory captures are written to, relative to the game folder.
    // [render] capture_dir = pesmod_capture
    const char* CaptureDir();

    // Where the end-of-session markdown summary is written.
    // [render] session_report = pesmod_render_report.md
    const char* SessionReportPath();

    // Virtual-key code that arms a full single-frame capture. 0 disables.
    // [render] capture_key = 0x78   (VK_F9)
    int CaptureHotkey();

    // Automatically capture this frame index, with no key press. Useful for
    // unattended runs and for grabbing a deterministic early frame — pressing
    // a key inside a fullscreen game is not always practical. 0 disables.
    // [render] capture_at_frame = 0
    int CaptureAtFrame();

    // Automatically capture the frame after the first world-space (non
    // XYZRHW) draw appears. That is the moment the game leaves the menus and
    // starts rendering an actual 3D scene, which is the interesting one and
    // is otherwise fiddly to trigger by hand.
    // [render] capture_on_first_3d = 0
    bool CaptureOnFirst3D();
}
