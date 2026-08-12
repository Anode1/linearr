# fit-stream.R: R doing the job the way every other implementation in
# scripts/bench.sh does it, so the R rows measure an idiom rather than a
# language.
#
# bench/fit.R is R's idiom: read.csv builds a frame, lm() fits per group, and
# the memory column is the size of the frame. This one reads the file in fixed
# chunks through one connection, folds each chunk into a cross-product matrix
# per group, and drops it. Memory is then set by the chunk and the number of
# groups, not by the number of rows, exactly as the C is.
#
# CHUNK is rows per read: large enough that the per-call overhead does not
# dominate, small enough to stay a bounded buffer. 20,000 rows of 10 columns is
# a few MB.
#
# The normal equations are formed uncentered here because the benchmark's
# generated data is well conditioned and the point being measured is memory,
# not conditioning. On an ill-conditioned file this loses digits, which is what
# linearr's --qr exists for and what doc/NUMERICS.md is about.
CHUNK <- 20000

args <- commandArgs(trailingOnly = TRUE)
con  <- file(args[1], open = "r")
on.exit(close(con))

hdr <- scan(con, what = "", nlines = 1, sep = ",", quiet = TRUE)
p1  <- length(hdr) - 1          # terms plus the intercept
acc <- new.env(parent = emptyenv())
order <- character(0)

repeat {
    chunk <- tryCatch(
        read.csv(con, header = FALSE, nrows = CHUNK, col.names = hdr,
                 check.names = FALSE, comment.char = "#",
                 colClasses = c("character", rep("numeric", length(hdr) - 1))),
        error = function(e) NULL)
    if (is.null(chunk) || nrow(chunk) == 0) break

    for (g in unique(chunk[[1]])) {
        sub <- chunk[chunk[[1]] == g, , drop = FALSE]
        X   <- cbind(1, as.matrix(sub[, 3:ncol(sub), drop = FALSE]))
        y   <- sub[[2]]
        a   <- acc[[g]]
        if (is.null(a)) {
            a <- list(XtX = matrix(0, p1, p1), Xty = numeric(p1))
            order <- c(order, g)
        }
        a$XtX <- a$XtX + crossprod(X)
        a$Xty <- a$Xty + crossprod(X, y)
        acc[[g]] <- a
    }
    if (nrow(chunk) < CHUNK) break
    rm(chunk)
}

cat(paste0("group,intercept,", paste(hdr[3:length(hdr)], collapse = ",")), "\n", sep = "")
for (g in order) {
    a  <- acc[[g]]
    # A term the data cannot identify makes XtX singular; linearr pins it to 0
    # and says so, and this writes 0 for the same reason.
    co <- tryCatch(as.numeric(solve(a$XtX, a$Xty)),
                   error = function(e) rep(0, p1))
    cat(g, ",", paste(sprintf("%.12g", co), collapse = ","), "\n", sep = "")
}
