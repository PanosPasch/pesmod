cmake_minimum_required(VERSION 3.20)
project(PESModHost VERSION 1.0.0 LANGUAGES CXX)

# ── CRITICAL: this target is 64-bit ──────────────────────────────────────
# The opposite of the main PESMod build, and for the reason the whole
# two-process split exists: NVIDIA's 32-bit Vulkan ICD exposes no ray tracing
# extensions, so the renderer cannot live inside the 32-bit game process.
# See docs/RENDERER.md section 1.
#
# It is a separate CMake project rather than another target in the parent
# because a single CMake generate cannot produce both Win32 and x64 binaries.
# Configure it on its own:
#
#   cmake -S src/render/host -B build-host -A x64
#   cmake --build build-host --config Release
#
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "PESModHost must be built as 64-bit. Re-run cmake with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Vulkan ───────────────────────────────────────────────────────────────
# Ray tracing is required, not optional, so a missing SDK is a hard error
# rather than a silent fallback to a raster path that was never the point.
find_package(Vulkan REQUIRED)
message(STATUS "PESModHost: Vulkan ${Vulkan_VERSION} at ${Vulkan_INCLUDE_DIRS}")

set(HOST_SOURCES
    main.cpp
    scene_receiver.cpp
    vk_device.cpp
    vk_alloc.cpp
    vk_accel.cpp
    vk_raytracer.cpp
    vk_textures.cpp
    image_write.cpp
    ../ipc/shared_ring.cpp
)

# ── Shaders ──────────────────────────────────────────────────────────────
# Compiled to SPIR-V at build time. glslangValidator ships with the Vulkan
# SDK, which find_package(Vulkan) already located.
set(SHADER_SRC
    shaders/primary.rgen
    shaders/sky.rmiss
    shaders/shadow.rmiss
    shaders/primary.rchit
    shaders/primary.rahit
)

set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

find_program(GLSLANG_EXE
    NAMES glslangValidator glslangValidator.exe
    HINTS ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} $ENV{VULKAN_SDK}/Bin
    REQUIRED)

set(SHADER_SPV)
foreach(SRC ${SHADER_SRC})
    get_filename_component(FNAME ${SRC} NAME)
    set(SPV ${SHADER_OUT_DIR}/${FNAME}.spv)
    add_custom_command(
        OUTPUT ${SPV}
        # --target-env vulkan1.3 is required for the ray tracing and position
        # fetch extensions the shaders use.
        COMMAND ${GLSLANG_EXE} --target-env vulkan1.3
                -I${CMAKE_CURRENT_SOURCE_DIR}/shaders
                -o ${SPV} ${CMAKE_CURRENT_SOURCE_DIR}/${SRC}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SRC}
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.glsl
        COMMENT "Compiling ${FNAME} to SPIR-V"
        VERBATIM)
    list(APPEND SHADER_SPV ${SPV})
endforeach()

add_custom_target(PESModShaders DEPENDS ${SHADER_SPV})

add_executable(PESModHost ${HOST_SOURCES})
add_dependencies(PESModHost PESModShaders)
# windowscodecs/ole32: WIC, for writing traced frames out as PNG.
target_link_libraries(PESModHost PRIVATE Vulkan::Vulkan windowscodecs ole32)

target_include_directories(PESModHost PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(MSVC)
    target_compile_options(PESModHost PRIVATE /W4 /EHsc)
endif()

# ── The IPC self-test ────────────────────────────────────────────────────
# Built here as 64-bit; the 32-bit counterpart comes from the parent project.
# Running one against the other is what proves the shared scene structures
# have identical layout under both ABIs.
add_executable(PESModIpcSelfTest
    ../ipc/ipc_selftest.cpp
    ../ipc/shared_ring.cpp
)
target_include_directories(PESModIpcSelfTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
if(MSVC)
    target_compile_options(PESModIpcSelfTest PRIVATE /W4 /EHsc)
endif()
