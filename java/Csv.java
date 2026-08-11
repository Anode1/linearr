/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* Csv.java: the Java twin of csv.c, in the style of the author's 2011 code:
 * no generics, no autoboxing, StringBuffer, and as little `new` in the row loop
 * as the language allows.
 *
 * Splitting a line with String.split() allocates a String[] and one String per
 * field, every row. Over half a million rows that is five million short-lived
 * objects, and the JVM's heap grows to hold them, which then gets read as
 * "Java needs 400 MB for this job". It does not. This class walks the line and
 * parses each number in place, so the row loop allocates nothing at all.
 *
 * The fast path handles what a numeric table contains: an optional sign, digits,
 * an optional decimal point, more digits. Anything else (an exponent, a very
 * long mantissa, a NaN) falls back to substring() and Double.parseDouble, so
 * correctness never depends on the shortcut. */
public final class Csv {

    /** Longest mantissa a long holds exactly. Past this, use the slow path. */
    private static final int SAFE_DIGITS = 17;

    private static final double[] POW10 = new double[23];
    static {
        POW10[0] = 1.0;
        for (int i = 1; i < POW10.length; i++) POW10[i] = POW10[i - 1] * 10.0;
    }

    /** Index just past the field starting at `from`, or line.length(). */
    public static int endOfField(String line, int from) {
        int i = line.indexOf(',', from);
        return (i < 0) ? line.length() : i;
    }

    /**
     * Parse line[from,to) as a double without allocating, where it can.
     * Returns Double.NaN if the field is empty, which the caller treats as an
     * error: a NaN must never reach a coefficient.
     */
    public static double parse(String line, int from, int to) {
        while (from < to && line.charAt(from) == ' ') from++;
        while (to > from && line.charAt(to - 1) == ' ') to--;
        if (from >= to) return Double.NaN;

        int  i = from;
        boolean neg = false;
        char ch = line.charAt(i);
        if (ch == '-') { neg = true; i++; }
        else if (ch == '+') { i++; }

        long mant = 0L;
        int  digits = 0, scale = 0;
        boolean seenDot = false, ok = (i < to);

        for (; i < to; i++) {
            ch = line.charAt(i);
            if (ch >= '0' && ch <= '9') {
                if (digits < SAFE_DIGITS) {
                    mant = mant * 10L + (ch - '0');
                    digits++;
                    if (seenDot) scale++;
                } else { ok = false; break; }    /* too long: slow path */
            } else if (ch == '.' && !seenDot) {
                seenDot = true;
            } else {
                ok = false; break;               /* exponent, nan, inf, junk */
            }
        }
        if (ok && digits > 0 && scale < POW10.length) {
            double v = (double) mant / POW10[scale];
            return neg ? -v : v;
        }
        /* Hexadecimal is refused, as los.c refuses it: Double.parseDouble
         * accepts the 0x1p4 form, and a CSV field written that way is a
         * mis-export or an identifier in a numeric column, not the number 16.
         * The two implementations are diffed against each other by
         * scripts/java-check.sh, so a difference here is a failing gate. */
        {   int p = from;
            if (p < to && (line.charAt(p) == '-' || line.charAt(p) == '+')) p++;
            if (p + 1 < to && line.charAt(p) == '0'
                && (line.charAt(p + 1) == 'x' || line.charAt(p + 1) == 'X'))
                return Double.NaN;
        }
        try {
            return Double.parseDouble(line.substring(from, to));
        } catch (NumberFormatException e) {
            return Double.NaN;
        }
    }
}
