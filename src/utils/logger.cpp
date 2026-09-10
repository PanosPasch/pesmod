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

// logger.cpp
#include "logger.h"
#include <windows.h>
#include <cstdio>
#include <ctime>
 
static FILE* g_logFile = nullptr;
static bool  g_console = false;

void Logger::Init(const char* filename, bool console)
{
    g_console = console;

    // Open log file next to the game EXE.
    // Use fopen_s for MSVC compatibility.
    fopen_s(&g_logFile, filename, "w");
    if (g_logFile)
    {
        // Write a timestamped header
        time_t t = time(nullptr);
        char timebuf[64];
        ctime_s(timebuf, sizeof(timebuf), &t);
        fprintf(g_logFile, "PESMod Log started: %s\n", timebuf);
        fflush(g_logFile);
    }

    // Only open a live console window when verbose logging is requested,
    // so a normal install does not spawn a console over the game.
    if (g_console)
    {
        AllocConsole();
        FILE* con;
        freopen_s(&con, "CONOUT$", "w", stdout);
    }
}
 
void Logger::Shutdown()
{
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
}
 
void Logger::Log(const char* fmt, ...)
{
    if (!g_logFile && !g_console) return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    if (g_logFile)
    {
        fprintf(g_logFile, "%s\n", buf);
        fflush(g_logFile); // Flush immediately so log is readable even on crash
    }

    if (g_console)
        printf("%s\n", buf);
}

