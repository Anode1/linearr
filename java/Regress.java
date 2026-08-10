/* Copyright (c) 2026 Vasili Gavrilov. BSD 2-Clause; see LICENSE. */
/* Regress.java: the Java twin of regress.c. Same algorithm, same order, same
 * names, so the two can be read side by side and matched line for line.
 *
 * Ordinary least squares for y = b0 + b1*x1 + ... + bp*xp. Observations are
 * ADDED and then forgotten: this holds cross-products, never rows, so ten
 * observations and ten million are fitted in the same memory.
 *
 * The three decisions are the C file's, for the same reasons:
 *
 * 1. CENTERED accumulation, not raw X'X. Sums of squares about zero are
 *    differences of large nearly-equal numbers, so a response with an offset
 *    destroys R^2 while leaving the coefficients correct.
 * 2. EQUILIBRATED rank test. X'X diagonals scale as the SQUARE of a column's
 *    units, so an absolute tolerance deletes a well-identified column for being
 *    measured in dollars rather than thousands.
 * 3. The intercept is NOT a column. It comes from the means, so it can never be
 *    pinned and a constant regressor is absorbed rather than fighting it. */
public final class Regress {

    /* Rank tolerance on the EQUILIBRATED matrix, whose diagonal is all ones.
     * A pure rank statement, carrying no units, which is the point. */
    static final double RANK_EPS = 1e-12;

    /* Why a term carries no coefficient. Different verdicts, different
     * consequences, so they are not both reported as an indistinguishable 0. */
    public static final int FITTED = 0;      /* estimated from the data        */
    public static final int CONSTANT = 1;    /* the column never varies        */
    public static final int COLLINEAR = 2;   /* inseparable from another column */

    public final int nvars;
    public long n;
    private final double[] mean;     /* nvars                    */
    private final double[] c;        /* nvars*nvars, row-major   */
    private final double[] cxy;      /* nvars                    */
    private double my, cyy;

    /* Allocated once with the fitter, not once per solve. Nothing in this class
     * calls new after construction. */
    private final double[] d;
    private final int[]    pivotCol;

    /** What the fit turned out to be. Mirrors struct regress_fit. */
    public static final class Fit {
        public int      pinned;
        public long     df;
        public double   r2;          /* -1 when undefined or not computable */
        public double   rss;         /* residual sum of squares             */
        /* Set when sse fell below what the subtraction can resolve, so sigma
         * is an upper bound rather than a value. The C prints `<` for it. */
        public boolean  sigmaIsBound;
        public double   sigma;       /* residual SD, sqrt(rss/df): how far a
                                        prediction typically lands from the
                                        truth, in the response's own units.
                                        R2 is a ratio and says nothing about
                                        that. -1 with no residual freedom.  */
        public double   condition;   /* largest/smallest accepted pivot     */
        public int[]    term;        /* FITTED / CONSTANT / COLLINEAR       */
    }

    public Regress(int nvars) {
        if (nvars < 1) throw new IllegalArgumentException("nvars");
        this.nvars = nvars;
        this.mean     = new double[nvars];
        this.c        = new double[nvars * nvars];
        this.cxy      = new double[nvars];
        this.d        = new double[nvars];
        this.pivotCol = new int[nvars];
    }

    /** Add one observation. Returns false if any value is not finite; one NaN
     *  admitted here reaches every coefficient. Allocates nothing. */
    public boolean add(double[] x, double y) {
        if (!isFinite(y)) return false;
        for (int i = 0; i < nvars; i++) if (!isFinite(x[i])) return false;

        n++;
        double dy = y - my;

        /* Online covariance: one deviation taken BEFORE its mean moves and one
         * AFTER, which is what makes the running form exact. */
        for (int i = 0; i < nvars; i++) {
            double dxi = x[i] - mean[i];
            int row = i * nvars;
            for (int j = 0; j < nvars; j++) {
                double dxjNew = x[j] - (mean[j] + (x[j] - mean[j]) / n);
                c[row + j] += dxi * dxjNew;
            }
            cxy[i] += dxi * (y - (my + dy / n));
        }
        for (int i = 0; i < nvars; i++) mean[i] += (x[i] - mean[i]) / n;
        my  += dy / n;
        cyy += dy * (y - my);
        return true;
    }

    /** Solve into beta[nvars+1]; beta[0] is the intercept. Fills fit if given.
     *  scratch must hold nvars*(nvars+1) doubles; the caller owns it, as in C. */
    public boolean solve(double[] beta, double[] scratch, Fit fit) {
        if (n == 0) return false;
        final int p = nvars, stride = p + 1;
        double pivmax = 0.0, pivmin = 0.0;
        int rank = 0;

        for (int i = 0; i <= p; i++) beta[i] = 0.0;
        if (fit != null) {
            if (fit.term == null || fit.term.length < p) fit.term = new int[p];
            for (int i = 0; i < p; i++) fit.term[i] = COLLINEAR;
        }

        /* Equilibrate to a unit diagonal, so the pivot test below is about rank
         * and not about units. A column with no variance gets scale 0 and is
         * pinned: correctly, it is collinear with the intercept. */
        for (int i = 0; i < p; i++) {
            double cii = c[i * p + i];
            d[i] = (cii > 0.0) ? Math.sqrt(cii) : 0.0;
        }
        for (int i = 0; i < p; i++) {
            for (int j = 0; j < p; j++)
                scratch[i * stride + j] =
                    (d[i] > 0.0 && d[j] > 0.0) ? c[i * p + j] / (d[i] * d[j]) : 0.0;
            scratch[i * stride + p] = (d[i] > 0.0) ? cxy[i] / d[i] : 0.0;
        }

        for (int col = 0; col < p && rank < p; col++) {
            int best = rank;
            for (int i = rank; i < p; i++)
                if (Math.abs(scratch[i * stride + col]) >
                    Math.abs(scratch[best * stride + col])) best = i;

            double piv = scratch[best * stride + col];
            if (d[col] == 0.0 || Math.abs(piv) <= RANK_EPS) {
                if (fit != null) fit.term[col] = (d[col] == 0.0) ? CONSTANT : COLLINEAR;
                continue;                       /* beta stays 0 */
            }
            if (best != rank)
                for (int j = col; j <= p; j++) {
                    double t = scratch[rank * stride + j];
                    scratch[rank * stride + j] = scratch[best * stride + j];
                    scratch[best * stride + j] = t;
                }
            piv = scratch[rank * stride + col];
            if (pivmax == 0.0 || Math.abs(piv) > pivmax) pivmax = Math.abs(piv);
            if (pivmin == 0.0 || Math.abs(piv) < pivmin) pivmin = Math.abs(piv);

            for (int j = col; j <= p; j++) scratch[rank * stride + j] /= piv;
            for (int i = 0; i < p; i++) {
                if (i == rank) continue;
                double f = scratch[i * stride + col];
                if (f == 0.0) continue;
                for (int j = col; j <= p; j++)
                    scratch[i * stride + j] -= f * scratch[rank * stride + j];
            }
            pivotCol[rank] = col;
            if (fit != null) fit.term[col] = FITTED;
            rank++;
        }

        for (int k = 0; k < rank; k++)
            beta[pivotCol[k] + 1] = scratch[k * stride + p] / d[pivotCol[k]];

        beta[0] = my;                            /* the intercept, from the means */
        for (int i = 0; i < p; i++) beta[0] -= beta[i + 1] * mean[i];

        for (int i = 0; i <= p; i++) if (!isFinite(beta[i])) return false;

        if (fit != null) {
            fit.pinned = p - rank;
            fit.sigmaIsBound = false;
            fit.df = n - rank - 1;
            fit.condition = (pivmin > 0.0) ? pivmax / pivmin : 1.0;
            fit.rss = -1.0;
            fit.sigma = -1.0;
            if (cyy > 0.0) {
                double sse = cyy;
                for (int i = 0; i < p; i++) sse -= beta[i + 1] * cxy[i];
                /* An exact fit leaves a few ulps, negative as often as positive.
                 * An SSE meaningfully below zero is arithmetic that has lost its
                 * meaning, and clamping it to zero manufactures a perfect score. */
                if (sse < -1e-9 * cyy) fit.r2 = -1.0;
                else {
                    if (sse < 0.0) sse = 0.0;
                    fit.r2 = 1.0 - sse / cyy;
                    if (fit.r2 < 0.0) fit.r2 = 0.0;
                    /* The floor under sse. Each y is held to |my|*eps, so the
                     * centred deviations carry that error and Cyy accumulates
                     * n of them: the subtraction cannot resolve below about
                     * |my|*eps*sqrt(n*Cyy). Below it, report the floor as an
                     * upper bound rather than a value that may be anything.
                     * regress.c carries the measurement. */
                    double floor = Math.abs(my) * 2.220446049250313e-16
                                 * Math.sqrt((double) n * cyy);
                    if (sse < floor) { sse = floor; fit.sigmaIsBound = true; }
                    fit.rss = sse;
                    if (fit.df > 0) fit.sigma = Math.sqrt(sse / (double) fit.df);
                }
            } else {
                fit.r2 = -1.0;                   /* the response never varies */
            }
        }
        return true;
    }

    private static boolean isFinite(double v) {
        return !Double.isNaN(v) && !Double.isInfinite(v);
    }
}
