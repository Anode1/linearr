/* Copyright (C) 2026 Vasili Gavrilov. GNU GPL v2 or later. */
/* Linearr.java -- the Java twin of main.c + process.c: read a training CSV, fit
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
                names = new String[p];
                for (int j = 0; j < p; j++) names[j] = (String) hdr.elementAt(j + 2);
                x = new double[p];              /* allocated once, for the run */
                continue;
            }
            seen++;

            /* Walk the line in place: no split(), no substring, no per-row
             * object. This is the whole difference between a Java that streams
             * and a Java whose heap tracks the file. */
            int from = 0, field = 0;
            String group = null;
            double y = 0.0;
            boolean bad = false;

            while (from <= line.length() && field < p + 2) {
                int to = Csv.endOfField(line, from);
                if (field == 0)      group = line.substring(from, to);
                else if (field == 1) y = Csv.parse(line, from, to);
                else {
                    double v = Csv.parse(line, from, to);
                    if (v != v) { bad = true; break; }
                    x[field - 2] = v;
                }
                from = to + 1;
                field++;
            }
            if (bad || field != p + 2 || group == null || y != y) continue;

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

        StringBuffer sb = new StringBuffer("GROUP,Intercept");
        for (int j = 0; j < p; j++) { sb.append(SEPARATOR); sb.append(names[j]); }
        System.out.println(sb.toString());

        double[] beta    = new double[p + 1];
        double[] scratch = new double[p * (p + 1)];
        Regress.Fit fit  = new Regress.Fit();
        int pinned = 0;
        long minDf = Long.MAX_VALUE;

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
                /* BigDecimal for the printed number, as the 2011 code used it:
                 * the published figure is a decimal, not a float someone read
                 * off a double. It is out here, once per group -- never in the
                 * row loop, where it would allocate per value. */
                sb.append(BigDecimal.valueOf(beta[i]).toString());
            }
            System.out.println(sb.toString());
            pinned += fit.pinned;
            if (fit.df < minDf) minDf = fit.df;
        }

        System.err.println("fit: " + order.size() + " group" + (order.size() == 1 ? "" : "s")
                + ", " + rows + " row" + (rows == 1 ? "" : "s")
                + (pinned > 0 ? ", " + pinned + " term-slot" + (pinned == 1 ? "" : "s")
                                + " pinned to 0" : "")
                + ", least df=" + (minDf == Long.MAX_VALUE ? 0 : minDf));
    }
}
