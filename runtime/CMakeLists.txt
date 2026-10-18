find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)

# Linked as objects, not an archive: strong import implementations here must
# replace the weak defaults in the generated imports.cpp, and the export
# registrations must not be dropped by the linker.
add_library(cafe_runtime OBJECT
    src/memory.cpp
    src/loader.cpp
    src/dispatch.cpp
    src/dynamic.cpp
    src/diagnostics.cpp
    src/exports.cpp
    src/sysmem.cpp
    src/os/thread.cpp
    src/os/sync.cpp
    src/os/alarm.cpp
    src/os/heap.cpp
    src/os/dynload.cpp
    src/os/coreinit_misc.cpp
    src/os/guest_format.cpp
    src/os/nn_services.cpp
    src/os/proc_ui.cpp
    src/os/system_config.cpp
    src/os/input.cpp
    src/os/fs.cpp
    src/os/ax.cpp
    src/os/snd_user.cpp
    src/os/network.cpp
    src/os/swkbd.cpp
    src/os/erreula.cpp
    src/os/dmae.cpp
    src/os/hardware.cpp
    src/vfs.cpp
    src/wua.cpp
    src/install.cpp
    src/gpu/command_processor.cpp
    src/gpu/display.cpp
    src/gpu/null_backend.cpp
    src/gpu/replacements.cpp
    src/gpu/shader_dump.cpp
    src/gpu/tiling.cpp
    src/gpu/vulkan/context.cpp
    src/gpu/vulkan/draw.cpp
    src/gpu/vulkan/formats.cpp
    src/gpu/vulkan/interpolate.cpp
    src/gpu/vulkan/present.cpp
    src/gpu/vulkan/png.cpp
    src/gpu/vulkan/renderer.cpp
    src/gpu/vulkan/targets.cpp
    src/gpu/vulkan/textures.cpp
    src/host/audio.cpp
    src/host/window.cpp
    src/host/platform.cpp
    src/host/thread.cpp
    src/host/file.cpp
    src/gx2/cbpool.cpp
    src/gx2/display.cpp
    src/gx2/draw.cpp
    src/gx2/format.cpp
    src/gx2/gx2r.cpp
    src/gx2/registers.cpp
    src/gx2/shader.cpp
    src/gx2/state.cpp
    src/gx2/surface.cpp
    src/gx2/texture.cpp
    src/title/presentation.cpp)
target_include_directories(cafe_runtime PUBLIC include PRIVATE src)
target_compile_features(cafe_runtime PUBLIC cxx_std_20)
find_package(Vulkan REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
# GLSL -> SPIR-V at run time. Prefer the vcpkg config target on Windows,
# keep the original shared-library lookup as a distro-friendly fallback.
find_package(unofficial-shaderc CONFIG QUIET)
if(TARGET unofficial::shaderc::shaderc)
    set(TTT2_SHADERC unofficial::shaderc::shaderc)
else()
    find_library(SHADERC_LIBRARY NAMES shaderc_shared shaderc REQUIRED)
    set(TTT2_SHADERC ${SHADERC_LIBRARY})
endif()
target_link_libraries(cafe_runtime PUBLIC cafe_rpx cafe_ppc_decode cafe_title OpenSSL::Crypto ZArchive::zarchive amd_addrlib cafe_latte
    Vulkan::Vulkan SDL3::SDL3 ${TTT2_SHADERC})
# Vulkan structures are initialised with only their sType.
ttt2_warnings(cafe_runtime)
if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(cafe_runtime PRIVATE /O2 /arch:AVX2 /clang:-Wno-missing-field-initializers)
    # The dynamic PPC executor is the dominant CPU cost of the zero-install
    # Player. Let Clang spend more optimization effort on this one translation
    # unit without inflating compile time for the whole Vulkan/runtime tree.
    set_source_files_properties(src/dynamic.cpp PROPERTIES COMPILE_OPTIONS "/clang:-O3")
elseif(NOT MSVC)
    target_compile_options(cafe_runtime PRIVATE -Wno-missing-field-initializers)
    set_source_files_properties(src/dynamic.cpp PROPERTIES COMPILE_OPTIONS "-O3")
endif()

# The title's file formats and the pictures and texts made from them
# (title/): no runtime, so tests use them on synthetic data.
add_library(cafe_title STATIC
    src/title/archive.cpp
    src/title/draw.cpp
    src/title/font.cpp
    src/title/image.cpp
    src/title/text.cpp
    src/title/title_screen.cpp)
target_include_directories(cafe_title PUBLIC src)
target_compile_features(cafe_title PUBLIC cxx_std_20)
target_link_libraries(cafe_title PUBLIC ZLIB::ZLIB)
ttt2_warnings(cafe_title)

# Latte shader decoding and translation, shared by the runtime and tools.
add_library(cafe_latte STATIC
    src/latte/isa.cpp
    src/latte/decode.cpp
    src/latte/disassemble.cpp
    src/latte/environment.cpp
    src/latte/translate.cpp)
target_include_directories(cafe_latte PUBLIC src)
target_compile_features(cafe_latte PUBLIC cxx_std_20)
ttt2_warnings(cafe_latte)

add_executable(cafe-shader src/latte/cafe_shader_main.cpp)
target_link_libraries(cafe-shader PRIVATE cafe_latte ${TTT2_SHADERC})
ttt2_warnings(cafe-shader)
