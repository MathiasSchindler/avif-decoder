# Fuzz regression inputs

Place minimized `.avif` inputs that previously caused a crash, sanitizer finding, workspace-contract failure, or serial/parallel mismatch in this directory.

Use libFuzzer's minimized reproducer as the input and give it a descriptive name. `make fuzz-seeds` copies every `.avif` here into the active corpus, so `make fuzz-smoke` and `make fuzz-campaign` replay regressions before generating new mutations.

Do not remove a regression input unless the corresponding behavior is covered by an equally focused unit test and the corpus no longer adds useful parser coverage.
