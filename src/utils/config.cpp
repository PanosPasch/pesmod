// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 Panagiotis Paschalis
//
// This file is part of PESMod.
//
// PESMod is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// PESMod is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along
// with PESMod.  If not, see <https://www.gnu.org/licenses/>.

// config.cpp
// Uses Windows GetPrivateProfileString for robust INI parsing.
#include "config.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
 
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
    try { return std::stoi(val); }
    catch (...) { return defaultVal; }
}
 
bool Config::GetBool(const char* section, const char* key, bool defaultVal)
{
    std::string val = GetString(section, key, defaultVal ? "1" : "0");
    // Convert to lowercase for comparison
    std::transform(val.begin(), val.end(), val.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return (val == "1" || val == "true" || val == "yes");
}

