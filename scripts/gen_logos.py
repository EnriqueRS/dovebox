#!/usr/bin/env python3
"""Genera dovebox_logos.c con los logos de los proveedores de noticias
como imágenes LVGL 9 embebidas (ARGB8888, 64x64).

Uso: python3 gen_logos.py /tmp/dovebox_logos /ruta/salida/dovebox_logos.c
"""
import os
import sys
from PIL import Image

SIZE = 64
FEEDS = [
    ("marca", "marca_fav.png"),
    ("besoccer", "besoccer_fav.png"),
    ("mundodeportivo", "mundodeportivo_fav.png"),
    ("elespanol", "elespanol_fav.png"),
    ("xataka", "xataka_fav.png"),
]


def rgba8888_to_argb_le(r, g, b, a):
    """LVGL ARGB8888 en memoria little-endian: bytes B,G,R,A."""
    return bytes([b, g, r, a])


def load_rgba(path):
    img = Image.open(path).convert("RGBA")
    # Ajustar al cuadrado más pequeño (favicons suelen traer padding) y escalar
    w, h = img.size
    side = min(w, h)
    img = img.crop(((w - side) // 2, (h - side) // 2,
                    (w + side) // 2, (h + side) // 2))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    return img


def main():
    src_dir, out_path = sys.argv[1], sys.argv[2]
    lines = []
    lines.append("// Generado por gen_logos.py — logos de proveedores de noticias")
    lines.append("// Formato LVGL 9: ARGB8888, 64x64, stride 256. NO editar a mano.")
    lines.append('#include "lvgl.h"')
    lines.append("")

    for name, fname in FEEDS:
        path = os.path.join(src_dir, fname)
        img = load_rgba(path)
        px = img.load()
        data = []
        for y in range(SIZE):
            for x in range(SIZE):
                r, g, b, a = px[x, y]
                data.append(rgba8888_to_argb_le(r, g, b, a))
        body = ", ".join(f"0x{byte:02x}" for byte in b"".join(data))
        lines.append(f"static const uint8_t logo_{name}_data[{SIZE*SIZE*4}] = {{")
        # 16 bytes por línea
        for i in range(0, len(b"".join(data)), 16):
            chunk = b"".join(data)[i:i+16]
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
        lines.append("};")
        lines.append(f"static const lv_image_dsc_t logo_{name} = {{")
        lines.append(f"    .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,")
        lines.append(f"                .w = {SIZE}, .h = {SIZE}, .stride = {SIZE*4} }},")
        lines.append(f"    .data_size = {SIZE*SIZE*4},")
        lines.append(f"    .data = logo_{name}_data,")
        lines.append("};")
        lines.append("")

    lines.append("const lv_image_dsc_t* dovebox_logo(const char* feed) {")
    lines.append('    if (!feed) return nullptr;')
    for name, _ in FEEDS:
        lines.append(f'    if (strcmp(feed, "{name}") == 0) return &logo_{name};')
    lines.append("    return nullptr;")
    lines.append("}")

    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"OK -> {out_path} ({os.path.getsize(out_path)} bytes)")


if __name__ == "__main__":
    main()
