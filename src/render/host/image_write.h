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

// image_write.h
//
// Writing a traced frame out to a file.
//
// This is deliberately not part of the ray tracer: encoding an image has
// nothing to do with tracing one, and the host will want the same two
// writers for anything else it captures.
//
// Two formats, because they serve different readers. PPM is trivial and
// every diff tool understands it, which is what the self-test needs. PNG is
// what a person needs, because Windows previews a PNG and does not preview
// a PPM. The extension chooses.
#pragma once

#include <cstdint>

namespace Host
{
    // `rgba` is width * height * 4 bytes in memory order R,G,B,A — that is,
    // exactly what VK_FORMAT_R8G8B8A8_UNORM produces on readback.
    bool WriteImagePpm(const char* path, const uint8_t* rgba,
                       uint32_t width, uint32_t height);

    bool WriteImagePng(const char* path, const uint8_t* rgba,
                       uint32_t width, uint32_t height);

    // Dispatches on the extension: ".ppm" writes a PPM, anything else a PNG.
    bool WriteImage(const char* path, const uint8_t* rgba,
                    uint32_t width, uint32_t height);
}
