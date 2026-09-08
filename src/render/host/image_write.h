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
