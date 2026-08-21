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

#define ID_MENU_OPEN_SVG     1001
#define ID_MENU_OPEN_FONT    1002
#define ID_MENU_RESET_VIEW   1003
#define ID_MENU_TOGGLE_GRID  1004
#define ID_MENU_TOGGLE_LOG   1005
#define ID_MENU_EXIT         1006

#define IDC_LOG_EDIT         2001
#define MAX_FONTS            64
#define MAX_SVG_NODES        4096

// --- Font Lookup Registry for SVG ---
typedef struct {
    char key[128]; // e.g. "cmmi10", "cmsy10", "cmr10"
    char file_path[MAX_PATH];
    plutovg_font_face_t* face;
} CachedFont;

typedef struct {
    CachedFont fonts[MAX_FONTS];
    int count;
    plutovg_font_face_t* fallback_face;
} FontRegistry;

// --- Node Types in SVG ---
typedef enum {
    SVG_NODE_PATH,
    SVG_NODE_TEXT
} SvgNodeType;

typedef struct {
    SvgNodeType type;
    plutovg_matrix_t matrix;

    // Stroke & Fill
    bool has_fill;
    plutovg_color_t fill_color;
    bool has_stroke;
    plutovg_color_t stroke_color;
    float stroke_width;
    plutovg_line_join_t stroke_join;
    plutovg_line_cap_t stroke_cap;

    // Path payload
    plutovg_path_t* path;

    // Text payload
    char text[512];
    int text_len;
    char font_family[128];
    float font_size;
    bool is_italic;
    int font_weight;
    float x, y;
} SvgNode;

typedef struct {
    float width;
    float height;
    SvgNode nodes[MAX_SVG_NODES];
    int node_count;
    int path_count;
    int text_count;
} SvgDocument;

typedef struct {
    HWND hwnd_main;
    HWND hwnd_log;
    HWND hwnd_log_edit;
    HFONT hfont_log;

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

// --- Non-Modal Logging Window Helper Functions ---

static void log_clear(void) {
    if (g_app.hwnd_log_edit) {
        SetWindowTextA(g_app.hwnd_log_edit, "");
    }
}

static void log_append(const char* fmt, ...) {
    if (!g_app.hwnd_log_edit) return;

    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    int len = GetWindowTextLengthA(g_app.hwnd_log_edit);
    SendMessageA(g_app.hwnd_log_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(g_app.hwnd_log_edit, EM_REPLACESEL, FALSE, (LPARAM)buffer);
    SendMessageA(g_app.hwnd_log_edit, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
}

static void toggle_log_window(void) {
    if (!g_app.hwnd_log) return;
    bool is_visible = IsWindowVisible(g_app.hwnd_log);
    ShowWindow(g_app.hwnd_log, is_visible ? SW_HIDE : SW_SHOW);
    if (!is_visible) {
        SetForegroundWindow(g_app.hwnd_log);
    }
    CheckMenuItem(GetMenu(g_app.hwnd_main), ID_MENU_TOGGLE_LOG, !is_visible ? MF_CHECKED : MF_UNCHECKED);
}

// --- XML Content Cleaning & Entity Unescaping ---
// 1. Strips any nested XML tags (e.g. <tspan ...> and </tspan>) so tag names aren't rendered as glyphs.
// 2. Normalizes tabs/newlines to prevent random math symbols.
// 3. Unescapes standard entities (&quot; -> byte 34 / epsilon in cmmi10).
static int clean_and_unescape_text(const char* src, size_t src_len, char* dst, size_t dst_max) {
    if (!src || src_len == 0 || dst_max == 0) {
        if (dst_max > 0) dst[0] = '\0';
        return 0;
    }

    // Step 1: Strip XML tags and filter out raw control characters
    char no_tags[1024];
    size_t nt_len = 0;
    bool in_tag = false;

    for (size_t i = 0; i < src_len && nt_len < sizeof(no_tags) - 1; ++i) {
        if (src[i] == '<') {
            in_tag = true;
        } else if (src[i] == '>') {
            in_tag = false;
        } else if (!in_tag) {
            char c = src[i];
            if (c == '\r' || c == '\n' || c == '\t') {
                c = ' ';
            }
            no_tags[nt_len++] = c;
        }
    }
    no_tags[nt_len] = '\0';

    // Step 2: Trim leading & trailing whitespace
    const char* start = no_tags;
    while (nt_len > 0 && isspace((unsigned char)*start)) {
        start++;
        nt_len--;
    }
    while (nt_len > 0 && isspace((unsigned char)*(start + nt_len - 1))) {
        nt_len--;
    }

    if (nt_len == 0) {
        dst[0] = '\0';
        return 0;
    }

    // Step 3: Precise Entity Decoding
    size_t d = 0;
    for (size_t i = 0; i < nt_len && d < dst_max - 1; ) {
        if (start[i] == '&') {
            if (strncmp(start + i, "&quot;", 6) == 0) {
                dst[d++] = '\"'; // ASCII 34 (0x22)
                i += 6;
            } else if (strncmp(start + i, "&amp;", 5) == 0) {
                dst[d++] = '&';  // ASCII 38
                i += 5;
            } else if (strncmp(start + i, "&apos;", 6) == 0) {
                dst[d++] = '\''; // ASCII 39
                i += 6;
            } else if (strncmp(start + i, "&lt;", 4) == 0) {
                dst[d++] = '<';  // ASCII 60
                i += 4;
            } else if (strncmp(start + i, "&gt;", 4) == 0) {
                dst[d++] = '>';  // ASCII 62
                i += 4;
            } else if (strncmp(start + i, "&#x", 3) == 0 || strncmp(start + i, "&#X", 3) == 0) {
                char* end = NULL;
                long val = strtol(start + i + 3, &end, 16);
                if (end && *end == ';') {
                    dst[d++] = (char)(val & 0xFF);
                    i = (size_t)(end - start) + 1;
                } else {
                    dst[d++] = start[i++];
                }
            } else if (strncmp(start + i, "&#", 2) == 0) {
                char* end = NULL;
                long val = strtol(start + i + 2, &end, 10);
                if (end && *end == ';') {
                    dst[d++] = (char)(val & 0xFF);
                    i = (size_t)(end - start) + 1;
                } else {
                    dst[d++] = start[i++];
                }
            } else {
                dst[d++] = start[i++];
            }
        } else {
            dst[d++] = start[i++];
        }
    }
    dst[d] = '\0';
    return (int)d;
}

// --- Font Lookup Registry ---

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

    log_append("  [FONT] Registered '%s' -> %s", name_key, path);

    if (!reg->fallback_face) {
        reg->fallback_face = plutovg_font_face_load_from_file(path, 0);
    }
}

static plutovg_font_face_t* registry_find_font(FontRegistry* reg, const char* family_name) {
    if (!family_name || strlen(family_name) == 0) return reg->fallback_face;

    char clean_key[128];
    sanitize_key(family_name, clean_key, sizeof(clean_key));

    for (int i = 0; i < reg->count; ++i) {
        if (strcmp(reg->fonts[i].key, clean_key) == 0) {
            return reg->fonts[i].face;
        }
    }

    for (int i = 0; i < reg->count; ++i) {
        if (strstr(reg->fonts[i].key, clean_key) || strstr(clean_key, reg->fonts[i].key)) {
            return reg->fonts[i].face;
        }
    }

    return reg->fallback_face;
}

static void discover_fonts_for_svg(AppState* app, const char* svg_path) {
    char svg_dir[MAX_PATH];
    strncpy(svg_dir, svg_path, MAX_PATH - 1);
    PathRemoveFileSpecA(svg_dir);

    snprintf(app->fonts_dir, sizeof(app->fonts_dir), "%s\\fonts", svg_dir);
    log_append("[FONTS] Scanning fonts directory: %s", app->fonts_dir);

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
}

// --- Attribute and Transform Parsing ---

static bool extract_attr(const char* tag_start, const char* attr_name, char* out_val, size_t max_len) {
    char search1[128], search2[128];
    snprintf(search1, sizeof(search1), " %s=\"", attr_name);
    snprintf(search2, sizeof(search2), " %s=\'", attr_name);

    const char* p = strstr(tag_start, search1);
    if (!p) p = strstr(tag_start, search2);
    if (!p) {
        if (strncmp(tag_start, attr_name, strlen(attr_name)) == 0) p = tag_start;
    }
    if (!p) return false;

    p = strchr(p, '=');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\"' || *p == '\'') p++;

    size_t i = 0;
    while (*p && *p != '\"' && *p != '\'' && i < max_len - 1) {
        out_val[i++] = *p++;
    }
    out_val[i] = '\0';
    return true;
}

static void parse_svg_transform(const char* str, plutovg_matrix_t* out_mat) {
    plutovg_matrix_init_identity(out_mat);
    if (!str || !*str) return;

    const char* p = str;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        if (strncmp(p, "matrix", 6) == 0) {
            const char* lp = strchr(p, '(');
            const char* rp = strchr(p, ')');
            if (lp && rp && rp > lp) {
                float a, b, c, d, e, f;
                char buf[128];
                size_t len = (size_t)(rp - (lp + 1));
                if (len < sizeof(buf)) {
                    strncpy(buf, lp + 1, len);
                    buf[len] = '\0';
                    for (size_t i = 0; i < len; ++i) if (buf[i] == ',') buf[i] = ' ';
                    if (sscanf(buf, "%f %f %f %f %f %f", &a, &b, &c, &d, &e, &f) == 6) {
                        plutovg_matrix_t m;
                        plutovg_matrix_init(&m, a, b, c, d, e, f);
                        plutovg_matrix_multiply(out_mat, &m, out_mat);
                    }
                }
                p = rp + 1;
            } else { p++; }
        } else if (strncmp(p, "scale", 5) == 0) {
            const char* lp = strchr(p, '(');
            const char* rp = strchr(p, ')');
            if (lp && rp && rp > lp) {
                float sx = 1.0f, sy = 1.0f;
                char buf[128];
                size_t len = (size_t)(rp - (lp + 1));
                if (len < sizeof(buf)) {
                    strncpy(buf, lp + 1, len);
                    buf[len] = '\0';
                    for (size_t i = 0; i < len; ++i) if (buf[i] == ',') buf[i] = ' ';
                    int cnt = sscanf(buf, "%f %f", &sx, &sy);
                    if (cnt == 1) sy = sx;
                    plutovg_matrix_t m;
                    plutovg_matrix_init_scale(&m, sx, sy);
                    plutovg_matrix_multiply(out_mat, &m, out_mat);
                }
                p = rp + 1;
            } else { p++; }
        } else if (strncmp(p, "translate", 9) == 0) {
            const char* lp = strchr(p, '(');
            const char* rp = strchr(p, ')');
            if (lp && rp && rp > lp) {
                float tx = 0.0f, ty = 0.0f;
                char buf[128];
                size_t len = (size_t)(rp - (lp + 1));
                if (len < sizeof(buf)) {
                    strncpy(buf, lp + 1, len);
                    buf[len] = '\0';
                    for (size_t i = 0; i < len; ++i) if (buf[i] == ',') buf[i] = ' ';
                    sscanf(buf, "%f %f", &tx, &ty);
                    plutovg_matrix_t m;
                    plutovg_matrix_init_translate(&m, tx, ty);
                    plutovg_matrix_multiply(out_mat, &m, out_mat);
                }
                p = rp + 1;
            } else { p++; }
        } else {
            p++;
        }
    }
}

static void parse_color_attr(const char* val, bool* has_paint, plutovg_color_t* color, plutovg_color_t default_color) {
    if (!val || strlen(val) == 0 || _stricmp(val, "none") == 0) {
        *has_paint = false;
        return;
    }
    if (_stricmp(val, "currentColor") == 0 || _stricmp(val, "black") == 0) {
        *has_paint = true;
        plutovg_color_init_rgb(color, 0, 0, 0);
        return;
    }
    if (_stricmp(val, "white") == 0) {
        *has_paint = true;
        plutovg_color_init_rgb(color, 1, 1, 1);
        return;
    }
    if (plutovg_color_parse(color, val, -1)) {
        *has_paint = true;
    } else {
        *has_paint = true;
        *color = default_color;
    }
}

static void free_svg_doc(SvgDocument* doc) {
    for (int i = 0; i < doc->node_count; ++i) {
        if (doc->nodes[i].type == SVG_NODE_PATH && doc->nodes[i].path) {
            plutovg_path_destroy(doc->nodes[i].path);
            doc->nodes[i].path = NULL;
        }
    }
    doc->node_count = 0;
    doc->path_count = 0;
    doc->text_count = 0;
}

static void parse_svg(AppState* app, const char* svg_path) {
    FILE* f = fopen(svg_path, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) { fclose(f); return; }

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) { fclose(f); return; }

    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    free_svg_doc(&app->svg);
    app->svg.width = 600.0f;
    app->svg.height = 400.0f;

    char val[1024];
    if (extract_attr(buffer, "width", val, sizeof(val))) app->svg.width = (float)atof(val);
    if (extract_attr(buffer, "height", val, sizeof(val))) app->svg.height = (float)atof(val);

    log_append("[SVG] Parsing elements (Document size: %.1fx%.1f)", app->svg.width, app->svg.height);

    const char* cur = buffer;
    while (*cur && app->svg.node_count < MAX_SVG_NODES) {
        const char* tag_open = strchr(cur, '<');
        if (!tag_open) break;

        if (strncmp(tag_open, "<!--", 4) == 0) {
            const char* end_cmt = strstr(tag_open, "-->");
            cur = end_cmt ? end_cmt + 3 : tag_open + 4;
            continue;
        }

        // 1. Parse <path ... />
        if (strncmp(tag_open, "<path", 5) == 0 && (isspace((unsigned char)tag_open[5]) || tag_open[5] == '>')) {
            const char* tag_end = strchr(tag_open, '>');
            if (!tag_end) { cur = tag_open + 5; continue; }

            char tag_buf[2048];
            size_t tag_len = (size_t)(tag_end - tag_open + 1);
            if (tag_len < sizeof(tag_buf)) {
                strncpy(tag_buf, tag_open, tag_len);
                tag_buf[tag_len] = '\0';

                char d_str[4096];
                if (extract_attr(tag_buf, "d", d_str, sizeof(d_str))) {
                    SvgNode* node = &app->svg.nodes[app->svg.node_count++];
                    memset(node, 0, sizeof(SvgNode));
                    node->type = SVG_NODE_PATH;
                    node->path = plutovg_path_create();
                    plutovg_path_parse(node->path, d_str, -1);

                    if (extract_attr(tag_buf, "transform", val, sizeof(val))) {
                        parse_svg_transform(val, &node->matrix);
                    } else {
                        plutovg_matrix_init_identity(&node->matrix);
                    }

                    node->stroke_width = 1.0f;
                    node->stroke_join = PLUTOVG_LINE_JOIN_MITER;
                    node->stroke_cap = PLUTOVG_LINE_CAP_BUTT;

                    if (extract_attr(tag_buf, "stroke-width", val, sizeof(val))) node->stroke_width = (float)atof(val);
                    if (extract_attr(tag_buf, "stroke-linejoin", val, sizeof(val))) {
                        if (_stricmp(val, "bevel") == 0) node->stroke_join = PLUTOVG_LINE_JOIN_BEVEL;
                        else if (_stricmp(val, "round") == 0) node->stroke_join = PLUTOVG_LINE_JOIN_ROUND;
                    }
                    if (extract_attr(tag_buf, "stroke-linecap", val, sizeof(val))) {
                        if (_stricmp(val, "round") == 0) node->stroke_cap = PLUTOVG_LINE_CAP_ROUND;
                        else if (_stricmp(val, "square") == 0) node->stroke_cap = PLUTOVG_LINE_CAP_SQUARE;
                    }

                    plutovg_color_t black;
                    plutovg_color_init_rgb(&black, 0, 0, 0);

                    if (extract_attr(tag_buf, "stroke", val, sizeof(val))) {
                        parse_color_attr(val, &node->has_stroke, &node->stroke_color, black);
                    }
                    if (extract_attr(tag_buf, "fill", val, sizeof(val))) {
                        parse_color_attr(val, &node->has_fill, &node->fill_color, black);
                    } else {
                        node->has_fill = !node->has_stroke;
                        node->fill_color = black;
                    }

                    app->svg.path_count++;
                }
            }
            cur = tag_end + 1;
            continue;
        }

        // 2. Parse <text ...>content</text>
        if (strncmp(tag_open, "<text", 5) == 0 && (isspace((unsigned char)tag_open[5]) || tag_open[5] == '>')) {
            const char* tag_end = strchr(tag_open, '>');
            const char* close_tag = strstr(tag_open, "</text>");
            if (tag_end && close_tag && close_tag > tag_end) {
                char tag_buf[1024];
                size_t tag_len = (size_t)(tag_end - tag_open + 1);
                if (tag_len < sizeof(tag_buf)) {
                    strncpy(tag_buf, tag_open, tag_len);
                    tag_buf[tag_len] = '\0';

                    SvgNode* node = &app->svg.nodes[app->svg.node_count++];
                    memset(node, 0, sizeof(SvgNode));
                    node->type = SVG_NODE_TEXT;
                    node->font_size = 1.0f;
                    node->font_weight = 400;
                    node->has_fill = true;
                    plutovg_color_init_rgb(&node->fill_color, 0, 0, 0);

                    if (extract_attr(tag_buf, "font-family", val, sizeof(val))) {
                        strncpy(node->font_family, val, sizeof(node->font_family) - 1);
                    }
                    if (extract_attr(tag_buf, "font-size", val, sizeof(val))) {
                        node->font_size = (float)atof(val);
                    }
                    if (extract_attr(tag_buf, "font-style", val, sizeof(val))) {
                        if (_stricmp(val, "italic") == 0) node->is_italic = true;
                    }
                    if (extract_attr(tag_buf, "font-weight", val, sizeof(val))) {
                        node->font_weight = atoi(val);
                    }
                    if (extract_attr(tag_buf, "x", val, sizeof(val))) node->x = (float)atof(val);
                    if (extract_attr(tag_buf, "y", val, sizeof(val))) node->y = (float)atof(val);

                    if (extract_attr(tag_buf, "fill", val, sizeof(val))) {
                        parse_color_attr(val, &node->has_fill, &node->fill_color, node->fill_color);
                    }

                    if (extract_attr(tag_buf, "transform", val, sizeof(val))) {
                        parse_svg_transform(val, &node->matrix);
                    } else {
                        plutovg_matrix_init_identity(&node->matrix);
                    }

                    // Extract & Clean inner text (stripping <tspan> tags and unescaping &quot;)
                    const char* text_start = tag_end + 1;
                    size_t raw_len = (size_t)(close_tag - text_start);
                    node->text_len = clean_and_unescape_text(text_start, raw_len, node->text, sizeof(node->text));

                    if (node->text_len > 0) {
                        app->svg.text_count++;
                        log_append("  [TEXT] family='%s', size=%.1f, len=%d, first_byte=0x%02X ('%c')",
                                   node->font_family, node->font_size, node->text_len,
                                   (unsigned char)node->text[0],
                                   isprint((unsigned char)node->text[0]) ? node->text[0] : '?');
                    } else {
                        // Discard empty text node
                        app->svg.node_count--;
                    }
                }
                cur = close_tag + 7;
                continue;
            }
        }

        cur = tag_open + 1;
    }

    log_append("[RESULT] Loaded %d paths and %d text nodes successfully.", app->svg.path_count, app->svg.text_count);
    free(buffer);
}

static void reset_view(AppState* app) {
    app->zoom = 1.0f;
    app->rotation_deg = 0.0f;
    app->shear_x = 0.0f;
    app->pan_x = ((float)app->width - app->svg.width) / 2.0f;
    app->pan_y = ((float)app->height - app->svg.height) / 2.0f;

    if (app->width > 0 && app->height > 0 && app->svg.width > 0 && app->svg.height > 0) {
        float sx = ((float)app->width * 0.85f) / app->svg.width;
        float sy = ((float)app->height * 0.85f) / app->svg.height;
        float fit = (sx < sy) ? sx : sy;
        if (fit < 1.0f && fit > 0.0001f) {
            app->zoom = fit;
            app->pan_x = ((float)app->width - (app->svg.width * app->zoom)) / 2.0f;
            app->pan_y = ((float)app->height - (app->svg.height * app->zoom)) / 2.0f;
        }
    }
}

static void load_svg_file(AppState* app, const char* path) {
    if (!path || strlen(path) == 0) return;

    log_clear();
    log_append("[APP] Opening SVG: %s", path);

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
    plutovg_canvas_restore(canvas);
}

static void render(AppState* app) {
    if (!app->canvas) return;

    // Viewport background
    plutovg_canvas_save(app->canvas);
    plutovg_canvas_reset_matrix(app->canvas);
    plutovg_canvas_set_rgb(app->canvas, 0.12f, 0.13f, 0.15f);
    plutovg_canvas_fill_rect(app->canvas, 0, 0, (float)app->width, (float)app->height);
    plutovg_canvas_restore(app->canvas);

    // Viewport transformations
    plutovg_canvas_save(app->canvas);
    plutovg_canvas_translate(app->canvas, app->pan_x, app->pan_y);
    plutovg_canvas_scale(app->canvas, app->zoom, app->zoom);
    plutovg_canvas_rotate(app->canvas, app->rotation_deg * (3.1415926535f / 180.0f));

    if (fabsf(app->shear_x) > 0.0001f) {
        plutovg_matrix_t sm;
        plutovg_matrix_init_shear(&sm, app->shear_x, 0.0f);
        plutovg_canvas_transform(app->canvas, &sm);
    }

    if (app->show_grid) draw_grid(app->canvas);

    // Render SVG Elements
    if (app->has_svg_loaded) {
        // White canvas background
        plutovg_canvas_save(app->canvas);
        plutovg_canvas_set_rgb(app->canvas, 1.0f, 1.0f, 1.0f);
        plutovg_canvas_fill_rect(app->canvas, 0, 0, app->svg.width, app->svg.height);
        plutovg_canvas_restore(app->canvas);

        for (int i = 0; i < app->svg.node_count; ++i) {
            SvgNode* node = &app->svg.nodes[i];

            plutovg_canvas_save(app->canvas);
            plutovg_canvas_transform(app->canvas, &node->matrix);

            if (node->type == SVG_NODE_PATH && node->path) {
                if (node->has_fill) {
                    plutovg_canvas_set_color(app->canvas, &node->fill_color);
                    plutovg_canvas_fill_path(app->canvas, node->path);
                }
                if (node->has_stroke) {
                    plutovg_canvas_set_color(app->canvas, &node->stroke_color);
                    plutovg_canvas_set_line_width(app->canvas, node->stroke_width);
                    plutovg_canvas_set_line_join(app->canvas, node->stroke_join);
                    plutovg_canvas_set_line_cap(app->canvas, node->stroke_cap);
                    plutovg_canvas_stroke_path(app->canvas, node->path);
                }
            } else if (node->type == SVG_NODE_TEXT) {
                plutovg_font_face_t* face = registry_find_font(&app->registry, node->font_family);
                if (face) {
                    plutovg_canvas_set_font_face(app->canvas, face);
                    plutovg_canvas_set_font_size(app->canvas, node->font_size);
                    plutovg_canvas_set_color(app->canvas, &node->fill_color);

                    // Latin-1 maps 1-byte character codes (0..255) to uninstalled font glyphs
                    plutovg_canvas_fill_text(app->canvas, node->text, node->text_len,
                                            PLUTOVG_TEXT_ENCODING_LATIN1, node->x, node->y);
                }
            }

            plutovg_canvas_restore(app->canvas);
        }
    }

    plutovg_canvas_restore(app->canvas);
}

static void zoom_at(AppState* app, float sx, float sy, float factor) {
    float new_zoom = app->zoom * factor;
    if (new_zoom < 0.001f) new_zoom = 0.001f;
    if (new_zoom > 500.0f) new_zoom = 500.0f;

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

// Log Window Procedure
static LRESULT CALLBACK LogWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (g_app.hwnd_log_edit) {
            MoveWindow(g_app.hwnd_log_edit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        CheckMenuItem(GetMenu(g_app.hwnd_main), ID_MENU_TOGGLE_LOG, MF_UNCHECKED);
        return 0; // Hide rather than destroy
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// Main Window Procedure
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
        case 'L':
            toggle_log_window();
            break;
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
        case ID_MENU_TOGGLE_LOG:
            toggle_log_window();
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
            bmi.bmiHeader.biHeight = -h;
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
        free_svg_doc(&g_app.svg);
        registry_init(&g_app.registry);
        if (g_app.hfont_log) DeleteObject(g_app.hfont_log);
        if (g_app.canvas) plutovg_canvas_destroy(g_app.canvas);
        if (g_app.surface) plutovg_surface_destroy(g_app.surface);
        if (g_app.hwnd_log) DestroyWindow(g_app.hwnd_log);
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
    AppendMenuA(hViewMenu, MF_STRING | MF_UNCHECKED, ID_MENU_TOGGLE_LOG, "Show Debug &Log\t(L)");

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

    // 1. Register Main Window Class
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "PlutoVGViewerWindowClass";
    if (!RegisterClassExA(&wc)) return 1;

    // 2. Register Modeless Log Window Class
    WNDCLASSEXA log_wc = {0};
    log_wc.cbSize = sizeof(WNDCLASSEXA);
    log_wc.style = CS_HREDRAW | CS_VREDRAW;
    log_wc.lpfnWndProc = LogWndProc;
    log_wc.hInstance = hInstance;
    log_wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    log_wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    log_wc.lpszClassName = "PlutoVGLogWindowClass";
    RegisterClassExA(&log_wc);

    // 3. Create Main Window
    g_app.hwnd_main = CreateWindowExA(
        WS_EX_ACCEPTFILES,
        wc.lpszClassName,
        "PlutoVG Typography & SVG Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1100, 800,
        NULL, NULL, hInstance, NULL
    );
    if (!g_app.hwnd_main) return 1;

    // 4. Create Modeless Popup Log Window (Hidden by Default)
    g_app.hwnd_log = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        log_wc.lpszClassName,
        "SVG & Typography Debug Log",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        640, 480,
        g_app.hwnd_main,
        NULL, hInstance, NULL
    );

    if (g_app.hwnd_log) {
        RECT rc;
        GetClientRect(g_app.hwnd_log, &rc);
        g_app.hwnd_log_edit = CreateWindowExA(
            0,
            "EDIT",
            "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            0, 0, rc.right, rc.bottom,
            g_app.hwnd_log,
            (HMENU)IDC_LOG_EDIT,
            hInstance,
            NULL
        );

        g_app.hfont_log = CreateFontA(
            15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas"
        );
        if (g_app.hfont_log) {
            SendMessageA(g_app.hwnd_log_edit, WM_SETFONT, (WPARAM)g_app.hfont_log, TRUE);
        }
    }

    create_app_menu(g_app.hwnd_main);
    ShowWindow(g_app.hwnd_main, nCmdShow);
    UpdateWindow(g_app.hwnd_main);

    // 5. Check Command Line Argument
    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argvW && argc > 1) {
        char initial_path[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, argvW[1], -1, initial_path, MAX_PATH, NULL, NULL);

        const char* ext = PathFindExtensionA(initial_path);
        if (_stricmp(ext, ".svg") == 0) {
            load_svg_file(&g_app, initial_path);
        }
        LocalFree(argvW);
        InvalidateRect(g_app.hwnd_main, NULL, FALSE);
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
