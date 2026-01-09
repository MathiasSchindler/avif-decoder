CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2

BUILD_DIR := build

.PHONY: all build-m0 build-m1 build-m2 build-m3a build-m3b clean

.PHONY: build-tests test-generated test test-symbol test-avifdec-info test-m3b-tile-trailing test-m3b-tile-exit1bool test-m3b-tile-exit8bool test-m3b-tile-exit8bool-2x2 sweep-corpus-m2 sweep-corpus-m3a sweep-corpus-m3b sweep-corpus-m3b-trailingbits sweep-corpus-m3b-trailingbits-strict sweep-corpus-m3b-exitprobe sweep-corpus-m3b-exitprobe-strict bench

M3B_CONSUME_BOOLS ?= 0

all: build-m0 build-m1 build-m2 build-m3a build-m3b

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

build-m0: $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/avif_boxdump src/m0-container-parser/avif_boxdump.c

build-m1: $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/avif_metadump src/m1-meta-parser/avif_metadump.c

build-m2: $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/avif_extract_av1 src/m2-av1-extract/avif_extract_av1.c

build-m3a: $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/av1_parse src/m3a-av1-parse/av1_parse.c

build-m3b: $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/av1_framehdr src/m3b-av1-decode/av1_framehdr.c src/m3b-av1-decode/av1_symbol.c src/m3b-av1-decode/av1_decode_tile.c

build-tests: $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/verify_generated tests/verify_generated.c
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/sweep_corpus tests/sweep_corpus.c
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/bench tests/bench.c
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/test_symbol tests/test_symbol.c src/m3b-av1-decode/av1_symbol.c
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/test_avifdec_info tests/test_avifdec_info.c


test-m3b-tile-trailing: build-m3b
	@set -e; \
	found=0; \
	for f in testFiles/generated/av1/*trailingonly*.av1; do \
		[ -e "$$f" ] || continue; \
		found=1; \
		./$(BUILD_DIR)/av1_framehdr --check-tile-trailing-strict "$$f" > /dev/null; \
	done; \
	[ "$$found" -eq 1 ]

test-m3b-tile-exit1bool: build-m3b
	@set -e; \
	f=testFiles/generated/av1/m3b_tilegroup_1tile_exit1bool.av1; \
	[ -e "$$f" ]; \
	./$(BUILD_DIR)/av1_framehdr --check-tile-trailing-strict --tile-consume-bools 1 "$$f" > /dev/null

test-m3b-tile-exit8bool: build-m3b
	@set -e; \
	f=testFiles/generated/av1/m3b_tilegroup_1tile_exit8bool.av1; \
	[ -e "$$f" ]; \
	./$(BUILD_DIR)/av1_framehdr --check-tile-trailing-strict --tile-consume-bools 8 "$$f" > /dev/null

test-m3b-tile-exit8bool-2x2: build-m3b
	@set -e; \
	f=testFiles/generated/av1/m3b_tilegroup_2x2_alltiles_flag0_exit8bool.av1; \
	[ -e "$$f" ]; \
	./$(BUILD_DIR)/av1_framehdr --check-tile-trailing-strict --tile-consume-bools 8 "$$f" > /dev/null

test-generated: all build-tests test-m3b-tile-trailing test-m3b-tile-exit1bool test-m3b-tile-exit8bool test-m3b-tile-exit8bool-2x2
	./$(BUILD_DIR)/verify_generated

test: test-generated

test-symbol: build-tests
	./$(BUILD_DIR)/test_symbol

test-avifdec-info: all build-tests
	@set -e; \
	if command -v avifdec > /dev/null; then \
		./$(BUILD_DIR)/test_avifdec_info; \
	else \
		echo "SKIP: avifdec not found on PATH"; \
	fi

sweep-corpus-m2: all build-tests
	./$(BUILD_DIR)/sweep_corpus --stage m2

sweep-corpus-m3a: all build-tests
	./$(BUILD_DIR)/sweep_corpus --stage m3a

sweep-corpus-m3b: all build-tests
	./$(BUILD_DIR)/sweep_corpus --stage m3b

sweep-corpus-m3b-trailingbits: all build-tests
	./$(BUILD_DIR)/sweep_corpus --stage m3b --m3b-check-trailingbits

sweep-corpus-m3b-trailingbits-strict: all build-tests
	./$(BUILD_DIR)/sweep_corpus --stage m3b --m3b-check-trailingbits-strict

sweep-corpus-m3b-exitprobe: all build-tests
	./$(BUILD_DIR)/sweep_corpus --stage m3b --m3b-exit-probe --m3b-consume-bools $(M3B_CONSUME_BOOLS)

sweep-corpus-m3b-exitprobe-strict: all build-tests
	./$(BUILD_DIR)/sweep_corpus --stage m3b --m3b-exit-probe-strict --m3b-consume-bools $(M3B_CONSUME_BOOLS)

bench: all build-tests
	./$(BUILD_DIR)/bench

clean:
	rm -rf $(BUILD_DIR)
