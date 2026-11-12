# Link project for a released runtime ("SDK"): builds the player's own game from the recompiled output and links it with the
# prebuilt runtime core (libpsprecomp_core.a). Everything game-derived (the generated C++) is compiled here, on the player's
# machine; nothing in the SDK itself contains game code.
#
#   cmake -S <sdk> -B <build> -DPSPRECOMP_OUTPUT_DIR=<workdir>/output \
#         -DPSPRECOMP_OVERLAY_DIRS="<workdir>/overlays/battle;<workdir>/overlays/menu;..."
#   cmake --build <build> --target psprecomp_runtime
#
# SDK layout (cmake --install of runtime/CMakeLists.txt): CMakeLists.txt (this file), lib/libpsprecomp_core.a,
# include/ (runtime headers), src/main.cpp. SDL2, FreeType and (optionally) libavcodec are found with pkg-config
# (a release puts its own .pc files first on PKG_CONFIG_PATH).
cmake_minimum_required(VERSION 3.20)
project(psprecomp_game CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PSPRECOMP_OUTPUT_DIR "" CACHE PATH "Recompiled game output directory (psprecomp recompile -o <dir>)")
set(PSPRECOMP_OVERLAY_DIRS "" CACHE STRING "Semicolon-separated overlay output dirs")
if(NOT EXISTS "${PSPRECOMP_OUTPUT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "PSPRECOMP_OUTPUT_DIR (${PSPRECOMP_OUTPUT_DIR}) is not a recompiled game output directory")
endif()
get_filename_component(PSPRECOMP_OUTPUT_DIR "${PSPRECOMP_OUTPUT_DIR}" ABSOLUTE)

if(PSPRECOMP_DEPS_CMAKE)   # set by the toolchain bundle's toolchain.cmake: static SDL2/FreeType/zlib/libavcodec built with it
    include("${PSPRECOMP_DEPS_CMAKE}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
    pkg_check_modules(AVCODEC IMPORTED_TARGET libavcodec libavutil)
    find_package(OpenGL REQUIRED)
    find_package(ZLIB REQUIRED)
    include("${CMAKE_CURRENT_LIST_DIR}/psp_mbedtls.cmake" OPTIONAL)
endif()

add_subdirectory(${PSPRECOMP_OUTPUT_DIR} ${CMAKE_CURRENT_BINARY_DIR}/recomp_build)
if(NOT TARGET psp::recomp)
    message(FATAL_ERROR "${PSPRECOMP_OUTPUT_DIR} does not define the psp::recomp target")
endif()

set(PSP_OVERLAY_LIBS "")
foreach(ovl_dir ${PSPRECOMP_OVERLAY_DIRS})
    get_filename_component(ovl_dir "${ovl_dir}" ABSOLUTE)
    get_filename_component(ovl_name "${ovl_dir}" NAME)
    file(GLOB OVL_SOURCES CONFIGURE_DEPENDS "${ovl_dir}/generated/batch_*.cpp")
    list(APPEND OVL_SOURCES "${ovl_dir}/dispatch.cpp" "${ovl_dir}/mid_entries.cpp")
    if(EXISTS "${ovl_dir}/overlay.txt")
        file(READ "${ovl_dir}/overlay.txt" OVL_META)
        if(OVL_META MATCHES "^0 ")
            list(APPEND OVL_SOURCES "${ovl_dir}/data_sections.cpp" "${ovl_dir}/syscall_table.cpp")
        endif()
    endif()
    add_library(psp_overlay_${ovl_name} STATIC ${OVL_SOURCES})
    target_include_directories(psp_overlay_${ovl_name} PRIVATE
        "${ovl_dir}" "${PSPRECOMP_OUTPUT_DIR}/include" "${PSPRECOMP_OUTPUT_DIR}")
    target_compile_options(psp_overlay_${ovl_name} PRIVATE -O2 -fno-lto -fno-strict-aliasing)
    list(APPEND PSP_OVERLAY_LIBS psp_overlay_${ovl_name})
endforeach()

# The prebuilt runtime core (next to this file in the SDK layout).
find_library(PSPRECOMP_CORE psprecomp_core PATHS "${CMAKE_CURRENT_LIST_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
add_library(psprecomp_core STATIC IMPORTED)
set_target_properties(psprecomp_core PROPERTIES IMPORTED_LOCATION "${PSPRECOMP_CORE}")
target_link_libraries(psprecomp_core INTERFACE PkgConfig::SDL2 PkgConfig::FREETYPE $<TARGET_NAME_IF_EXISTS:OpenGL::GL> ZLIB::ZLIB
    $<$<NOT:$<OR:$<PLATFORM_ID:Windows>,$<PLATFORM_ID:Android>>>:pthread> $<$<PLATFORM_ID:Android>:GLESv3 log android> $<$<PLATFORM_ID:Windows>:ws2_32>)
if(AVCODEC_FOUND)
    target_link_libraries(psprecomp_core INTERFACE PkgConfig::AVCODEC)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/psp_vkgl_link.cmake" OPTIONAL)   # the core was built with the Vulkan backend
if(PSP_VKGL_ENABLED)
    target_compile_definitions(psprecomp_core INTERFACE PSPRECOMP_HAVE_VKGL)
    if(PSP_VKGL_LIBS)
        target_link_libraries(psprecomp_core INTERFACE ${PSP_VKGL_LIBS})
    endif()
endif()
if(TARGET PkgConfig::MBEDTLS)
    target_link_libraries(psprecomp_core INTERFACE PkgConfig::MBEDTLS)
endif()
if(TARGET PkgConfig::ADRENOTOOLS)   # custom GPU drivers on Android (the core was built with it when the bundle's deps.cmake had it)
    target_link_libraries(psprecomp_core INTERFACE PkgConfig::ADRENOTOOLS)
endif()

if(ANDROID)
    # SDL's Java activity loads libmain.so and calls SDL_main (src/main.cpp); android/build_game.py packs it into the APK.
    add_library(psprecomp_runtime SHARED "${CMAKE_CURRENT_LIST_DIR}/src/main.cpp")
    set_target_properties(psprecomp_runtime PROPERTIES OUTPUT_NAME main)
else()
    add_executable(psprecomp_runtime "${CMAKE_CURRENT_LIST_DIR}/src/main.cpp")
endif()
target_include_directories(psprecomp_runtime PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${PSPRECOMP_OUTPUT_DIR}/include"   # recomp.h
    "${PSPRECOMP_OUTPUT_DIR}"           # funcs.h
    ${SDL2_INCLUDE_DIRS})
target_compile_options(psprecomp_runtime PRIVATE -O2)
target_link_libraries(psprecomp_runtime PRIVATE
    $<$<NOT:$<PLATFORM_ID:Darwin>>:-Wl,--start-group> psprecomp_core ${PSP_OVERLAY_LIBS} psp::recomp
    $<$<NOT:$<PLATFORM_ID:Darwin>>:-Wl,--end-group>)
