// patcher.cpp
#include "patcher.h"
#include "pattern_scan.h"
#include "../utils/logger.h"
#include "../utils/config.h"
#include <cstring>
 
bool Patcher::WriteBytes(uintptr_t address, const void* data, size_t size)
{
    DWORD oldProtect;
    if (!VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Logger::Log("[Patcher] VirtualProtect FAILED at 0x%08X", address);
        return false;
    }
    memcpy((void*)address, data, size);
    VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPVOID)address, size);
    return true;
}
 
bool Patcher::NopRegion(uintptr_t address, size_t count)
{
    if (count == 0) return true;
    uint8_t* nops = new uint8_t[count];
    memset(nops, 0x90, count);
    bool r = WriteBytes(address, nops, count);
    delete[] nops;
    return r;
}
 
bool Patcher::WriteByte (uintptr_t a, uint8_t  v) { return WriteBytes(a, &v, 1); }
bool Patcher::WriteWord (uintptr_t a, uint16_t v) { return WriteBytes(a, &v, 2); }
bool Patcher::WriteDword(uintptr_t a, uint32_t v) { return WriteBytes(a, &v, 4); }
bool Patcher::WriteFloat(uintptr_t a, float    v) { return WriteBytes(a, &v, 4); }
 
bool Patcher::VerifyBytes(uintptr_t address, const uint8_t* expected, size_t size)
{
    return memcmp((const void*)address, expected, size) == 0;
}
 
// ─────────────────────────────────────────────────────────────────────────
// ApplyAll: define all direct EXE patches here.
// Each patch should:
//   1. Find its address (via pattern scan or known constant)
//   2. Optionally verify existing bytes match expectations
//   3. Apply the patch
//   4. Log success or failure
// ─────────────────────────────────────────────────────────────────────────
void Patcher::ApplyAll()
{
    Logger::Log("[Patcher] Applying patches...");
 
    // ── Patch example 1: Remove a hardcoded speed cap ─────────────────
    // Ghidra shows at 0x004A1350: COMISS XMM0, [rel constant_1_0f]
    //                             JA    0x004A1360    (jump if above 1.0)
    // This clamps a speed value to 1.0. We NOP the jump to remove the cap.
    //
    // Pattern for the JA instruction at this site:
    const char* patSpeedCapJump = "\x77\x0E";  // JA short +14
    const char* mskSpeedCapJump = "xx";
    uintptr_t addrSpeedCapJump = ScanExe(patSpeedCapJump, mskSpeedCapJump);
 
    // NOTE: A 2-byte pattern will match many places. In practice you would
    // use a longer unique pattern including the preceding COMISS instruction.
    // This is abbreviated for clarity.
 
    if (addrSpeedCapJump)
    {
        // Verify we have the expected bytes before patching
        const uint8_t expected[] = { 0x77, 0x0E };
        if (VerifyBytes(addrSpeedCapJump, expected, 2))
        {
            NopRegion(addrSpeedCapJump, 2);
            Logger::Log("[Patcher] SpeedCap JA removed at 0x%08X", addrSpeedCapJump);
        }
        else
        {
            Logger::Log("[Patcher] SpeedCap bytes mismatch — wrong EXE version? Skipped.");
        }
    }
    else
    {
        Logger::Log("[Patcher] SpeedCap pattern not found. Skipping.");
    }
 
    Logger::Log("[Patcher] Patches applied.");
}

