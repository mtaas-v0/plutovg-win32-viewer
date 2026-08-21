# PlutoVG Win32 Viewer

A lightweight native Win32 vector graphics and typography viewer powered by [PlutoVG](https://github.com/sammycage/plutovg).

## Features

- **Interactive Navigation:** Cursor-centered zoom-in/zoom-out and panning.
- **Font Rendering & TTF Lookup:** Automatically resolves standard Windows fonts (`Segoe UI`, `Arial`, `Calibri`, etc.).
- **Custom Font Loading:** Drag & drop `.ttf`/`.otf` files or open via the `File` menu.
- **Affine Transformations:** Interactive rotation, shearing, and scaling.
- **High DPI Aware:** Crystal-clear rendering across standard and high-DPI displays.

## Controls

| Action | Control |
|---|---|
| **Zoom** | Mouse Wheel / `+` / `-` |
| **Pan** | Click & Drag (Left Mouse Button) |
| **Rotate** | `R` (Clockwise) / `Shift + R` (Counter-Clockwise) |
| **Shear** | `S` / `Shift + S` |
| **Reset View** | `Space` or `0` |
| **Toggle Grid** | `G` |
| **Open Font** | `O` or Drag & Drop `.ttf`/`.otf` |

## Building

```bash
git clone https://github.com/<your-user>/plutovg-win32-viewer.git
cd plutovg-win32-viewer
cmake -B build
cmake --build build --config Release
```
