# Encoder fuzz regression inputs

Place minimized libFuzzer inputs for `tests/encoder_fuzz.c` in this directory
with a `.seed` suffix. `make encoder-fuzz-seeds` copies them into the working
corpus together with deterministic baseline seeds.