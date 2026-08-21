#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>

#include <plutovg.h>
#include "font_helper.h"

#define ID_MENU_OPEN_FONT   1001
#define ID_MENU_RESET_VIEW  1002
#define ID_MENU_TOGGLE_GRID 1003
#define ID_MENU_EXIT        1004

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

    // Font state
    FontInfo current_font;
    plutovg_font_face_t* font_face;

    // Preferences
    bool show_grid;
} AppState;

static AppState g_app;

static void reset_view(AppState* app) {
    app->zoom = 1.0f;
    app->pan_x = (float)app->width / 2.0f;
    app->pan_y = (float)app->height / 2.0f;
    app->rotation_deg = 0.0f;
    app->shear_x = 0.0f;
}

static void load_font(AppState* app, const char* path) {
    if (app->font_face) {
        plutovg_font_face_destroy(app->font_face);
        app->font_face = NULL;
    }

    app->font_face = plutovg_font_face_load_from_file(path, 0);
    if (app->font_face) {
        snprintf(g_app.current_font.font_path, sizeof(g_app.current_font.font_path), "%s", path);
        FontHelper_GetFontName(path, g_app.current_font.font_name, sizeof(g_app.current_font.font_name));
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

static void draw_grid(plutovg_canvas_t* canvas, int width, int height) {
    plutovg_canvas_save(canvas);
    plutovg_canvas_set_rgb(canvas, 0.22f, 0.24f, 0.28f);
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
    plutovg_canvas_set_rgb(canvas, 0.8f, 0.25f, 0.25f); // X-axis red
    plutovg_canvas_set_line_width(canvas, 2.0f);
    plutovg_canvas_move_to(canvas, -extent, 0);
    plutovg_canvas_line_to(canvas, extent, 0);
    plutovg_canvas_stroke(canvas);

    plutovg_canvas_set_rgb(canvas, 0.25f, 0.8f, 0.25f); // Y-axis green
    plutovg_canvas_move_to(canvas, 0, -extent);
    plutovg_canvas_line_to(canvas, 0, extent);
    plutovg_canvas_stroke(canvas);

    plutovg_canvas_restore(canvas);
}

static void draw_hud(AppState* app) {
    plutovg_canvas_save(app->canvas);
    plutovg_canvas_reset_matrix(app->canvas);

    // Overlay Card
    plutovg_canvas_set_rgba(app->canvas, 0.08f, 0.09f, 0.11f, 0.85f);
    plutovg_canvas_round_rect(app->canvas, 16, 16, 360, 150, 10, 10);
    plutovg_canvas_fill(app->canvas);

    if (app->font_face) {
        plutovg_canvas_set_font_face(app->canvas, app->font_face);
        plutovg_canvas_set_font_size(app->canvas, 13.0f);
        plutovg_canvas_set_rgb(app->canvas, 0.95f, 0.95f, 0.95f);

        char buf[256];
        snprintf(buf, sizeof(buf), "Font: %s", app->current_font.font_name);
        plutovg_canvas_fill_text(app->canvas, buf, -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 42);

        snprintf(buf, sizeof(buf), "Zoom: %.1f%% | Pan: (%.0f, %.0f)", app->zoom * 100.0f, app->pan_x, app->pan_y);
        plutovg_canvas_fill_text(app->canvas, buf, -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 66);

        snprintf(buf, sizeof(buf), "Rotation: %.1f deg | Shear: %.2f", app->rotation_deg, app->shear_x);
        plutovg_canvas_fill_text(app->canvas, buf, -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 90);

        plutovg_canvas_set_rgb(app->canvas, 0.65f, 0.70f, 0.75f);
        plutovg_canvas_fill_text(app->canvas, "Mouse Wheel: Zoom | Left-Drag: Pan", -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 118);
        plutovg_canvas_fill_text(app->canvas, "R/Shift+R: Rotate | S/Shift+S: Shear | Space: Reset", -1, PLUTOVG_TEXT_ENCODING_UTF8, 28, 140);
    }
    plutovg_canvas_restore(app->canvas);
}

static void render(AppState* app) {
    if (!app->canvas) return;

    // Clear background
    plutovg_canvas_save(app->canvas);
    plutovg_canvas_reset_matrix(app->canvas);
    plutovg_canvas_set_rgb(app->canvas, 0.12f, 0.13f, 0.15f);
    plutovg_canvas_fill_rect(app->canvas, 0, 0, (float)app->width, (float)app->height);
    plutovg_canvas_restore(app->canvas);

    // Apply Viewport Transformation: Translate -> Rotate -> Shear -> Scale
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
        draw_grid(app->canvas, app->width, app->height);
    }

    // Render Sample Text & Transformed Typography
    if (app->font_face) {
        plutovg_canvas_set_font_face(app->canvas, app->font_face);

        // Heading
        plutovg_canvas_set_font_size(app->canvas, 42.0f);
        plutovg_canvas_set_rgb(app->canvas, 0.28f, 0.65f, 0.98f);
        plutovg_canvas_fill_text(app->canvas, "PlutoVG Vector & Font Engine", -1, PLUTOVG_TEXT_ENCODING_UTF8, -250, -120);

        // Pangram
        plutovg_canvas_set_font_size(app->canvas, 24.0f);
        plutovg_canvas_set_rgb(app->canvas, 0.92f, 0.92f, 0.94f);
        plutovg_canvas_fill_text(app->canvas, "The quick brown fox jumps over the lazy dog.", -1, PLUTOVG_TEXT_ENCODING_UTF8, -250, -60);

        // Glyph Set
        plutovg_canvas_set_font_size(app->canvas, 18.0f);
        plutovg_canvas_set_rgb(app->canvas, 0.70f, 0.72f, 0.78f);
        plutovg_canvas_fill_text(app->canvas, "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789", -1, PLUTOVG_TEXT_ENCODING_UTF8, -250, -10);

        // Stroked / Outlined Text
        plutovg_canvas_set_font_size(app->canvas, 48.0f);
        plutovg_canvas_set_rgb(app->canvas, 0.95f, 0.62f, 0.15f);
        plutovg_canvas_set_line_width(app->canvas, 1.5f);
        plutovg_canvas_stroke_text(app->canvas, "OUTLINE TRANSFORMATION", -1, PLUTOVG_TEXT_ENCODING_UTF8, -250, 60);

        // Geometric Decoration
        plutovg_canvas_save(app->canvas);
        plutovg_canvas_translate(app->canvas, -200, 150);
        plutovg_canvas_set_rgb(app->canvas, 0.9f, 0.2f, 0.4f);
        plutovg_canvas_arc(app->canvas, 0, 0, 40, 0, 6.2831853f, 0);
        plutovg_canvas_fill(app->canvas);
        plutovg_canvas_restore(app->canvas);
    }

    plutovg_canvas_restore(app->canvas);

    // Draw HUD
    draw_hud(app);
}

static void zoom_at(AppState* app, float screen_x, float screen_y, float factor) {
    float new_zoom = app->zoom * factor;
    if (new_zoom < 0.05f) new_zoom = 0.05f;
    if (new_zoom > 50.0f) new_zoom = 50.0f;

    // Maintain focal point under mouse cursor
    app->pan_x = screen_x - (screen_x - app->pan_x) * (new_zoom / app->zoom);
    app->pan_y = screen_y - (screen_y - app->pan_y) * (new_zoom / app->zoom);
    app->zoom = new_zoom;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE);
        g_app.show_grid = true;
        g_app.zoom = 1.0f;

        // Discover and load default font
        if (FontHelper_GetSystemFont(&g_app.current_font)) {
            load_font(&g_app, g_app.current_font.font_path);
        }
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
        case 'O':
            if (FontHelper_BrowseFont(hwnd, &g_app.current_font)) {
                load_font(&g_app, g_app.current_font.font_path);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        char dropped_file[MAX_PATH];
        if (DragQueryFileA(hDrop, 0, dropped_file, MAX_PATH)) {
            load_font(&g_app, dropped_file);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_MENU_OPEN_FONT:
            if (FontHelper_BrowseFont(hwnd, &g_app.current_font)) {
                load_font(&g_app, g_app.current_font.font_path);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
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
        return 1; // Prevent flicker during resize

    case WM_DESTROY:
        if (g_app.font_face) plutovg_font_face_destroy(g_app.font_face);
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

    AppendMenuA(hFileMenu, MF_STRING, ID_MENU_OPEN_FONT, "&Open Font...\t(O)");
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

    // Enable Per-Monitor DPI Awareness
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
        "PlutoVG Typography & Vector Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1024, 768,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        MessageBoxA(NULL, "Failed to create window.", "Error", MB_ICONERROR);
        return 1;
    }

    create_app_menu(hwnd);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
