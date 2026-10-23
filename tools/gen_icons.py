#!/usr/bin/env python3
"""Renders the carousel icons as anti-aliased alpha masks into main/icons.h.

Each icon is drawn as a vector at 4x supersampling, then box-averaged down to
MASTER x MASTER 8-bit alpha. The firmware samples these bilinearly at any size,
so the icons stay smooth instead of blocky.
"""
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MASTER = 96
SS = 4
HI = MASTER * SS


class Canvas:
    def __init__(self):
        self.px = bytearray(HI * HI)

    def set(self, x, y):
        if 0 <= x < HI and 0 <= y < HI:
            self.px[y * HI + x] = 1

    def disc(self, cx, cy, r):
        for y in range(max(0, int(cy - r)), min(HI, int(cy + r + 1))):
            for x in range(max(0, int(cx - r)), min(HI, int(cx + r + 1))):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    self.px[y * HI + x] = 1

    def ring(self, cx, cy, r, thickness):
        for y in range(max(0, int(cy - r)), min(HI, int(cy + r + 1))):
            for x in range(max(0, int(cx - r)), min(HI, int(cx + r + 1))):
                d = math.hypot(x - cx, y - cy)
                if r - thickness <= d <= r:
                    self.px[y * HI + x] = 1

    def arc(self, cx, cy, r, a0, a1, thickness):
        steps = int(r * 8) + 20
        for i in range(steps + 1):
            a = math.radians(a0 + (a1 - a0) * i / steps)
            self.disc(cx + (r - thickness / 2) * math.cos(a), cy + (r - thickness / 2) * math.sin(a), thickness / 2)

    def line(self, x0, y0, x1, y1, thickness):
        steps = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for i in range(steps + 1):
            x = x0 + (x1 - x0) * i / steps
            y = y0 + (y1 - y0) * i / steps
            self.disc(x, y, thickness / 2)

    def rect(self, x0, y0, x1, y1):
        for y in range(max(0, int(y0)), min(HI, int(y1) + 1)):
            for x in range(max(0, int(x0)), min(HI, int(x1) + 1)):
                self.px[y * HI + x] = 1

    def round_rect(self, x0, y0, x1, y1, r):
        self.rect(x0 + r, y0, x1 - r, y1)
        self.rect(x0, y0 + r, x1, y1 - r)
        for cx, cy in ((x0 + r, y0 + r), (x1 - r, y0 + r), (x0 + r, y1 - r), (x1 - r, y1 - r)):
            self.disc(cx, cy, r)

    def clear_disc(self, cx, cy, r):
        for y in range(max(0, int(cy - r)), min(HI, int(cy + r + 1))):
            for x in range(max(0, int(cx - r)), min(HI, int(cx + r + 1))):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    self.px[y * HI + x] = 0

    def clear_rect(self, x0, y0, x1, y1):
        for y in range(max(0, int(y0)), min(HI, int(y1) + 1)):
            for x in range(max(0, int(x0)), min(HI, int(x1) + 1)):
                self.px[y * HI + x] = 0

    def downsample(self):
        alpha = bytearray(MASTER * MASTER)
        for y in range(MASTER):
            for x in range(MASTER):
                total = 0
                for dy in range(SS):
                    base = (y * SS + dy) * HI + x * SS
                    for dx in range(SS):
                        total += self.px[base + dx]
                alpha[y * MASTER + x] = total * 255 // (SS * SS)
        return alpha


# All coordinates are in the HI (supersampled) space. U = one MASTER pixel.
U = SS


def wifi():
    c = Canvas()
    cx, cy = 48 * U, 76 * U
    for r in (22, 40, 58):
        c.arc(cx, cy, r * U, 222, 318, 9 * U / 2)
    c.disc(cx, cy, 8 * U)
    return c


def bluetooth():
    c = Canvas()
    t = 5 * U
    c.line(48 * U, 16 * U, 48 * U, 80 * U, t)
    c.line(48 * U, 16 * U, 68 * U, 32 * U, t)
    c.line(68 * U, 32 * U, 48 * U, 48 * U, t)
    c.line(48 * U, 48 * U, 68 * U, 64 * U, t)
    c.line(68 * U, 64 * U, 48 * U, 80 * U, t)
    c.line(28 * U, 32 * U, 48 * U, 48 * U, t)
    c.line(28 * U, 64 * U, 48 * U, 48 * U, t)
    return c


def folder():
    c = Canvas()
    c.round_rect(16 * U, 30 * U, 80 * U, 78 * U, 6 * U)   # body
    c.round_rect(16 * U, 22 * U, 44 * U, 40 * U, 5 * U)   # tab
    c.clear_rect(23 * U, 40 * U, 73 * U, 71 * U)          # hollow
    c.round_rect(16 * U, 34 * U, 80 * U, 44 * U, 4 * U)   # front lip
    return c


def gear():
    c = Canvas()
    cx = cy = 48 * U
    for i in range(8):
        a = math.radians(i * 45)
        c.line(cx, cy, cx + 38 * U * math.cos(a), cy + 38 * U * math.sin(a), 13 * U)
    c.disc(cx, cy, 30 * U)
    c.clear_disc(cx, cy, 13 * U)
    return c


def chip():
    c = Canvas()
    c.round_rect(26 * U, 26 * U, 70 * U, 70 * U, 6 * U)
    c.clear_rect(34 * U, 34 * U, 62 * U, 62 * U)
    c.rect(44 * U, 44 * U, 52 * U, 52 * U)
    for i in range(3):
        o = (32 + i * 16) * U
        c.rect(o - 3 * U, 14 * U, o + 3 * U, 26 * U)   # top
        c.rect(o - 3 * U, 70 * U, o + 3 * U, 82 * U)   # bottom
        c.rect(14 * U, o - 3 * U, 26 * U, o + 3 * U)   # left
        c.rect(70 * U, o - 3 * U, 82 * U, o + 3 * U)   # right
    return c


def prompt():
    c = Canvas()
    c.round_rect(10 * U, 18 * U, 86 * U, 78 * U, 8 * U)
    c.clear_rect(16 * U, 24 * U, 80 * U, 72 * U)
    c.line(26 * U, 40 * U, 42 * U, 50 * U, 6 * U)   # > chevron
    c.line(42 * U, 50 * U, 26 * U, 60 * U, 6 * U)
    c.rect(48 * U, 57 * U, 72 * U, 62 * U)          # underscore
    return c


ICONS = [("wifi", wifi), ("bluetooth", bluetooth), ("folder", folder),
         ("gear", gear), ("chip", chip), ("prompt", prompt)]


def main():
    out = ["// Generated by tools/gen_icons.py. Anti-aliased alpha masks, MASTER x MASTER, 1 byte each.",
           "#pragma once", "#include <stdint.h>", "",
           f"#define ICON_SIZE {MASTER}", "",
           "typedef struct {", "    const char* name;", "    const uint8_t* alpha; // ICON_SIZE*ICON_SIZE", "} Icon;", ""]
    for name, draw in ICONS:
        alpha = draw().downsample()
        out.append(f"static const uint8_t {name}_alpha[{MASTER * MASTER}] = {{")
        for i in range(0, len(alpha), 24):
            out.append("    " + ",".join(str(v) for v in alpha[i:i + 24]) + ",")
        out += ["};", ""]
    out.append("static const Icon icons[] = {")
    for name, _ in ICONS:
        out.append(f'    {{"{name}", {name}_alpha}},')
    out += ["};", "#define ICON_COUNT (sizeof(icons) / sizeof(icons[0]))"]
    open(os.path.join(ROOT, "main", "icons.h"), "w").write("\n".join(out) + "\n")

    # ASCII preview with shades
    shades = " .:-=+*#%@"
    for name, draw in ICONS:
        alpha = draw().downsample()
        print(name)
        for y in range(0, MASTER, 3):
            print("".join(shades[alpha[y * MASTER + x] * (len(shades) - 1) // 255] for x in range(0, MASTER, 2)))
        print()


main()
