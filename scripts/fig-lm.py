#!/usr/bin/env python3
# fig-lm.py: draw the coefficient agreement against lm(), one row per example.
#
# Reads the table scripts/r-check.sh prints, on stdin, so the picture cannot
# disagree with the check that produced it. No R, no rows, no figure: it exits
# non-zero rather than drawing a stale one.
#
#   sh scripts/r-check.sh | python3 scripts/fig-lm.py doc/img/lm-agreement.svg
import math
import sys

BG, INK, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
BLUE, ORANGE, LINK = "#2a78d6", "#eb6834", "#d5d4d0"
SANS = "Inter,Segoe UI,Helvetica,Arial,sans-serif"
MONO = "ui-monospace,SFMono-Regular,Menlo,Consolas,monospace"
TOL = 1e-6                      # what make check holds --qr to
W = 1200
X0, X1 = 300, 1140              # plot area
LO, HI = -16.5, -4.0            # log10 limits, wide enough for exact zeros
Y0, ROWH = 232, 38


def read_table(lines):
    """(name, qr, default) per example file. Anything else on the stream is
    ignored: r-check.sh prints a header and a summary line around the table."""
    rows = []
    for line in lines:
        f = line.split()
        if len(f) != 3 or not f[0].endswith(".csv"):
            continue
        try:
            qr, dflt = float(f[1]), float(f[2])
        except ValueError:
            continue
        rows.append((f[0].rsplit("/", 1)[-1], qr, dflt))
    return sorted(rows, key=lambda r: -max(r[1], r[2]))


def main(out):
    rows = read_table(sys.stdin)
    if not rows:
        sys.exit("fig-lm: no comparison rows on stdin (no R?)")

    height = Y0 + (len(rows) - 1) * ROWH + 168
    bot = Y0 + (len(rows) - 1) * ROWH + 26
    # A difference of zero has no logarithm; the axis floor stands in for it.
    px = lambda v: X0 + (max(math.log10(v), LO) - LO) / (HI - LO) * (X1 - X0)

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{height}" '
         f'viewBox="0 0 {W} {height}" font-family="{SANS}">',
         f'<rect width="{W}" height="{height}" fill="{BG}"/>',
         f'<text x="56" y="60" font-size="30" font-weight="600" fill="{INK}">'
         'Both solvers against R&#8217;s lm(), file by file</text>',
         f'<text x="56" y="92" font-size="18" fill="{MUTED}">Worst relative difference against lm() in any '
         'coefficient of any group, per example file. Log scale: further left is closer to R.</text>',
         f'<circle cx="62" cy="126" r="8" fill="{ORANGE}"/>',
         f'<circle cx="62" cy="126" r="5.5" fill="{BLUE}" stroke="{BG}" stroke-width="2"/>',
         f'<text x="78" y="131" font-size="15" fill="{MUTED}">both solvers, identical</text>',
         f'<circle cx="258" cy="126" r="6.5" fill="{BLUE}" stroke="{BG}" stroke-width="2"/>',
         f'<text x="272" y="131" font-size="15" fill="{MUTED}">--qr</text>',
         f'<circle cx="336" cy="126" r="6.5" fill="{ORANGE}" stroke="{BG}" stroke-width="2"/>',
         f'<text x="350" y="131" font-size="15" fill="{MUTED}">normal equations (default)</text>']

    for e in range(-16, -3, 2):
        gx = px(10.0 ** e)
        o.append(f'<line x1="{gx:.1f}" y1="176" x2="{gx:.1f}" y2="{bot}" stroke="{GRID}" stroke-width="1"/>')
        o.append(f'<text x="{gx:.1f}" y="{bot + 24}" font-size="13" fill="{MUTED}" '
                 f'text-anchor="middle" font-family="{MONO}">1e{e}</text>')

    tx = px(TOL)
    o.append(f'<line x1="{tx:.1f}" y1="176" x2="{tx:.1f}" y2="{bot}" stroke="{MUTED}" '
             'stroke-width="1.5" stroke-dasharray="5 4"/>')
    o.append(f'<text x="{tx - 10:.1f}" y="196" font-size="14" fill="{MUTED}" text-anchor="end">'
             f'make check fails --qr to the right of {TOL:g}</text>')

    same = 0
    for i, (name, qr, dflt) in enumerate(rows):
        y = Y0 + i * ROWH
        o.append(f'<text x="{X0 - 22}" y="{y + 5}" font-size="15" fill="{INK}" '
                 f'text-anchor="end" font-family="{MONO}">{name}</text>')
        if qr == dflt:
            same += 1
            o.append(f'<circle cx="{px(qr):.1f}" cy="{y}" r="8" fill="{ORANGE}"/>')
            o.append(f'<circle cx="{px(qr):.1f}" cy="{y}" r="5.5" fill="{BLUE}" '
                     f'stroke="{BG}" stroke-width="2"/>')
            continue
        o.append(f'<line x1="{px(qr):.1f}" y1="{y}" x2="{px(dflt):.1f}" y2="{y}" '
                 f'stroke="{LINK}" stroke-width="2.5"/>')
        o.append(f'<circle cx="{px(dflt):.1f}" cy="{y}" r="6.5" fill="{ORANGE}" stroke="{BG}" stroke-width="2"/>')
        o.append(f'<circle cx="{px(qr):.1f}" cy="{y}" r="6.5" fill="{BLUE}" stroke="{BG}" stroke-width="2"/>')
        # Two labels need half a decade between them or they collide.
        if abs(math.log10(max(dflt, 1e-99)) - math.log10(max(qr, 1e-99))) >= 0.5:
            o.append(f'<text x="{px(dflt) + 16:.1f}" y="{y + 5}" font-size="13" fill="{MUTED}" '
                     f'font-family="{MONO}">{dflt:.2e}</text>')
            o.append(f'<text x="{px(qr) - 16:.1f}" y="{y + 5}" font-size="13" fill="{MUTED}" '
                     f'text-anchor="end" font-family="{MONO}">{qr:.2e}</text>')

    o.append(f'<text x="56" y="{height - 64}" font-size="15" fill="{MUTED}">{same} of the {len(rows)} land in '
             'exactly the same place under both solvers. Where they part, the normal equations are paying</text>')
    o.append(f'<text x="56" y="{height - 40}" font-size="15" fill="{MUTED}">for squaring the condition number: '
             '<tspan font-family="' + MONO + '">nearly-the-same.csv</tspan> is the file that exists to show it, '
             'five correct digits</text>')
    o.append(f'<text x="56" y="{height - 16}" font-size="15" fill="{MUTED}">where --qr gets eleven. '
             'Regenerate with: sh scripts/r-check.sh | python3 scripts/fig-lm.py</text>')
    o.append('</svg>')

    with open(out, "w") as fh:
        fh.write("\n".join(o) + "\n")
    print(f"fig-lm: {out}, {len(rows)} files")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "doc/img/lm-agreement.svg")
