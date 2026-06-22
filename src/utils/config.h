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

