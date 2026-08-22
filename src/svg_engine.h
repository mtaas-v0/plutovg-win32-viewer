#ifndef SVG_ENGINE_H
#define SVG_ENGINE_H

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <plutovg.h>

#define MAX_FONTS         64
#define MAX_SVG_NODES     16384
#define MAX_GROUP_DEPTH   64
#define MAX_DASH_COUNT    16

typedef enum {
    SVG_NODE_PATH,
    SVG_NODE_TEXT
} SvgNodeType;

typedef struct {
    char key[128];
    char file_path[MAX_PATH];
    plutovg_font_face_t* face;
} CachedFont;

typedef struct {
    CachedFont fonts[MAX_FONTS];
    int count;
    plutovg_font_face_t* fallback_face;
} FontRegistry;

typedef struct {
    SvgNodeType type;
    plutovg_matrix_t matrix;

    bool has_fill;
    plutovg_color_t fill_color;
    plutovg_fill_rule_t fill_rule;

    bool has_stroke;
    plutovg_color_t stroke_color;
    float stroke_width;
    plutovg_line_join_t stroke_join;
    plutovg_line_cap_t stroke_cap;

    bool has_dash;
    float dash_array[MAX_DASH_COUNT];
    int dash_count;
    float dash_offset;

    plutovg_path_t* path;

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
    char fonts_dir[MAX_PATH];
    FontRegistry registry;
} SvgDocument;

typedef void (*SvgLogCallback)(const char* fmt, ...);

// Document Lifecycle
bool SvgDocument_LoadFromFile(SvgDocument* doc, const char* svg_path, SvgLogCallback log_cb);
void SvgDocument_Free(SvgDocument* doc);

// Rendering
void SvgDocument_Render(const SvgDocument* doc, plutovg_canvas_t* canvas);

// Standalone Vector Export
bool SvgDocument_ExportWithEmbeddedPaths(const SvgDocument* doc, const char* out_path, SvgLogCallback log_cb);

// Font Discovery & Lookup
void SvgEngine_DiscoverFonts(FontRegistry* reg, char* out_fonts_dir, size_t dir_len, const char* svg_path, SvgLogCallback log_cb);
plutovg_font_face_t* SvgEngine_FindFont(const FontRegistry* reg, const char* family_name);

#endif // SVG_ENGINE_H
