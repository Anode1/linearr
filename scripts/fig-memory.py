#!/usr/bin/env python3
# fig-memory.py: draw peak memory against rows, streaming beside the frame.
#
# Reads doc/img/memory.data, which holds the measurements and says where they
# came from. The figure is only as current as that file; refresh it with the
# bench.sh commands its header prints.
#
#   python3 scripts/fig-memory.py doc/img/memory.data doc/img/memory-scaling.svg
import math
import sys

BG, INK, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
BLUE, ORANGE = "#2a78d6", "#eb6834"
SANS = "Inter,Segoe UI,Helvetica,Arial,sans-serif"
MONO = "ui-monospace,SFMono-Regular,Menlo,Consolas,monospace"
W, H = 1200, 700
X0, X1, Y0, Y1 = 130, 1050, 210, 560
XLO, XHI = math.log10(3e5), math.log10(7e7)
YLO, YHI = math.log10(1_000), math.log10(2e7)
EXTRAP_TO = 50_000_000


def read_data(path):
    series, rate = {}, None
    for line in open(path):
        line = line.split("#", 1)[0].split()
        if not line:
            continue
        if line[0] == "r_bytes_per_row":
            rate = float(line[1])
            continue
        rows, impl, kb = int(line[0]), line[1], float(line[2])
        series.setdefault(impl, []).append((rows, kb))
    for pts in series.values():
        pts.sort()
    return series, rate


def px(rows):
    return X0 + (math.log10(rows) - XLO) / (XHI - XLO) * (X1 - X0)


def py(kb):
    return Y1 - (math.log10(kb) - YLO) / (YHI - YLO) * (Y1 - Y0)


def mem(kb):
    return f"{kb / 1024:.1f} MB" if kb < 1024 * 1024 else f"{kb / 1024 / 1024:.1f} GB"


def line(a, b, colour, dashed=False):
    dash = ' stroke-dasharray="7 5" opacity="0.75"' if dashed else ""
    return (f'<line x1="{px(a[0]):.1f}" y1="{py(a[1]):.1f}" x2="{px(b[0]):.1f}" y2="{py(b[1]):.1f}" '
            f'stroke="{colour}" stroke-width="3"{dash}/>')


def main(data_path, out):
    series, rate = read_data(data_path)
    for impl in ("linearr", "R"):
        if len(series.get(impl, [])) < 2:
            sys.exit(f"fig-memory: {data_path} needs two points for {impl}")

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         f'font-family="{SANS}">',
         f'<rect width="{W}" height="{H}" fill="{BG}"/>',
         f'<text x="56" y="62" font-size="31" font-weight="600" fill="{INK}">'
         'Memory is decided by the model, not by the data</text>',
         f'<text x="56" y="96" font-size="18" fill="{MUTED}">Peak RSS fitting the same 8-term model over 50 '
         'groups. Both produce the same coefficients, agreeing to 1e-12.</text>']

    for kb, lab in [(1024, "1 MB"), (10240, "10 MB"), (102400, "100 MB"),
                    (1048576, "1 GB"), (10485760, "10 GB")]:
        y = py(kb)
        o.append(f'<line x1="{X0}" y1="{y:.1f}" x2="{X1}" y2="{y:.1f}" stroke="{GRID}" stroke-width="1"/>')
        o.append(f'<text x="{X0 - 14}" y="{y + 5:.1f}" font-size="14" fill="{MUTED}" text-anchor="end">{lab}</text>')
    for rows, lab in [(500_000, "500k"), (5_000_000, "5M"), (50_000_000, "50M")]:
        o.append(f'<text x="{px(rows):.1f}" y="{Y1 + 34:.1f}" font-size="14" fill="{MUTED}" '
                 f'text-anchor="middle">{lab}</text>')
    o.append(f'<line x1="{X0}" y1="{Y1}" x2="{X1}" y2="{Y1}" stroke="{MUTED}" stroke-width="1"/>')

    r_pts, l_pts = series["R"], series["linearr"]
    ext_kb = EXTRAP_TO * rate / 1024
    o.append(line(r_pts[0], r_pts[1], ORANGE))
    o.append(line(r_pts[1], (EXTRAP_TO, ext_kb), ORANGE, dashed=True))
    o.append(line(l_pts[0], l_pts[1], BLUE))
    o.append(line(l_pts[1], (EXTRAP_TO, l_pts[1][1]), BLUE, dashed=True))

    for pts, colour in ((r_pts, ORANGE), (l_pts, BLUE)):
        for rows, kb in pts:
            o.append(f'<circle cx="{px(rows):.1f}" cy="{py(kb):.1f}" r="7" fill="{colour}" '
                     f'stroke="{BG}" stroke-width="2.5"/>')
            o.append(f'<text x="{px(rows) - 14:.1f}" y="{py(kb) - 16:.1f}" font-size="15" fill="{MUTED}" '
                     f'text-anchor="end" font-family="{MONO}">{mem(kb)}</text>')

    o.append(f'<text x="{px(EXTRAP_TO) + 12:.1f}" y="{py(ext_kb) + 5:.1f}" font-size="16" fill="{ORANGE}" '
             'font-weight="600">R, read.csv + lm()</text>')
    o.append(f'<text x="{px(EXTRAP_TO) + 12:.1f}" y="{py(l_pts[1][1]) + 5:.1f}" font-size="16" fill="{BLUE}" '
             'font-weight="600">linearr, streaming</text>')
    o.append(f'<text x="{px(2.2e7):.1f}" y="{py(ext_kb) - 22:.1f}" font-size="13" fill="{MUTED}" '
             f'text-anchor="middle">extrapolated at its own {rate:g} bytes/row</text>')

    o.append(f'<text x="56" y="{H - 66}" font-size="15" fill="{MUTED}">Measured with '
             '<tspan font-family="' + MONO + '">/usr/bin/time -f %M</tspan> on one machine, at 500,000 and '
             '5,000,000 rows. A row is folded into the cross-products and dropped, so</text>')
    o.append(f'<text x="56" y="{H - 42}" font-size="15" fill="{MUTED}">the accumulator is sized by the number of '
             'groups rather than the number of rows. The frame is R&#8217;s idiom, not a limit of the language: '
             'streaming Python holds</text>')
    o.append(f'<text x="56" y="{H - 18}" font-size="15" fill="{MUTED}">at 10 MB and awk at 5.5 MB across the same '
             'tenfold increase. Streaming is the property that matters, not the language.</text>')
    o.append('</svg>')

    with open(out, "w") as fh:
        fh.write("\n".join(o) + "\n")
    print(f"fig-memory: {out}")


if __name__ == "__main__":
    args = sys.argv[1:]
    main(args[0] if args else "doc/img/memory.data",
         args[1] if len(args) > 1 else "doc/img/memory-scaling.svg")
