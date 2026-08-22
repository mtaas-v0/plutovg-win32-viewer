#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <plutovg.h>
#include "svg_engine.h"
#include "font_helper.h"

// Command IDs
#define ID_MENU_OPEN_SVG          1001
#define ID_MENU_OPEN_FONT         1002
#define ID_MENU_SAVE_ANNOT        1003
#define ID_MENU_SAVE_ANNOT_AS     1004
#define ID_MENU_EXPORT_SVG        1005
#define ID_MENU_EXIT              1006

#define ID_MENU_RESET_VIEW        1010
#define ID_MENU_TOGGLE_GRID       1011
#define ID_MENU_TOGGLE_LOG        1012
#define ID_MENU_TOGGLE_MODE       1013
#define ID_MENU_TOGGLE_SELECTABLE 1014
#define ID_MENU_DELETE_ANNOT      1015
#define ID_MENU_CLEAR_ANNOT       1016
#define ID_MENU_CYCLE_CLASS       1017
#define ID_MENU_EDIT_URLLINK      1018

#define ID_MENU_CLASS_BASE        1100
#define ID_MENU_ZOOM_IN           1201
#define ID_MENU_ZOOM_OUT          1202

#define IDC_LOG_EDIT              2001

// URL Dialog Controls
#define IDC_DLG_URL_EDIT          3001
#define IDC_DLG_VIEWPORT_EDIT     3002
#define IDC_DLG_BTN_BROWSE        3003
#define IDC_DLG_BTN_PICK_VIEWPORT 3004
#define IDC_DLG_BTN_OK            3005
#define IDC_DLG_BTN_CANCEL        3006

// Viewport Picker Controls
#define IDC_VP_APPLY_BOX          4001
#define IDC_VP_APPLY_VIEW         4002
#define IDC_VP_CANCEL             4003

#define MAX_ANNOTATIONS           512
#define MAX_CATEGORIES            9

typedef enum {
    APP_MODE_NAVIGATE,
    APP_MODE_ANNOTATE
} AppInteractionMode;

typedef enum {
    HANDLE_NONE = 0,
    HANDLE_BODY,
    HANDLE_TOP_LEFT,
    HANDLE_TOP_RIGHT,
    HANDLE_BOTTOM_LEFT,
    HANDLE_BOTTOM_RIGHT
} ResizeHandle;

typedef struct {
    int id;
    char name[32];
    float r, g, b;
} AnnotationCategory;

static const AnnotationCategory DEFAULT_CATEGORIES[MAX_CATEGORIES] = {
    { 0, "formula",  0.95f, 0.20f, 0.20f }, // Red
    { 1, "symbol",   0.15f, 0.80f, 0.35f }, // Green
    { 2, "text",     0.10f, 0.55f, 0.98f }, // Blue
    { 3, "operator", 1.00f, 0.60f, 0.10f }, // Orange
    { 4, "fraction", 0.70f, 0.30f, 0.90f }, // Purple
    { 5, "matrix",   0.10f, 0.80f, 0.85f }, // Cyan
    { 6, "script",   0.95f, 0.85f, 0.15f }, // Yellow
    { 7, "diagram",  0.95f, 0.30f, 0.65f }, // Pink
    { 8, "urllink",  0.05f, 0.85f, 0.80f }  // Teal
};

typedef struct {
    int id;
    int class_id;
    char class_name[32];

    float x_center;
    float y_center;
    float width;
    float height;

    float x, y, w, h;

    char url[512];
    bool has_dest_viewport;
    float dest_viewport[4];
} YoloAnnotation;

typedef struct {
    HWND hwnd_main;
    HWND hwnd_log;
    HWND hwnd_log_edit;
    HFONT hfont_log;

    // Viewport
    float zoom;
    float pan_x;
    float pan_y;
    float rotation_deg;
    float shear_x;

    // Mouse Interaction
    bool is_dragging_view;
    bool is_drawing_box;
    bool is_transforming_box;
    ResizeHandle active_handle;
    POINT last_mouse;
    float drag_start_canvas_x;
    float drag_start_canvas_y;
    float drag_curr_canvas_x;
    float drag_curr_canvas_y;

    // Annotation Data & Settings
    AppInteractionMode interaction_mode;
    bool annotations_selectable;
    bool is_dirty;
    YoloAnnotation annotations[MAX_ANNOTATIONS];
    int annotation_count;
    int selected_annotation_idx;
    int active_class_id;
    char annot_file_path[MAX_PATH];

    // Canvas Surface
    int width;
    int height;
    plutovg_surface_t* surface;
    plutovg_canvas_t* canvas;

    // Isolated SVG Document
    char current_svg_path[MAX_PATH];
    char current_svg_name[128];
    SvgDocument svg;
    bool has_svg_loaded;
    bool show_grid;

    plutovg_font_face_t* system_mono_face;
} AppState;

static AppState g_app;

// Forward declarations
static void update_window_title(void);
static bool save_yolo_annotations(AppState* app, const char* yaml_path);
static bool browse_file(HWND parent, const char* filter, char* out_path, size_t max_len, bool is_save, const char* def_ext);
static void screen_to_canvas(AppState* app, float sx, float sy, float* cx, float* cy);
static void load_svg_file(AppState* app, const char* path);
static void open_destination_viewport_picker(HWND parent, const char* target_url, HWND hwnd_target_edit);

// --- Logging Helper Functions ---

static void log_clear(void) {
    if (g_app.hwnd_log_edit) SetWindowTextA(g_app.hwnd_log_edit, "");
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
    if (!is_visible) SetForegroundWindow(g_app.hwnd_log);
    CheckMenuItem(GetMenu(g_app.hwnd_main), ID_MENU_TOGGLE_LOG, !is_visible ? MF_CHECKED : MF_UNCHECKED);
}

// --- Window Title & Dirty Check ---

static void update_window_title(void) {
    char title[512];
    if (g_app.has_svg_loaded) {
        snprintf(title, sizeof(title), "%s%s - PlutoVG Typography & YOLO Annotation Editor",
                 g_app.is_dirty ? "*" : "", g_app.current_svg_name);
    } else {
        snprintf(title, sizeof(title), "PlutoVG Typography & YOLO Annotation Editor");
    }
    SetWindowTextA(g_app.hwnd_main, title);
}

static bool check_save_changes_prompt(HWND hwnd) {
    if (!g_app.is_dirty || g_app.annotation_count == 0) return true;

    int res = MessageBoxA(hwnd,
        "You have unsaved annotations.\nDo you want to save changes before continuing?",
        "Unsaved Annotations",
        MB_YESNOCANCEL | MB_ICONQUESTION);

    if (res == IDYES) {
        if (strlen(g_app.annot_file_path) > 0) {
            return save_yolo_annotations(&g_app, g_app.annot_file_path);
        } else {
            char path[MAX_PATH];
            if (browse_file(hwnd, "YOLO Annotation YAML (*.yaml)\0*.yaml\0All Files (*.*)\0*.*\0", path, sizeof(path), true, "yaml")) {
                return save_yolo_annotations(&g_app, path);
            }
            return false;
        }
    } else if (res == IDNO) {
        return true;
    } else {
        return false;
    }
}

static void load_system_mono_font(AppState* app) {
    const char* sys_fonts[] = {
        "C:\\Windows\\Fonts\\consola.ttf",
        "C:\\Windows\\Fonts\\cour.ttf",
        "C:\\Windows\\Fonts\\lucon.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf"
    };

    for (size_t i = 0; i < sizeof(sys_fonts) / sizeof(sys_fonts[0]); ++i) {
        if (GetFileAttributesA(sys_fonts[i]) != INVALID_FILE_ATTRIBUTES) {
            app->system_mono_face = plutovg_font_face_load_from_file(sys_fonts[i], 0);
            if (app->system_mono_face) return;
        }
    }
}

static void screen_to_canvas(AppState* app, float sx, float sy, float* cx, float* cy) {
    float dx = (sx - app->pan_x) / app->zoom;
    float dy = (sy - app->pan_y) / app->zoom;
    float rad = -app->rotation_deg * (3.1415926535f / 180.0f);
    *cx = dx * cosf(rad) - dy * sinf(rad);
    *cy = dx * sinf(rad) + dy * cosf(rad);
}

static void focus_viewport_bbox(AppState* app, float bx, float by, float bw, float bh) {
    if (bw <= 0 || bh <= 0 || app->width <= 0 || app->height <= 0) return;

    float sx = ((float)app->width * 0.90f) / bw;
    float sy = ((float)app->height * 0.90f) / bh;
    app->zoom = (sx < sy) ? sx : sy;
    app->rotation_deg = 0.0f;
    app->shear_x = 0.0f;

    float box_center_x = bx + bw / 2.0f;
    float box_center_y = by + bh / 2.0f;
    app->pan_x = ((float)app->width / 2.0f) - (box_center_x * app->zoom);
    app->pan_y = ((float)app->height / 2.0f) - (box_center_y * app->zoom);
}

static void update_annotation_pixels(AppState* app, YoloAnnotation* a) {
    float sw = app->svg.width > 0 ? app->svg.width : 800.0f;
    float sh = app->svg.height > 0 ? app->svg.height : 600.0f;

    a->w = a->width * sw;
    a->h = a->height * sh;
    a->x = (a->x_center * sw) - (a->w / 2.0f);
    a->y = (a->y_center * sh) - (a->h / 2.0f);
}

static void update_annotation_normalized(AppState* app, YoloAnnotation* a, float x, float y, float w, float h) {
    float sw = app->svg.width > 0 ? app->svg.width : 800.0f;
    float sh = app->svg.height > 0 ? app->svg.height : 600.0f;

    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }

    a->x = x; a->y = y; a->w = w; a->h = h;
    a->width = w / sw;
    a->height = h / sh;
    a->x_center = (x + w / 2.0f) / sw;
    a->y_center = (y + h / 2.0f) / sh;
}

// --- Extended YOLO YAML Serializer & Loader ---

static void get_default_annot_path(const char* svg_path, char* out_yaml, size_t max_len) {
    strncpy(out_yaml, svg_path, max_len - 1);
    char* dot = strrchr(out_yaml, '.');
    if (dot) *dot = '\0';
    strncat(out_yaml, "-yoloAnnot.yaml", max_len - strlen(out_yaml) - 1);
}

static bool save_yolo_annotations(AppState* app, const char* yaml_path) {
    if (!yaml_path || strlen(yaml_path) == 0) return false;

    FILE* f = fopen(yaml_path, "w");
    if (!f) {
        log_append("[ANNOT] Error: Failed to open %s for writing.", yaml_path);
        return false;
    }

    fprintf(f, "# YOLO Extended Annotation File for %s\n", app->current_svg_name);
    fprintf(f, "image: \"%s\"\n", app->current_svg_name);
    fprintf(f, "image_width: %.2f\n", app->svg.width);
    fprintf(f, "image_height: %.2f\n\n", app->svg.height);

    fprintf(f, "categories:\n");
    for (int i = 0; i < MAX_CATEGORIES; ++i) {
        fprintf(f, "  - id: %d\n    name: \"%s\"\n", DEFAULT_CATEGORIES[i].id, DEFAULT_CATEGORIES[i].name);
    }

    fprintf(f, "\nannotations:\n");
    for (int i = 0; i < app->annotation_count; ++i) {
        YoloAnnotation* a = &app->annotations[i];
        fprintf(f, "  - id: %d\n", i);
        fprintf(f, "    class_id: %d\n", a->class_id);
        fprintf(f, "    class_name: \"%s\"\n", a->class_name);
        fprintf(f, "    bbox: [%.6f, %.6f, %.6f, %.6f]\n", a->x_center, a->y_center, a->width, a->height);
        fprintf(f, "    rect: [%.2f, %.2f, %.2f, %.2f]\n", a->x, a->y, a->w, a->h);

        if (strlen(a->url) > 0) {
            fprintf(f, "    url: \"%s\"\n", a->url);
        }
        if (a->has_dest_viewport) {
            fprintf(f, "    destination_viewport_bbox: [%.2f, %.2f, %.2f, %.2f]\n",
                    a->dest_viewport[0], a->dest_viewport[1], a->dest_viewport[2], a->dest_viewport[3]);
        }
        fprintf(f, "\n");
    }

    fclose(f);
    strncpy(app->annot_file_path, yaml_path, MAX_PATH - 1);
    app->is_dirty = false;
    update_window_title();
    log_append("[ANNOT] Saved %d annotations to %s", app->annotation_count, yaml_path);
    return true;
}

static bool load_yolo_annotations(AppState* app, const char* yaml_path) {
    if (!yaml_path || GetFileAttributesA(yaml_path) == INVALID_FILE_ATTRIBUTES) return false;

    FILE* f = fopen(yaml_path, "r");
    if (!f) return false;

    app->annotation_count = 0;
    app->selected_annotation_idx = -1;

    char line[512];
    YoloAnnotation current = {0};
    bool in_annotation = false;

    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p && isspace((unsigned char)*p)) p++;

        if (*p == '#' || *p == '\0') continue;

        if (strncmp(p, "- id:", 5) == 0) {
            if (in_annotation && app->annotation_count < MAX_ANNOTATIONS) {
                update_annotation_pixels(app, &current);
                app->annotations[app->annotation_count++] = current;
                memset(&current, 0, sizeof(YoloAnnotation));
            }
            in_annotation = true;
            current.id = atoi(p + 5);
        } else if (in_annotation) {
            if (strncmp(p, "class_id:", 9) == 0) {
                current.class_id = atoi(p + 9);
                if (current.class_id >= 0 && current.class_id < MAX_CATEGORIES) {
                    strncpy(current.class_name, DEFAULT_CATEGORIES[current.class_id].name, sizeof(current.class_name) - 1);
                }
            } else if (strncmp(p, "class_name:", 11) == 0) {
                char* q1 = strchr(p, '\"');
                char* q2 = q1 ? strchr(q1 + 1, '\"') : NULL;
                if (q1 && q2) {
                    size_t len = q2 - (q1 + 1);
                    if (len < sizeof(current.class_name)) {
                        strncpy(current.class_name, q1 + 1, len);
                        current.class_name[len] = '\0';
                    }
                }
            } else if (strncmp(p, "bbox:", 5) == 0) {
                char* b_start = strchr(p, '[');
                if (b_start) {
                    sscanf(b_start, "[%f, %f, %f, %f]",
                           &current.x_center, &current.y_center, &current.width, &current.height);
                }
            } else if (strncmp(p, "url:", 4) == 0) {
                char* q1 = strchr(p, '\"');
                char* q2 = q1 ? strchr(q1 + 1, '\"') : NULL;
                if (q1 && q2) {
                    size_t ulen = q2 - (q1 + 1);
                    if (ulen < sizeof(current.url)) {
                        strncpy(current.url, q1 + 1, ulen);
                        current.url[ulen] = '\0';
                    }
                } else {
                    char* ustart = p + 4;
                    while (*ustart && isspace((unsigned char)*ustart)) ustart++;
                    size_t ulen = strlen(ustart);
                    while (ulen > 0 && isspace((unsigned char)ustart[ulen - 1])) ulen--;
                    if (ulen < sizeof(current.url)) {
                        strncpy(current.url, ustart, ulen);
                        current.url[ulen] = '\0';
                    }
                }
            } else if (strncmp(p, "destination_viewport_bbox:", 26) == 0) {
                char* b_start = strchr(p, '[');
                if (b_start) {
                    if (sscanf(b_start, "[%f, %f, %f, %f]",
                               &current.dest_viewport[0], &current.dest_viewport[1],
                               &current.dest_viewport[2], &current.dest_viewport[3]) == 4) {
                        current.has_dest_viewport = true;
                    }
                }
            }
        }
    }

    if (in_annotation && app->annotation_count < MAX_ANNOTATIONS) {
        update_annotation_pixels(app, &current);
        app->annotations[app->annotation_count++] = current;
    }

    fclose(f);
    strncpy(app->annot_file_path, yaml_path, MAX_PATH - 1);
    app->is_dirty = false;
    update_window_title();
    log_append("[ANNOT] Auto-loaded %d annotations from %s", app->annotation_count, yaml_path);
    return true;
}

static void launch_urllink(const YoloAnnotation* a) {
    if (!a || strlen(a->url) == 0) return;

    log_append("[LINK] Launching URL: %s", a->url);

    if (_strnicmp(a->url, "http://", 7) == 0 || _strnicmp(a->url, "https://", 8) == 0) {
        ShellExecuteA(NULL, "open", a->url, NULL, NULL, SW_SHOWNORMAL);
        return;
    }

    char target_file[MAX_PATH];
    const char* url_str = a->url;
    if (_strnicmp(url_str, "file:///", 8) == 0) url_str += 8;
    else if (_strnicmp(url_str, "file://", 7) == 0) url_str += 7;

    strncpy(target_file, url_str, MAX_PATH - 1);
    for (char* p = target_file; *p; ++p) {
        if (*p == '/') *p = '\\';
    }

    const char* ext = PathFindExtensionA(target_file);
    if (_stricmp(ext, ".svg") == 0) {
        char exe_path[MAX_PATH];
        GetModuleFileNameA(NULL, exe_path, MAX_PATH);

        char cmd_line[1024];
        if (a->has_dest_viewport) {
            snprintf(cmd_line, sizeof(cmd_line), "\"%s\" \"%s\" --bbox %g %g %g %g",
                     exe_path, target_file,
                     a->dest_viewport[0], a->dest_viewport[1],
                     a->dest_viewport[2], a->dest_viewport[3]);
        } else {
            snprintf(cmd_line, sizeof(cmd_line), "\"%s\" \"%s\"", exe_path, target_file);
        }

        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi = {0};
        if (CreateProcessA(NULL, cmd_line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            log_append("[LINK] Launched new viewer instance: %s", cmd_line);
        } else {
            log_append("[LINK] Error launching process: %s", cmd_line);
        }
    } else {
        ShellExecuteA(NULL, "open", target_file, NULL, NULL, SW_SHOWNORMAL);
    }
}

// --- Hit Testing ---

static ResizeHandle hit_test_handles(AppState* app, const YoloAnnotation* a, float cx, float cy) {
    float handle_size = 12.0f / app->zoom;
    float hs2 = handle_size / 2.0f;

    if (cx >= a->x - hs2 && cx <= a->x + hs2 && cy >= a->y - hs2 && cy <= a->y + hs2) return HANDLE_TOP_LEFT;
    if (cx >= a->x + a->w - hs2 && cx <= a->x + a->w + hs2 && cy >= a->y - hs2 && cy <= a->y + hs2) return HANDLE_TOP_RIGHT;
    if (cx >= a->x - hs2 && cx <= a->x + hs2 && cy >= a->y + a->h - hs2 && cy <= a->y + a->h + hs2) return HANDLE_BOTTOM_LEFT;
    if (cx >= a->x + a->w - hs2 && cx <= a->x + a->w + hs2 && cy >= a->y + a->h - hs2 && cy <= a->y + a->h + hs2) return HANDLE_BOTTOM_RIGHT;

    if (cx >= a->x && cx <= a->x + a->w && cy >= a->y && cy <= a->y + a->h) return HANDLE_BODY;
    return HANDLE_NONE;
}

static int hit_test_annotations(AppState* app, float cx, float cy, ResizeHandle* out_handle) {
    if (app->selected_annotation_idx >= 0 && app->selected_annotation_idx < app->annotation_count) {
        ResizeHandle h = hit_test_handles(app, &app->annotations[app->selected_annotation_idx], cx, cy);
        if (h != HANDLE_NONE) {
            *out_handle = h;
            return app->selected_annotation_idx;
        }
    }

    for (int i = app->annotation_count - 1; i >= 0; --i) {
        ResizeHandle h = hit_test_handles(app, &app->annotations[i], cx, cy);
        if (h != HANDLE_NONE) {
            *out_handle = h;
            return i;
        }
    }

    *out_handle = HANDLE_NONE;
    return -1;
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

    if (!SvgDocument_LoadFromFile(&app->svg, path, log_append)) {
        log_append("[APP] Error: Failed to parse %s", path);
        return;
    }

    strncpy(app->current_svg_path, path, MAX_PATH - 1);
    FontHelper_GetFontName(path, app->current_svg_name, sizeof(app->current_svg_name));
    app->has_svg_loaded = true;

    char yml_path[MAX_PATH];
    get_default_annot_path(path, yml_path, sizeof(yml_path));
    if (GetFileAttributesA(yml_path) != INVALID_FILE_ATTRIBUTES) {
        load_yolo_annotations(app, yml_path);
    } else {
        app->annotation_count = 0;
        app->selected_annotation_idx = -1;
        strncpy(app->annot_file_path, yml_path, MAX_PATH - 1);
        app->is_dirty = false;
        update_window_title();
    }

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

// --- Red/Green Origin Marker & Grid ---
static void draw_grid_and_origin(plutovg_canvas_t* canvas) {
    plutovg_canvas_save(canvas);

    plutovg_canvas_set_rgb(canvas, 0.18f, 0.20f, 0.23f);
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

    // X-Axis (Red) & Y-Axis (Green)
    plutovg_canvas_set_rgb(canvas, 0.85f, 0.25f, 0.25f);
    plutovg_canvas_set_line_width(canvas, 2.0f);
    plutovg_canvas_move_to(canvas, -extent, 0.0f);
    plutovg_canvas_line_to(canvas, extent, 0.0f);
    plutovg_canvas_stroke(canvas);

    plutovg_canvas_set_rgb(canvas, 0.25f, 0.85f, 0.35f);
    plutovg_canvas_set_line_width(canvas, 2.0f);
    plutovg_canvas_move_to(canvas, 0.0f, -extent);
    plutovg_canvas_line_to(canvas, 0.0f, extent);
    plutovg_canvas_stroke(canvas);

    // Directional Arrows
    plutovg_canvas_set_rgb(canvas, 1.0f, 0.3f, 0.3f);
    plutovg_canvas_set_line_width(canvas, 3.0f);
    plutovg_canvas_move_to(canvas, 0.0f, 0.0f);
    plutovg_canvas_line_to(canvas, 45.0f, 0.0f);
    plutovg_canvas_line_to(canvas, 37.0f, -5.0f);
    plutovg_canvas_move_to(canvas, 45.0f, 0.0f);
    plutovg_canvas_line_to(canvas, 37.0f, 5.0f);
    plutovg_canvas_stroke(canvas);

    plutovg_canvas_set_rgb(canvas, 0.3f, 1.0f, 0.4f);
    plutovg_canvas_set_line_width(canvas, 3.0f);
    plutovg_canvas_move_to(canvas, 0.0f, 0.0f);
    plutovg_canvas_line_to(canvas, 0.0f, 45.0f);
    plutovg_canvas_line_to(canvas, -5.0f, 37.0f);
    plutovg_canvas_move_to(canvas, 0.0f, 45.0f);
    plutovg_canvas_line_to(canvas, 5.0f, 37.0f);
    plutovg_canvas_stroke(canvas);

    // Center Origin Dot
    plutovg_canvas_set_rgb(canvas, 1.0f, 0.85f, 0.2f);
    plutovg_canvas_arc(canvas, 0.0f, 0.0f, 3.5f, 0.0f, 6.2831853f, 0);
    plutovg_canvas_fill(canvas);

    plutovg_canvas_restore(canvas);
}

// --- Render Semi-Transparent YOLO Annotations ---
static void draw_annotations(AppState* app) {
    if (!app->canvas) return;

    for (int i = 0; i < app->annotation_count; ++i) {
        YoloAnnotation* a = &app->annotations[i];
        bool is_sel = (i == app->selected_annotation_idx);

        const AnnotationCategory* cat = &DEFAULT_CATEGORIES[0];
        if (a->class_id >= 0 && a->class_id < MAX_CATEGORIES) {
            cat = &DEFAULT_CATEGORIES[a->class_id];
        }

        plutovg_canvas_save(app->canvas);

        plutovg_canvas_set_rgba(app->canvas, cat->r, cat->g, cat->b, is_sel ? 0.50f : 0.35f);
        plutovg_canvas_fill_rect(app->canvas, a->x, a->y, a->w, a->h);

        plutovg_canvas_set_rgba(app->canvas, cat->r, cat->g, cat->b, 0.95f);
        plutovg_canvas_set_line_width(app->canvas, is_sel ? 2.5f / app->zoom : 1.5f / app->zoom);
        plutovg_canvas_stroke_rect(app->canvas, a->x, a->y, a->w, a->h);

        if (app->system_mono_face) {
            float badge_h = 16.0f / app->zoom;
            float font_sz = 11.0f / app->zoom;

            char badge[128];
            if (a->class_id == 8 && strlen(a->url) > 0) {
                snprintf(badge, sizeof(badge), "[LINK] %s", PathFindFileNameA(a->url));
            } else {
                snprintf(badge, sizeof(badge), "[%d] %s", a->class_id, a->class_name);
            }

            float badge_w = (float)(strlen(badge) * 7.0f) / app->zoom + 6.0f / app->zoom;
            if (badge_w < 60.0f / app->zoom) badge_w = 60.0f / app->zoom;

            plutovg_canvas_set_rgba(app->canvas, cat->r, cat->g, cat->b, 0.95f);
            plutovg_canvas_fill_rect(app->canvas, a->x, a->y - badge_h, badge_w, badge_h);

            plutovg_canvas_set_font_face(app->canvas, app->system_mono_face);
            plutovg_canvas_set_font_size(app->canvas, font_sz);
            plutovg_canvas_set_rgb(app->canvas, 1.0f, 1.0f, 1.0f);
            plutovg_canvas_fill_text(app->canvas, badge, -1, PLUTOVG_TEXT_ENCODING_UTF8,
                                    a->x + 3.0f / app->zoom, a->y - 3.5f / app->zoom);
        }

        if (is_sel && app->annotations_selectable) {
            float hs = 8.0f / app->zoom;
            float hs2 = hs / 2.0f;
            plutovg_canvas_set_rgb(app->canvas, 1.0f, 1.0f, 1.0f);
            plutovg_canvas_set_line_width(app->canvas, 1.0f / app->zoom);

            float cx[4] = { a->x, a->x + a->w, a->x, a->x + a->w };
            float cy[4] = { a->y, a->y, a->y + a->h, a->y + a->h };

            for (int k = 0; k < 4; ++k) {
                plutovg_canvas_fill_rect(app->canvas, cx[k] - hs2, cy[k] - hs2, hs, hs);
                plutovg_canvas_set_rgb(app->canvas, 0.1f, 0.1f, 0.1f);
                plutovg_canvas_stroke_rect(app->canvas, cx[k] - hs2, cy[k] - hs2, hs, hs);
                plutovg_canvas_set_rgb(app->canvas, 1.0f, 1.0f, 1.0f);
            }
        }

        plutovg_canvas_restore(app->canvas);
    }

    if (app->is_drawing_box) {
        float x1 = app->drag_start_canvas_x;
        float y1 = app->drag_start_canvas_y;
        float x2 = app->drag_curr_canvas_x;
        float y2 = app->drag_curr_canvas_y;
        float bx = x1 < x2 ? x1 : x2;
        float by = y1 < y2 ? y1 : y2;
        float bw = fabsf(x2 - x1);
        float bh = fabsf(y2 - y1);

        const AnnotationCategory* cat = &DEFAULT_CATEGORIES[app->active_class_id];
        plutovg_canvas_save(app->canvas);
        plutovg_canvas_set_rgba(app->canvas, cat->r, cat->g, cat->b, 0.35f);
        plutovg_canvas_fill_rect(app->canvas, bx, by, bw, bh);

        plutovg_canvas_set_rgba(app->canvas, 1.0f, 1.0f, 1.0f, 0.95f);
        plutovg_canvas_set_line_width(app->canvas, 1.5f / app->zoom);
        plutovg_canvas_stroke_rect(app->canvas, bx, by, bw, bh);
        plutovg_canvas_restore(app->canvas);
    }
}

static void render(AppState* app) {
    if (!app->canvas) return;

    plutovg_canvas_save(app->canvas);
    plutovg_canvas_reset_matrix(app->canvas);
    plutovg_canvas_set_rgb(app->canvas, 0.12f, 0.13f, 0.15f);
    plutovg_canvas_fill_rect(app->canvas, 0, 0, (float)app->width, (float)app->height);
    plutovg_canvas_restore(app->canvas);

    plutovg_canvas_save(app->canvas);
    plutovg_canvas_translate(app->canvas, app->pan_x, app->pan_y);
    plutovg_canvas_scale(app->canvas, app->zoom, app->zoom);
    plutovg_canvas_rotate(app->canvas, app->rotation_deg * (3.1415926535f / 180.0f));

    if (fabsf(app->shear_x) > 0.0001f) {
        plutovg_matrix_t sm;
        plutovg_matrix_init_shear(&sm, app->shear_x, 0.0f);
        plutovg_canvas_transform(app->canvas, &sm);
    }

    if (app->show_grid) draw_grid_and_origin(app->canvas);

    if (app->has_svg_loaded) {
        // Isolated SVG Document Rendering
        SvgDocument_Render(&app->svg, app->canvas);
        draw_annotations(app);
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

static bool browse_file(HWND parent, const char* filter, char* out_path, size_t max_len, bool is_save, const char* def_ext) {
    OPENFILENAMEA ofn = {0};
    char buf[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(OPENFILENAMEA);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.Flags = OFN_NOCHANGEDIR;

    if (is_save) {
        ofn.Flags |= OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        ofn.lpstrDefExt = def_ext;
        if (GetSaveFileNameA(&ofn)) {
            strncpy(out_path, buf, max_len - 1);
            return true;
        }
    } else {
        ofn.Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) {
            strncpy(out_path, buf, max_len - 1);
            return true;
        }
    }
    return false;
}

// --- Destination SVG Viewport Picker Window ---

typedef struct {
    HWND hwnd;
    HWND hwnd_target_edit;
    char svg_path[MAX_PATH];
    SvgDocument svg;

    int width;
    int height;
    plutovg_surface_t* surface;
    plutovg_canvas_t* canvas;

    float zoom;
    float pan_x;
    float pan_y;

    bool is_dragging_view;
    bool is_selecting_box;
    POINT last_mouse;
    float drag_start_cx;
    float drag_start_cy;
    float drag_curr_cx;
    float drag_curr_cy;

    bool has_box;
    float box_x, box_y, box_w, box_h;
} ViewportPicker;

static ViewportPicker g_vp_picker;

static void vp_screen_to_canvas(ViewportPicker* vp, float sx, float sy, float* cx, float* cy) {
    *cx = (sx - vp->pan_x) / vp->zoom;
    *cy = (sy - vp->pan_y) / vp->zoom;
}

static void vp_render(ViewportPicker* vp) {
    if (!vp->canvas) return;

    plutovg_canvas_save(vp->canvas);
    plutovg_canvas_reset_matrix(vp->canvas);
    plutovg_canvas_set_rgb(vp->canvas, 0.12f, 0.13f, 0.15f);
    plutovg_canvas_fill_rect(vp->canvas, 0, 0, (float)vp->width, (float)vp->height);
    plutovg_canvas_restore(vp->canvas);

    plutovg_canvas_save(vp->canvas);
    plutovg_canvas_translate(vp->canvas, vp->pan_x, vp->pan_y);
    plutovg_canvas_scale(vp->canvas, vp->zoom, vp->zoom);

    // Render Destination SVG Artboard using isolated SVG Engine
    SvgDocument_Render(&vp->svg, vp->canvas);

    if (vp->is_selecting_box || vp->has_box) {
        float bx, by, bw, bh;
        if (vp->is_selecting_box) {
            float x1 = vp->drag_start_cx, y1 = vp->drag_start_cy;
            float x2 = vp->drag_curr_cx, y2 = vp->drag_curr_cy;
            bx = x1 < x2 ? x1 : x2;
            by = y1 < y2 ? y1 : y2;
            bw = fabsf(x2 - x1);
            bh = fabsf(y2 - y1);
        } else {
            bx = vp->box_x; by = vp->box_y; bw = vp->box_w; bh = vp->box_h;
        }

        plutovg_canvas_save(vp->canvas);
        plutovg_canvas_set_rgba(vp->canvas, 0.05f, 0.85f, 0.80f, 0.35f);
        plutovg_canvas_fill_rect(vp->canvas, bx, by, bw, bh);

        plutovg_canvas_set_rgba(vp->canvas, 0.95f, 0.85f, 0.15f, 0.95f);
        plutovg_canvas_set_line_width(vp->canvas, 2.0f / vp->zoom);
        plutovg_canvas_stroke_rect(vp->canvas, bx, by, bw, bh);
        plutovg_canvas_restore(vp->canvas);
    }

    plutovg_canvas_restore(vp->canvas);
}

static LRESULT CALLBACK ViewportPickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ViewportPicker* vp = &g_vp_picker;

    switch (msg) {
    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        if (w > 0 && h > 0) {
            vp->width = w;
            vp->height = h;
            if (vp->canvas) plutovg_canvas_destroy(vp->canvas);
            if (vp->surface) plutovg_surface_destroy(vp->surface);
            vp->surface = plutovg_surface_create(w, h);
            vp->canvas = plutovg_canvas_create(vp->surface);
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
        float new_zoom = vp->zoom * factor;
        if (new_zoom > 0.01f && new_zoom < 200.0f) {
            vp->pan_x = (float)pt.x - ((float)pt.x - vp->pan_x) * (new_zoom / vp->zoom);
            vp->pan_y = (float)pt.y - ((float)pt.y - vp->pan_y) * (new_zoom / vp->zoom);
            vp->zoom = new_zoom;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        float cx, cy;
        vp_screen_to_canvas(vp, (float)mx, (float)my, &cx, &cy);

        vp->is_selecting_box = true;
        vp->drag_start_cx = cx;
        vp->drag_start_cy = cy;
        vp->drag_curr_cx = cx;
        vp->drag_curr_cy = cy;
        SetCapture(hwnd);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        vp->is_dragging_view = true;
        vp->last_mouse.x = GET_X_LPARAM(lParam);
        vp->last_mouse.y = GET_Y_LPARAM(lParam);
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        if (vp->is_selecting_box) {
            float cx, cy;
            vp_screen_to_canvas(vp, (float)mx, (float)my, &cx, &cy);
            vp->drag_curr_cx = cx;
            vp->drag_curr_cy = cy;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (vp->is_dragging_view) {
            vp->pan_x += (float)(mx - vp->last_mouse.x);
            vp->pan_y += (float)(my - vp->last_mouse.y);
            vp->last_mouse.x = mx;
            vp->last_mouse.y = my;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (vp->is_selecting_box) {
            vp->is_selecting_box = false;
            float x1 = vp->drag_start_cx, y1 = vp->drag_start_cy;
            float x2 = vp->drag_curr_cx, y2 = vp->drag_curr_cy;
            float bw = fabsf(x2 - x1);
            float bh = fabsf(y2 - y1);
            if (bw > 4.0f && bh > 4.0f) {
                vp->has_box = true;
                vp->box_x = x1 < x2 ? x1 : x2;
                vp->box_y = y1 < y2 ? y1 : y2;
                vp->box_w = bw;
                vp->box_h = bh;
            }
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        if (vp->is_dragging_view) {
            vp->is_dragging_view = false;
            ReleaseCapture();
        }
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDC_VP_APPLY_BOX: {
            if (vp->has_box && vp->hwnd_target_edit) {
                char buf[128];
                snprintf(buf, sizeof(buf), "%.2f %.2f %.2f %.2f", vp->box_x, vp->box_y, vp->box_w, vp->box_h);
                SetWindowTextA(vp->hwnd_target_edit, buf);
            }
            DestroyWindow(hwnd);
            break;
        }
        case IDC_VP_APPLY_VIEW: {
            if (vp->hwnd_target_edit) {
                float cx1, cy1, cx2, cy2;
                vp_screen_to_canvas(vp, 0, 0, &cx1, &cy1);
                vp_screen_to_canvas(vp, (float)vp->width, (float)(vp->height - 48), &cx2, &cy2);
                float vx = cx1 < cx2 ? cx1 : cx2;
                float vy = cy1 < cy2 ? cy1 : cy2;
                float vw = fabsf(cx2 - cx1);
                float vh = fabsf(cy2 - cy1);

                char buf[128];
                snprintf(buf, sizeof(buf), "%.2f %.2f %.2f %.2f", vx, vy, vw, vh);
                SetWindowTextA(vp->hwnd_target_edit, buf);
            }
            DestroyWindow(hwnd);
            break;
        }
        case IDC_VP_CANCEL:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        vp_render(vp);

        if (vp->surface) {
            int w = plutovg_surface_get_width(vp->surface);
            int h = plutovg_surface_get_height(vp->surface);
            const unsigned char* data = plutovg_surface_get_data(vp->surface);

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
    case WM_DESTROY: {
        SvgDocument_Free(&vp->svg);
        if (vp->canvas) plutovg_canvas_destroy(vp->canvas);
        if (vp->surface) plutovg_surface_destroy(vp->surface);
        vp->canvas = NULL;
        vp->surface = NULL;
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void open_destination_viewport_picker(HWND parent, const char* target_url, HWND hwnd_target_edit) {
    char clean_path[MAX_PATH];
    const char* url_str = target_url;
    if (_strnicmp(url_str, "file:///", 8) == 0) url_str += 8;
    else if (_strnicmp(url_str, "file://", 7) == 0) url_str += 7;

    strncpy(clean_path, url_str, MAX_PATH - 1);
    for (char* p = clean_path; *p; ++p) {
        if (*p == '/') *p = '\\';
    }

    if (GetFileAttributesA(clean_path) == INVALID_FILE_ATTRIBUTES) {
        if (!browse_file(parent, "SVG Files (*.svg)\0*.svg\0All Files (*.*)\0*.*\0", clean_path, sizeof(clean_path), false, "svg")) {
            return;
        }
    }

    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXA wc = { sizeof(wc) };
        wc.lpfnWndProc = ViewportPickerWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_CROSS);
        wc.lpszClassName = "PlutoVGViewportPickerClass";
        RegisterClassExA(&wc);
        s_registered = true;
    }

    ViewportPicker* vp = &g_vp_picker;
    memset(vp, 0, sizeof(ViewportPicker));
    strncpy(vp->svg_path, clean_path, MAX_PATH - 1);
    vp->hwnd_target_edit = hwnd_target_edit;

    if (!SvgDocument_LoadFromFile(&vp->svg, clean_path, NULL)) {
        return;
    }

    HWND hwnd_picker = CreateWindowExA(
        WS_EX_TOPMOST,
        "PlutoVGViewportPickerClass",
        "Select Destination Viewport (Drag Box with Left Mouse, Pan with Right Mouse)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 680,
        parent, NULL, GetModuleHandle(NULL), NULL
    );
    vp->hwnd = hwnd_picker;

    RECT rc;
    GetClientRect(hwnd_picker, &rc);
    vp->width = rc.right;
    vp->height = rc.bottom;
    vp->zoom = 1.0f;
    if (vp->svg.width > 0 && vp->svg.height > 0) {
        float sx = ((float)vp->width * 0.85f) / vp->svg.width;
        float sy = ((float)vp->height * 0.85f) / vp->svg.height;
        vp->zoom = (sx < sy) ? sx : sy;
    }
    vp->pan_x = ((float)vp->width - (vp->svg.width * vp->zoom)) / 2.0f;
    vp->pan_y = ((float)vp->height - (vp->svg.height * vp->zoom)) / 2.0f;

    CreateWindowA("BUTTON", "Apply Selected Box", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                  20, vp->height - 42, 170, 30, hwnd_picker, (HMENU)IDC_VP_APPLY_BOX, NULL, NULL);
    CreateWindowA("BUTTON", "Apply Current View", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  200, vp->height - 42, 170, 30, hwnd_picker, (HMENU)IDC_VP_APPLY_VIEW, NULL, NULL);
    CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  380, vp->height - 42, 100, 30, hwnd_picker, (HMENU)IDC_VP_CANCEL, NULL, NULL);
}

// --- URL Link Editing Dialog ---

static LRESULT CALLBACK UrlLinkDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static YoloAnnotation* s_annot = NULL;

    switch (msg) {
    case WM_INITDIALOG:
    case WM_CREATE: {
        s_annot = (YoloAnnotation*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        break;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDC_DLG_BTN_BROWSE: {
            char path[MAX_PATH];
            if (browse_file(hwnd, "SVG / All Files (*.svg;*.*)\0*.svg;*.*\0", path, sizeof(path), false, "svg")) {
                char file_url[MAX_PATH + 16];
                snprintf(file_url, sizeof(file_url), "file://%s", path);
                SetDlgItemTextA(hwnd, IDC_DLG_URL_EDIT, file_url);
            }
            break;
        }
        case IDC_DLG_BTN_PICK_VIEWPORT: {
            char target_url[MAX_PATH];
            GetDlgItemTextA(hwnd, IDC_DLG_URL_EDIT, target_url, sizeof(target_url));
            open_destination_viewport_picker(hwnd, target_url, GetDlgItem(hwnd, IDC_DLG_VIEWPORT_EDIT));
            break;
        }
        case IDC_DLG_BTN_OK: {
            s_annot = (YoloAnnotation*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            if (s_annot) {
                GetDlgItemTextA(hwnd, IDC_DLG_URL_EDIT, s_annot->url, sizeof(s_annot->url));
                s_annot->class_id = 8;
                strncpy(s_annot->class_name, "urllink", sizeof(s_annot->class_name) - 1);

                char vp_text[128] = {0};
                GetDlgItemTextA(hwnd, IDC_DLG_VIEWPORT_EDIT, vp_text, sizeof(vp_text));
                for (char* p = vp_text; *p; ++p) if (*p == ',' || *p == '[' || *p == ']') *p = ' ';

                float vx, vy, vw, vh;
                if (sscanf(vp_text, "%f %f %f %f", &vx, &vy, &vw, &vh) == 4) {
                    s_annot->dest_viewport[0] = vx;
                    s_annot->dest_viewport[1] = vy;
                    s_annot->dest_viewport[2] = vw;
                    s_annot->dest_viewport[3] = vh;
                    s_annot->has_dest_viewport = true;
                } else {
                    s_annot->has_dest_viewport = false;
                }
                g_app.is_dirty = true;
                update_window_title();
                log_append("[ANNOT] Configured URLLink: url='%s', viewport=%s",
                           s_annot->url, s_annot->has_dest_viewport ? vp_text : "(scale to fit)");
            }
            DestroyWindow(hwnd);
            InvalidateRect(g_app.hwnd_main, NULL, FALSE);
            break;
        }
        case IDC_DLG_BTN_CANCEL:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void open_urllink_dialog(HWND parent, YoloAnnotation* a) {
    if (!a) return;

    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXA wc = { sizeof(wc) };
        wc.lpfnWndProc = UrlLinkDlgProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "PlutoVGUrlLinkDlgClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassExA(&wc);
        s_registered = true;
    }

    HWND hwnd_dlg = CreateWindowExA(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        "PlutoVGUrlLinkDlgClass",
        "Edit URL Link Annotation",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 540, 270,
        parent, NULL, GetModuleHandle(NULL), NULL
    );

    SetWindowLongPtr(hwnd_dlg, GWLP_USERDATA, (LONG_PTR)a);

    CreateWindowA("STATIC", "Target URL (e.g. file://c:/example.svg or https://...):",
                  WS_CHILD | WS_VISIBLE, 20, 16, 480, 18, hwnd_dlg, NULL, NULL, NULL);

    HWND hUrlEdit = CreateWindowA("EDIT", a->url, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                  20, 38, 380, 24, hwnd_dlg, (HMENU)IDC_DLG_URL_EDIT, NULL, NULL);

    CreateWindowA("BUTTON", "Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  410, 38, 90, 24, hwnd_dlg, (HMENU)IDC_DLG_BTN_BROWSE, NULL, NULL);

    CreateWindowA("STATIC", "Destination Viewport BBox [x y w h] (Optional):",
                  WS_CHILD | WS_VISIBLE, 20, 78, 480, 18, hwnd_dlg, NULL, NULL, NULL);

    char vp_str[128] = "";
    if (a->has_dest_viewport) {
        snprintf(vp_str, sizeof(vp_str), "%.2f %.2f %.2f %.2f",
                 a->dest_viewport[0], a->dest_viewport[1], a->dest_viewport[2], a->dest_viewport[3]);
    }
    HWND hVpEdit = CreateWindowA("EDIT", vp_str, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 20, 100, 220, 24, hwnd_dlg, (HMENU)IDC_DLG_VIEWPORT_EDIT, NULL, NULL);

    CreateWindowA("BUTTON", "Select Viewport on SVG...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  250, 100, 250, 24, hwnd_dlg, (HMENU)IDC_DLG_BTN_PICK_VIEWPORT, NULL, NULL);

    CreateWindowA("BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                  310, 175, 90, 28, hwnd_dlg, (HMENU)IDC_DLG_BTN_OK, NULL, NULL);
    CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  410, 175, 90, 28, hwnd_dlg, (HMENU)IDC_DLG_BTN_CANCEL, NULL, NULL);

    HFONT hSysFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageA(hUrlEdit, WM_SETFONT, (WPARAM)hSysFont, TRUE);
    SendMessageA(hVpEdit, WM_SETFONT, (WPARAM)hSysFont, TRUE);

    RECT prc, drc;
    GetWindowRect(parent, &prc);
    GetWindowRect(hwnd_dlg, &drc);
    int x = prc.left + (prc.right - prc.left - (drc.right - drc.left)) / 2;
    int y = prc.top + (prc.bottom - prc.top - (drc.bottom - drc.top)) / 2;
    SetWindowPos(hwnd_dlg, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
}

// Log Window Procedure
static LRESULT CALLBACK LogWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (g_app.hwnd_log_edit) MoveWindow(g_app.hwnd_log_edit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        CheckMenuItem(GetMenu(g_app.hwnd_main), ID_MENU_TOGGLE_LOG, MF_UNCHECKED);
        return 0;
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
        g_app.interaction_mode = APP_MODE_NAVIGATE;
        g_app.annotations_selectable = true;
        g_app.active_class_id = 0;
        g_app.selected_annotation_idx = -1;
        load_system_mono_font(&g_app);
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
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        float cx, cy;
        screen_to_canvas(&g_app, (float)mx, (float)my, &cx, &cy);

        SetCapture(hwnd);

        if (g_app.interaction_mode == APP_MODE_ANNOTATE && g_app.annotations_selectable) {
            g_app.is_drawing_box = true;
            g_app.drag_start_canvas_x = cx;
            g_app.drag_start_canvas_y = cy;
            g_app.drag_curr_canvas_x = cx;
            g_app.drag_curr_canvas_y = cy;
        } else if (g_app.annotations_selectable) {
            ResizeHandle handle = HANDLE_NONE;
            int hit = hit_test_annotations(&g_app, cx, cy, &handle);

            if (hit >= 0) {
                g_app.selected_annotation_idx = hit;
                g_app.active_handle = handle;
                g_app.is_transforming_box = true;
                g_app.drag_start_canvas_x = cx;
                g_app.drag_start_canvas_y = cy;
            } else {
                g_app.selected_annotation_idx = -1;
                g_app.is_dragging_view = true;
                g_app.last_mouse.x = mx;
                g_app.last_mouse.y = my;
            }
        } else {
            g_app.selected_annotation_idx = -1;
            g_app.is_dragging_view = true;
            g_app.last_mouse.x = mx;
            g_app.last_mouse.y = my;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        float cx, cy;
        screen_to_canvas(&g_app, (float)mx, (float)my, &cx, &cy);

        if (g_app.is_drawing_box) {
            g_app.drag_curr_canvas_x = cx;
            g_app.drag_curr_canvas_y = cy;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (g_app.is_transforming_box && g_app.selected_annotation_idx >= 0) {
            YoloAnnotation* a = &g_app.annotations[g_app.selected_annotation_idx];
            float dcx = cx - g_app.drag_start_canvas_x;
            float dcy = cy - g_app.drag_start_canvas_y;

            if (g_app.active_handle == HANDLE_BODY) {
                update_annotation_normalized(&g_app, a, a->x + dcx, a->y + dcy, a->w, a->h);
            } else if (g_app.active_handle == HANDLE_TOP_LEFT) {
                update_annotation_normalized(&g_app, a, a->x + dcx, a->y + dcy, a->w - dcx, a->h - dcy);
            } else if (g_app.active_handle == HANDLE_TOP_RIGHT) {
                update_annotation_normalized(&g_app, a, a->x, a->y + dcy, a->w + dcx, a->h - dcy);
            } else if (g_app.active_handle == HANDLE_BOTTOM_LEFT) {
                update_annotation_normalized(&g_app, a, a->x + dcx, a->y, a->w - dcx, a->h + dcy);
            } else if (g_app.active_handle == HANDLE_BOTTOM_RIGHT) {
                update_annotation_normalized(&g_app, a, a->x, a->y, a->w + dcx, a->h + dcy);
            }

            g_app.drag_start_canvas_x = cx;
            g_app.drag_start_canvas_y = cy;
            g_app.is_dirty = true;
            update_window_title();
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (g_app.is_dragging_view) {
            g_app.pan_x += (float)(mx - g_app.last_mouse.x);
            g_app.pan_y += (float)(my - g_app.last_mouse.y);
            g_app.last_mouse.x = mx;
            g_app.last_mouse.y = my;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_app.is_drawing_box) {
            g_app.is_drawing_box = false;
            float x1 = g_app.drag_start_canvas_x;
            float y1 = g_app.drag_start_canvas_y;
            float x2 = g_app.drag_curr_canvas_x;
            float y2 = g_app.drag_curr_canvas_y;
            float bw = fabsf(x2 - x1);
            float bh = fabsf(y2 - y1);

            if (bw > 4.0f && bh > 4.0f && g_app.annotation_count < MAX_ANNOTATIONS) {
                YoloAnnotation* a = &g_app.annotations[g_app.annotation_count++];
                memset(a, 0, sizeof(YoloAnnotation));
                a->id = g_app.annotation_count - 1;
                a->class_id = g_app.active_class_id;
                strncpy(a->class_name, DEFAULT_CATEGORIES[g_app.active_class_id].name, sizeof(a->class_name) - 1);
                update_annotation_normalized(&g_app, a, x1 < x2 ? x1 : x2, y1 < y2 ? y1 : y2, bw, bh);
                g_app.selected_annotation_idx = g_app.annotation_count - 1;

                g_app.is_dirty = true;
                update_window_title();

                if (a->class_id == 8) {
                    open_urllink_dialog(hwnd, a);
                } else {
                    log_append("[ANNOT] Added box #%d: [%.3f, %.3f, %.3f, %.3f] (class: %s)",
                               a->id, a->x_center, a->y_center, a->width, a->height, a->class_name);
                }
            }
        }
        g_app.is_transforming_box = false;
        g_app.is_dragging_view = false;
        g_app.active_handle = HANDLE_NONE;
        ReleaseCapture();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        float cx, cy;
        screen_to_canvas(&g_app, (float)mx, (float)my, &cx, &cy);

        ResizeHandle handle = HANDLE_NONE;
        int hit = hit_test_annotations(&g_app, cx, cy, &handle);
        if (hit >= 0) {
            g_app.selected_annotation_idx = hit;
            YoloAnnotation* a = &g_app.annotations[hit];

            if (a->class_id == 8 || _stricmp(a->class_name, "urllink") == 0 || strlen(a->url) > 0) {
                bool shift_or_ctrl = (GetKeyState(VK_SHIFT) & 0x8000) || (GetKeyState(VK_CONTROL) & 0x8000);
                if (shift_or_ctrl || strlen(a->url) == 0) {
                    open_urllink_dialog(hwnd, a);
                } else {
                    launch_urllink(a);
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }
        return 0;
    }

    case WM_RBUTTONDOWN: {
        g_app.is_dragging_view = true;
        g_app.last_mouse.x = GET_X_LPARAM(lParam);
        g_app.last_mouse.y = GET_Y_LPARAM(lParam);
        SetCapture(hwnd);
        return 0;
    }

    case WM_RBUTTONUP: {
        if (g_app.is_dragging_view) {
            g_app.is_dragging_view = false;
            ReleaseCapture();
        }
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        float pan_step = shift ? 120.0f : 40.0f;

        switch (wParam) {
        case VK_LEFT:  g_app.pan_x += pan_step; InvalidateRect(hwnd, NULL, FALSE); break;
        case VK_RIGHT: g_app.pan_x -= pan_step; InvalidateRect(hwnd, NULL, FALSE); break;
        case VK_UP:    g_app.pan_y += pan_step; InvalidateRect(hwnd, NULL, FALSE); break;
        case VK_DOWN:  g_app.pan_y -= pan_step; InvalidateRect(hwnd, NULL, FALSE); break;
        case 'S':
            if (ctrl) SendMessage(hwnd, WM_COMMAND, ID_MENU_SAVE_ANNOT, 0);
            else { g_app.shear_x += shift ? -0.05f : 0.05f; InvalidateRect(hwnd, NULL, FALSE); }
            break;
        case 'O':
            if (ctrl) SendMessage(hwnd, WM_COMMAND, ID_MENU_OPEN_SVG, 0);
            else SendMessage(hwnd, WM_COMMAND, ID_MENU_OPEN_FONT, 0);
            break;
        case 'K':
            if (ctrl) SendMessage(hwnd, WM_COMMAND, ID_MENU_EDIT_URLLINK, 0);
            break;
        case 'E':
            if (ctrl) SendMessage(hwnd, WM_COMMAND, ID_MENU_EXPORT_SVG, 0);
            break;
        case 'N':
        case VK_TAB:
            SendMessage(hwnd, WM_COMMAND, ID_MENU_TOGGLE_MODE, 0);
            break;
        case VK_F2:
            SendMessage(hwnd, WM_COMMAND, ID_MENU_TOGGLE_SELECTABLE, 0);
            break;
        case 'C':
            SendMessage(hwnd, WM_COMMAND, ID_MENU_CYCLE_CLASS, 0);
            break;
        case VK_DELETE:
        case VK_BACK:
            SendMessage(hwnd, WM_COMMAND, ID_MENU_DELETE_ANNOT, 0);
            break;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            SendMessage(hwnd, WM_COMMAND, ID_MENU_CLASS_BASE + (int)(wParam - '1'), 0);
            break;
        case 'L': SendMessage(hwnd, WM_COMMAND, ID_MENU_TOGGLE_LOG, 0); break;
        case 'G': SendMessage(hwnd, WM_COMMAND, ID_MENU_TOGGLE_GRID, 0); break;
        case VK_SPACE: SendMessage(hwnd, WM_COMMAND, ID_MENU_RESET_VIEW, 0); break;
        case 'R':
            g_app.rotation_deg += shift ? -5.0f : 5.0f;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case VK_OEM_PLUS: case VK_ADD: SendMessage(hwnd, WM_COMMAND, ID_MENU_ZOOM_IN, 0); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: SendMessage(hwnd, WM_COMMAND, ID_MENU_ZOOM_OUT, 0); break;
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        char file[MAX_PATH];
        if (DragQueryFileA(hDrop, 0, file, MAX_PATH)) {
            const char* ext = PathFindExtensionA(file);
            if (_stricmp(ext, ".svg") == 0) {
                if (check_save_changes_prompt(hwnd)) {
                    load_svg_file(&g_app, file);
                }
            } else if (_stricmp(ext, ".yaml") == 0 || _stricmp(ext, ".yml") == 0) {
                load_yolo_annotations(&g_app, file);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_COMMAND: {
        int cmd = LOWORD(wParam);
        if (cmd >= ID_MENU_CLASS_BASE && cmd < ID_MENU_CLASS_BASE + MAX_CATEGORIES) {
            int cid = cmd - ID_MENU_CLASS_BASE;
            g_app.active_class_id = cid;
            if (g_app.selected_annotation_idx >= 0 && g_app.selected_annotation_idx < g_app.annotation_count) {
                YoloAnnotation* a = &g_app.annotations[g_app.selected_annotation_idx];
                a->class_id = cid;
                strncpy(a->class_name, DEFAULT_CATEGORIES[cid].name, sizeof(a->class_name) - 1);
                g_app.is_dirty = true;
                update_window_title();
            }
            log_append("[ANNOT] Active Class set to: [%d] %s", cid, DEFAULT_CATEGORIES[cid].name);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        switch (cmd) {
        case ID_MENU_OPEN_SVG: {
            if (!check_save_changes_prompt(hwnd)) break;
            char path[MAX_PATH];
            if (browse_file(hwnd, "Scalable Vector Graphics (*.svg)\0*.svg\0All Files (*.*)\0*.*\0", path, sizeof(path), false, "svg")) {
                load_svg_file(&g_app, path);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        case ID_MENU_SAVE_ANNOT: {
            if (strlen(g_app.annot_file_path) > 0) {
                save_yolo_annotations(&g_app, g_app.annot_file_path);
            } else {
                SendMessage(hwnd, WM_COMMAND, ID_MENU_SAVE_ANNOT_AS, 0);
            }
            break;
        }
        case ID_MENU_SAVE_ANNOT_AS: {
            char path[MAX_PATH];
            if (browse_file(hwnd, "YOLO Annotation YAML (*.yaml)\0*.yaml\0All Files (*.*)\0*.*\0", path, sizeof(path), true, "yaml")) {
                save_yolo_annotations(&g_app, path);
            }
            break;
        }
        case ID_MENU_TOGGLE_MODE: {
            g_app.interaction_mode = (g_app.interaction_mode == APP_MODE_NAVIGATE) ? APP_MODE_ANNOTATE : APP_MODE_NAVIGATE;
            CheckMenuItem(GetMenu(hwnd), ID_MENU_TOGGLE_MODE, (g_app.interaction_mode == APP_MODE_ANNOTATE) ? MF_CHECKED : MF_UNCHECKED);
            log_append("[MODE] Switched to %s Mode (Tab/N to toggle)",
                       (g_app.interaction_mode == APP_MODE_ANNOTATE) ? "DRAW ANNOTATION BOX" : "NAVIGATE / SELECT");
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        case ID_MENU_TOGGLE_SELECTABLE: {
            g_app.annotations_selectable = !g_app.annotations_selectable;
            CheckMenuItem(GetMenu(hwnd), ID_MENU_TOGGLE_SELECTABLE, g_app.annotations_selectable ? MF_CHECKED : MF_UNCHECKED);
            if (!g_app.annotations_selectable) {
                g_app.selected_annotation_idx = -1;
            }
            log_append("[MODE] Annotations Selectable: %s", g_app.annotations_selectable ? "ON" : "OFF");
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        case ID_MENU_EDIT_URLLINK: {
            if (g_app.selected_annotation_idx >= 0 && g_app.selected_annotation_idx < g_app.annotation_count) {
                open_urllink_dialog(hwnd, &g_app.annotations[g_app.selected_annotation_idx]);
            } else {
                MessageBoxA(hwnd, "Please select an annotation box first.", "Edit URL Link", MB_ICONINFORMATION);
            }
            break;
        }
        case ID_MENU_DELETE_ANNOT: {
            if (g_app.selected_annotation_idx >= 0 && g_app.selected_annotation_idx < g_app.annotation_count) {
                for (int i = g_app.selected_annotation_idx; i < g_app.annotation_count - 1; ++i) {
                    g_app.annotations[i] = g_app.annotations[i + 1];
                    g_app.annotations[i].id = i;
                }
                g_app.annotation_count--;
                g_app.selected_annotation_idx = -1;
                g_app.is_dirty = true;
                update_window_title();
                log_append("[ANNOT] Deleted annotation.");
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        case ID_MENU_CLEAR_ANNOT: {
            g_app.annotation_count = 0;
            g_app.selected_annotation_idx = -1;
            g_app.is_dirty = true;
            update_window_title();
            log_append("[ANNOT] Cleared all annotations.");
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        case ID_MENU_CYCLE_CLASS: {
            g_app.active_class_id = (g_app.active_class_id + 1) % MAX_CATEGORIES;
            SendMessage(hwnd, WM_COMMAND, ID_MENU_CLASS_BASE + g_app.active_class_id, 0);
            break;
        }
        case ID_MENU_EXPORT_SVG: {
            if (!g_app.has_svg_loaded) {
                MessageBoxA(hwnd, "Please load an SVG file first.", "Export SVG", MB_ICONINFORMATION);
                break;
            }
            char path[MAX_PATH];
            if (browse_file(hwnd, "Scalable Vector Graphics (*.svg)\0*.svg\0All Files (*.*)\0*.*\0", path, sizeof(path), true, "svg")) {
                SvgDocument_ExportWithEmbeddedPaths(&g_app.svg, path, log_append);
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
        case ID_MENU_ZOOM_IN:
            zoom_at(&g_app, (float)g_app.width / 2.0f, (float)g_app.height / 2.0f, 1.15f);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case ID_MENU_ZOOM_OUT:
            zoom_at(&g_app, (float)g_app.width / 2.0f, (float)g_app.height / 2.0f, 1.0f / 1.15f);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case ID_MENU_EXIT:
            SendMessage(hwnd, WM_CLOSE, 0, 0);
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

    case WM_CLOSE:
        if (!check_save_changes_prompt(hwnd)) return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        SvgDocument_Free(&g_app.svg);
        if (g_app.system_mono_face) plutovg_font_face_destroy(g_app.system_mono_face);
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
    HMENU hEditMenu = CreateMenu();
    HMENU hClassMenu = CreateMenu();
    HMENU hViewMenu = CreateMenu();

    // File Menu
    AppendMenuA(hFileMenu, MF_STRING, ID_MENU_OPEN_SVG, "Open .&SVG...\t(Ctrl+O)");
    AppendMenuA(hFileMenu, MF_STRING, ID_MENU_SAVE_ANNOT, "&Save YOLO Annotations\t(Ctrl+S)");
    AppendMenuA(hFileMenu, MF_STRING, ID_MENU_SAVE_ANNOT_AS, "Save YOLO Annotations &As...");
    AppendMenuA(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFileMenu, MF_STRING, ID_MENU_EXPORT_SVG, "&Export SVG (Glyphs to Paths)...\t(Ctrl+E)");
    AppendMenuA(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFileMenu, MF_STRING, ID_MENU_EXIT, "E&xit");

    // Edit Menu
    AppendMenuA(hEditMenu, MF_STRING | MF_UNCHECKED, ID_MENU_TOGGLE_MODE, "&Draw Box Mode\t(Tab / N)");
    AppendMenuA(hEditMenu, MF_STRING | MF_CHECKED, ID_MENU_TOGGLE_SELECTABLE, "Annotations &Selectable\t(F2)");
    AppendMenuA(hEditMenu, MF_STRING, ID_MENU_EDIT_URLLINK, "&Edit URL Link...\t(Ctrl+K)");
    AppendMenuA(hEditMenu, MF_STRING, ID_MENU_DELETE_ANNOT, "&Delete Selected Box\t(Del)");
    AppendMenuA(hEditMenu, MF_STRING, ID_MENU_CLEAR_ANNOT, "&Clear All Annotations");
    AppendMenuA(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hEditMenu, MF_STRING, ID_MENU_CYCLE_CLASS, "C&ycle Active Class\t(C)");

    // Category / Class Menu
    for (int i = 0; i < MAX_CATEGORIES; ++i) {
        char item[64];
        snprintf(item, sizeof(item), "[%d] %s\t(%d)", i, DEFAULT_CATEGORIES[i].name, i + 1);
        AppendMenuA(hClassMenu, MF_STRING, ID_MENU_CLASS_BASE + i, item);
    }

    // View Menu
    AppendMenuA(hViewMenu, MF_STRING, ID_MENU_RESET_VIEW, "&Reset View\t(Space)");
    AppendMenuA(hViewMenu, MF_STRING, ID_MENU_TOGGLE_GRID, "Toggle &Grid & Origin\t(G)");
    AppendMenuA(hViewMenu, MF_STRING | MF_UNCHECKED, ID_MENU_TOGGLE_LOG, "Show Debug &Log\t(L)");

    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hFileMenu, "&File");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hEditMenu, "&Edit");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hClassMenu, "&Class");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hViewMenu, "&View");

    SetMenu(hwnd, hMenuBar);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);
        SetProcessDpiAwarenessContextProc setDpiContext = 
            (SetProcessDpiAwarenessContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpiContext) setDpiContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        else SetProcessDPIAware();
    }

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "PlutoVGViewerWindowClass";
    if (!RegisterClassExA(&wc)) return 1;

    WNDCLASSEXA log_wc = {0};
    log_wc.cbSize = sizeof(WNDCLASSEXA);
    log_wc.style = CS_HREDRAW | CS_VREDRAW;
    log_wc.lpfnWndProc = LogWndProc;
    log_wc.hInstance = hInstance;
    log_wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    log_wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    log_wc.lpszClassName = "PlutoVGLogWindowClass";
    RegisterClassExA(&log_wc);

    g_app.hwnd_main = CreateWindowExA(
        WS_EX_ACCEPTFILES,
        wc.lpszClassName,
        "PlutoVG Typography & YOLO Annotation Editor",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1150, 820,
        NULL, NULL, hInstance, NULL
    );
    if (!g_app.hwnd_main) return 1;

    g_app.hwnd_log = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        log_wc.lpszClassName,
        "SVG & YOLO Annotation Debug Log",
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
            0, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            0, 0, rc.right, rc.bottom,
            g_app.hwnd_log,
            (HMENU)IDC_LOG_EDIT,
            hInstance, NULL
        );

        g_app.hfont_log = CreateFontA(
            15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas"
        );
        if (g_app.hfont_log) SendMessageA(g_app.hwnd_log_edit, WM_SETFONT, (WPARAM)g_app.hfont_log, TRUE);
    }

    create_app_menu(g_app.hwnd_main);

    ACCEL accels[] = {
        { FCONTROL | FVIRTKEY, 'O', ID_MENU_OPEN_SVG },
        { FCONTROL | FVIRTKEY, 'S', ID_MENU_SAVE_ANNOT },
        { FCONTROL | FVIRTKEY, 'E', ID_MENU_EXPORT_SVG },
        { FCONTROL | FVIRTKEY, 'K', ID_MENU_EDIT_URLLINK },
        { FVIRTKEY, 'N', ID_MENU_TOGGLE_MODE },
        { FVIRTKEY, VK_TAB, ID_MENU_TOGGLE_MODE },
        { FVIRTKEY, VK_F2, ID_MENU_TOGGLE_SELECTABLE },
        { FVIRTKEY, 'C', ID_MENU_CYCLE_CLASS },
        { FVIRTKEY, VK_DELETE, ID_MENU_DELETE_ANNOT },
        { FVIRTKEY, VK_BACK, ID_MENU_DELETE_ANNOT },
        { FVIRTKEY, 'L', ID_MENU_TOGGLE_LOG },
        { FVIRTKEY, 'G', ID_MENU_TOGGLE_GRID },
        { FVIRTKEY, VK_SPACE, ID_MENU_RESET_VIEW },
        { FVIRTKEY, VK_OEM_PLUS, ID_MENU_ZOOM_IN },
        { FVIRTKEY, VK_ADD, ID_MENU_ZOOM_IN },
        { FVIRTKEY, VK_OEM_MINUS, ID_MENU_ZOOM_OUT },
        { FVIRTKEY, VK_SUBTRACT, ID_MENU_ZOOM_OUT }
    };
    HACCEL hAccel = CreateAcceleratorTableA(accels, sizeof(accels) / sizeof(accels[0]));

    ShowWindow(g_app.hwnd_main, nCmdShow);
    UpdateWindow(g_app.hwnd_main);

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argvW && argc > 1) {
        char initial_path[MAX_PATH] = {0};
        WideCharToMultiByte(CP_UTF8, 0, argvW[1], -1, initial_path, MAX_PATH, NULL, NULL);

        float target_bbox[4] = {0};
        bool has_target_bbox = false;

        for (int i = 2; i < argc; ++i) {
            char arg[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, arg, MAX_PATH, NULL, NULL);
            if ((_stricmp(arg, "--bbox") == 0 || _stricmp(arg, "-bbox") == 0) && i + 4 < argc) {
                char a1[64], a2[64], a3[64], a4[64];
                WideCharToMultiByte(CP_UTF8, 0, argvW[i+1], -1, a1, sizeof(a1), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, argvW[i+2], -1, a2, sizeof(a2), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, argvW[i+3], -1, a3, sizeof(a3), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, argvW[i+4], -1, a4, sizeof(a4), NULL, NULL);
                target_bbox[0] = (float)atof(a1);
                target_bbox[1] = (float)atof(a2);
                target_bbox[2] = (float)atof(a3);
                target_bbox[3] = (float)atof(a4);
                has_target_bbox = true;
                break;
            }
        }

        const char* ext = PathFindExtensionA(initial_path);
        if (_stricmp(ext, ".svg") == 0) {
            load_svg_file(&g_app, initial_path);
            if (has_target_bbox) {
                focus_viewport_bbox(&g_app, target_bbox[0], target_bbox[1], target_bbox[2], target_bbox[3]);
            }
        }
        LocalFree(argvW);
        InvalidateRect(g_app.hwnd_main, NULL, FALSE);
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (!TranslateAcceleratorA(g_app.hwnd_main, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    if (hAccel) DestroyAcceleratorTable(hAccel);
    return (int)msg.wParam;
}
