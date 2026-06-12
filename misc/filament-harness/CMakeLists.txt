cmake_minimum_required(VERSION 3.24)
project(filament-harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prebuilt Filament release (see docs/superpowers/plans/...-m2.md Task 1)
set(FILAMENT_DIST "$ENV{HOME}/Work/filament-dist/v1.71.6/filament" CACHE PATH "Filament prebuilt dist")

add_executable(filament-harness main.cpp)

target_include_directories(filament-harness PRIVATE ${FILAMENT_DIST}/include)

# Explicit link list — .a archives, order matters for ld
set(FLIBS
    gltfio_core
    uberarchive
    uberzlib
    filament
    backend
    bluegl
    bluevk
    filabridge
    filaflat
    filament-iblprefilter
    ibl
    image
    ktxreader
    basis_transcoder
    dracodec
    meshoptimizer
    mikktspace
    geometry
    filameshio
    shaders
    smol-v
    stb
    utils
    zstd
    abseil
    perfetto
)
foreach(L ${FLIBS})
    target_link_libraries(filament-harness PRIVATE ${FILAMENT_DIST}/lib/arm64/lib${L}.a)
endforeach()

target_link_libraries(filament-harness PRIVATE
    "-framework Metal"
    "-framework MetalKit"
    "-framework CoreFoundation"
    "-framework Foundation"
    "-framework QuartzCore"
    "-framework Cocoa"
)
