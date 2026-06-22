// logger.h
#pragma once
#include <cstdarg>
 
namespace Logger
{
    // Opens the log file. When `console` is true, also allocates a live
    // console window and echoes log lines to it (driven by verbose_logging
    // in PESMod.ini). The log file itself is always written.
    void Init(const char* filename, bool console = false);
    void Shutdown();
    void Log(const char* fmt, ...);
}

