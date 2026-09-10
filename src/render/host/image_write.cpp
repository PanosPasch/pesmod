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

// image_write.cpp
#include "image_write.h"

#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Host
{
namespace
{
    bool EndsWithNoCase(const char* s, const char* suffix)
    {
        const size_t ls = strlen(s), lx = strlen(suffix);
        return ls >= lx && _stricmp(s + ls - lx, suffix) == 0;
    }

    // Releases a COM interface and clears the pointer. The WIC path acquires
    // five of these and has to unwind them on every failure edge.
    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->Release(); p = nullptr; }
    }

    // Creates the directories leading to `path` if they are missing. Without
    // this, --save-path pointing somewhere sensible but not yet existing
    // fails per frame with nothing to suggest the folder is the problem.
    void EnsureParentDirectory(const char* path)
    {
        std::string dir(path);
        const size_t slash = dir.find_last_of("\\/");
        if (slash == std::string::npos) return;      // bare filename, cwd
        dir.resize(slash);

        // Walk forwards so each level exists before the next is attempted.
        // Existing levels and drive roots both fail harmlessly.
        for (size_t i = 1; i <= dir.size(); ++i)
        {
            if (i != dir.size() && dir[i] != '\\' && dir[i] != '/') continue;
            CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
        }
    }
}

bool WriteImagePpm(const char* path, const uint8_t* rgba,
                   uint32_t width, uint32_t height)
{
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) return false;

    fprintf(f, "P6\n%u %u\n255\n", width, height);

    std::vector<uint8_t> row((size_t)width * 3);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t s = ((size_t)y * width + x) * 4;
            row[x * 3 + 0] = rgba[s + 0];
            row[x * 3 + 1] = rgba[s + 1];
            row[x * 3 + 2] = rgba[s + 2];
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

bool WriteImagePng(const char* path, const uint8_t* rgba,
                   uint32_t width, uint32_t height)
{
    // WIC rather than a vendored encoder: it is part of Windows, this host is
    // Windows-only by construction, and the 32-bit side already depends on it
    // for the decode direction (see custom_logo_loader.cpp).

    // COM may or may not already be up on this thread. Either is fine; only an
    // initialisation this call actually performed is undone at the end.
    const HRESULT coInit  = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool    ownsCom = SUCCEEDED(coInit);

    // The source is R8G8B8A8 in memory order, and the PNG encoder reliably
    // accepts 32bppBGRA. Swapping here is cheaper than trusting SetPixelFormat
    // to leave an RGBA request intact — it is allowed to substitute.
    std::vector<uint8_t> bgra((size_t)width * height * 4);
    for (size_t i = 0; i < bgra.size(); i += 4)
    {
        bgra[i + 0] = rgba[i + 2];
        bgra[i + 1] = rgba[i + 1];
        bgra[i + 2] = rgba[i + 0];
        bgra[i + 3] = 255;      // the trace has no alpha to preserve
    }

    wchar_t wide[MAX_PATH] = { 0 };
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH) == 0)
    {
        if (ownsCom) CoUninitialize();
        return false;
    }

    IWICImagingFactory*    factory = nullptr;
    IWICStream*            stream  = nullptr;
    IWICBitmapEncoder*     encoder = nullptr;
    IWICBitmapFrameEncode* frame   = nullptr;
    IPropertyBag2*         props   = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(wide, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng,
                                                   nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) hr = frame->Initialize(props);
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);

    // SetPixelFormat succeeds while quietly handing back a different layout if
    // the encoder cannot take the requested one. Writing the buffer anyway
    // would produce a file with the wrong channels rather than an error.
    if (SUCCEEDED(hr) && !IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA))
        hr = E_FAIL;

    if (SUCCEEDED(hr)) hr = frame->WritePixels(height, width * 4,
                                               (UINT)bgra.size(), bgra.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();

    const bool ok = SUCCEEDED(hr);

    SafeRelease(props);
    SafeRelease(frame);
    SafeRelease(encoder);
    SafeRelease(stream);
    SafeRelease(factory);
    if (ownsCom) CoUninitialize();
    return ok;
}

bool WriteImage(const char* path, const uint8_t* rgba,
                uint32_t width, uint32_t height)
{
    EnsureParentDirectory(path);
    return EndsWithNoCase(path, ".ppm")
               ? WriteImagePpm(path, rgba, width, height)
               : WriteImagePng(path, rgba, width, height);
}
}
