#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include <plutovg.h>
#include <plutosvg.h>
#include "font_helper.h"

#define ID_MENU_OPEN_SVG    1001
#define ID_MENU_OPEN_FONT   1002
#define ID_MENU_RESET_VIEW  1003
#define ID_MENU_TOGGLE_GRID 1004
#define ID_MENU_EXIT        1005

#define MAX_LOADED_FONTS 32

typedef struct {
    char family_name[128];
    char file_path[MAX_PATH];
    plutovg_font_face_t* face;
} LoadedFont;

typedef struct {
    float zoom;
    float pan_x;
    float pan_y;
    float rotation_deg;
    float shear_x;
    
    // Drag state
    bool is_dragging;
    POINT last_mouse;

    // Window buffer
    int width;
    int height;
    plutovg_surface_t* surface;
    plutovg_canvas_t* canvas;

    // Active SVG State
    char current_svg_path[MAX_PATH];
    char current_svg_name[128];
    char preferred_fonts_dir[MAX_PATH];
    plutosvg_document_t* svg_doc;
    float svg_width;
    float svg_height;

    // Font Cache & Fallback
    LoadedFont fonts[MAX_LOADED_FONTS];
    int font_count;
    plutovg_font_face_t* active_font_face;
    char active_font_name[128];

    // Preferences
    bool show_grid;
} AppState;

static AppState g_app;

// Forward Declarations
static void load_svg_file(AppState* app, const char* path);
static void load_font_face(AppState* app, const char* path, const char* alias);
static void scan_and_load_local_fonts(AppState* app, const char* svg_path);
static void reset_view(AppState* app);

// Case-insensitive string search
static const char* stristr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return haystack;

    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            if (_strnicmp(haystack, needle, needle_len) == 0) {
                return haystack;
            }
        }
    }
    return NULL;
}

static void free_loaded_fonts(AppState* app) {
    for (int i = 0; i < app->font_count; i++) {
        if (app->fonts[i].face) {
            plutovg_font_face_destroy(app->fonts[i].face);
            app->fonts[i].face = NULL;
        }
    }
    app->font_count = 0;
    app->active_font_face = NULL;
}

static void load_font_face(AppState* app, const char* path, const char* alias) {
    if (app->font_count >= MAX_LOADED_FONTS) return;

    // Avoid loading duplicates
    for (int i = 0; i < app->font_count; i++) {
        if (_stricmp(app->fonts[i].file_path, path) == 0) {
            return;
        }
    }

    plutovg_font_face_t* face = plutovg_font_face_load_from_file(path, 0);
    if (face) {
        LoadedFont* lf = &app->fonts[app->font_count++];
        lf->face = face;
        strncpy(lf->file_path, path, MAX_PATH - 1);

        if (alias && strlen(alias) > 0) {
            strncpy(lf->family_name, alias, sizeof(lf->family_name) - 1);
        } else {
            FontHelper_GetFontName(path, lf->family_name, sizeof(lf->family_name));
        }

        // Set as default active face if none is set
        if (!app->active_font_face) {
            app->active_font_face = face;
            strncpy(app->active_font_name, lf->family_name, sizeof(app->active_font_name) - 1);
        }
    }
}

// Scans preferred <svg_dir>/fonts/ and <svg_dir>/ for .ttf and .otf files
static void scan_and_load_local_fonts(AppState* app, const char* svg_path) {
    char svg_dir[MAX_PATH];
    strncpy(svg_dir, svg_path, MAX_PATH - 1);
    PathRemoveFileSpecA(svg_dir);

    // 1. Check relative 'fonts/' directory first
    snprintf(app->preferred_fonts_dir, sizeof(app->preferred_fonts_dir), "%s\\fonts", svg_dir);
    
    char search_pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;

    // Scan <svg_dir>/fonts/*.ttf and *.otf
    const char* extensions[] = { "\\*.ttf", "\\*.otf", "\\*.ttc" };
    for (int ext = 0; ext < 3; ext++) {
        snprintf(search_pattern, sizeof(search_pattern), "%s%s", app->preferred_fonts_dir, extensions[ext]);
        HANDLE hFind = FindFirstFileA(search_pattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    char font_full_path[MAX_PATH];
                    snprintf(font_full_path, sizeof(font_full_path), "%s\\%s", app->preferred_fonts_dir, fd.cFileName);
                    load_font_face(app, font_full_path, fd.cFileName);
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }

    // 2. Also scan directly in the same folder as the SVG
    for (int ext = 0; ext < 3; ext++) {
        snprintf(search_pattern, sizeof(search_pattern), "%s%s", svg_dir, extensions[ext]);
        HANDLE hFind = FindFirstFileA(search_pattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    char font_full_path[MAX_PATH];
                    snprintf(font_full_path, sizeof(font_full_path), "%s\\%s", svg_dir, fd.cFileName);
                    load_font_face(app, font_full_path, fd.cFileName);
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }
}

// Inspect SVG text elements for custom font-family names and try matching relative files
static void parse_svg_and_preload_fonts(AppState* app, const char* svg_path) {
    FILE* f = fopen(svg_path, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size > 0 && size < (10 * 1024 * 1024)) { // Max 10MB inspection buffer
        char* buffer = (char*)malloc(size + 1);
        if (buffer) {
            fread(buffer, 1, size, f);
            buffer[size] = '\0';

            // Look for font-family="..." or font-family: ...
            const char* ptr = buffer;
            while ((ptr = stristr(ptr, "font-family")) != NULL) {
                ptr += 11;
                while (*ptr == ' ' || *ptr == '=' || *ptr == ':' || *ptr == '\'' || *ptr == '\"') ptr++;
                
                char family[128] = {0};
                int idx = 0;
                while (*ptr && *ptr != '\"' && *ptr != '\'' && *ptr != ';' && *ptr != ',' && *ptr != '}' && idx < 127) {
                    family[idx++] = *ptr++;
                }
                family[idx] = '\0';

                // Look for <svg_dir>/fonts/<family>.ttf or .otf
                if (strlen(family) > 0) {
                    char candidate_path[MAX_PATH];
                    snprintf(candidate_path, sizeof(candidate_path), "%s\\%s.ttf", app->preferred_fonts_dir, family);
                    if (GetFileAttributesA(candidate_path) != INVALID_FILE_ATTRIBUTES) {
                        load_font_face(app, candidate_path, family);
                    } else {
                        snprintf(candidate_path, sizeof(candidate_path), "%s\\%s.otf", app->preferred_fonts_dir, family);
                        if (GetFileAttributesA(candidate_path) != INVALID_FILE_ATTRIBUTES) {
                            load_font_face(app, candidate_path, family);
                        }
                    }
                }
            }
            free(buffer);
        }
    }
    fclose(f);
}

static void load_svg_file(AppState* app, const char* path) {
    if (!path || strlen(path) == 0) return;

    if (app->svg_doc) {
        plutosvg_document_destroy(app->svg_doc);
        app->svg_doc = NULL;
    }

    free_loaded_fonts(app);

    // 1. Scan and register fonts from the SVG's relative fonts/ directory
    scan_and_load_local_fonts(app, path);
    parse_svg_and_preload_fonts(app, path);

    // 2. Fallback to system font if nothing was found in local fonts folder
    if (app->font_count == 0) {
        FontInfo sys_font;
        if (FontHelper_GetSystemFont(&sys_font)) {
            load_font_face(app, sys_font.font_path, sys_font.font_name);
        }
    }

    // 3. Load the SVG Document using PlutoSVG
    app->svg_doc = plutosvg_document_load_from_file(path, -1, -1);
    if (app->svg_doc) {
        strncpy(app->current_svg_path, path, MAX_PATH - 1);
        FontHelper_GetFontName(path, app->current_svg_name, sizeof(app->current_svg_name));
        app->svg_width = plutosvg_document_get_width(app->svg_doc);
        app->svg_height = plutosvg_document_get_height(app->svg_doc);

        if (app->svg_width <= 0) app->svg_width = 800;
        if (app->svg_height <= 0) app->svg_height = 600;

        reset_view(app);
    }
}

static void reset_view(AppState* app) {
    app->zoom = 1.0f;
    app->rotation_deg = 0.0f;
    app->shear_x = 0.0f;

    if (app->svg_doc) {
        // Center the SVG in the viewport
        app->pan_x = ((float)app->width - app->svg_width) / 2.0f;
        app->pan_y = ((float)app->height - app->svg_height) / 2.0f;

        // Auto-fit zoom if SVG is larger than window
        if (app->width > 0 && app->height > 0) {
            float scale_x = ((float)app->width * 0.85f) / app->svg_width;
            float scale_y = ((float)app->height * 0.85f) / app->svg_height;
            float fit_scale = (scale_x < scale_y) ? scale_x : scale_y;
            if (fit_scale < 1.0f && fit_scale > 0.01f) {
                app->zoom = fit_scale;
                app->pan_x = ((float)app->width - (app->svg_width * app->zoom)) / 2.0f;
                app->pan_y = ((float)app->height - (app->svg_height * app->zoom)) / 2.0f;
            }
        }
    } else {
        app->pan_x = (float)app->width / 2.0f;
        app->pan_y = (float)app->height / 2.0f;
    }
}

static void resize_surface(AppState* app, int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (app->canvas) plutovg_canvas_destroy(app->canvas);
    if (app->surface) plutovg_surface_destroy(app->surface);

    app->width = width;
    app->height = height;
    app->surface = plutovg_surface_create(width, height);
    app->canvas = plutovg_canvas_create(app->surface);
}

static void draw_grid(plutovg_canvas_t* canvas) {
    plutovg_canvas_save(canvas);
    plutovg_canvas_set_rgb(canvas, 0.20f, 0.22f, 0.26f);
    plutovg_canvas_set_line_width(canvas, 1.0f);

    const float step = 50.0f;
    const float extent = 5000.0f;

    for (float x = -extent; x <= extent; x += step) {
        plutovg_canvas_move_to(canvas, x, -extent);
        plutovg_canvas_line_to(canvas, x, extent);
    }
    for (float y = -extent; y <= extent; y += step) {
        plutovg_canvas_move_to(canvas, -extent, y);
        plutovg_canvas_line_to(canvas, extent, y);
    }
    plutovg_canvas_stroke(canvas);

    // Coordinate Axes
    plutovg_canvas_set_rgb(canvas, 0.75f, 0.25f, 0.25f);
    plutovg_canvas_set_line_width(canvas, 2.0f);
    plutovg_canvas_move_to(canvas, -extent, 0);
    plutovg_canvas_line_to(canvas, extent, 0);
    plutovg_canvas_stroke(canvas);

    plutovg_canvas_set_rgb(canvas, 0.25f, 0.75f, 0.25f);
    plutovg_canvas_move_to(canvas, 0, -extent);
    plutovg_canvas_line_to(canvas, 0, extent);
    plutovg_canvas_stroke(canvas);

    plutovg_canvas_restore(canvas);
}

static void draw_hud(AppState* app) {
    plutovg_canvas_save(app->canvas);
    plutovg_canvas_reset_matrix(app->canvas);

    // HUD background box
    plutovg_canvas_set_rgba(app->canvas, 0.08f, 0.09f, 0.12f, 0.88f);
    plutovg_canvas_round_rect(app->canvas, 16, 16, 420, 160, 8, 8);
    plutovg_canvas_fill(app->canvas);

    if (app->active_font_face) {
        plutovg_canvas_set_font_face(app->canvas, app->active_font_face);
        plutovg_canvas_set_font_size(app->canvas, 13.0f);

        char buf[512];
        plutovg_canvas_set_rgb(app->canvas, 0.38f, 0.75f, 0.98f);
        snprintf(buf, sizeof(buf), "SVG: %s", app->svg_doc ? app->current_svg_name : "(No SVG Loaded - Showing Sample)");
        plutovg_canvas_fill_text(app->canvas, buf, -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 40);

        plutovg_canvas_set_rgb(app->canvas, 0.88f, 0.88f, 0.90f);
        snprintf(buf, sizeof(buf), "Preferred Fonts: %d loaded (%s)", app->font_count, app->active_font_name);
        plutovg_canvas_fill_text(app->canvas, buf, -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 64);

        snprintf(buf, sizeof(buf), "Zoom: %.1f%% | Pan: (%.0f, %.0f) | Rot: %.1f deg", 
                 app->zoom * 100.0f, app->pan_x, app->pan_y, app->rotation_deg);
        plutovg_canvas_fill_text(app->canvas, buf, -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 88);

        plutovg_canvas_set_rgb(app->canvas, 0.60f, 0.65f, 0.72f);
        plutovg_canvas_fill_text(app->canvas, "Controls: Mouse Wheel = Zoom | Left Drag = Pan", -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 114);
        plutovg_canvas_fill_text(app->canvas, "R/Shift+R = Rotate | S/Shift+S = Shear | Space = Reset", -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 136);
        plutovg_canvas_fill_text(app->canvas, "File Menu / Drag & Drop to open .svg or .ttf", -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 158);
    }
    plutovg_canvas_restore(app->canvas);
}

static void render(AppState* app) {
    if (!app->canvas) return;

    // Clear background
    plutovg_canvas_save(app->canvas);
    plutovg_canvas_reset_matrix(app->canvas);
    plutovg_canvas_set_rgb(app->canvas, 0.12f, 0.13f, 0.16f);
    plutovg_canvas_fill_rect(app->canvas, 0, 0, (float)app->width, (float)app->height);
    plutovg_canvas_restore(app->canvas);

    // Apply Viewport Transformations
    plutovg_canvas_save(app->canvas);
    plutovg_canvas_translate(app->canvas, app->pan_x, app->pan_y);
    plutovg_canvas_scale(app->canvas, app->zoom, app->zoom);
    plutovg_canvas_rotate(app->canvas, app->rotation_deg * (3.1415926535f / 180.0f));

    if (fabsf(app->shear_x) > 0.0001f) {
        plutovg_matrix_t shear_mat;
        plutovg_matrix_init_shear(&shear_mat, app->shear_x, 0.0f);
        plutovg_canvas_transform(app->canvas, &shear_mat);
    }

    if (app->show_grid) {
        draw_grid(app->canvas);
    }

    // Render Loaded SVG Document or Fallback Typography Demo
    if (app->svg_doc) {
        // Draw white artboard backing for SVG
        plutovg_canvas_save(app->canvas);
        plutovg_canvas_set_rgb(app->canvas, 1.0f, 1.0f, 1.0f);
        plutovg_canvas_fill_rect(app->canvas, 0, 0, app->svg_width, app->svg_height);
        
        // Render SVG elements
        plutosvg_document_render(app->svg_doc, NULL, app->canvas, NULL, 0, NULL);
        plutovg_canvas_restore(app->canvas);
    } else if (app->active_font_face) {
        // Standalone Typography Demo when no SVG is opened
        plutovg_canvas_set_font_face(app->canvas, app->active_font_face);
        
        plutovg_canvas_set_font_size(app->canvas, 38.0f);
        plutovg_canvas_set_rgb(app->canvas, 0.28f, 0.68f, 0.98f);
        plutovg_canvas_fill_text(app->canvas, "PlutoVG Vector & Font Viewer", -1, PLUTOVG_TEXT_ENCODING_UTF8, -250, -80);

        plutovg_canvas_set_font_size(app->canvas, 22.0f);
        plutovg_canvas_set_rgb(app->canvas, 0.92f, 0.92f, 0.94f);
        plutovg_canvas_fill_text(app->canvas, "Open any .svg file referencing uninstalled fonts.", -1, PLUTOVG_TEXT_ENCODING_UTF8, -250, -30);
        plutovg_canvas_fill_text(app->canvas, "Fonts in relative /fonts/ directory are automatically bound.", -1, PLUTOVG_TEXT_ENCODING_UTF8, -250, 10);

        plutovg_canvas_set_font_size(app->canvas, 42.0f);
        plutovg_canvas_set_rgb(app->canvas, 0.95f, 0.65f, 0.15f);
        plutovg_canvas_set_line_width(app->canvas, 1.5f);
        plutovg_canvas_stroke_text(app->canvas, "TRANSFORMED TYPOGRAPHY", -1, PLUTOVG_TEXT_ENCODING_UTF8, -250, 80);
    }

    plutovg_canvas_restore(app->canvas);

    // Draw On-Screen HUD Overlay
    draw_hud(app);
}

static void zoom_at(AppState* app, float screen_x, float screen_y, float factor) {
    float new_zoom = app->zoom * factor;
    if (new_zoom < 0.01f) new_zoom = 0.01f;
    if (new_zoom > 100.0f) new_zoom = 100.0f;

    // Anchor the zoom around the current mouse cursor location
    app->pan_x = screen_x - (screen_x - app->pan_x) * (new_zoom / app->zoom);
    app->pan_y = screen_y - (screen_y - app->pan_y) * (new_zoom / app->zoom);
    app->zoom = new_zoom;
}

static bool browse_svg_file(HWND hwnd_parent, char* out_path, size_t max_len) {
    OPENFILENAMEA ofn = {0};
    char file_buffer[MAX_PATH] = {0};

    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = hwnd_parent;
    ofn.lpstrFilter = "Scalable Vector Graphics (*.svg)\0*.svg\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file_buffer;
    ofn.nMaxFile = sizeof(file_buffer);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        strncpy(out_path, file_buffer, max_len - 1);
        return true;
    }
    return false;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE);
        g_app.show_grid = true;
        g_app.zoom = 1.0f;
        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        if (w > 0 && h > 0) {
            bool initial = (g_app.width == 0 && g_app.height == 0);
            resize_surface(&g_app, w, h);
            if (initial) {
                reset_view(&g_app);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        ScreenToClient(hwnd, &pt);

        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float factor = (delta > 0) ? 1.15f : (1.0f / 1.15f);
        zoom_at(&g_app, (float)pt.x, (float)pt.y, factor);

        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        g_app.is_dragging = true;
        g_app.last_mouse.x = GET_X_LPARAM(lParam);
        g_app.last_mouse.y = GET_Y_LPARAM(lParam);
        SetCapture(hwnd);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (g_app.is_dragging) {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);

            g_app.pan_x += (float)(mx - g_app.last_mouse.x);
            g_app.pan_y += (float)(my - g_app.last_mouse.y);

            g_app.last_mouse.x = mx;
            g_app.last_mouse.y = my;

            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_app.is_dragging) {
            g_app.is_dragging = false;
            ReleaseCapture();
        }
        return 0;
    }

    case WM_KEYDOWN: {
        switch (wParam) {
        case 'R': {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            g_app.rotation_deg += shift ? -5.0f : 5.0f;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        case 'S': {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            g_app.shear_x += shift ? -0.05f : 0.05f;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        case VK_OEM_PLUS:
        case VK_ADD:
            zoom_at(&g_app, (float)g_app.width / 2.0f, (float)g_app.height / 2.0f, 1.15f);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            zoom_at(&g_app, (float)g_app.width / 2.0f, (float)g_app.height / 2.0f, 1.0f / 1.15f);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case VK_SPACE:
        case '0':
            reset_view(&g_app);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case 'G':
            g_app.show_grid = !g_app.show_grid;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case 'O': {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl) {
                char svg_path[MAX_PATH];
                if (browse_svg_file(hwnd, svg_path, MAX_PATH)) {
                    load_svg_file(&g_app, svg_path);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            } else {
                FontInfo fi;
                if (FontHelper_BrowseFont(hwnd, &fi)) {
                    load_font_face(&g_app, fi.font_path, fi.font_name);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            break;
        }
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        char dropped_file[MAX_PATH];
        if (DragQueryFileA(hDrop, 0, dropped_file, MAX_PATH)) {
            const char* ext = PathFindExtensionA(dropped_file);
            if (_stricmp(ext, ".svg") == 0) {
                load_svg_file(&g_app, dropped_file);
            } else if (_stricmp(ext, ".ttf") == 0 || _stricmp(ext, ".otf") == 0 || _stricmp(ext, ".ttc") == 0) {
                load_font_face(&g_app, dropped_file, NULL);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_MENU_OPEN_SVG: {
            char svg_path[MAX_PATH];
            if (browse_svg_file(hwnd, svg_path, sizeof(svg_path))) {
                load_svg_file(&g_app, svg_path);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        case ID_MENU_OPEN_FONT: {
            FontInfo fi;
            if (FontHelper_BrowseFont(hwnd, &fi)) {
                load_font_face(&g_app, fi.font_path, fi.font_name);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        case ID_MENU_RESET_VIEW:
            reset_view(&g_app);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case ID_MENU_TOGGLE_GRID:
            g_app.show_grid = !g_app.show_grid;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case ID_MENU_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        render(&g_app);

        if (g_app.surface) {
            int w = plutovg_surface_get_width(g_app.surface);
            int h = plutovg_surface_get_height(g_app.surface);
            const unsigned char* data = plutovg_surface_get_data(g_app.surface);

            BITMAPINFO bmi = {0};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h; // Top-down DIB
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            StretchDIBits(
                hdc,
                0, 0, w, h,
                0, 0, w, h,
                data,
                &bmi,
                DIB_RGB_COLORS,
                SRCCOPY
            );
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // Double buffer: avoid flickering

    case WM_DESTROY:
        if (g_app.svg_doc) plutosvg_document_destroy(g_app.svg_doc);
        free_loaded_fonts(&g_app);
        if (g_app.canvas) plutovg_canvas_destroy(g_app.canvas);
        if (g_app.surface) plutovg_surface_destroy(g_app.surface);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void create_app_menu(HWND hwnd) {
    HMENU hMenuBar = CreateMenu();
    HMENU hFileMenu = CreateMenu();
    HMENU hViewMenu = CreateMenu();

    AppendMenuA(hFileMenu, MF_STRING, ID_MENU_OPEN_SVG, "Open .&SVG...\t(Ctrl+O)");
    AppendMenuA(hFileMenu, MF_STRING, ID_MENU_OPEN_FONT, "Open &Font...\t(O)");
    AppendMenuA(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFileMenu, MF_STRING, ID_MENU_EXIT, "E&xit");

    AppendMenuA(hViewMenu, MF_STRING, ID_MENU_RESET_VIEW, "&Reset View\t(Space)");
    AppendMenuA(hViewMenu, MF_STRING, ID_MENU_TOGGLE_GRID, "Toggle &Grid\t(G)");

    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hFileMenu, "&File");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hViewMenu, "&View");

    SetMenu(hwnd, hMenuBar);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    // Enable Per-Monitor High DPI Awareness
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);
        SetProcessDpiAwarenessContextProc setDpiContext = 
            (SetProcessDpiAwarenessContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpiContext) {
            setDpiContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            SetProcessDPIAware();
        }
    }

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "PlutoVGViewerWindowClass";

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Failed to register window class.", "Error", MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowExA(
        WS_EX_ACCEPTFILES,
        wc.lpszClassName,
        "PlutoVG & PlutoSVG Vector Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1100, 800,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        MessageBoxA(NULL, "Failed to create window.", "Error", MB_ICONERROR);
        return 1;
    }

    create_app_menu(hwnd);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Check for command line argument (e.g. `PlutoVGViewer.exe "path/to/file.svg"`)
    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argvW && argc > 1) {
        char initial_path[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, argvW[1], -1, initial_path, MAX_PATH, NULL, NULL);
        
        const char* ext = PathFindExtensionA(initial_path);
        if (_stricmp(ext, ".svg") == 0) {
            load_svg_file(&g_app, initial_path);
        } else if (_stricmp(ext, ".ttf") == 0 || _stricmp(ext, ".otf") == 0 || _stricmp(ext, ".ttc") == 0) {
            load_font_face(&g_app, initial_path, NULL);
        }
        LocalFree(argvW);
        InvalidateRect(hwnd, NULL, FALSE);
    } else {
        // Default system font initialization if started without arguments
        FontInfo sys_font;
        if (FontHelper_GetSystemFont(&sys_font)) {
            load_font_face(&g_app, sys_font.font_path, sys_font.font_name);
        }
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
