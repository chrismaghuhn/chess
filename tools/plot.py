#!/usr/bin/env python3
"""Draws the training curve from generations.csv as an SVG, without extra packages.

Usage: tools/plot.py RUN_DIR/generations.csv [OUT.svg]

x: cumulative CPU seconds spent on self-play and tuning
y: Elo chained over generations (each generation against the previous one),
   with 95% error bars; labels are generation numbers.
"""
import csv
import math
import sys

WIDTH, HEIGHT = 760, 460
LEFT, RIGHT, TOP, BOTTOM = 70, 30, 40, 60


def nice_ticks(low, high, count=6):
    span = high - low or 1.0
    step = 10 ** math.floor(math.log10(span / count))
    for factor in (1, 2, 5, 10):
        if span / (step * factor) <= count:
            step *= factor
            break
    first = math.floor(low / step) * step
    ticks = []
    value = first
    while value <= high + step * 0.001:
        if value >= low - step * 0.001:
            ticks.append(round(value, 10))
        value += step
    return ticks


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(2)
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) == 3 else path.rsplit(".", 1)[0] + ".svg"

    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    points = [(float(r["cum_cpu_s"]), float(r["elo_chain"]), float(r["err_chain"]), r["gen"]) for r in rows]

    xs = [p[0] for p in points]
    lows = [p[1] - p[2] for p in points]
    highs = [p[1] + p[2] for p in points]
    x_ticks = nice_ticks(0, max(xs) or 1)
    y_ticks = nice_ticks(min(0, min(lows)), max(highs) or 1)
    x_max, y_min, y_max = x_ticks[-1], y_ticks[0], y_ticks[-1]

    def sx(x):
        return LEFT + (x / x_max) * (WIDTH - LEFT - RIGHT)

    def sy(y):
        return HEIGHT - BOTTOM - (y - y_min) / (y_max - y_min) * (HEIGHT - TOP - BOTTOM)

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" '
        f'font-family="sans-serif" font-size="12">',
        f'<rect width="{WIDTH}" height="{HEIGHT}" fill="white"/>',
        f'<text x="{WIDTH / 2}" y="22" text-anchor="middle" font-size="15">Elo vs. training cost</text>',
    ]
    for t in x_ticks:
        parts.append(f'<line x1="{sx(t):.1f}" y1="{TOP}" x2="{sx(t):.1f}" y2="{HEIGHT - BOTTOM}" stroke="#eee"/>')
        parts.append(f'<text x="{sx(t):.1f}" y="{HEIGHT - BOTTOM + 18}" text-anchor="middle">{t:g}</text>')
    for t in y_ticks:
        parts.append(f'<line x1="{LEFT}" y1="{sy(t):.1f}" x2="{WIDTH - RIGHT}" y2="{sy(t):.1f}" stroke="#eee"/>')
        parts.append(f'<text x="{LEFT - 8}" y="{sy(t) + 4:.1f}" text-anchor="end">{t:g}</text>')
    parts.append(f'<line x1="{LEFT}" y1="{HEIGHT - BOTTOM}" x2="{WIDTH - RIGHT}" y2="{HEIGHT - BOTTOM}" stroke="#333"/>')
    parts.append(f'<line x1="{LEFT}" y1="{TOP}" x2="{LEFT}" y2="{HEIGHT - BOTTOM}" stroke="#333"/>')
    parts.append(f'<text x="{(LEFT + WIDTH - RIGHT) / 2}" y="{HEIGHT - 15}" text-anchor="middle">'
                 'cumulative CPU seconds (self-play + tuning)</text>')
    parts.append(f'<text transform="translate(18 {(TOP + HEIGHT - BOTTOM) / 2}) rotate(-90)" '
                 'text-anchor="middle">Elo (chained, vs. gen 0)</text>')

    line = " ".join(f"{sx(x):.1f},{sy(y):.1f}" for x, y, _, _ in points)
    parts.append(f'<polyline points="{line}" fill="none" stroke="#2563eb" stroke-width="2"/>')
    for x, y, err, gen in points:
        parts.append(f'<line x1="{sx(x):.1f}" y1="{sy(y - err):.1f}" x2="{sx(x):.1f}" y2="{sy(y + err):.1f}" '
                     'stroke="#2563eb" stroke-opacity="0.5"/>')
        parts.append(f'<circle cx="{sx(x):.1f}" cy="{sy(y):.1f}" r="4" fill="#2563eb"/>')
        parts.append(f'<text x="{sx(x) + 6:.1f}" y="{sy(y) - 6:.1f}">{gen}</text>')
    parts.append("</svg>")

    with open(out, "w") as f:
        f.write("\n".join(parts) + "\n")
    print(out)


if __name__ == "__main__":
    main()
