# fit.R: the R baseline for scripts/bench.sh, and the one exception in it.
#
# Every other implementation streams, because its language lets it. This one
# does what R does: read.csv reads the whole file into memory as a data frame, then
# lm() fits per group. That is not a strawman, it is the idiom, and it is why
# R's memory column is large while the others are flat.
#
# The number below is therefore about R's DATA FRAME, not about R the language:
# a streaming R using readLines and a manual accumulator would look like the
# others. Read the memory figure as the cost of the idiom people actually write.
args <- commandArgs(trailingOnly = TRUE)
d <- read.csv(args[1], comment.char = "#", check.names = FALSE)
terms <- names(d)[3:ncol(d)]
resp  <- names(d)[2]
grp   <- names(d)[1]

cat(paste0("group,intercept,", paste(terms, collapse = ",")), "\n", sep = "")
for (g in unique(d[[grp]])) {
  sub <- d[d[[grp]] == g, , drop = FALSE]
  f   <- as.formula(paste0("`", resp, "` ~ ", paste0("`", terms, "`", collapse = " + ")))
  co  <- coef(lm(f, data = sub))
  co[is.na(co)] <- 0          # lm() reports an aliased term as NA; we write 0
  cat(g, ",", paste(sprintf("%.12g", co), collapse = ","), "\n", sep = "")
}
