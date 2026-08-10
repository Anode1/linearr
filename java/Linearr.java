/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* Linearr.java: the Java twin of main.c + process.c: read a training CSV, fit
 * one line per group in a single pass, write the coefficient table.
 *
 * Written in the style of the author's 2011 code, which is the style this
 * project's C came from: BufferedReader and a readLine() loop, Hashtable for
 * lookup, Vector for order, StringBuffer for output, no generics, and one
 * reused record rather than a new one per row. Reusing the record was an
 * ordinary optimisation when this was written in the late nineties; it is
 * unfashionable now and it is still the reason the memory column below is flat.
 *
 * The arithmetic is Regress.java, which is regress.c line for line. The two
 * implementations are meant to be read side by side.
 *
 *   javac *.java && java Linearr train.csv > model.csv
 */
import java.io.BufferedReader;
import java.io.FileReader;
import java.math.BigDecimal;
import java.util.Enumeration;
import java.util.Hashtable;
import java.util.Vector;

public class Linearr {

    static final char SEPARATOR = ',';

    /** Significant digits in a published coefficient. See the C's los.c. */
    static final java.math.MathContext COEF_DIGITS = new java.math.MathContext(12);

    /** The C's printf("%.4g"), which Java's %.4g is not: C removes trailing
     *  zeros and a trailing point, Java keeps them, so 0.5 prints as 0.5000 and
     *  the two summary lines stop matching. Four significant digits, because
     *  a residual SD quoted to more than that is quoting the noise. */
    static String g4(double v) { return g(v, 4); }
    static String g3(double v) { return g(v, 3); }

    static String g(double v, int digits) {
        if (v != v || v == Double.POSITIVE_INFINITY || v == Double.NEGATIVE_INFINITY)
            return String.valueOf(v);
        String s = String.format("%." + digits + "g", Double.valueOf(v));
        int e = s.indexOf('e');
        String mant = (e < 0) ? s : s.substring(0, e);
        String rest = (e < 0) ? "" : s.substring(e);
        if (mant.indexOf('.') >= 0) {
            int end = mant.length();
            while (end > 0 && mant.charAt(end - 1) == '0') end--;
            if (end > 0 && mant.charAt(end - 1) == '.') end--;
            mant = mant.substring(0, end);
        }
        return mant + rest;
    }

    /** A field's name for a refusal message: the group, the response, or a
     *  term. The C's field_label(). */
    static String fieldLabel(int idx, String response, String[] names) {
        if (idx == 0) return "the group column";
        if (idx == 1) return (response != null && response.length() > 0)
                             ? response : "the value column";
        int t = idx - 2;
        return (t >= 0 && t < names.length) ? names[t] : "a column";
    }

    /** Enough of a field to recognise it, elided. The C's show_field(). */
    static String showField(String s) {
        if (s.length() == 0)  return "empty";
        if (s.length() <= 24) return "'" + s + "'";
        return "'" + s.substring(0, 24) + "...'";
    }

    /** Refuse the file, in the C's words and with the C's status. This used to
     *  be a `continue`: a row the C rejects was SKIPPED here, silently, and the
     *  model came out fitted on whatever was left. A subset with no indication
     *  is worse than a refusal, and the two implementations disagreeing about
     *  which rows count is worse still. */
    static void refuse(String file, long row, String why) {
        System.err.println("cannot fit: " + file + " row " + row + ": " + why);
        System.exit(1);
    }

    /** The C's format_pinned(), same wording and same order: constants first,
     *  then collinears. Returns null when every term was estimated.
     *
     *  It was missing here, so the Java printed a coefficient of 0 with nothing
     *  saying whether it was measured as 0 or pinned there because the column
     *  could not be separated from another. Those are different facts and the
     *  C says which. */
    static String pinnedNote(String group, Regress.Fit fit, String[] names, int p) {
        boolean any = false;
        for (int i = 0; i < p; i++) if (fit.term[i] != Regress.FITTED) any = true;
        if (!any) return null;

        StringBuffer sb = new StringBuffer("# pinned ");
        sb.append(group);
        sb.append(':');
        for (int kind = Regress.CONSTANT; kind <= Regress.COLLINEAR; kind++) {
            boolean first = true;
            for (int i = 0; i < p; i++) {
                if (fit.term[i] != kind) continue;
                if (first) sb.append(kind == Regress.CONSTANT ? " constant " : " collinear ");
                else       sb.append(',');
                sb.append(names[i]);
                first = false;
            }
        }
        return sb.toString();
    }

    /** One group's fit. The C calls this struct group_fit. */
    static class Group {
        String  name;
        Regress r;
        Group(String name, int nvars) { this.name = name; this.r = new Regress(nvars); }
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: java Linearr TRAIN.CSV");
            System.exit(2);
        }

        BufferedReader reader = new BufferedReader(new FileReader(args[0]));
        Hashtable index = new Hashtable();      /* group name -> Group */
        Vector    order = new Vector();         /* first-seen order    */
        String[]  names = null;
        String    response = null;              /* the header's second field */
        double[]  x     = null;                 /* THE reused record   */
        int       p     = 0;
        long      rows  = 0, seen = 0;
        String    line;

        while ((line = reader.readLine()) != null) {
            if (line.length() == 0 || line.charAt(0) == '#') continue;

            if (names == null) {                /* the header names the terms */
                Vector hdr = new Vector();
                int from = 0;
                while (from <= line.length()) {
                    int to = Csv.endOfField(line, from);
                    hdr.addElement(line.substring(from, to).trim());
                    from = to + 1;
                }
                p = hdr.size() - 2;
                if (p < 1) { System.err.println("no terms in the header"); System.exit(1); }
                response = (String) hdr.elementAt(1);
                names = new String[p];
                for (int j = 0; j < p; j++) names[j] = (String) hdr.elementAt(j + 2);
                x = new double[p];              /* allocated once, for the run */
                continue;
            }
            seen++;

            /* Count the fields first, so a wrong count is reported as a wrong
             * count rather than met halfway through, and so the message can
             * name the number found. Two walks of a line, no allocation. */
            int nf = 0;
            for (int at = 0; at <= line.length(); ) {
                nf++;
                at = Csv.endOfField(line, at) + 1;
            }
            if (nf != p + 2) {
                if (nf == 1 && (line.indexOf(';') >= 0 || line.indexOf('\t') >= 0))
                    refuse(args[0], seen, "it has no commas, but does have "
                        + (line.indexOf(';') >= 0 ? "semicolons" : "tabs")
                        + ". The header is comma-separated and this row is not; "
                        + "this program reads commas only");
                else if (line.indexOf('"') >= 0 || line.indexOf('\'') >= 0)
                    refuse(args[0], seen, "it has " + nf + " field"
                        + (nf == 1 ? "" : "s") + " and the header names " + (p + 2)
                        + ", and the row contains a quote. A quoted field holding "
                        + "a comma splits in two here, and one holding a line "
                        + "break runs onto the next line: this reads plain "
                        + "comma-separated fields, with no quoting");
                else
                    refuse(args[0], seen, "it has " + nf + " field"
                        + (nf == 1 ? "" : "s") + " and the header names " + (p + 2)
                        + ": a group, the value, and " + p + " term"
                        + (p == 1 ? "" : "s"));
            }

            /* Walk the line in place: no split(), no substring per field, no
             * per-row object. This is the whole difference between a Java that
             * streams and a Java whose heap tracks the file. */
            int from = 0, field = 0;
            String group = null;
            double y = 0.0;

            while (from <= line.length() && field < p + 2) {
                int to = Csv.endOfField(line, from);
                int a = from, b = to;
                while (a < b && line.charAt(a) == ' ') a++;
                while (b > a && line.charAt(b - 1) == ' ') b--;
                if (a < b && (line.charAt(a) == '"' || line.charAt(a) == '\''))
                    refuse(args[0], seen, fieldLabel(field, response, names)
                        + " is quoted (" + showField(line.substring(a, b))
                        + "). This reads plain comma-separated fields: quotes "
                        + "are not stripped, so a quoted name would become a "
                        + "different name and a quoted number would not be a "
                        + "number. Export without quoting");
                if (field == 0) {
                    group = line.substring(a, b);
                    if (group.length() == 0)
                        refuse(args[0], seen, "the group column is empty, which "
                            + "is empty or longer than the 32 characters a group "
                            + "name may have");
                } else {
                    double v = Csv.parse(line, from, to);
                    if (v != v)
                        refuse(args[0], seen,
                            (field == 1 ? "" : "term ")
                            + fieldLabel(field, response, names) + " is "
                            + showField(line.substring(a, b)) + ", which is not "
                            + "a number. This fits numbers only: there is no "
                            + "imputation for an empty field and no encoding "
                            + "for a category name");
                    if (field == 1) y = v; else x[field - 2] = v;
                }
                from = to + 1;
                field++;
            }

            Group g = (Group) index.get(group);
            if (g == null) {
                g = new Group(group, p);        /* once per GROUP, not per row */
                index.put(group, g);
                order.addElement(g);
            }
            if (!g.r.add(x, y)) continue;
            rows++;
        }
        reader.close();

        /* What the file predicts, from the header's second field. A table of
         * coefficients that does not name its response cannot be identified a
         * week later: group,intercept,km,stops says nothing about minutes. */
        if (response != null && response.length() > 0)
            System.out.println("# response: " + response);

        StringBuffer sb = new StringBuffer("group,intercept");
        for (int j = 0; j < p; j++) { sb.append(SEPARATOR); sb.append(names[j]); }
        System.out.println(sb.toString());

        double[] beta    = new double[p + 1];
        double[] scratch = new double[p * (p + 1)];
        Regress.Fit fit  = new Regress.Fit();
        int pinned = 0;
        long minDf = Long.MAX_VALUE;
        /* The worst residual SD over the groups. The C reports it and this did
         * not, so the one number in the summary that says how far a prediction
         * typically lands from the truth was missing from the Java. */
        double worstSigma = -1.0;
        /* And the worst conditioning. The C prints it, and warns above 1e8,
         * because an ill-conditioned design fits its own sample beautifully and
         * predicts nothing: R2 cannot see that failure. This file reported
         * neither, so the Java's summary looked healthier than the C's on the
         * same data. Its solver is the normal equations, as regress.c is, so
         * the parenthesis says so for the same reason the C's does. */
        double worstCond = 1.0;

        for (Enumeration e = order.elements(); e.hasMoreElements(); ) {
            Group g = (Group) e.nextElement();
            if (!g.r.solve(beta, scratch, fit)) {
                System.err.println("cannot fit group " + g.name);
                System.exit(1);
            }
            sb.setLength(0);
            sb.append(g.name);
            for (int i = 0; i <= p; i++) {
                sb.append(SEPARATOR);
                /* BigDecimal for the printed number, as the 2011 code used
                 * it: a published coefficient is a decimal, not a float that
                 * someone read off a double. Twelve significant digits, which
                 * matches the C and is well below the residual SD of any fit
                 * that produced it; without the rounding this prints 5 as
                 * 4.999999999999999, which is the binary representation showing
                 * through rather than a measurement. It is out here, once per
                 * group, never in the row loop where it would allocate per
                 * value. */
                sb.append(new BigDecimal(beta[i], COEF_DIGITS)
                              .stripTrailingZeros().toPlainString());
            }
            System.out.println(sb.toString());
            String note = pinnedNote(g.name, fit, names, p);
            if (note != null) System.out.println(note);
            pinned += fit.pinned;
            if (fit.df < minDf) minDf = fit.df;
            if (fit.sigma > worstSigma) worstSigma = fit.sigma;
            if (fit.condition > worstCond) worstCond = fit.condition;
        }

        /* What the file was read AS. The layout is fixed and nothing in the
         * data can say which column is the response, so a file written in
         * another order fits perfectly well and answers a different question.
         * The C says which column it took; so does this. */
        long leastDf = (minDf == Long.MAX_VALUE) ? 0 : minDf;
        if (response != null && response.length() > 0)
            System.err.println("reading: column 1 is the group, '" + response
                + "' is the value being predicted, and the other " + p + " are terms");
        System.err.println("fit: " + order.size() + " group" + (order.size() == 1 ? "" : "s")
                + ", " + rows + " row" + (rows == 1 ? "" : "s")
                + (pinned > 0 ? ", " + pinned + " term-slot" + (pinned == 1 ? "" : "s")
                                + " pinned to 0" : "")
                + ", least df=" + leastDf
                + (worstSigma >= 0.0 ? ", worst resid SD=" + g4(worstSigma) : "")
                + (worstCond > 1.0 ? ", worst cond=" + g3(worstCond)
                                     + " (normal equations)" : ""));

        /* The same two warnings the C prints, in the same order and the same
         * words. The residual checks are not here: diag.c has no twin in this
         * directory, and saying so is better than a summary that is quietly
         * less careful than the one it is meant to mirror. */
        if (leastDf <= 0)
            System.err.println("warning: at least one group has no residual degrees of "
                + "freedom; its line passes through every row by construction. "
                + "Fit those groups on more rows.");
        if (worstCond > 1e8)
            System.err.println("warning: at least one group is ill-conditioned (cond="
                + g3(worstCond) + "); the trailing digits of its coefficients are "
                + "noise. Try --qr, which does not square the condition number.");
    }
}
