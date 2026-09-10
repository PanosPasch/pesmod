// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 Panagiotis Paschalis
//
// This file is part of PESMod.
//
// PESMod is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// PESMod is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with PESMod.  If not, see <https://www.gnu.org/licenses/>.

// config.cpp
// Uses Windows GetPrivateProfileString for robust INI parsing.
#include "config.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
 
static char g_iniPath[MAX_PATH] = {};
 
void Config::Load(const char* iniPath)
{
    // Resolve to absolute path relative to the game EXE directory
    GetFullPathNameA(iniPath, MAX_PATH, g_iniPath, nullptr);
}
 
std::string Config::GetString(
    const char* section, const char* key, const char* defaultVal)
{
    char buf[512];
    GetPrivateProfileStringA(section, key, defaultVal, buf, sizeof(buf), g_iniPath);
    return std::string(buf);
}
 
float Config::GetFloat(const char* section, const char* key, float defaultVal)
{
    std::string val = GetString(section, key, "");
    if (val.empty()) return defaultVal;
    try { return std::stof(val); }
    catch (...) { return defaultVal; }
}
 
int Config::GetInt(const char* section, const char* key, int defaultVal)
{
    std::string val = GetString(section, key, "");
    if (val.empty()) return defaultVal;

    // Accept both decimal and 0x-prefixed hex. std::stoi defaults to base 10,
    // which parses "0x78" as 0 and stops at the 'x' — silently wrong rather
    // than throwing, so a hex value would quietly become zero.
    //
    // Base is chosen explicitly rather than passing 0 to strtol, because
    // base 0 would also treat a leading zero as octal and turn a value like
    // "0120" into 80.
    // strtol consumes the "0x" prefix itself once base 16 is selected.
    const size_t digitStart = (val[0] == '-' || val[0] == '+') ? 1u : 0u;
    const bool isHex = val.size() > digitStart + 1 &&
                       val[digitStart] == '0' &&
                       (val[digitStart + 1] == 'x' || val[digitStart + 1] == 'X');
    const int base = isHex ? 16 : 10;

    char* end = nullptr;
    const long parsed = strtol(val.c_str(), &end, base);
    if (end == val.c_str()) return defaultVal;   // nothing numeric at all
    return (int)parsed;
}
 
bool Config::GetBool(const char* section, const char* key, bool defaultVal)
{
    std::string val = GetString(section, key, defaultVal ? "1" : "0");
    // Convert to lowercase for comparison
    std::transform(val.begin(), val.end(), val.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return (val == "1" || val == "true" || val == "yes");
}

