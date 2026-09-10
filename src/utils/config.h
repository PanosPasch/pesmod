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

// config.h
#pragma once
#include <string>
 
namespace Config
{
    void Load(const char* iniPath);
 
    // Get a string value from [section] key=value
    // Returns defaultVal if key not found
    std::string GetString(const char* section, const char* key,
                          const char* defaultVal = "");
 
    // Get a float value. Returns defaultVal if not found or not a number.
    float GetFloat(const char* section, const char* key,
                   float defaultVal = 0.0f);
 
    // Get an integer value.
    int GetInt(const char* section, const char* key,
               int defaultVal = 0);
 
    // Get a bool value (reads "1"/"true"/"yes" as true).
    bool GetBool(const char* section, const char* key,
                 bool defaultVal = false);
}

