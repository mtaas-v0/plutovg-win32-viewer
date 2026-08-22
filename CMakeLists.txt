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
    src/svg_engine.c
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

    install(TARGETS PlutoVGViewer DESTINATION bin)
            
    if(TRUE) #MINGW
        # 1. Normalize the compiler path to use standard CMake forward slashes
        file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" CLEAN_COMPILER_PATH)
        
        # 2. Extract the bin directory
        get_filename_component(MINGW_BIN_DIR "${CLEAN_COMPILER_PATH}" DIRECTORY)
        
        # 3. Use find_file to safely verify the DLL exists in the path
        find_file(WINPTHREAD_DLL
            NAMES "libwinpthread-1.dll"
            PATHS "${MINGW_BIN_DIR}"
            NO_DEFAULT_PATH
        )
        
        # 4. Fallback: If toolchain tracking fails, look in standard GitHub Actions MSYS2 locations
        if(NOT WINPTHREAD_DLL)
            find_file(WINPTHREAD_DLL
                NAMES "libwinpthread-1.dll"
                PATHS "C:/msys64/mingw64/bin" "C:/msys64/ucrt64/bin" "C:/msys64/clang64/bin"
            )
        endif()
        
        # 5. Handle the installation step
        if(WINPTHREAD_DLL)
            message(STATUS "Found winpthread DLL for installation: ${WINPTHREAD_DLL}")
            
            # Installs the DLL directly into the same 'bin' directory as your executable
            install(FILES "${WINPTHREAD_DLL}"
                DESTINATION bin
            )
        else()
            message(FATAL_ERROR "Could not find libwinpthread-1.dll for installation!")
        endif()
    endif()

endif()
