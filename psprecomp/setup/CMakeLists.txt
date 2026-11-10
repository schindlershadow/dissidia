# psprecomp_setup: the installer / launcher wizard (one per platform, built with the toolchain bundle in CI).
# Independent of the game: it needs SDL2 and FreeType only, plus the runtime's generic menu renderer.
#
#   cmake -S setup -B build-setup -G Ninja -DCMAKE_BUILD_TYPE=Release [-DCMAKE_TOOLCHAIN_FILE=<bundle>/toolchain.cmake]
#   cmake --build build-setup
cmake_minimum_required(VERSION 3.20)
project(psprecomp_setup CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RT ${CMAKE_CURRENT_SOURCE_DIR}/../runtime)

if(PSPRECOMP_DEPS_CMAKE)
    include("${PSPRECOMP_DEPS_CMAKE}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
endif()

# The menu fonts (OFL) are embedded, like in the runtime.
set(FONTS_CPP ${CMAKE_CURRENT_BINARY_DIR}/gen/psp_ui_fonts.cpp)
file(GLOB FONT_FILES ${RT}/assets/fonts/*.ttf)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FONT_FILES} ${RT}/assets/embed_fonts.cmake)
execute_process(COMMAND ${CMAKE_COMMAND} -DFONT_DIR=${RT}/assets/fonts -DOUT=${FONTS_CPP} -P ${RT}/assets/embed_fonts.cmake)

add_executable(psprecomp_setup
    setup_main.cpp install.cpp proc.cpp payload.cpp third_party/miniz.c
    ${RT}/src/hle/psp_savedata_dialog.cpp ${RT}/src/hle/psp_savemgr.cpp ${RT}/src/hle/psp_savedata_crypto.cpp
    ${FONTS_CPP})
target_include_directories(psprecomp_setup PRIVATE ${RT}/include ${RT}/src/hle)
target_link_libraries(psprecomp_setup PRIVATE PkgConfig::SDL2 PkgConfig::FREETYPE
    $<$<NOT:$<PLATFORM_ID:Windows>>:pthread>
    $<$<PLATFORM_ID:Windows>:ws2_32 ole32 oleaut32 shell32 comdlg32 uuid user32>)   # portable-file-dialogs uses COM dialogs on Windows
target_compile_options(psprecomp_setup PRIVATE -O2)
# miniz (public domain / MIT) unpacks the single-file release; its zlib-compatible names would clash with a static zlib
set_source_files_properties(third_party/miniz.c PROPERTIES COMPILE_DEFINITIONS "MINIZ_NO_ZLIB_COMPATIBLE_NAMES;MINIZ_NO_ARCHIVE_WRITING_APIS")
if(WIN32 AND CMAKE_RC_COMPILER)
    # The program's icon (Explorer, taskbar, shortcuts): zig's rc compiles the resource, zig cc links the .res.
    set(APP_ICO ${CMAKE_CURRENT_SOURCE_DIR}/../games/dissidia/icon/icon.ico)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/app.rc "1 ICON \"${APP_ICO}\"\n")
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/app.res
        COMMAND ${CMAKE_RC_COMPILER} /fo ${CMAKE_CURRENT_BINARY_DIR}/app.res ${CMAKE_CURRENT_BINARY_DIR}/app.rc
        DEPENDS ${APP_ICO} VERBATIM)
    add_custom_target(app_res DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/app.res)
    add_dependencies(psprecomp_setup app_res)
    target_link_libraries(psprecomp_setup PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/app.res)
endif()
if(WIN32)
    target_link_options(psprecomp_setup PRIVATE -Wl,--subsystem,windows)   # no console window; see attach_parent_console()
endif()
install(TARGETS psprecomp_setup RUNTIME DESTINATION . COMPONENT setup)

# Unit tests of the wizard's logic (no window): cmake -DPSPRECOMP_SETUP_TESTS=ON, then ctest or run test_setup_install.
option(PSPRECOMP_SETUP_TESTS "Build the setup wizard's unit tests" OFF)
if(PSPRECOMP_SETUP_TESTS)
    enable_testing()
    add_executable(test_setup_install tests/test_install.cpp install.cpp proc.cpp)
    target_link_libraries(test_setup_install PRIVATE $<$<NOT:$<PLATFORM_ID:Windows>>:pthread>)
    add_test(NAME setup_install COMMAND test_setup_install)
    add_executable(test_payload tests/test_payload.cpp payload.cpp third_party/miniz.c)
    set_source_files_properties(third_party/miniz.c PROPERTIES COMPILE_DEFINITIONS "MINIZ_NO_ZLIB_COMPATIBLE_NAMES;MINIZ_NO_ARCHIVE_WRITING_APIS")
    add_test(NAME payload COMMAND test_payload)
endif()
