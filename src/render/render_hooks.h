// render_hooks.h
#pragma once

namespace RenderHooks
{
    // Installs the D3D8 interception layer. A no-op unless [render] enabled=1
    // in PESMod.ini, so an install that has not opted in is untouched.
    void Register();

    // Writes the session report if one is pending. Hook removal itself is
    // handled by MinHook's global teardown in ModShutdown.
    void Unregister();
}
