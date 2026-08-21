#include "font_helper.h"
#include <stdio.h>
#include <shlobj.h>
#include <shlwapi.h>

static const char* FALLBACK_FONTS[] = {
    "segoeui.ttf",
    "arial.ttf",
    "calibri.ttf",
    "tahoma.ttf",
    "consola.ttf"
};

void FontHelper_GetFontName(const char* path, char* out_name, size_t max_len) {
    const char* filename = PathFindFileNameA(path);
    snprintf(out_name, max_len, "%s", filename);
}

bool FontHelper_GetSystemFont(FontInfo* out_info) {
    char fonts_folder[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_FONTS, NULL, 0, fonts_folder))) {
        char win_dir[MAX_PATH];
        if (!GetWindowsDirectoryA(win_dir, MAX_PATH)) {
            return false;
        }
        snprintf(fonts_folder, sizeof(fonts_folder), "%s\\Fonts", win_dir);
    }

    for (size_t i = 0; i < sizeof(FALLBACK_FONTS) / sizeof(FALLBACK_FONTS[0]); ++i) {
        char full_path[MAX_FONT_PATH];
        snprintf(full_path, sizeof(full_path), "%s\\%s", fonts_folder, FALLBACK_FONTS[i]);
        if (GetFileAttributesA(full_path) != INVALID_FILE_ATTRIBUTES) {
            snprintf(out_info->font_path, sizeof(out_info->font_path), "%s", full_path);
            FontHelper_GetFontName(full_path, out_info->font_name, sizeof(out_info->font_name));
            return true;
        }
    }
    return false;
}

bool FontHelper_BrowseFont(HWND hwnd_parent, FontInfo* out_info) {
    OPENFILENAMEA ofn = {0};
    char file_buffer[MAX_FONT_PATH] = {0};

    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = hwnd_parent;
    ofn.lpstrFilter = "TrueType / OpenType Fonts (*.ttf;*.otf;*.ttc)\0*.ttf;*.otf;*.ttc\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file_buffer;
    ofn.nMaxFile = sizeof(file_buffer);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        snprintf(out_info->font_path, sizeof(out_info->font_path), "%s", file_buffer);
        FontHelper_GetFontName(file_buffer, out_info->font_name, sizeof(out_info->font_name));
        return true;
    }
    return false;
}