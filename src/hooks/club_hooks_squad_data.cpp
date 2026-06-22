// =============================================================================
// club_hooks_squad_data.cpp
//
// Hooks for the squad / player-record pipeline. Companion to
// club_hooks_kit_data.cpp; same INI-first / trampoline-fallback pattern.
//
// Hooked functions (signatures verified against Ghidra decompiles —
// see docs/INTERNALS.md "Squad-data pipeline" section):
//
//   FUN_00862710  GetTeamPlayerID    — squad slot (teamID, slot, mode) → playerID
//   FUN_00861b90  GetPlayerRecord    — playerID → 0x7c-byte record pointer
//                                        (the worker; the wrapper at 0x00861d20
//                                        is barely called during a match — see
//                                        hook_GetPlayerRecord_Naked docblock)
//   FUN_00862c10  GetTeamPlayerAttr  — per-slot squad-number / position byte
//   FUN_00862200  SetTeamPlayerAttr  — companion writer for the above
//   FUN_00861ad0  SetTeamPlayerID    — squad-slot writer (alias-copy path)
//
// Why we hook (study notes for future modders):
//   PES6's stock player-ID table covers teamIDs 0x40..0xCB (clubs) and
//   nationals 0x112..0x117 plus aliases 0x126/0x127. Anything outside
//   those ranges either reads garbage (the type-3 fallback in
//   ClassifyTeamID_ForPlayerLookup at 0x00861850 has no bounds check and
//   blindly indexes the national-team table) or returns 0 ("no player").
//
//   Custom teams 200-203 (0xC8-0xCB) sit *inside* the club range and
//   resolve normally to slots in the player-ID table — but those slots
//   are unpopulated in the stock OPT blob, so GetTeamPlayerID returns 0
//   and the match starts with NULL players. Custom teams 0xCC+ would
//   read garbage.
//
//   The fix: an INI under `squad/<teamID>.ini` declares the 32 player
//   IDs for the team's roster; an INI under `player/<id>.ini` declares
//   a synthetic 0x7c-byte player record for any custom player ID
//   (preferably in the 0x8000-0x80B7 range — 184 reserved slots that
//   the stock binary never populates).
//
//   The hooks short-circuit on INI hit, otherwise forward to the
//   trampoline. Cache entries are pointer-stable (std::unordered_map
//   guarantees iterator stability across other-key insertions), and we
//   never evict — downstream callers may keep the returned pointer for
//   the lifetime of a frame. Same reasoning as KitCacheEntry in
//   club_hooks_kit_data.cpp.
//
// INI formats — see LoadSquadIni and LoadPlayerIni docblocks for full
// keysets. Both INIs live under the game's CWD:
//
//   squad/<teamID>.ini       slot_0..31 = <playerID>; attr_0..31 = <byte>
//   player/<playerID>.ini    template / name / shirt_name / callname_id /
//                            typed stats (attack..gk_skills, height,
//                            weight) / special_team / byte_<off> overrides
//
// Cache entries are NEVER evicted (pointer stability), and the file is
// only re-read on first miss — to pick up edits without restarting,
// either add a "/reload" command in the future or restart the game.
//
// Write hooks (SetTeamPlayerAttr / SetTeamPlayerID): pass through. Live
// edits don't invalidate the read cache because the cache is sourced
// from the INI, not from the in-engine table.
// =============================================================================

#include "club_hooks_common.h"
#include "../utils/logger.h"

#include <unordered_map>
#include <array>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Forward decl for the naked trampoline-call helper defined further down.
// LoadPlayerIni (anonymous namespace, above) and hook_GetPlayerRecord_C
// (below) both call it. The body sits next to hook_GetPlayerRecord_Naked
// because the two share calling-convention rationale.
extern "C" void* CallOrigGetPlayerRecord(uint16_t playerID, int bank);

// =============================================================================
// Constants
// =============================================================================

// Number of player slots in a team squad (matches the loop bound used by
// AssignTeamToAlias at 0x006b22d0 and the slotIdx<0x20 guard inside
// GetTeamPlayerID).
static constexpr int    SQUAD_SLOT_COUNT = 32;

// Player record stride (matches the memcpy length used by AssignTeamToAlias
// — 31 dwords = 0x7c bytes — see docs/INTERNALS.md).
static constexpr size_t PLAYER_RECORD_SIZE = 0x7C;

// Reserved custom-player ID range. FUN_00861b90 reads this range from
// DAT_01131ff8 (resolved via FUN_00866ba0); 184 slots × 0x7c bytes.
// Recommended home for hook-injected custom players, but not enforced
// by the hook — any playerID outside the stock table is a candidate.
static constexpr uint16_t CUSTOM_PLAYER_ID_BASE = 0x8000;
static constexpr uint16_t CUSTOM_PLAYER_ID_END  = 0x80B8;  // exclusive

// Sentinel used in squad INI to mean "leave this slot empty" — the hook
// will fall through to the trampoline for the slot in that case so the
// stock behaviour (returns 0 → NULL player) is preserved.
static constexpr uint16_t SQUAD_SLOT_EMPTY = 0xFFFF;

// =============================================================================
// SECTION — Squad cache (one entry per teamID)
//
// SquadCacheEntry mirrors KitCacheEntry's positive/negative-cache shape:
//   loaded == true  → INI was parsed; `slots` has the 32 playerIDs
//   loaded == false → "we already checked, no INI exists" — never erased
//
// Pointer stability is required by callers that may keep a pointer into
// `slots` between frames. std::unordered_map guarantees stability across
// insertions of other keys; we never erase.
// =============================================================================

namespace {

struct SquadCacheEntry {
    bool                                       loaded;
    std::array<uint16_t, SQUAD_SLOT_COUNT>     slots;       // playerIDs
    std::array<uint8_t,  SQUAD_SLOT_COUNT>     attrs;       // squad-number/pos byte
    std::array<bool,     SQUAD_SLOT_COUNT>     attrPresent; // attr declared in INI?
};

std::unordered_map<uint32_t, SquadCacheEntry> g_squadCache;
std::mutex                                     g_squadCacheMutex;

// =============================================================================
// SECTION — Player-record cache (one entry per playerID)
//
// Each entry owns a 0x7c-byte synthesized record. The buffer's address is
// what we hand back to the engine, so it must remain valid forever (same
// pointer-stability constraint as KitCacheEntry::buffer).
// =============================================================================

struct PlayerCacheEntry {
    bool                                       loaded;
    std::array<uint8_t, PLAYER_RECORD_SIZE>    record;
};

std::unordered_map<uint16_t, PlayerCacheEntry> g_playerCache;
std::mutex                                      g_playerCacheMutex;

// =============================================================================
// SECTION — INI helpers (lightweight wrappers around Win32 profile API)
//
// We re-declare these locally instead of sharing with club_hooks_kit_data.cpp
// to keep the two files independent (the kit-data versions are in an
// anonymous namespace there). Both wrap GetPrivateProfileStringA the same
// way so the behaviour is identical.
// =============================================================================

bool IniExists(const char* path)
{
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadIniString(const char* path, const char* section, const char* key,
                   char* out, DWORD outSize)
{
    DWORD n = GetPrivateProfileStringA(section, key, "", out, outSize, path);
    return n > 0 && out[0] != '\0';
}

// Parse "0x80A3", "32931", "0123" with auto-base detection. Returns
// `fallback` on parse failure or when the string is empty/missing.
uint32_t ParseUInt(const char* text, uint32_t fallback)
{
    if (!text || !*text) return fallback;
    while (*text == ' ' || *text == '\t') ++text;
    char* endp = nullptr;
    unsigned long v = std::strtoul(text, &endp, 0);
    if (endp == text) return fallback;
    return static_cast<uint32_t>(v);
}

// Recognise "true"/"false" (case-insensitive, with common synonyms).
// Returns 1 for true, 0 for false, -1 for "not a boolean" so callers
// can fall through to numeric parsing.
int ParseBool(const char* text)
{
    if (!text || !*text) return -1;
    while (*text == ' ' || *text == '\t') ++text;
    auto ieq = [](const char* a, const char* b) {
        for (;;) {
            unsigned char ca = static_cast<unsigned char>(*a);
            unsigned char cb = static_cast<unsigned char>(*b);
            if (ca >= 'A' && ca <= 'Z') ca = ca + 0x20;
            if (cb >= 'A' && cb <= 'Z') cb = cb + 0x20;
            if (ca != cb) return false;
            if (ca == 0)  return true;
            ++a; ++b;
        }
    };
    if (ieq(text, "true")  || ieq(text, "yes") || ieq(text, "on"))  return 1;
    if (ieq(text, "false") || ieq(text, "no")  || ieq(text, "off")) return 0;
    return -1;
}

// =============================================================================
// SECTION — Cache resolution (lazy load + negative-cache)
//
// EnsureSquadCacheEntry / EnsurePlayerCacheEntry are the single entry
// points the hooks consult. They lock, look up, and on first miss load
// the INI (or record a negative entry). Returns nullptr when no INI
// exists — callers MUST treat nullptr as "fall through to trampoline".
// =============================================================================

// Load 32 slot_N keys + 32 attr_N keys from `squad/<teamID>.ini` into
// the entry. Returns true if the INI exists and was parsed (entry.loaded
// is set on the caller side). Missing keys leave their slot at the
// empty sentinel so the hook falls through to the trampoline for those
// slots — i.e., a partial INI overrides only what it declares.
//
// INI format:
//
//   [squad]
//   slot_0   = 0x8000        ; player ID for squad slot 0
//   slot_1   = 0x8001        ; player ID for squad slot 1
//   ...
//   slot_31  = 0x801F        ; player ID for squad slot 31
//   attr_0   = 1             ; squad number / position byte (Step 4)
//   ...
//
// Slots accept any base (0x8000, 32768, 0100000) via std::strtoul base 0.
// Slot value of 0xFFFF means "leave empty / fall through to original".
static bool LoadSquadIni(uint32_t teamID, SquadCacheEntry& entry)
{
    char iniPath[MAX_PATH];
    std::snprintf(iniPath, sizeof(iniPath), ".\\squad\\%u.ini",
                   static_cast<unsigned>(teamID));
    if (!IniExists(iniPath)) return false;

    char val[64];
    char key[32];

    for (int slot = 0; slot < SQUAD_SLOT_COUNT; ++slot) {
        std::snprintf(key, sizeof(key), "slot_%d", slot);
        if (ReadIniString(iniPath, "squad", key, val, sizeof(val))) {
            uint32_t v = ParseUInt(val, SQUAD_SLOT_EMPTY);
            entry.slots[slot] = static_cast<uint16_t>(v & 0xFFFF);
        }

        std::snprintf(key, sizeof(key), "attr_%d", slot);
        if (ReadIniString(iniPath, "squad", key, val, sizeof(val))) {
            uint32_t v = ParseUInt(val, 0);
            entry.attrs[slot]       = static_cast<uint8_t>(v & 0xFF);
            entry.attrPresent[slot] = true;
        }
    }

    Logger::Log("[SquadData] Loaded squad config for teamID=%u from %s",
                static_cast<unsigned>(teamID), iniPath);
    return true;
}

SquadCacheEntry* EnsureSquadCacheEntry(uint32_t teamID)
{
    std::lock_guard<std::mutex> lock(g_squadCacheMutex);

    auto it = g_squadCache.find(teamID);
    if (it != g_squadCache.end()) {
        return it->second.loaded ? &it->second : nullptr;
    }

    SquadCacheEntry entry{};
    entry.loaded = false;
    entry.slots.fill(SQUAD_SLOT_EMPTY);
    entry.attrs.fill(0);
    entry.attrPresent.fill(false);

    if (LoadSquadIni(teamID, entry)) {
        entry.loaded = true;
    }

    auto result = g_squadCache.emplace(teamID, std::move(entry));
    SquadCacheEntry& stored = result.first->second;
    return stored.loaded ? &stored : nullptr;
}

// Table of typed u8 stat keys. Every entry maps `<key>` in the INI to
// a single byte at the given offset. Keep ordered by offset for easy
// cross-referencing with docs/INTERNALS.md.
//
// The mapping below comes from field-mapping work tracked in the
// the internals notes "Player record layout" section. Several of these bytes have
// a high-bit flag (positions / movement etc.) that's not yet decoded;
// the `byte_<off>` escape hatch can OR/AND that bit explicitly via
// `= true` / `= false` (see step 6 below).
struct PlayerStatField {
    size_t       off;
    const char*  key;
};

static const PlayerStatField kPlayerStatFields[] = {
    { 0x36, "attack"              },
    { 0x37, "defence"             },
    { 0x38, "balance"             },
    { 0x39, "stamina"             },
    { 0x3a, "speed"               },
    { 0x3b, "acceleration"        },
    { 0x3c, "response"            },
    { 0x3d, "agility"             },
    { 0x3e, "dribble_accuracy"    },
    { 0x3f, "dribble_speed"       },
    { 0x40, "short_pass_accuracy" },
    { 0x41, "short_pass_speed"    },
    { 0x42, "long_pass_accuracy"  },
    { 0x43, "long_pass_speed"     },
    { 0x44, "shot_accuracy"       },
    { 0x45, "shot_power"          },
    { 0x46, "shot_technique"      },
    { 0x47, "free_kick_accuracy"  },
    { 0x48, "swerve"              },
    { 0x49, "heading"             },
    { 0x4a, "jump"                },
    { 0x4b, "teamwork"            },
    { 0x4c, "technique"           },
    { 0x4d, "aggression"          },
    { 0x4e, "mentality"           },
    { 0x4f, "gk_skills"           },
    { 0x58, "height"              },
    { 0x59, "weight"              },
};

// Load a player record from `player/<playerID>.ini`. Returns true if
// the INI exists and was parsed.
//
// INI format (all keys optional):
//
//   [player]
//   template     = 1            ; clone bytes 0..0x7B from this stock player
//                                ; (default = 1; use -1 / 0xFFFFFFFF to skip
//                                ; cloning and start from all-zero bytes)
//   name         = "John Doe"   ; UTF-16 long name at +0x00 (max 15 wchars
//                                ; + NUL — 0x20 bytes total). Source bytes
//                                ; are interpreted as the system ANSI
//                                ; codepage.
//   shirt_name   = "DOE"        ; ASCII shirt-name at +0x20 (max 15 + NUL;
//                                ; 0x10 bytes total)
//   callname_id  = 0            ; u16 at +0x30 (call-name index)
//   <stat>       = 80           ; u8 stat (see kPlayerStatFields below for
//                                ; the full list — attack, defence, ...,
//                                ; gk_skills, height, weight)
//   special_team = 0            ; u8  at +0x6F (per RE notes)
//   byte_<off>   = <0..255>     ; raw byte override at <off> (0..0x7B); off
//                                ; accepts any base. Whole-byte set form.
//   byte_<off>   = true / false ; bitwise high-bit (0x80) set / clear,
//                                ; preserving the lower 7 bits. Useful for
//                                ; the still-unmapped position/movement
//                                ; flags packed into the high bit of stat
//                                ; bytes.
//
// Cloning the template first means a custom playerID inherits a fully-
// formed (and therefore non-crashing) stats block by default; the user
// only needs to override fields they care about.
static bool LoadPlayerIni(uint16_t playerID, PlayerCacheEntry& entry)
{
    char iniPath[MAX_PATH];
    std::snprintf(iniPath, sizeof(iniPath), ".\\player\\%u.ini",
                   static_cast<unsigned>(playerID));
    if (!IniExists(iniPath)) return false;

    char val[256];

    // 1) Template clone — copy 0x7c bytes from a stock player record.
    //    Default template is playerID 1 (first stock player; ID 0 is
    //    sometimes a sentinel/null in PES tables).
    uint32_t templateID = 1;
    if (ReadIniString(iniPath, "player", "template", val, sizeof(val))) {
        templateID = ParseUInt(val, 1);
    }

    bool zeroFill = (templateID == 0xFFFFFFFFu);
    if (!zeroFill && orig_GetPlayerRecord) {
        // bank=0 — match the default match-flow read path. We can't call
        // the trampoline as a C function because it expects ECX=playerID
        // and EAX=bank (no stack args). CallOrigGetPlayerRecord is the
        // naked thunk that sets up those registers before tail-jumping
        // in. The trampoline is the un-hooked code path, so this never
        // re-enters our hook (and therefore never tries to re-lock the
        // cache mutex we're holding).
        void* src = CallOrigGetPlayerRecord(
            static_cast<uint16_t>(templateID), 0);
        if (src) {
            std::memcpy(entry.record.data(), src, PLAYER_RECORD_SIZE);
        }
    }

    // 2) Long player name — UTF-16 at +0x00 (0x20 bytes = 16 wchars total,
    //    so 15 chars + NUL). INI value is read as the system ANSI codepage
    //    and converted via MultiByteToWideChar so accented characters in
    //    Latin-1 / Latin-2 codepages survive.
    if (ReadIniString(iniPath, "player", "name", val, sizeof(val))) {
        constexpr size_t kNameOff   = 0x00;
        constexpr size_t kNameBytes = 0x20;
        constexpr int    kMaxChars  = static_cast<int>(kNameBytes / sizeof(wchar_t));
        wchar_t wide[kMaxChars];
        std::memset(wide, 0, sizeof(wide));
        // Cap the converted length at kMaxChars - 1 so we always leave the
        // last wchar as 0 for NUL termination.
        MultiByteToWideChar(CP_ACP, 0, val, -1, wide, kMaxChars - 1);
        wide[kMaxChars - 1] = 0;
        std::memcpy(entry.record.data() + kNameOff, wide, kNameBytes);
    }

    // 3) Shirt name (ASCII) at +0x20 (16 bytes — 15 + NUL).
    if (ReadIniString(iniPath, "player", "shirt_name", val, sizeof(val))) {
        constexpr size_t kShirtOff = 0x20;
        constexpr size_t kShirtLen = 0x10;
        size_t i = 0;
        while (i < kShirtLen - 1 && val[i]) {
            entry.record[kShirtOff + i] = static_cast<uint8_t>(val[i]);
            ++i;
        }
        while (i < kShirtLen) {
            entry.record[kShirtOff + i] = 0;
            ++i;
        }
    }

    // 4) Callname ID (u16 at +0x30).
    if (ReadIniString(iniPath, "player", "callname_id", val, sizeof(val))) {
        uint32_t v = ParseUInt(val, 0);
        *reinterpret_cast<uint16_t*>(entry.record.data() + 0x30) =
            static_cast<uint16_t>(v & 0xFFFF);
    }

    // 5) Typed u8 stat keys (see kPlayerStatFields). Each writes the whole
    //    byte; high-bit flags can be toggled afterwards via byte_<off>.
    for (const auto& f : kPlayerStatFields) {
        if (ReadIniString(iniPath, "player", f.key, val, sizeof(val))) {
            uint32_t v = ParseUInt(val, 0);
            entry.record[f.off] = static_cast<uint8_t>(v & 0xFF);
        }
    }

    // 6) Special-team byte at +0x6F.
    if (ReadIniString(iniPath, "player", "special_team", val, sizeof(val))) {
        uint32_t v = ParseUInt(val, 0);
        entry.record[0x6F] = static_cast<uint8_t>(v & 0xFF);
    }

    // 7) Raw byte overrides — `byte_<offset>` for any offset in
    //    [0, 0x7B]. The offset accepts any base (decimal, 0xNN, 0NNN).
    //    Two value forms:
    //      `= <number>`    sets the entire byte to that value
    //      `= true|false`  sets / clears the high bit (0x80) only,
    //                       preserving the lower 7 bits — useful when
    //                       a typed stat key (step 5) just wrote the
    //                       lower stat value and we want to flip the
    //                       still-unmapped position/movement flag.
    //    This is the escape hatch for any byte we don't have a typed
    //    key for, AND the flag-bit handle for ones we do.
    for (size_t off = 0; off < PLAYER_RECORD_SIZE; ++off) {
        char key[16];

        auto applyByteVal = [&](const char* text) {
            int b = ParseBool(text);
            if (b == 1) {
                entry.record[off] |= 0x80;
            } else if (b == 0) {
                entry.record[off] &= 0x7F;
            } else {
                uint32_t v = ParseUInt(text, 0);
                entry.record[off] = static_cast<uint8_t>(v & 0xFF);
            }
        };

        std::snprintf(key, sizeof(key), "byte_0x%02X",
                       static_cast<unsigned>(off));
        if (ReadIniString(iniPath, "player", key, val, sizeof(val))) {
            applyByteVal(val);
            continue;
        }
        // Also accept decimal-suffix form (`byte_50`) for convenience.
        std::snprintf(key, sizeof(key), "byte_%u",
                       static_cast<unsigned>(off));
        if (ReadIniString(iniPath, "player", key, val, sizeof(val))) {
            applyByteVal(val);
        }
    }

    Logger::Log("[SquadData] Loaded player config for playerID=%u from %s "
                "(template=%u, zeroFill=%d)",
                static_cast<unsigned>(playerID), iniPath,
                static_cast<unsigned>(templateID), zeroFill ? 1 : 0);
    return true;
}

PlayerCacheEntry* EnsurePlayerCacheEntry(uint16_t playerID)
{
    std::lock_guard<std::mutex> lock(g_playerCacheMutex);

    auto it = g_playerCache.find(playerID);
    if (it != g_playerCache.end()) {
        return it->second.loaded ? &it->second : nullptr;
    }

    PlayerCacheEntry entry{};
    entry.loaded = false;
    entry.record.fill(0);

    if (LoadPlayerIni(playerID, entry)) {
        entry.loaded = true;
    }

    auto result = g_playerCache.emplace(playerID, std::move(entry));
    PlayerCacheEntry& stored = result.first->second;
    return stored.loaded ? &stored : nullptr;
}

}  // namespace

// =============================================================================
// SECTION — Hooks
//
// Read hooks (GetTeamPlayerID / GetPlayerRecord / GetTeamPlayerAttr)
// consult the cache first; cache miss falls through to the trampoline.
// Write hooks (SetTeamPlayerID / SetTeamPlayerAttr) are unconditional
// pass-throughs (the read cache is sourced from INI, not from live
// engine state, so writes don't need to invalidate it).
// =============================================================================

uint32_t __cdecl hook_GetTeamPlayerID(uint32_t teamID, uint8_t slotIdx,
                                        int mode)
{
    // INI-first: if `squad/<teamID>.ini` declares a non-empty player ID
    // for this slot, return that as a u16 in the low half of the eax
    // register (matching the original's CONCAT22(garbage, ushort) shape).
    // The original returns a uint where the high 16 bits are unspecified
    // garbage from caller register state — we just zero them.
    if (slotIdx < SQUAD_SLOT_COUNT) {
        if (SquadCacheEntry* e = EnsureSquadCacheEntry(teamID)) {
            uint16_t pid = e->slots[slotIdx];
            if (pid != SQUAD_SLOT_EMPTY) {
                return static_cast<uint32_t>(pid);
            }
        }
    }
    return orig_GetTeamPlayerID(teamID, slotIdx, mode);
}

// hook_GetPlayerRecord_C is the cdecl C-side of the GetPlayerRecord hook.
// The naked thunk hook_GetPlayerRecord_Naked (defined below) is what
// MinHook actually patches in at FUN_00861b90; the thunk pushes the
// register-only original args (ECX=playerID, EAX=bank) onto the stack
// and forwards into this handler.
extern "C" void* __cdecl hook_GetPlayerRecord_C(uint16_t playerID, int bank)
{
    // INI-first: if `player/<playerID>.ini` exists, return a pointer
    // into the cache's stable buffer. Pointer-stability is critical:
    // the engine may keep the returned pointer across frames (matches
    // hold per-player pointers in the active squad arrays), so we
    // never erase cache entries.
    //
    // We honour `bank` only for the trampoline fallback: our cache is
    // a single record per playerID, not per (playerID, bank). If a
    // future feature needs per-bank synthetic records, extend the
    // cache key to (playerID, bank).
    if (PlayerCacheEntry* e = EnsurePlayerCacheEntry(playerID)) {
        return e->record.data();
    }
    return CallOrigGetPlayerRecord(playerID, bank);
}

// =============================================================================
// hook_GetPlayerRecord_Naked  — __declspec(naked) thunk
//
// Bridges the original FUN_00861b90's register-only convention
// (ECX = playerID, EAX = bank, no stack args, returns in EAX) into
// our cdecl C handler. Pattern mirrors hook_FillKitTeamSlot_Naked in
// club_hooks_kit_data.cpp.
//
// On entry from the MinHook patch:
//   [esp+0] = return address (back to the game caller)
//   ECX     = playerID
//   EAX     = bank
//
// Cdecl pushes args right-to-left, so we push bank first (becomes arg2)
// then playerID (becomes arg1), CALL the handler, clean up the 8 bytes,
// and RET. The game caller used a register-only convention — no stack
// args to clean up — so a plain `ret` is correct.
//
// EAX returns the void* result naturally (the C handler's return value
// is already in EAX after the CALL completes).
// =============================================================================
extern "C" __declspec(naked) void hook_GetPlayerRecord_Naked()
{
    __asm
    {
        push eax                  // bank      (cdecl arg 2)
        push ecx                  // playerID  (cdecl arg 1)
        call hook_GetPlayerRecord_C
        add  esp, 8
        ret
    }
}

// =============================================================================
// CallOrigGetPlayerRecord — naked helper that calls the trampoline.
//
// The trampoline pointer expects the original's register-only convention.
// This helper takes ordinary cdecl args, sets ECX/EAX, and TAIL-JUMPS
// (not CALLs) into the trampoline. The trampoline's RET returns directly
// to whoever called us — in cdecl that means our caller cleans up the
// 2 stack args via its own `add esp, 8`.
//
// We zero-extend playerID from its 16-bit slot so the original's
// later use of ECX/EDI doesn't see garbage in the upper half.
// =============================================================================
extern "C" __declspec(naked) void* CallOrigGetPlayerRecord(
    uint16_t /*playerID*/, int /*bank*/)
{
    __asm
    {
        movzx ecx, word ptr [esp + 4]
        mov   eax, dword ptr [esp + 8]
        jmp   dword ptr [orig_GetPlayerRecord]
    }
}

uint8_t __cdecl hook_GetTeamPlayerAttr(uint32_t teamID, uint32_t slotIdx,
                                         int mode)
{
    // INI-first: if `squad/<teamID>.ini` declares an `attr_<slot>` key,
    // return that byte. The squad cache also drives this — same entry,
    // separate `attrPresent` flag so we can distinguish "not in INI"
    // from "explicitly set to 0".
    if (slotIdx < SQUAD_SLOT_COUNT) {
        if (SquadCacheEntry* e = EnsureSquadCacheEntry(teamID)) {
            if (e->attrPresent[slotIdx]) {
                return e->attrs[slotIdx];
            }
        }
    }
    return orig_GetTeamPlayerAttr(teamID, slotIdx, mode);
}

void __cdecl hook_SetTeamPlayerAttr(uint32_t teamID, uint32_t slotIdx,
                                      int mode, uint32_t value)
{
    // Writes are forwarded unconditionally — modifying the attr live
    // doesn't disturb our cache (the cache only feeds reads). If a
    // future feature wants to persist edits back to the INI we'd
    // intercept here.
    orig_SetTeamPlayerAttr(teamID, slotIdx, mode, value);
}

void __cdecl hook_SetTeamPlayerID(uint32_t teamID, uint8_t slotIdx,
                                    uint16_t playerID)
{
    // Same rationale as hook_SetTeamPlayerAttr: writes pass through.
    orig_SetTeamPlayerID(teamID, slotIdx, playerID);
}
