cmake_minimum_required(VERSION 3.20)
project(PlutoVGWin32Viewer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

# Fetch PlutoVG (2D vector drawing engine)
FetchContent_Declare(
    plutovg
    GIT_REPOSITORY https://github.com/sammycage/plutovg.git
    GIT_TAG main
)

# Fetch PlutoSVG (SVG rendering support on top of PlutoVG)
FetchContent_Declare(
    plutosvg
    GIT_REPOSITORY https://github.com/sammycage/plutosvg.git
    GIT_TAG master
)

FetchContent_MakeAvailable(plutovg plutosvg)


set(SOURCES
    src/main.c
    src/font_helper.c
    res/app.rc
)

add_executable(PlutoVGViewer WIN32 ${SOURCES})

target_include_directories(PlutoVGViewer PRIVATE src)
target_link_libraries(PlutoVGViewer PRIVATE plutovg plutosvg)

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
    target_link_options(PlutoVGViewer PRIVATE "/MANIFEST:NO")
else()
    target_compile_options(PlutoVGViewer PRIVATE -Wall -Wextra)
)
    install(TARGETS PlutoVGViewer DESTINATION bin)
            
    if(MINGW)
        # Get the directory of the active MinGW compiler
        get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        
        # List the runtime DLLs you want to bundle
        set(MINGW_RUNTIMES
            "${MINGW_BIN_DIR}/libwinpthread-1.dll"
            "${MINGW_BIN_DIR}/libgcc_s_seh-1.dll"
            "${MINGW_BIN_DIR}/libstdc++-6.dll"
        )
    
        # Copy them during the install phase
        foreach(dll ${MINGW_RUNTIMES})
            if(EXISTS "${dll}")
                install(FILES "${dll}" DESTINATION bin)
            endif()
        endforeach()
    endif()

endif()
