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
#include "font_helper.h"

#define ID_MENU_OPEN_SVG    1001
#define ID_MENU_OPEN_FONT   1002
#define ID_MENU_RESET_VIEW  1003
#define ID_MENU_TOGGLE_GRID 1004
#define ID_MENU_EXIT        1005

#define MAX_FONTS 64
#define MAX_SVG_ELEMENTS 1024

// --- Font Lookup Registry ---
typedef struct {
    char key[128];               // e.g. "cmsy10", "arial", "timesnewroman"
    char file_path[MAX_PATH];
    plutovg_font_face_t* face;
} CachedFont;

typedef struct {
    CachedFont fonts[MAX_FONTS];
    int count;
    plutovg_font_face_t* fallback_face;
} FontRegistry;

// --- SVG Text Element Representation ---
typedef struct {
    char text[512];
    char font_family[128];
    float font_size;
    bool is_italic;
    int font_weight;
    float x, y;
    plutovg_color_t color;
    plutovg_matrix_t matrix;
    bool has_matrix;
} SvgTextNode;

typedef struct {
    float width;
    float height;
    SvgTextNode text_nodes[MAX_SVG_ELEMENTS];
    int text_node_count;
    char raw_xml_sample[1024];
} SvgDocument;

typedef struct {
    float zoom;
    float pan_x;
    float pan_y;
    float rotation_deg;
    float shear_x;

    bool is_dragging;
    POINT last_mouse;

    int width;
    int height;
    plutovg_surface_t* surface;
    plutovg_canvas_t* canvas;

    char current_svg_path[MAX_PATH];
    char current_svg_name[128];
    char fonts_dir[MAX_PATH];

    FontRegistry registry;
    SvgDocument svg;
    bool has_svg_loaded;
    bool show_grid;
} AppState;

static AppState g_app;

// --- Helper Utilities ---

static void sanitize_key(const char* src, char* dst, size_t dst_len) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j < dst_len - 1; ++i) {
        if (isalnum((unsigned char)src[i])) {
            dst[j++] = (char)tolower((unsigned char)src[i]);
        }
    }
    dst[j] = '\0';
}

static void registry_init(FontRegistry* reg) {
    for (int i = 0; i < reg->count; ++i) {
        if (reg->fonts[i].face) {
            plutovg_font_face_destroy(reg->fonts[i].face);
            reg->fonts[i].face = NULL;
        }
    }
    reg->count = 0;
    if (reg->fallback_face) {
        plutovg_font_face_destroy(reg->fallback_face);
        reg->fallback_face = NULL;
    }
}

static void registry_add_font(FontRegistry* reg, const char* name_key, const char* path) {
    if (reg->count >= MAX_FONTS || !path) return;

    char clean_key[128];
    sanitize_key(name_key, clean_key, sizeof(clean_key));

    // Check if already registered
    for (int i = 0; i < reg->count; ++i) {
        if (strcmp(reg->fonts[i].key, clean_key) == 0) {
            return;
        }
    }

    plutovg_font_face_t* face = plutovg_font_face_load_from_file(path, 0);
    if (!face) return;

    CachedFont* cf = &reg->fonts[reg->count++];
    strncpy(cf->key, clean_key, sizeof(cf->key) - 1);
    strncpy(cf->file_path, path, MAX_PATH - 1);
    cf->face = face;

    if (!reg->fallback_face) {
        reg->fallback_face = plutovg_font_face_load_from_file(path, 0);
    }
}

static plutovg_font_face_t* registry_find_font(FontRegistry* reg, const char* family_name) {
    if (!family_name || strlen(family_name) == 0) {
        return reg->fallback_face;
    }

    char clean_key[128];
    sanitize_key(family_name, clean_key, sizeof(clean_key));

    for (int i = 0; i < reg->count; ++i) {
        if (strcmp(reg->fonts[i].key, clean_key) == 0) {
            return reg->fonts[i].face;
        }
    }

    // Partial prefix matching fallback (e.g. "cmsy" matching "cmsy10")
    for (int i = 0; i < reg->count; ++i) {
        if (strstr(reg->fonts[i].key, clean_key) || strstr(clean_key, reg->fonts[i].key)) {
            return reg->fonts[i].face;
        }
    }

    return reg->fallback_face;
}

// Scans <svg_dir>/fonts/*.ttf and <svg_dir>/*.ttf for uninstalled fonts
static void discover_fonts_for_svg(AppState* app, const char* svg_path) {
    char svg_dir[MAX_PATH];
    strncpy(svg_dir, svg_path, MAX_PATH - 1);
    PathRemoveFileSpecA(svg_dir);

    snprintf(app->fonts_dir, sizeof(app->fonts_dir), "%s\\fonts", svg_dir);

    const char* search_folders[] = { app->fonts_dir, svg_dir };
    const char* exts[] = { "\\*.ttf", "\\*.otf", "\\*.ttc" };

    for (int f = 0; f < 2; ++f) {
        for (int e = 0; e < 3; ++e) {
            char search_pattern[MAX_PATH];
            snprintf(search_pattern, sizeof(search_pattern), "%s%s", search_folders[f], exts[e]);

            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(search_pattern, &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        char full_path[MAX_PATH];
                        snprintf(full_path, sizeof(full_path), "%s\\%s", search_folders[f], fd.cFileName);

                        char font_key[128];
                        strncpy(font_key, fd.cFileName, sizeof(font_key) - 1);
                        PathRemoveExtensionA(font_key);

                        registry_add_font(&app->registry, font_key, full_path);
                    }
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
        }
    }

    // Load Windows system fallback font if nothing local was found
    if (!app->registry.fallback_face) {
        FontInfo sys_font;
        if (FontHelper_GetSystemFont(&sys_font)) {
            registry_add_font(&app->registry, "system_default", sys_font.font_path);
        }
    }
}

// Helper XML attribute extractor
static bool extract_attribute(const char* tag_start, const char* attr_name, char* out_val, size_t max_len) {
    char search_pattern[128];
    snprintf(search_pattern, sizeof(search_pattern), "%s=\"", attr_name);
    const char* p = strstr(tag_start, search_pattern);
    if (!p) {
        snprintf(search_pattern, sizeof(search_pattern), "%s=\'", attr_name);
        p = strstr(tag_start, search_pattern);
    }
    if (!p) return false;

    p += strlen(search_pattern);
    size_t i = 0;
    while (*p && *p != '\"' && *p != '\'' && i < max_len - 1) {
        out_val[i++] = *p++;
    }
    out_val[i] = '\0';
    return true;
}

// Parses <text ...>content</text> components from SVG
static void parse_svg(AppState* app, const char* svg_path) {
    FILE* f = fopen(svg_path, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return;
    }

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        fclose(f);
        return;
    }

    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    app->svg.width = 600;
    app->svg.height = 400;
    app->svg.text_node_count = 0;

    // Viewport dimensions
    char val[128];
    if (extract_attribute(buffer, "width", val, sizeof(val))) app->svg.width = (float)atof(val);
    if (extract_attribute(buffer, "height", val, sizeof(val))) app->svg.height = (float)atof(val);

    const char* cur = buffer;
    while ((cur = strstr(cur, "<text")) != NULL) {
        if (app->svg.text_node_count >= MAX_SVG_ELEMENTS) break;

        const char* tag_end = strchr(cur, '>');
        const char* close_tag = strstr(cur, "</text>");
        if (!tag_end || !close_tag || close_tag <= tag_end) {
            cur += 5;
            continue;
        }

        SvgTextNode* node = &app->svg.text_nodes[app->svg.text_node_count++];
        memset(node, 0, sizeof(SvgTextNode));
        node->font_size = 16.0f;
        node->font_weight = 400;
        plutovg_color_init_rgb(&node->color, 0.95f, 0.95f, 0.95f);
        plutovg_matrix_init_identity(&node->matrix);

        if (extract_attribute(cur, "font-family", val, sizeof(val))) {
            strncpy(node->font_family, val, sizeof(node->font_family) - 1);
        }
        if (extract_attribute(cur, "font-size", val, sizeof(val))) {
            node->font_size = (float)atof(val);
        }
        if (extract_attribute(cur, "font-style", val, sizeof(val))) {
            if (_stricmp(val, "italic") == 0 || _stricmp(val, "oblique") == 0) node->is_italic = true;
        }
        if (extract_attribute(cur, "font-weight", val, sizeof(val))) {
            node->font_weight = atoi(val);
        }
        if (extract_attribute(cur, "x", val, sizeof(val))) {
            node->x = (float)atof(val);
        }
        if (extract_attribute(cur, "y", val, sizeof(val))) {
            node->y = (float)atof(val);
        }

        // Parse transform="matrix(a b c d e f)"
        if (extract_attribute(cur, "transform", val, sizeof(val))) {
            char* m = strstr(val, "matrix(");
            if (m) {
                m += 7;
                float a, b, c, d, e, f_val;
                for (char* p = m; *p; ++p) if (*p == ',') *p = ' ';
                if (sscanf(m, "%f %f %f %f %f %f", &a, &b, &c, &d, &e, &f_val) == 6) {
                    plutovg_matrix_init(&node->matrix, a, b, c, d, e, f_val);
                    node->has_matrix = true;
                }
            }
        }

        // Parse text content
        const char* text_start = tag_end + 1;
        size_t len = (size_t)(close_tag - text_start);
        if (len > sizeof(node->text) - 1) len = sizeof(node->text) - 1;
        
        // Trim leading/trailing whitespace
        while (len > 0 && isspace((unsigned char)*text_start)) { text_start++; len--; }
        while (len > 0 && isspace((unsigned char)*(text_start + len - 1))) { len--; }

        strncpy(node->text, text_start, len);
        node->text[len] = '\0';

        cur = close_tag + 7;
    }

    free(buffer);
}

static void reset_view(AppState* app) {
    app->zoom = 1.0f;
    app->rotation_deg = 0.0f;
    app->shear_x = 0.0f;
    app->pan_x = ((float)app->width - app->svg.width) / 2.0f;
    app->pan_y = ((float)app->height - app->svg.height) / 2.0f;

    if (app->width > 0 && app->height > 0 && app->svg.width > 0 && app->svg.height > 0) {
        float sx = ((float)app->width * 0.8f) / app->svg.width;
        float sy = ((float)app->height * 0.8f) / app->svg.height;
        float fit = (sx < sy) ? sx : sy;
        if (fit < 1.0f && fit > 0.001f) {
            app->zoom = fit;
            app->pan_x = ((float)app->width - (app->svg.width * app->zoom)) / 2.0f;
            app->pan_y = ((float)app->height - (app->svg.height * app->zoom)) / 2.0f;
        }
    }
}

static void load_svg_file(AppState* app, const char* path) {
    if (!path || strlen(path) == 0) return;

    registry_init(&app->registry);
    discover_fonts_for_svg(app, path);
    parse_svg(app, path);

    strncpy(app->current_svg_path, path, MAX_PATH - 1);
    FontHelper_GetFontName(path, app->current_svg_name, sizeof(app->current_svg_name));
    app->has_svg_loaded = true;

    reset_view(app);
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
    plutovg_canvas_set_rgb(canvas, 0.20f, 0.22f, 0.25f);
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

    // X/Y Major axes
    plutovg_canvas_set_rgb(canvas, 0.7f, 0.25f, 0.25f);
    plutovg_canvas_move_to(canvas, -extent, 0);
    plutovg_canvas_line_to(canvas, extent, 0);
    plutovg_canvas_stroke(canvas);

    plutovg_canvas_set_rgb(canvas, 0.25f, 0.7f, 0.25f);
    plutovg_canvas_move_to(canvas, 0, -extent);
    plutovg_canvas_line_to(canvas, 0, extent);
    plutovg_canvas_stroke(canvas);

    plutovg_canvas_restore(canvas);
}

static void render(AppState* app) {
    if (!app->canvas) return;

    // Viewport background
    plutovg_canvas_save(app->canvas);
    plutovg_canvas_reset_matrix(app->canvas);
    plutovg_canvas_set_rgb(app->canvas, 0.11f, 0.12f, 0.14f);
    plutovg_canvas_fill_rect(app->canvas, 0, 0, (float)app->width, (float)app->height);
    plutovg_canvas_restore(app->canvas);

    // Apply Navigation Transform (Pan -> Zoom -> Rotate -> Shear)
    plutovg_canvas_save(app->canvas);
    plutovg_canvas_translate(app->canvas, app->pan_x, app->pan_y);
    plutovg_canvas_scale(app->canvas, app->zoom, app->zoom);
    plutovg_canvas_rotate(app->canvas, app->rotation_deg * (3.1415926535f / 180.0f));

    if (fabsf(app->shear_x) > 0.0001f) {
        plutovg_matrix_t sm;
        plutovg_matrix_init_shear(&sm, app->shear_x, 0.0f);
        plutovg_canvas_transform(app->canvas, &sm);
    }

    if (app->show_grid) {
        draw_grid(app->canvas);
    }

    // Render SVG Artboard
    if (app->has_svg_loaded) {
        // Document boundaries
        plutovg_canvas_save(app->canvas);
        plutovg_canvas_set_rgb(app->canvas, 0.16f, 0.18f, 0.22f);
        plutovg_canvas_fill_rect(app->canvas, 0, 0, app->svg.width, app->svg.height);
        plutovg_canvas_set_rgb(app->canvas, 0.35f, 0.40f, 0.50f);
        plutovg_canvas_set_line_width(app->canvas, 1.5f);
        plutovg_canvas_stroke_rect(app->canvas, 0, 0, app->svg.width, app->svg.height);
        plutovg_canvas_restore(app->canvas);

        // Render each SVG Text Component
        for (int i = 0; i < app->svg.text_node_count; ++i) {
            SvgTextNode* node = &app->svg.text_nodes[i];
            plutovg_font_face_t* face = registry_find_font(&app->registry, node->font_family);

            if (face) {
                plutovg_canvas_save(app->canvas);

                // Apply SVG transform="matrix(...)"
                if (node->has_matrix) {
                    plutovg_canvas_transform(app->canvas, &node->matrix);
                }

                plutovg_canvas_set_font_face(app->canvas, face);
                plutovg_canvas_set_font_size(app->canvas, node->font_size);
                plutovg_canvas_set_color(app->canvas, &node->color);

                plutovg_canvas_fill_text(app->canvas, node->text, -1, PLUTOVG_TEXT_ENCODING_UTF8, node->x, node->y);
                plutovg_canvas_restore(app->canvas);
            }
        }
    }

    plutovg_canvas_restore(app->canvas);

    // On-Screen HUD Overlay
    plutovg_canvas_save(app->canvas);
    plutovg_canvas_reset_matrix(app->canvas);
    plutovg_canvas_set_rgba(app->canvas, 0.06f, 0.07f, 0.09f, 0.88f);
    plutovg_canvas_round_rect(app->canvas, 16, 16, 460, 160, 8, 8);
    plutovg_canvas_fill(app->canvas);

    if (app->registry.fallback_face) {
        plutovg_canvas_set_font_face(app->canvas, app->registry.fallback_face);
        plutovg_canvas_set_font_size(app->canvas, 13.0f);

        char buf[512];
        plutovg_canvas_set_rgb(app->canvas, 0.35f, 0.75f, 1.0f);
        snprintf(buf, sizeof(buf), "File: %s", app->has_svg_loaded ? app->current_svg_name : "(None)");
        plutovg_canvas_fill_text(app->canvas, buf, -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 40);

        plutovg_canvas_set_rgb(app->canvas, 0.90f, 0.90f, 0.92f);
        snprintf(buf, sizeof(buf), "Discovered Fonts in /fonts/: %d loaded", app->registry.count);
        plutovg_canvas_fill_text(app->canvas, buf, -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 64);

        snprintf(buf, sizeof(buf), "Zoom: %.1f%% | Pan: (%.0f, %.0f) | Rotation: %.1f deg",
                 app->zoom * 100.0f, app->pan_x, app->pan_y, app->rotation_deg);
        plutovg_canvas_fill_text(app->canvas, buf, -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 88);

        plutovg_canvas_set_rgb(app->canvas, 0.60f, 0.65f, 0.72f);
        plutovg_canvas_fill_text(app->canvas, "Mouse Wheel: Zoom | Left-Drag: Pan | R/Shift+R: Rotate", -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 114);
        plutovg_canvas_fill_text(app->canvas, "Drag & Drop .svg file or pass via command line", -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 136);
        plutovg_canvas_fill_text(app->canvas, "Space: Reset View | G: Toggle Grid | Ctrl+O: Open", -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 158);
    }
    plutovg_canvas_restore(app->canvas);
}

static void zoom_at(AppState* app, float sx, float sy, float factor) {
    float new_zoom = app->zoom * factor;
    if (new_zoom < 0.005f) new_zoom = 0.005f;
    if (new_zoom > 200.0f) new_zoom = 200.0f;

    app->pan_x = sx - (sx - app->pan_x) * (new_zoom / app->zoom);
    app->pan_y = sy - (sy - app->pan_y) * (new_zoom / app->zoom);
    app->zoom = new_zoom;
}

static bool browse_file(HWND parent, const char* filter, char* out_path, size_t max_len) {
    OPENFILENAMEA ofn = {0};
    char buf[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        strncpy(out_path, buf, max_len - 1);
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
            bool first = (g_app.width == 0 && g_app.height == 0);
            resize_surface(&g_app, w, h);
            if (first) reset_view(&g_app);
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
            char path[MAX_PATH];
            if (ctrl) {
                if (browse_file(hwnd, "Scalable Vector Graphics (*.svg)\0*.svg\0All Files (*.*)\0*.*\0", path, sizeof(path))) {
                    load_svg_file(&g_app, path);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            } else {
                if (browse_file(hwnd, "Fonts (*.ttf;*.otf)\0*.ttf;*.otf\0All Files (*.*)\0*.*\0", path, sizeof(path))) {
                    registry_add_font(&g_app.registry, PathFindFileNameA(path), path);
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
        char file[MAX_PATH];
        if (DragQueryFileA(hDrop, 0, file, MAX_PATH)) {
            const char* ext = PathFindExtensionA(file);
            if (_stricmp(ext, ".svg") == 0) {
                load_svg_file(&g_app, file);
            } else if (_stricmp(ext, ".ttf") == 0 || _stricmp(ext, ".otf") == 0 || _stricmp(ext, ".ttc") == 0) {
                registry_add_font(&g_app.registry, PathFindFileNameA(file), file);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_MENU_OPEN_SVG: {
            char path[MAX_PATH];
            if (browse_file(hwnd, "Scalable Vector Graphics (*.svg)\0*.svg\0All Files (*.*)\0*.*\0", path, sizeof(path))) {
                load_svg_file(&g_app, path);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        case ID_MENU_OPEN_FONT: {
            char path[MAX_PATH];
            if (browse_file(hwnd, "Fonts (*.ttf;*.otf)\0*.ttf;*.otf\0All Files (*.*)\0*.*\0", path, sizeof(path))) {
                registry_add_font(&g_app.registry, PathFindFileNameA(path), path);
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
            bmi.bmiHeader.biHeight = -h; // Top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            StretchDIBits(hdc, 0, 0, w, h, 0, 0, w, h, data, &bmi, DIB_RGB_COLORS, SRCCOPY);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        registry_init(&g_app.registry);
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

    // High-DPI Awareness
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

    if (!RegisterClassExA(&wc)) return 1;

    HWND hwnd = CreateWindowExA(
        WS_EX_ACCEPTFILES,
        wc.lpszClassName,
        "PlutoVG Typography & SVG Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1100, 800,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) return 1;

    create_app_menu(hwnd);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // First argument passed via Command Line: PlutoVGViewer.exe "path/to/diagram.svg"
    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argvW && argc > 1) {
        char initial_path[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, argvW[1], -1, initial_path, MAX_PATH, NULL, NULL);

        const char* ext = PathFindExtensionA(initial_path);
        if (_stricmp(ext, ".svg") == 0) {
            load_svg_file(&g_app, initial_path);
        } else {
            discover_fonts_for_svg(&g_app, initial_path);
        }
        LocalFree(argvW);
        InvalidateRect(hwnd, NULL, FALSE);
    } else {
        FontInfo sys_font;
        if (FontHelper_GetSystemFont(&sys_font)) {
            registry_add_font(&g_app.registry, "system_default", sys_font.font_path);
        }
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
