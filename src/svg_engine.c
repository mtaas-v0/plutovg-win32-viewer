#include "svg_engine.h"
#include "font_helper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <shlwapi.h>

typedef struct {
    plutovg_matrix_t matrix;
    bool has_stroke;
    plutovg_color_t stroke_color;
    bool has_stroke_width;
    float stroke_width;
    bool has_fill;
    plutovg_color_t fill_color;
    bool has_fill_rule;
    plutovg_fill_rule_t fill_rule;
} SvgGroupState;

typedef struct {
    char* data;
    size_t size;
    size_t capacity;
} PathStringBuffer;

static void psb_init(PathStringBuffer* b, size_t initial_cap) {
    b->capacity = initial_cap ? initial_cap : 16384;
    b->size = 0;
    b->data = (char*)malloc(b->capacity);
    if (b->data) b->data[0] = '\0';
}

static void psb_append(PathStringBuffer* b, const char* str) {
    if (!str || !b->data) return;
    size_t len = strlen(str);
    while (b->size + len + 1 > b->capacity) {
        b->capacity *= 2;
        b->data = (char*)realloc(b->data, b->capacity);
    }
    memcpy(b->data + b->size, str, len);
    b->size += len;
    b->data[b->size] = '\0';
}

static void psb_free(PathStringBuffer* b) {
    if (b->data) free(b->data);
    b->data = NULL;
    b->size = b->capacity = 0;
}

// --- UTF-8 & Entity Decoder ---

static void append_utf8_codepoint(char* dst, size_t dst_max, size_t* d, uint32_t cp) {
    if (cp <= 0x7F) {
        if (*d < dst_max - 1) dst[(*d)++] = (char)cp;
    } else if (cp <= 0x7FF) {
        if (*d + 2 <= dst_max - 1) {
            dst[(*d)++] = (char)(0xC0 | ((cp >> 6) & 0x1F));
            dst[(*d)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp <= 0xFFFF) {
        if (*d + 3 <= dst_max - 1) {
            dst[(*d)++] = (char)(0xE0 | ((cp >> 12) & 0x0F));
            dst[(*d)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[(*d)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp <= 0x10FFFF) {
        if (*d + 4 <= dst_max - 1) {
            dst[(*d)++] = (char)(0xF0 | ((cp >> 18) & 0x07));
            dst[(*d)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            dst[(*d)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[(*d)++] = (char)(0x80 | (cp & 0x3F));
        }
    }
}

static int clean_and_unescape_text(const char* src, size_t src_len, char* dst, size_t dst_max) {
    if (!src || src_len == 0 || dst_max == 0) {
        if (dst_max > 0) dst[0] = '\0';
        return 0;
    }

    char no_tags[1024];
    size_t nt_len = 0;
    bool in_tag = false;

    for (size_t i = 0; i < src_len && nt_len < sizeof(no_tags) - 1; ++i) {
        if (src[i] == '<') in_tag = true;
        else if (src[i] == '>') in_tag = false;
        else if (!in_tag) {
            char c = src[i];
            if (c == '\r' || c == '\n' || c == '\t') c = ' ';
            no_tags[nt_len++] = c;
        }
    }
    no_tags[nt_len] = '\0';

    const char* start = no_tags;
    while (nt_len > 0 && isspace((unsigned char)*start)) { start++; nt_len--; }
    while (nt_len > 0 && isspace((unsigned char)*(start + nt_len - 1))) { nt_len--; }

    if (nt_len == 0) {
        dst[0] = '\0';
        return 0;
    }

    size_t d = 0;
    for (size_t i = 0; i < nt_len && d < dst_max - 1; ) {
        if (start[i] == '&') {
            if (strncmp(start + i, "&quot;", 6) == 0) {
                append_utf8_codepoint(dst, dst_max, &d, 0x22);
                i += 6;
            } else if (strncmp(start + i, "&amp;", 5) == 0) {
                append_utf8_codepoint(dst, dst_max, &d, 0x26);
                i += 5;
            } else if (strncmp(start + i, "&apos;", 6) == 0) {
                append_utf8_codepoint(dst, dst_max, &d, 0x27);
                i += 6;
            } else if (strncmp(start + i, "&lt;", 4) == 0) {
                append_utf8_codepoint(dst, dst_max, &d, 0x3C);
                i += 4;
            } else if (strncmp(start + i, "&gt;", 4) == 0) {
                append_utf8_codepoint(dst, dst_max, &d, 0x3E);
                i += 4;
            } else if (strncmp(start + i, "&#x", 3) == 0 || strncmp(start + i, "&#X", 3) == 0) {
                char* end = NULL;
                unsigned long cp = strtoul(start + i + 3, &end, 16);
                if (end && *end == ';') {
                    append_utf8_codepoint(dst, dst_max, &d, (uint32_t)cp);
                    i = (size_t)(end - start) + 1;
                } else dst[d++] = start[i++];
            } else if (strncmp(start + i, "&#", 2) == 0) {
                char* end = NULL;
                unsigned long cp = strtoul(start + i + 2, &end, 10);
                if (end && *end == ';') {
                    append_utf8_codepoint(dst, dst_max, &d, (uint32_t)cp);
                    i = (size_t)(end - start) + 1;
                } else dst[d++] = start[i++];
            } else dst[d++] = start[i++];
        } else dst[d++] = start[i++];
    }
    dst[d] = '\0';
    return (int)d;
}

// --- Font Lookup Registry ---

static void sanitize_key(const char* src, char* dst, size_t dst_len) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j < dst_len - 1; ++i) {
        if (isalnum((unsigned char)src[i])) dst[j++] = (char)tolower((unsigned char)src[i]);
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

static void registry_add_font(FontRegistry* reg, const char* name_key, const char* path, SvgLogCallback log_cb) {
    if (reg->count >= MAX_FONTS || !path) return;

    char clean_key[128];
    sanitize_key(name_key, clean_key, sizeof(clean_key));

    for (int i = 0; i < reg->count; ++i) {
        if (strcmp(reg->fonts[i].key, clean_key) == 0) return;
    }

    plutovg_font_face_t* face = plutovg_font_face_load_from_file(path, 0);
    if (!face) return;

    CachedFont* cf = &reg->fonts[reg->count++];
    strncpy(cf->key, clean_key, sizeof(cf->key) - 1);
    strncpy(cf->file_path, path, MAX_PATH - 1);
    cf->face = face;

    if (log_cb) log_cb("  [FONT] Registered '%s' -> %s", name_key, path);

    if (!reg->fallback_face) {
        reg->fallback_face = plutovg_font_face_load_from_file(path, 0);
    }
}

plutovg_font_face_t* SvgEngine_FindFont(const FontRegistry* reg, const char* family_name) {
    if (!family_name || strlen(family_name) == 0) return reg->fallback_face;

    char clean_key[128];
    sanitize_key(family_name, clean_key, sizeof(clean_key));

    for (int i = 0; i < reg->count; ++i) {
        if (strcmp(reg->fonts[i].key, clean_key) == 0) return reg->fonts[i].face;
    }

    for (int i = 0; i < reg->count; ++i) {
        if (strstr(reg->fonts[i].key, clean_key) || strstr(clean_key, reg->fonts[i].key)) {
            return reg->fonts[i].face;
        }
    }

    return reg->fallback_face;
}

void SvgEngine_DiscoverFonts(FontRegistry* reg, char* out_fonts_dir, size_t dir_len, const char* svg_path, SvgLogCallback log_cb) {
    char svg_dir[MAX_PATH];
    strncpy(svg_dir, svg_path, MAX_PATH - 1);
    PathRemoveFileSpecA(svg_dir);

    snprintf(out_fonts_dir, dir_len, "%s\\fonts", svg_dir);

    const char* search_folders[] = { out_fonts_dir, svg_dir };
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

                        registry_add_font(reg, font_key, full_path, log_cb);
                    }
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
        }
    }

    if (!reg->fallback_face) {
        FontInfo sys_font;
        if (FontHelper_GetSystemFont(&sys_font)) {
            registry_add_font(reg, "system_default", sys_font.font_path, log_cb);
        }
    }
}

// --- Attribute & CSS Property Locator ---

static const char* find_attr_slice(const char* tag_start, const char* tag_end, const char* attr_name, size_t* out_len) {
    if (!tag_start || !tag_end || tag_start >= tag_end || !attr_name) return NULL;
    size_t name_len = strlen(attr_name);

    const char* p = tag_start;
    while (p < tag_end) {
        if (p + name_len < tag_end && strncmp(p, attr_name, name_len) == 0 &&
            (p == tag_start || isspace((unsigned char)*(p - 1)))) {
            const char* eq = p + name_len;
            while (eq < tag_end && isspace((unsigned char)*eq)) eq++;
            if (eq < tag_end && *eq == '=') {
                const char* quote = eq + 1;
                while (quote < tag_end && isspace((unsigned char)*quote)) quote++;
                if (quote < tag_end && (*quote == '\"' || *quote == '\'')) {
                    char qchar = *quote;
                    const char* val_start = quote + 1;
                    const char* val_end = val_start;
                    while (val_end < tag_end && *val_end != qchar) val_end++;
                    if (val_end <= tag_end) {
                        *out_len = (size_t)(val_end - val_start);
                        return val_start;
                    }
                }
            }
        }
        p++;
    }
    return NULL;
}

static const char* find_prop_slice(const char* tag_start, const char* tag_end, const char* prop_name, size_t* out_len) {
    if (!tag_start || !tag_end || tag_start >= tag_end || !prop_name) return NULL;

    size_t direct_len = 0;
    const char* direct_val = find_attr_slice(tag_start, tag_end, prop_name, &direct_len);
    if (direct_val && direct_len > 0) {
        *out_len = direct_len;
        return direct_val;
    }

    size_t style_len = 0;
    const char* style_str = find_attr_slice(tag_start, tag_end, "style", &style_len);
    if (style_str && style_len > 0) {
        const char* style_end = style_str + style_len;
        const char* p = style_str;
        size_t name_len = strlen(prop_name);

        while (p < style_end) {
            while (p < style_end && (isspace((unsigned char)*p) || *p == ';')) p++;
            if (p >= style_end) break;

            if (p + name_len < style_end && strncmp(p, prop_name, name_len) == 0) {
                const char* colon = p + name_len;
                while (colon < style_end && isspace((unsigned char)*colon)) colon++;
                if (colon < style_end && *colon == ':') {
                    const char* val_start = colon + 1;
                    while (val_start < style_end && isspace((unsigned char)*val_start)) val_start++;
                    const char* val_end = val_start;
                    while (val_end < style_end && *val_end != ';' && *val_end != '\"' && *val_end != '\'') val_end++;
                    while (val_end > val_start && isspace((unsigned char)*(val_end - 1))) val_end--;

                    if (val_end > val_start) {
                        *out_len = (size_t)(val_end - val_start);
                        return val_start;
                    }
                }
            }
            while (p < style_end && *p != ';') p++;
            if (p < style_end && *p == ';') p++;
        }
    }

    return NULL;
}

static bool is_self_closing_tag(const char* tag_open, const char* tag_end) {
    if (!tag_open || !tag_end || tag_end <= tag_open) return false;
    const char* p = tag_end - 1;
    while (p > tag_open && isspace((unsigned char)*p)) p--;
    return (p > tag_open && *p == '/');
}

static void parse_svg_transform(const char* str, size_t len, plutovg_matrix_t* out_mat) {
    plutovg_matrix_init_identity(out_mat);
    if (!str || len == 0) return;

    const char* end = str + len;
    const char* p = str;

    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end) break;

        const char* lp = (const char*)memchr(p, '(', (size_t)(end - p));
        if (!lp) break;
        const char* rp = (const char*)memchr(lp, ')', (size_t)(end - lp));
        if (!rp) break;

        size_t name_len = (size_t)(lp - p);
        while (name_len > 0 && isspace((unsigned char)p[name_len - 1])) name_len--;

        char name[32];
        if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
        strncpy(name, p, name_len);
        name[name_len] = '\0';

        char buf[256];
        size_t arg_len = (size_t)(rp - (lp + 1));
        if (arg_len >= sizeof(buf)) arg_len = sizeof(buf) - 1;
        strncpy(buf, lp + 1, arg_len);
        buf[arg_len] = '\0';

        for (size_t i = 0; i < arg_len; ++i) {
            if (buf[i] == ',') buf[i] = ' ';
        }

        plutovg_matrix_t m;
        plutovg_matrix_init_identity(&m);

        if (_stricmp(name, "matrix") == 0) {
            float a, b, c, d, e, f;
            if (sscanf(buf, "%f %f %f %f %f %f", &a, &b, &c, &d, &e, &f) == 6) {
                plutovg_matrix_init(&m, a, b, c, d, e, f);
                plutovg_matrix_multiply(out_mat, out_mat, &m);
            }
        } else if (_stricmp(name, "translate") == 0) {
            float tx = 0.0f, ty = 0.0f;
            int count = sscanf(buf, "%f %f", &tx, &ty);
            if (count >= 1) {
                plutovg_matrix_init_translate(&m, tx, ty);
                plutovg_matrix_multiply(out_mat, out_mat, &m);
            }
        } else if (_stricmp(name, "scale") == 0) {
            float sx = 1.0f, sy = 1.0f;
            int count = sscanf(buf, "%f %f", &sx, &sy);
            if (count == 1) sy = sx;
            if (count >= 1) {
                plutovg_matrix_init_scale(&m, sx, sy);
                plutovg_matrix_multiply(out_mat, out_mat, &m);
            }
        } else if (_stricmp(name, "rotate") == 0) {
            float angle = 0.0f, cx = 0.0f, cy = 0.0f;
            int count = sscanf(buf, "%f %f %f", &angle, &cx, &cy);
            float rad = angle * (3.14159265358979323846f / 180.0f);
            if (count == 3) {
                plutovg_matrix_t t1, r, t2;
                plutovg_matrix_init_translate(&t1, cx, cy);
                plutovg_matrix_init_rotate(&r, rad);
                plutovg_matrix_init_translate(&t2, -cx, -cy);
                plutovg_matrix_multiply(&m, &t1, &r);
                plutovg_matrix_multiply(&m, &m, &t2);
                plutovg_matrix_multiply(out_mat, out_mat, &m);
            } else if (count >= 1) {
                plutovg_matrix_init_rotate(&m, rad);
                plutovg_matrix_multiply(out_mat, out_mat, &m);
            }
        } else if (_stricmp(name, "skewX") == 0) {
            float angle = 0.0f;
            if (sscanf(buf, "%f", &angle) == 1) {
                float rad = angle * (3.14159265358979323846f / 180.0f);
                plutovg_matrix_init_shear(&m, tanf(rad), 0.0f);
                plutovg_matrix_multiply(out_mat, out_mat, &m);
            }
        } else if (_stricmp(name, "skewY") == 0) {
            float angle = 0.0f;
            if (sscanf(buf, "%f", &angle) == 1) {
                float rad = angle * (3.14159265358979323846f / 180.0f);
                plutovg_matrix_init_shear(&m, 0.0f, tanf(rad));
                plutovg_matrix_multiply(out_mat, out_mat, &m);
            }
        }

        p = rp + 1;
    }
}

static void parse_color_slice(const char* val, size_t len, bool* has_paint, plutovg_color_t* color, plutovg_color_t default_color) {
    if (!val || len == 0) { *has_paint = false; return; }

    char buf[64];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    strncpy(buf, val, len);
    buf[len] = '\0';

    char* p = buf;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t blen = strlen(p);
    while (blen > 0 && isspace((unsigned char)p[blen - 1])) p[--blen] = '\0';

    if (_stricmp(p, "none") == 0) {
        *has_paint = false;
        return;
    }

    if (p[0] == '#') {
        p++;
        blen--;
        unsigned int r = 0, g = 0, b = 0, a = 255;
        if (blen == 3) {
            unsigned int rgb;
            if (sscanf(p, "%3x", &rgb) == 1) {
                r = ((rgb >> 8) & 0xF) * 17;
                g = ((rgb >> 4) & 0xF) * 17;
                b = (rgb & 0xF) * 17;
                plutovg_color_init_rgba(color, r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
                *has_paint = true;
                return;
            }
        } else if (blen == 6) {
            if (sscanf(p, "%02x%02x%02x", &r, &g, &b) == 3) {
                plutovg_color_init_rgba(color, r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
                *has_paint = true;
                return;
            }
        } else if (blen == 8) {
            if (sscanf(p, "%02x%02x%02x%02x", &r, &g, &b, &a) == 4) {
                plutovg_color_init_rgba(color, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
                *has_paint = true;
                return;
            }
        }
    }

    if (_strnicmp(p, "rgb(", 4) == 0) {
        float r = 0, g = 0, b = 0;
        char cbuf[64];
        strncpy(cbuf, p + 4, sizeof(cbuf) - 1);
        cbuf[sizeof(cbuf) - 1] = '\0';
        for (char* cp = cbuf; *cp; ++cp) if (*cp == ',' || *cp == ')') *cp = ' ';
        if (sscanf(cbuf, "%f %f %f", &r, &g, &b) == 3) {
            plutovg_color_init_rgba(color, r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
            *has_paint = true;
            return;
        }
    }

    if (_strnicmp(p, "rgba(", 5) == 0) {
        float r = 0, g = 0, b = 0, a = 1.0f;
        char cbuf[64];
        strncpy(cbuf, p + 5, sizeof(cbuf) - 1);
        cbuf[sizeof(cbuf) - 1] = '\0';
        for (char* cp = cbuf; *cp; ++cp) if (*cp == ',' || *cp == ')') *cp = ' ';
        if (sscanf(cbuf, "%f %f %f %f", &r, &g, &b, &a) == 4) {
            plutovg_color_init_rgba(color, r / 255.0f, g / 255.0f, b / 255.0f, a);
            *has_paint = true;
            return;
        }
    }

    if (_stricmp(p, "currentColor") == 0 || _stricmp(p, "black") == 0) {
        *has_paint = true; plutovg_color_init_rgb(color, 0, 0, 0); return;
    }
    if (_stricmp(p, "white") == 0) {
        *has_paint = true; plutovg_color_init_rgb(color, 1, 1, 1); return;
    }
    if (_stricmp(p, "red") == 0) {
        *has_paint = true; plutovg_color_init_rgb(color, 1, 0, 0); return;
    }
    if (_stricmp(p, "green") == 0) {
        *has_paint = true; plutovg_color_init_rgb(color, 0, 0.5f, 0); return;
    }
    if (_stricmp(p, "blue") == 0) {
        *has_paint = true; plutovg_color_init_rgb(color, 0, 0, 1); return;
    }
    if (_stricmp(p, "yellow") == 0) {
        *has_paint = true; plutovg_color_init_rgb(color, 1, 1, 0); return;
    }
    if (_stricmp(p, "cyan") == 0) {
        *has_paint = true; plutovg_color_init_rgb(color, 0, 1, 1); return;
    }
    if (_stricmp(p, "magenta") == 0) {
        *has_paint = true; plutovg_color_init_rgb(color, 1, 0, 1); return;
    }

    if (plutovg_color_parse(color, p, (int)blen)) {
        *has_paint = true;
    } else {
        *has_paint = true;
        *color = default_color;
    }
}

void SvgDocument_Free(SvgDocument* doc) {
    for (int i = 0; i < doc->node_count; ++i) {
        if (doc->nodes[i].type == SVG_NODE_PATH && doc->nodes[i].path) {
            plutovg_path_destroy(doc->nodes[i].path);
            doc->nodes[i].path = NULL;
        }
    }
    doc->node_count = 0;
    doc->path_count = 0;
    doc->text_count = 0;
    registry_init(&doc->registry);
}

bool SvgDocument_LoadFromFile(SvgDocument* doc, const char* svg_path, SvgLogCallback log_cb) {
    FILE* f = fopen(svg_path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return false; }

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) { fclose(f); return false; }

    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    SvgDocument_Free(doc);
    doc->width = 600.0f;
    doc->height = 400.0f;

    SvgEngine_DiscoverFonts(&doc->registry, doc->fonts_dir, sizeof(doc->fonts_dir), svg_path, log_cb);

    SvgGroupState group_stack[MAX_GROUP_DEPTH];
    int group_depth = 0;
    memset(&group_stack[0], 0, sizeof(SvgGroupState));
    plutovg_matrix_init_identity(&group_stack[0].matrix);

    size_t val_len = 0;
    const char* val_ptr = NULL;

    const char* root_svg = strstr(buffer, "<svg");
    if (root_svg) {
        const char* root_end = strchr(root_svg, '>');
        if (root_end) {
            float vb_x = 0, vb_y = 0, vb_w = 0, vb_h = 0;
            bool has_viewbox = false;
            if ((val_ptr = find_prop_slice(root_svg, root_end, "viewBox", &val_len))) {
                char vb_buf[128];
                if (val_len < sizeof(vb_buf)) {
                    strncpy(vb_buf, val_ptr, val_len);
                    vb_buf[val_len] = '\0';
                    for (size_t i = 0; i < val_len; ++i) if (vb_buf[i] == ',') vb_buf[i] = ' ';
                    if (sscanf(vb_buf, "%f %f %f %f", &vb_x, &vb_y, &vb_w, &vb_h) == 4 && vb_w > 0 && vb_h > 0) {
                        has_viewbox = true;
                    }
                }
            }

            float doc_w = 0.0f, doc_h = 0.0f;
            bool has_explicit_w = false, has_explicit_h = false;

            if ((val_ptr = find_prop_slice(root_svg, root_end, "width", &val_len))) {
                if (memchr(val_ptr, '%', val_len) == NULL) {
                    doc_w = (float)atof(val_ptr);
                    if (doc_w > 0) has_explicit_w = true;
                }
            }
            if ((val_ptr = find_prop_slice(root_svg, root_end, "height", &val_len))) {
                if (memchr(val_ptr, '%', val_len) == NULL) {
                    doc_h = (float)atof(val_ptr);
                    if (doc_h > 0) has_explicit_h = true;
                }
            }

            if (has_viewbox) {
                if (has_explicit_w && has_explicit_h) {
                    doc->width = doc_w;
                    doc->height = doc_h;
                    float sx = doc_w / vb_w;
                    float sy = doc_h / vb_h;
                    plutovg_matrix_init(&group_stack[0].matrix, sx, 0, 0, sy, -vb_x * sx, -vb_y * sy);
                } else {
                    doc->width = vb_w;
                    doc->height = vb_h;
                    plutovg_matrix_init_translate(&group_stack[0].matrix, -vb_x, -vb_y);
                }
            } else {
                doc->width = has_explicit_w ? doc_w : 800.0f;
                doc->height = has_explicit_h ? doc_h : 600.0f;
                plutovg_matrix_init_identity(&group_stack[0].matrix);
            }
        }
    }

    const char* cur = buffer;
    while (*cur && doc->node_count < MAX_SVG_NODES) {
        const char* tag_open = strchr(cur, '<');
        if (!tag_open) break;

        if (strncmp(tag_open, "<!--", 4) == 0) {
            const char* end_cmt = strstr(tag_open, "-->");
            cur = end_cmt ? end_cmt + 3 : tag_open + 4;
            continue;
        }

        if (strncmp(tag_open, "<defs", 5) == 0 || strncmp(tag_open, "<clipPath", 9) == 0 ||
            strncmp(tag_open, "<mask", 5) == 0 || strncmp(tag_open, "<pattern", 8) == 0 ||
            strncmp(tag_open, "<symbol", 7) == 0 || strncmp(tag_open, "<style", 6) == 0 ||
            strncmp(tag_open, "<script", 7) == 0) {
            const char* tag_end = strchr(tag_open, '>');
            if (tag_end) {
                if (is_self_closing_tag(tag_open, tag_end)) { cur = tag_end + 1; continue; }
                const char* close_tag = NULL;
                if (strncmp(tag_open, "<defs", 5) == 0) close_tag = strstr(tag_end, "</defs>");
                else if (strncmp(tag_open, "<clipPath", 9) == 0) close_tag = strstr(tag_end, "</clipPath>");
                else if (strncmp(tag_open, "<mask", 5) == 0) close_tag = strstr(tag_end, "</mask>");
                else if (strncmp(tag_open, "<pattern", 8) == 0) close_tag = strstr(tag_end, "</pattern>");
                else if (strncmp(tag_open, "<symbol", 7) == 0) close_tag = strstr(tag_end, "</symbol>");
                else if (strncmp(tag_open, "<style", 6) == 0) close_tag = strstr(tag_end, "</style>");
                else if (strncmp(tag_open, "<script", 7) == 0) close_tag = strstr(tag_end, "</script>");

                if (close_tag) {
                    const char* close_end = strchr(close_tag, '>');
                    cur = close_end ? close_end + 1 : close_tag + 6;
                    continue;
                }
            }
        }

        if (strncmp(tag_open, "</g", 3) == 0 && (isspace((unsigned char)tag_open[3]) || tag_open[3] == '>')) {
            if (group_depth > 0) group_depth--;
            const char* tag_end = strchr(tag_open, '>');
            cur = tag_end ? tag_end + 1 : tag_open + 3;
            continue;
        }

        if (tag_open[1] == '/') {
            const char* tag_end = strchr(tag_open, '>');
            cur = tag_end ? tag_end + 1 : tag_open + 2;
            continue;
        }

        if (strncmp(tag_open, "<g", 2) == 0 && (isspace((unsigned char)tag_open[2]) || tag_open[2] == '>')) {
            const char* tag_end = strchr(tag_open, '>');
            if (tag_end) {
                if (is_self_closing_tag(tag_open, tag_end)) {
                    cur = tag_end + 1;
                    continue;
                }

                if (group_depth < MAX_GROUP_DEPTH - 1) {
                    SvgGroupState* parent = &group_stack[group_depth];
                    group_depth++;
                    SvgGroupState* g = &group_stack[group_depth];
                    *g = *parent;

                    size_t t_len = 0;
                    const char* t_str = find_prop_slice(tag_open, tag_end, "transform", &t_len);
                    if (t_str && t_len > 0) {
                        plutovg_matrix_t g_mat;
                        parse_svg_transform(t_str, t_len, &g_mat);
                        plutovg_matrix_multiply(&g->matrix, &parent->matrix, &g_mat);
                    }

                    size_t a_len = 0;
                    const char* a_str = NULL;
                    plutovg_color_t black;
                    plutovg_color_init_rgb(&black, 0, 0, 0);

                    if ((a_str = find_prop_slice(tag_open, tag_end, "stroke", &a_len))) {
                        parse_color_slice(a_str, a_len, &g->has_stroke, &g->stroke_color, black);
                    }
                    if ((a_str = find_prop_slice(tag_open, tag_end, "stroke-width", &a_len))) {
                        g->has_stroke_width = true;
                        g->stroke_width = (float)atof(a_str);
                    }
                    if ((a_str = find_prop_slice(tag_open, tag_end, "fill", &a_len))) {
                        parse_color_slice(a_str, a_len, &g->has_fill, &g->fill_color, black);
                    }
                    if ((a_str = find_prop_slice(tag_open, tag_end, "fill-rule", &a_len))) {
                        g->has_fill_rule = true;
                        g->fill_rule = (a_len >= 7 && strncmp(a_str, "evenodd", 7) == 0) ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO;
                    }
                }
                cur = tag_end + 1;
                continue;
            }
        }

        bool is_path = (strncmp(tag_open, "<path", 5) == 0 && (isspace((unsigned char)tag_open[5]) || tag_open[5] == '>'));
        bool is_ellipse = (strncmp(tag_open, "<ellipse", 8) == 0 && (isspace((unsigned char)tag_open[8]) || tag_open[8] == '>'));
        bool is_circle = (strncmp(tag_open, "<circle", 7) == 0 && (isspace((unsigned char)tag_open[7]) || tag_open[7] == '>'));
        bool is_rect = (strncmp(tag_open, "<rect", 5) == 0 && (isspace((unsigned char)tag_open[5]) || tag_open[5] == '>'));
        bool is_line = (strncmp(tag_open, "<line", 5) == 0 && (isspace((unsigned char)tag_open[5]) || tag_open[5] == '>'));
        bool is_polyline = (strncmp(tag_open, "<polyline", 9) == 0 && (isspace((unsigned char)tag_open[9]) || tag_open[9] == '>'));
        bool is_polygon = (strncmp(tag_open, "<polygon", 8) == 0 && (isspace((unsigned char)tag_open[8]) || tag_open[8] == '>'));

        if (is_path || is_ellipse || is_circle || is_rect || is_line || is_polyline || is_polygon) {
            const char* tag_end = strchr(tag_open, '>');
            if (!tag_end) { cur = tag_open + 5; continue; }

            SvgNode* node = &doc->nodes[doc->node_count++];
            memset(node, 0, sizeof(SvgNode));
            node->type = SVG_NODE_PATH;
            node->path = plutovg_path_create();
            node->fill_rule = group_stack[group_depth].has_fill_rule ? group_stack[group_depth].fill_rule : PLUTOVG_FILL_RULE_NON_ZERO;

            size_t a_len = 0;
            const char* a_str = NULL;

            size_t d_len = 0;
            const char* d_str = find_prop_slice(tag_open, tag_end, "d", &d_len);

            if (d_str && d_len > 0) {
                plutovg_path_parse(node->path, d_str, (int)d_len);
            } else if (is_ellipse || is_circle) {
                float cx = 0, cy = 0, rx = 0, ry = 0;
                if ((a_str = find_prop_slice(tag_open, tag_end, "cx", &a_len))) cx = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "cy", &a_len))) cy = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "rx", &a_len))) rx = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "ry", &a_len))) ry = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "r", &a_len))) { rx = ry = (float)atof(a_str); }
                plutovg_path_add_ellipse(node->path, cx, cy, rx, ry);
            } else if (is_rect) {
                float rx = 0, ry = 0, rw = 0, rh = 0, r_rx = 0, r_ry = 0;
                if ((a_str = find_prop_slice(tag_open, tag_end, "x", &a_len))) rx = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "y", &a_len))) ry = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "width", &a_len))) rw = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "height", &a_len))) rh = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "rx", &a_len))) r_rx = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "ry", &a_len))) r_ry = (float)atof(a_str);
                if (r_rx > 0 || r_ry > 0) plutovg_path_add_round_rect(node->path, rx, ry, rw, rh, r_rx, r_ry);
                else plutovg_path_add_rect(node->path, rx, ry, rw, rh);
            } else if (is_line) {
                float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                if ((a_str = find_prop_slice(tag_open, tag_end, "x1", &a_len))) x1 = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "y1", &a_len))) y1 = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "x2", &a_len))) x2 = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "y2", &a_len))) y2 = (float)atof(a_str);
                plutovg_path_move_to(node->path, x1, y1);
                plutovg_path_line_to(node->path, x2, y2);
            } else if (is_polyline || is_polygon) {
                size_t pts_len = 0;
                const char* pts_str = find_prop_slice(tag_open, tag_end, "points", &pts_len);
                if (pts_str && pts_len > 0) {
                    const char* pe = pts_str + pts_len;
                    const char* pp = pts_str;
                    bool first = true;
                    while (pp < pe) {
                        while (pp < pe && (isspace((unsigned char)*pp) || *pp == ',')) pp++;
                        if (pp >= pe) break;
                        float px = (float)atof(pp);
                        while (pp < pe && !isspace((unsigned char)*pp) && *pp != ',') pp++;
                        while (pp < pe && (isspace((unsigned char)*pp) || *pp == ',')) pp++;
                        if (pp >= pe) break;
                        float py = (float)atof(pp);
                        while (pp < pe && !isspace((unsigned char)*pp) && *pp != ',') pp++;

                        if (first) { plutovg_path_move_to(node->path, px, py); first = false; }
                        else { plutovg_path_line_to(node->path, px, py); }
                    }
                    if (is_polygon) plutovg_path_close(node->path);
                }
            }

            size_t t_len = 0;
            const char* t_str = find_prop_slice(tag_open, tag_end, "transform", &t_len);
            plutovg_matrix_t elem_mat;
            if (t_str && t_len > 0) parse_svg_transform(t_str, t_len, &elem_mat);
            else plutovg_matrix_init_identity(&elem_mat);
            plutovg_matrix_multiply(&node->matrix, &group_stack[group_depth].matrix, &elem_mat);

            size_t fr_len = 0;
            const char* fr_str = find_prop_slice(tag_open, tag_end, "fill-rule", &fr_len);
            if (fr_str && fr_len >= 7 && strncmp(fr_str, "evenodd", 7) == 0) node->fill_rule = PLUTOVG_FILL_RULE_EVEN_ODD;

            node->stroke_width = group_stack[group_depth].has_stroke_width ? group_stack[group_depth].stroke_width : 1.0f;
            node->stroke_join = PLUTOVG_LINE_JOIN_MITER;
            node->stroke_cap = PLUTOVG_LINE_CAP_BUTT;

            if ((a_str = find_prop_slice(tag_open, tag_end, "stroke-width", &a_len))) node->stroke_width = (float)atof(a_str);
            if ((a_str = find_prop_slice(tag_open, tag_end, "stroke-linejoin", &a_len))) {
                if (a_len >= 5 && strncmp(a_str, "bevel", 5) == 0) node->stroke_join = PLUTOVG_LINE_JOIN_BEVEL;
                else if (a_len >= 5 && strncmp(a_str, "round", 5) == 0) node->stroke_join = PLUTOVG_LINE_JOIN_ROUND;
            }
            if ((a_str = find_prop_slice(tag_open, tag_end, "stroke-linecap", &a_len))) {
                if (a_len >= 5 && strncmp(a_str, "round", 5) == 0) node->stroke_cap = PLUTOVG_LINE_CAP_ROUND;
                else if (a_len >= 6 && strncmp(a_str, "square", 6) == 0) node->stroke_cap = PLUTOVG_LINE_CAP_SQUARE;
            }

            plutovg_color_t black;
            plutovg_color_init_rgb(&black, 0, 0, 0);

            const char* stroke_val = find_prop_slice(tag_open, tag_end, "stroke", &a_len);
            if (stroke_val) {
                parse_color_slice(stroke_val, a_len, &node->has_stroke, &node->stroke_color, black);
            } else if (group_stack[group_depth].has_stroke) {
                node->has_stroke = true;
                node->stroke_color = group_stack[group_depth].stroke_color;
            }

            const char* fill_val = find_prop_slice(tag_open, tag_end, "fill", &a_len);
            if (fill_val) {
                parse_color_slice(fill_val, a_len, &node->has_fill, &node->fill_color, black);
            } else if (group_stack[group_depth].has_fill) {
                node->has_fill = true;
                node->fill_color = group_stack[group_depth].fill_color;
            } else {
                if (is_line || is_polyline) node->has_fill = false;
                else {
                    node->has_fill = !node->has_stroke;
                    node->fill_color = black;
                }
            }

            doc->path_count++;
            cur = tag_end + 1;
            continue;
        }

        if (strncmp(tag_open, "<text", 5) == 0 && (isspace((unsigned char)tag_open[5]) || tag_open[5] == '>')) {
            const char* tag_end = strchr(tag_open, '>');
            const char* close_tag = strstr(tag_open, "</text>");
            if (tag_end && close_tag && close_tag > tag_end) {
                SvgNode* node = &doc->nodes[doc->node_count++];
                memset(node, 0, sizeof(SvgNode));
                node->type = SVG_NODE_TEXT;
                node->font_size = 1.0f;
                node->font_weight = 400;
                node->has_fill = true;
                node->fill_rule = PLUTOVG_FILL_RULE_NON_ZERO;
                plutovg_color_init_rgb(&node->fill_color, 0, 0, 0);

                size_t a_len = 0;
                const char* a_str = NULL;

                if ((a_str = find_prop_slice(tag_open, tag_end, "font-family", &a_len))) {
                    size_t cplen = a_len < sizeof(node->font_family) - 1 ? a_len : sizeof(node->font_family) - 1;
                    strncpy(node->font_family, a_str, cplen);
                    node->font_family[cplen] = '\0';
                }
                if ((a_str = find_prop_slice(tag_open, tag_end, "font-size", &a_len))) node->font_size = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "font-style", &a_len))) {
                    if (a_len >= 6 && strncmp(a_str, "italic", 6) == 0) node->is_italic = true;
                }
                if ((a_str = find_prop_slice(tag_open, tag_end, "font-weight", &a_len))) node->font_weight = atoi(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "x", &a_len))) node->x = (float)atof(a_str);
                if ((a_str = find_prop_slice(tag_open, tag_end, "y", &a_len))) node->y = (float)atof(a_str);

                if ((a_str = find_prop_slice(tag_open, tag_end, "fill", &a_len))) {
                    parse_color_slice(a_str, a_len, &node->has_fill, &node->fill_color, node->fill_color);
                } else if (group_stack[group_depth].has_fill) {
                    node->has_fill = true;
                    node->fill_color = group_stack[group_depth].fill_color;
                }

                size_t t_len = 0;
                const char* t_str = find_prop_slice(tag_open, tag_end, "transform", &t_len);
                plutovg_matrix_t elem_mat;
                if (t_str && t_len > 0) parse_svg_transform(t_str, t_len, &elem_mat);
                else plutovg_matrix_init_identity(&elem_mat);
                plutovg_matrix_multiply(&node->matrix, &group_stack[group_depth].matrix, &elem_mat);

                const char* text_start = tag_end + 1;
                size_t raw_len = (size_t)(close_tag - text_start);
                node->text_len = clean_and_unescape_text(text_start, raw_len, node->text, sizeof(node->text));

                if (node->text_len > 0) {
                    doc->text_count++;
                } else {
                    doc->node_count--;
                }
                cur = close_tag + 7;
                continue;
            }
        }
        cur = tag_open + 1;
    }

    free(buffer);
    return true;
}

void SvgDocument_Render(const SvgDocument* doc, plutovg_canvas_t* canvas) {
    if (!doc || !canvas) return;

    // White Artboard Background
    plutovg_canvas_save(canvas);
    plutovg_canvas_set_rgb(canvas, 1.0f, 1.0f, 1.0f);
    plutovg_canvas_fill_rect(canvas, 0, 0, doc->width, doc->height);
    plutovg_canvas_restore(canvas);

    for (int i = 0; i < doc->node_count; ++i) {
        const SvgNode* node = &doc->nodes[i];
        plutovg_canvas_save(canvas);
        plutovg_canvas_transform(canvas, &node->matrix);

        if (node->type == SVG_NODE_PATH && node->path) {
            plutovg_canvas_set_fill_rule(canvas, node->fill_rule);
            if (node->has_fill) {
                plutovg_canvas_set_color(canvas, &node->fill_color);
                plutovg_canvas_fill_path(canvas, node->path);
            }
            if (node->has_stroke) {
                plutovg_canvas_set_color(canvas, &node->stroke_color);
                plutovg_canvas_set_line_width(canvas, node->stroke_width);
                plutovg_canvas_set_line_join(canvas, node->stroke_join);
                plutovg_canvas_set_line_cap(canvas, node->stroke_cap);
                plutovg_canvas_stroke_path(canvas, node->path);
            }
        } else if (node->type == SVG_NODE_TEXT) {
            plutovg_font_face_t* face = SvgEngine_FindFont(&doc->registry, node->font_family);
            if (face) {
                plutovg_canvas_set_font_face(canvas, face);
                plutovg_canvas_set_font_size(canvas, node->font_size);
                plutovg_canvas_set_color(canvas, &node->fill_color);
                plutovg_canvas_fill_text(canvas, node->text, node->text_len, PLUTOVG_TEXT_ENCODING_UTF8, node->x, node->y);
            }
        }
        plutovg_canvas_restore(canvas);
    }
}

static void svg_path_traverse_cb(void* closure, plutovg_path_command_t command, const plutovg_point_t* points, int npoints) {
    (void)npoints;
    PathStringBuffer* psb = (PathStringBuffer*)closure;
    char temp[128];

    switch (command) {
    case PLUTOVG_PATH_COMMAND_MOVE_TO:
        snprintf(temp, sizeof(temp), "M%g %g ", points[0].x, points[0].y);
        psb_append(psb, temp);
        break;
    case PLUTOVG_PATH_COMMAND_LINE_TO:
        snprintf(temp, sizeof(temp), "L%g %g ", points[0].x, points[0].y);
        psb_append(psb, temp);
        break;
    case PLUTOVG_PATH_COMMAND_CUBIC_TO:
        snprintf(temp, sizeof(temp), "C%g %g %g %g %g %g ",
                 points[0].x, points[0].y, points[1].x, points[1].y, points[2].x, points[2].y);
        psb_append(psb, temp);
        break;
    case PLUTOVG_PATH_COMMAND_CLOSE:
        psb_append(psb, "Z ");
        break;
    }
}

bool SvgDocument_ExportWithEmbeddedPaths(const SvgDocument* doc, const char* out_path, SvgLogCallback log_cb) {
    if (!doc) return false;

    FILE* f = fopen(out_path, "wb");
    if (!f) return false;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%g\" height=\"%g\" viewBox=\"0 0 %g %g\">\n",
            doc->width, doc->height, doc->width, doc->height);

    if (log_cb) log_cb("[EXPORT] Converting %d elements (with font glyphs -> vector paths)...", doc->node_count);

    plutovg_surface_t* temp_surf = plutovg_surface_create(1, 1);
    plutovg_canvas_t* temp_canvas = plutovg_canvas_create(temp_surf);

    PathStringBuffer psb;
    psb_init(&psb, 32768);

    for (int i = 0; i < doc->node_count; ++i) {
        const SvgNode* node = &doc->nodes[i];
        psb.size = 0;
        if (psb.data) psb.data[0] = '\0';

        if (node->type == SVG_NODE_PATH && node->path) {
            plutovg_path_traverse(node->path, svg_path_traverse_cb, &psb);

            fprintf(f, "  <path transform=\"matrix(%g,%g,%g,%g,%g,%g)\" ",
                    node->matrix.a, node->matrix.b, node->matrix.c, node->matrix.d, node->matrix.e, node->matrix.f);

            if (node->fill_rule == PLUTOVG_FILL_RULE_EVEN_ODD) fprintf(f, "fill-rule=\"evenodd\" ");
            else fprintf(f, "fill-rule=\"nonzero\" ");

            if (node->has_fill) {
                fprintf(f, "fill=\"#%02X%02X%02X\" ",
                        (int)(node->fill_color.r * 255.0f + 0.5f),
                        (int)(node->fill_color.g * 255.0f + 0.5f),
                        (int)(node->fill_color.b * 255.0f + 0.5f));
            } else fprintf(f, "fill=\"none\" ");

            if (node->has_stroke) {
                fprintf(f, "stroke=\"#%02X%02X%02X\" stroke-width=\"%g\" ",
                        (int)(node->stroke_color.r * 255.0f + 0.5f),
                        (int)(node->stroke_color.g * 255.0f + 0.5f),
                        (int)(node->stroke_color.b * 255.0f + 0.5f),
                        node->stroke_width);
                if (node->stroke_join == PLUTOVG_LINE_JOIN_BEVEL) fprintf(f, "stroke-linejoin=\"bevel\" ");
                else if (node->stroke_join == PLUTOVG_LINE_JOIN_ROUND) fprintf(f, "stroke-linejoin=\"round\" ");
                if (node->stroke_cap == PLUTOVG_LINE_CAP_ROUND) fprintf(f, "stroke-linecap=\"round\" ");
                else if (node->stroke_cap == PLUTOVG_LINE_CAP_SQUARE) fprintf(f, "stroke-linecap=\"square\" ");
            }
            fprintf(f, "d=\"%s\"/>\n", psb.data);

        } else if (node->type == SVG_NODE_TEXT) {
            plutovg_font_face_t* face = SvgEngine_FindFont(&doc->registry, node->font_family);
            if (face) {
                plutovg_canvas_save(temp_canvas);
                plutovg_canvas_new_path(temp_canvas);
                plutovg_canvas_set_font_face(temp_canvas, face);
                plutovg_canvas_set_font_size(temp_canvas, node->font_size);
                plutovg_canvas_add_text(temp_canvas, node->text, node->text_len, PLUTOVG_TEXT_ENCODING_UTF8, node->x, node->y);

                const plutovg_path_t* text_path = plutovg_canvas_get_path(temp_canvas);
                if (text_path) {
                    plutovg_path_traverse(text_path, svg_path_traverse_cb, &psb);

                    char fill_str[32] = "#000000";
                    if (node->has_fill) {
                        snprintf(fill_str, sizeof(fill_str), "#%02X%02X%02X",
                                 (int)(node->fill_color.r * 255.0f + 0.5f),
                                 (int)(node->fill_color.g * 255.0f + 0.5f),
                                 (int)(node->fill_color.b * 255.0f + 0.5f));
                    }
                    fprintf(f, "  <path transform=\"matrix(%g,%g,%g,%g,%g,%g)\" fill=\"%s\" fill-rule=\"nonzero\" d=\"%s\"/>\n",
                            node->matrix.a, node->matrix.b, node->matrix.c, node->matrix.d, node->matrix.e, node->matrix.f,
                            fill_str, psb.data);
                }
                plutovg_canvas_restore(temp_canvas);
            }
        }
    }

    fprintf(f, "</svg>\n");
    fclose(f);

    psb_free(&psb);
    plutovg_canvas_destroy(temp_canvas);
    plutovg_surface_destroy(temp_surf);

    if (log_cb) log_cb("[EXPORT] Successfully exported standalone SVG with glyph paths to: %s", out_path);
    return true;
}
