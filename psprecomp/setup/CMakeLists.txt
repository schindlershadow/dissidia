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
    setup_main.cpp install.cpp proc.cpp
    ${RT}/src/hle/psp_savedata_dialog.cpp
    ${FONTS_CPP})
target_include_directories(psprecomp_setup PRIVATE ${RT}/include ${RT}/src/hle)
target_link_libraries(psprecomp_setup PRIVATE PkgConfig::SDL2 PkgConfig::FREETYPE
    $<$<NOT:$<PLATFORM_ID:Windows>>:pthread>
    $<$<PLATFORM_ID:Windows>:ws2_32 ole32 oleaut32 shell32 comdlg32 uuid user32>)   # portable-file-dialogs uses COM dialogs on Windows
target_compile_options(psprecomp_setup PRIVATE -O2)
install(TARGETS psprecomp_setup RUNTIME DESTINATION . COMPONENT setup)
