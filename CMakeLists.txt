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

    # Ensure this block runs when deploying your executable target (e.g., "my_app")
    install(TARGETS PlutoVGViewer DESTINATION bin)
        
    # Fix: Force CMake to look into the MSYS2 environment directory at install time
    install(CODE [[
        # Get the directory of the active MinGW compiler
        get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES $<TARGET_FILE:PlutoVGViewer>
            RESOLVED_DEPENDENCIES_VAR _resolved_deps
            UNRESOLVED_DEPENDENCIES_VAR _unresolved_deps
            DIRECTORIES "${MINGW_BIN_DIR}"  # Enables MSYS2 resolution
            
            # OVERRIDE BUILT-IN FILTERS: Only exclude explicit Windows API layers here
            PRE_EXCLUDE_REGEXES "^[Aa][Pp][Ii]-[Mm][Ss]-" "^[Ee][Xx][Tt]-[Mm][Ss]-"
            
            # Post-exclude standard system files but ALLOW MinGW/MSYS2 paths
            POST_EXCLUDE_REGEXES 
                "^[Cc]:/[Ww][Ii][Nn][Dd][Oo][Wws]/[Ss][Yy][Ss][Tt][Ee][Mm]32"
                "^[Cc]:/[Ww][Ii][Nn][Dd][Oo][Wws]/[Ss][Yy][Ss][Ww][Oo][Ww]64"
        )
        
        # Corrected install syntax loop
        foreach(_file ${_resolved_deps})
            file(INSTALL "${_file}" DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
        endforeach()
    ]])

endif()
