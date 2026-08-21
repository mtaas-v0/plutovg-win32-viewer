#ifndef FONT_HELPER_H
#define FONT_HELPER_H

#include <stdbool.h>
#include <windows.h>

#define MAX_FONT_PATH 512

typedef struct {
    char font_path[MAX_FONT_PATH];
    char font_name[128];
} FontInfo;

// Discovers a default system TTF font (Segoe UI, Arial, Calibri, etc.)
bool FontHelper_GetSystemFont(FontInfo* out_info);

// Opens a Windows file dialog to let the user select a TTF/OTF font file
bool FontHelper_BrowseFont(HWND hwnd_parent, FontInfo* out_info);

// Extracts font file name from path
void FontHelper_GetFontName(const char* path, char* out_name, size_t max_len);

#endif // FONT_HELPER_H