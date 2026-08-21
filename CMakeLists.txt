cmake_minimum_required(VERSION 3.20)
project(PlutoVGWin32Viewer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    plutovg
    GIT_REPOSITORY https://github.com/sammycage/plutovg.git
    GIT_TAG main
)
FetchContent_MakeAvailable(plutovg)

set(SOURCES
    src/main.c
    src/font_helper.c
    res/app.rc
)

add_executable(PlutoVGViewer WIN32 ${SOURCES})

target_include_directories(PlutoVGViewer PRIVATE src)
target_link_libraries(PlutoVGViewer PRIVATE plutovg)

# Windows system libraries
target_link_libraries(PlutoVGViewer PRIVATE
    user32
    gdi32
    comdlg32
    shell32
    shlwapi
)

if(MSVC)
    target_compile_options(PlutoVGViewer PRIVATE /W4)
else()
    target_compile_options(PlutoVGViewer PRIVATE -Wall -Wextra)
endif()