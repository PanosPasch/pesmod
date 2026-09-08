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

    // Automatically capture once a frame contains world-space geometry.
    // [render] capture_on_first_3d = 0
    bool CaptureOnFirst3D();

    // How many world-space draws a frame needs before that trigger fires.
    //
    // A threshold of 1 fires on the *first* 3D draw anywhere, which in
    // practice means a menu: the team-select and kit-preview screens render a
    // handful of 3D elements long before kick-off, and those frames are not
    // what the ray tracer is being built for. A match frame issues several
    // hundred world-space draws, so a threshold around 100 skips the menus
    // and lands on real gameplay.
    // [render] capture_min_3d_draws = 100
    int CaptureMin3DDraws();

    // ── Scene streaming to the 64-bit render host ────────────────────────
    // Publishes the world-space draw stream over shared memory so the host
    // process can ray trace it. Independent of `capture`, but implies the
    // same interception work (resource registry, readable buffers, state
    // shadow), so enabling it turns those on too.
    // [render] stream = 0|1   (default 0)
    bool StreamEnabled();

    // Shared section name; must match the host's --section argument.
    // [render] stream_section = Local\PESMod.SceneStream
    const char* StreamSection();

    // Ring size in MB. This is address space inside the 32-bit game process,
    // which only has about 2 GB of it, so keep it modest.
    // [render] stream_ring_mb = 64
    int StreamRingMB();
}
