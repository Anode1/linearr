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
BLUE, ORANGE, GREY = "#2a78d6", "#eb6834", "#6b6a67"
SANS = "Inter,Segoe UI,Helvetica,Arial,sans-serif"
MONO = "ui-monospace,SFMono-Regular,Menlo,Consolas,monospace"
W, H = 1200, 700
X0, X1, Y0, Y1 = 130, 1050, 210, 560
XLO, XHI = math.log10(3e5), math.log10(7e7)
YLO, YHI = math.log10(1_000), math.log10(2e7)
EXTRAP_TO = 50_000_000


def read_data(path):
    series, scalars = {}, {}
    for line in open(path):
        line = line.split("#", 1)[0].split()
        if not line:
            continue
        if not line[0].isdigit():
            scalars[line[0]] = float(line[1])
            continue
        rows, impl, kb = int(line[0]), line[1], float(line[2])
        series.setdefault(impl, []).append((rows, kb))
    for pts in series.values():
        pts.sort()
    return series, scalars


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
    series, scalars = read_data(data_path)
    for impl in ("linearr", "R"):
        if len(series.get(impl, [])) < 2:
            sys.exit(f"fig-memory: {data_path} needs two points for {impl}")

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         f'font-family="{SANS}">',
         f'<rect width="{W}" height="{H}" fill="{BG}"/>',
         f'<text x="56" y="62" font-size="31" font-weight="600" fill="{INK}">'
         'Memory is decided by the model, not by the data</text>',
         f'<text x="56" y="96" font-size="18" fill="{MUTED}">Peak RSS fitting the same 8-term model over 50 '
         'groups. All three produce the same coefficients, two of them exactly.</text>']

    for kb, lab in [(1024, "1 MB"), (10240, "10 MB"), (102400, "100 MB"),
                    (1048576, "1 GB"), (10485760, "10 GB")]:
        y = py(kb)
        o.append(f'<line x1="{X0}" y1="{y:.1f}" x2="{X1}" y2="{y:.1f}" stroke="{GRID}" stroke-width="1"/>')
        o.append(f'<text x="{X0 - 14}" y="{y + 5:.1f}" font-size="14" fill="{MUTED}" text-anchor="end">{lab}</text>')
    for rows, lab in [(500_000, "500k"), (5_000_000, "5M"), (50_000_000, "50M")]:
        o.append(f'<text x="{px(rows):.1f}" y="{Y1 + 34:.1f}" font-size="14" fill="{MUTED}" '
                 f'text-anchor="middle">{lab}</text>')
    o.append(f'<line x1="{X0}" y1="{Y1}" x2="{X1}" y2="{Y1}" stroke="{MUTED}" stroke-width="1"/>')

    rate = scalars["r_bytes_per_row"]
    baseline = scalars.get("r_baseline_kb")
    if baseline:
        o.append(f'<line x1="{X0}" y1="{py(baseline):.1f}" x2="{X1}" y2="{py(baseline):.1f}" '
                 f'stroke="{GREY}" stroke-width="1.5" stroke-dasharray="2 4"/>')
        o.append(f'<text x="{X1}" y="{py(baseline) - 10:.1f}" font-size="13" fill="{GREY}" '
                 f'text-anchor="end">an Rscript process that fits nothing: {mem(baseline)}</text>')

    # Each series ends where its own measurements say it goes: the frame at its
    # measured bytes per row, the streaming pair flat, since neither moved over
    # a tenfold increase.
    # dy: the two R series are 5 MB apart at 500,000 rows, so their labels sit
    # on opposite sides of the point or they overprint each other.
    plots = [("R", ORANGE, "R, read.csv + lm()", EXTRAP_TO * rate / 1024, -40),
             ("R-stream", GREY, "R, streaming (chunked)", None, -14),
             ("linearr", BLUE, "linearr, streaming", None, -16)]
    for impl, colour, label, ext, dy in plots:
        pts = series.get(impl)
        if not pts:
            continue
        end_kb = ext if ext else pts[-1][1]
        o.append(line(pts[0], pts[1], colour))
        o.append(line(pts[1], (EXTRAP_TO, end_kb), colour, dashed=True))
        for rows, kb in pts:
            o.append(f'<circle cx="{px(rows):.1f}" cy="{py(kb):.1f}" r="7" fill="{colour}" '
                     f'stroke="{BG}" stroke-width="2.5"/>')
            o.append(f'<text x="{px(rows) - 14:.1f}" y="{py(kb) + dy:.1f}" font-size="15" fill="{MUTED}" '
                     f'text-anchor="end" font-family="{MONO}">{mem(kb)}</text>')
        o.append(f'<text x="{px(EXTRAP_TO) + 12:.1f}" y="{py(end_kb) + 5:.1f}" font-size="16" fill="{colour}" '
                 f'font-weight="600">{label}</text>')

    ext_kb = EXTRAP_TO * rate / 1024
    o.append(f'<text x="{px(2.2e7):.1f}" y="{py(ext_kb) - 22:.1f}" font-size="13" fill="{MUTED}" '
             f'text-anchor="middle">extrapolated at its own {rate:g} bytes/row</text>')

    o.append(f'<text x="56" y="{H - 66}" font-size="15" fill="{MUTED}">Measured with '
             '<tspan font-family="' + MONO + '">/usr/bin/time -f %M</tspan> on one machine, at 500,000 and '
             '5,000,000 rows. Streaming is a property of the loop, not of the language: R reading in</text>')
    o.append(f'<text x="56" y="{H - 42}" font-size="15" fill="{MUTED}">20,000-row chunks '
             '(<tspan font-family="' + MONO + '">bench/fit-stream.R</tspan>) is as flat as the C and returns '
             'the identical coefficients. What it cannot put down is the interpreter:</text>')
    o.append(f'<text x="56" y="{H - 18}" font-size="15" fill="{MUTED}">at 500,000 rows the frame costs only about '
             '5 MB more than the chunked read, and nearly all of both figures is R itself.</text>')
    o.append('</svg>')

    with open(out, "w") as fh:
        fh.write("\n".join(o) + "\n")
    print(f"fig-memory: {out}")


if __name__ == "__main__":
    args = sys.argv[1:]
    main(args[0] if args else "doc/img/memory.data",
         args[1] if len(args) > 1 else "doc/img/memory-scaling.svg")
