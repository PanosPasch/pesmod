# Third-party notices

PESMod itself is licensed under the GNU General Public License, version 3 or
later (see [LICENSE](LICENSE)). It bundles or depends on the third-party
components below, which keep their own licenses. Each of those licenses is
compatible with the GPLv3, and their terms continue to apply to the component
in question.

## MinHook

Location: `include/MinHook/`
License: BSD 2-Clause "Simplified" License
Upstream: https://github.com/TsudaKageyu/minhook

Reproduced verbatim from `include/MinHook/include/MinHook.h`:

     MinHook - The Minimalistic API Hooking Library for x64/x86
     Copyright (C) 2009-2017 Tsuda Kageyu.
     All rights reserved.

     Redistribution and use in source and binary forms, with or without
     modification, are permitted provided that the following conditions
     are met:

      1. Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.
      2. Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in the
         documentation and/or other materials provided with the distribution.

     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
     "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
     TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
     PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
     OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
     EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
     PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
     PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
     LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
     NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
     SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## Vulkan SDK

Location: not vendored; located at build time by `find_package(Vulkan)`
License: Apache License 2.0

`PESModHost` (`src/render/host/`) builds against the Vulkan headers, loader and
`glslangValidator` from the LunarG Vulkan SDK. These are Apache-2.0 licensed,
which is compatible with the GPLv3, and they are not redistributed here.

## Windows system libraries

`PESModHost` links `windowscodecs` and `ole32`, and the capture layer builds
against the Direct3D 8 ABI declared in `src/render/d3d8/d3d8_min.h`. These are
operating-system components rather than parts of this work, and fall under the
GPLv3's System Library exception (section 1).

## 010 Editor template helpers

Location: `docs/templates/common/`
License: not stated upstream; origin not established

`common/types.h` and `common/utils.h` are generic 010 Editor helper headers
that were not written for this project and that carry no upstream attribution.
They are deliberately left out of the PESMod copyright notice and carry no
GPL header. If their provenance is established they should be credited here or
replaced.

## npm dependencies (ui-bin-viewer)

`tools/ui-bin-viewer/` uses React, React DOM, three.js, Vite and
`@vitejs/plugin-react`. These are fetched from npm rather than vendored in this
repository, and each is MIT licensed.
