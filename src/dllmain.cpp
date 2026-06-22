// dllmain.cpp
//
// Entry point for the PESMod ASI.
// The ASI Loader calls LoadLibraryA on this file, which triggers DllMain.
//
#include <windows.h>
#include "mod_core.h"
 
BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        //
        // DLL_PROCESS_ATTACH fires when LoadLibraryA is called by the ASI Loader.
        // At this point the game's EXE is fully loaded in memory and all other
        // import DLLs (DirectX, etc.) are already present. This is the ideal
        // moment to install hooks — the game code exists in memory but the
        // main game loop has not yet started.
        //

        DisableThreadLibraryCalls(hModule); // Suppress DLL_THREAD_ATTACH/DETACH
        ModInitialise(hModule);
        break;
 
    case DLL_PROCESS_DETACH:
        //
        // DLL_PROCESS_DETACH fires when the process exits or if someone calls
        // FreeLibrary on our module. We clean up hooks here.
        // Note: lpReserved is non-NULL if the process is terminating —
        // in that case the OS will clean up for us and we should do minimal work.
        //
        if (lpReserved == nullptr)
        {
            // Clean FreeLibrary path — safe to clean up
            ModShutdown();
        }
        break;
    }
    return TRUE;
}

