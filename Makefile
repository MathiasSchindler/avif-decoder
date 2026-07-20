CC ?= cc
BENCHMARK_ITERATIONS ?= 1

MACHO_DYLIB_REMOVER := build/host/macho-dylib-remover
LINK_TOOLS :=
POST_LINK :=
TARGET_CFLAGS :=
OS := $(shell uname -s)
ARCH := $(shell uname -m)
BUILD_DIR := build/$(ARCH)
TARGET := $(BUILD_DIR)/avifdec
ENCODER_TARGET := $(BUILD_DIR)/avifenc
STRICT_UNIT := $(BUILD_DIR)/unit
HOST_UNIT := build/host/unit
ENCODER_STRICT_UNIT := $(BUILD_DIR)/encoder-unit
ENCODER_HOST_UNIT := build/host/encoder-unit
ENCODER_PARALLEL_UNIT := build/host/encoder-parallel-unit
IMAGE_INPUT_STRICT_UNIT := $(BUILD_DIR)/image-input-unit
IMAGE_INPUT_HOST_UNIT := build/host/image-input-unit
ENCODER_BENCHMARK := build/host/encoder-benchmark
ENCODER_SCORECARD_BASELINE := tests/encoder-scorecard-baseline.jsonl
OBU_TRACE := $(BUILD_DIR)/obu-trace
THREAD_UNIT := $(BUILD_DIR)/thread-unit
FUZZ_BUILD_DIR := build/fuzz
FUZZ_TARGET := $(FUZZ_BUILD_DIR)/avifdec_fuzzer
FUZZ_CORPUS := $(FUZZ_BUILD_DIR)/corpus
ENCODER_FUZZ_TARGET := $(FUZZ_BUILD_DIR)/avifenc_fuzzer
ENCODER_FUZZ_CORPUS := $(FUZZ_BUILD_DIR)/encoder-corpus
WASM_BUILD_DIR := build/wasm
WASM_LOADER := $(WASM_BUILD_DIR)/avif-decoder.js
WASM_BINARY := $(WASM_BUILD_DIR)/avif-decoder.wasm
WASM_ASSETS := index.html app.js decoder-worker.js styles.css
.DEFAULT_GOAL := $(TARGET)
COEFF_TABLES := src/codec/av1_coeff_tables.inc
COEFF_TABLES_CHECK := build/generated-check/av1_coeff_tables.inc
PALETTE_TABLES := src/codec/av1_palette_tables.inc
PALETTE_TABLES_CHECK := build/generated-check/av1_palette_tables.inc
QUANT_TABLES := src/codec/av1_quant_tables.inc
QUANT_TABLES_CHECK := build/generated-check/av1_quant_tables.inc
WARP_TABLES := src/decoder/av1_warp_tables.inc
WARP_TABLES_CHECK := build/generated-check/av1_warp_tables.inc
FILM_GRAIN_TABLE := src/decoder/av1_film_grain_gaussian.inc
FILM_GRAIN_TABLE_CHECK := build/generated-check/av1_film_grain_gaussian.inc
GENERATED_CHECK := build/generated-check/.verified
AV1_SPEC := docs/av1.html
TEST_GENERATED_CHECK :=
ifneq ($(wildcard $(AV1_SPEC)),)
TEST_GENERATED_CHECK := $(GENERATED_CHECK)
endif

ifeq ($(OS),Linux)
ifeq ($(ARCH),x86_64)
ARCH_DIR := src/arch/x86_64/linux
ARCH_SOURCES := $(ARCH_DIR)/crt0.S $(ARCH_DIR)/syscall_stubs.S
PLATFORM_DIR := src/platform/linux
LDFLAGS := \
	-nostdlib -static-pie \
	-Wl,--gc-sections -Wl,--build-id=none -Wl,-e,_start
else
$(error Unsupported Linux architecture '$(ARCH)'; currently only x86_64 is supported)
endif
else ifeq ($(OS),Darwin)
ifeq ($(ARCH),arm64)
ARCH_DIR := src/arch/aarch64/macos
ARCH_SOURCES := $(ARCH_DIR)/crt0.S $(ARCH_DIR)/av1_dsp.S
PLATFORM_DIR := src/platform/macos
MACOS_SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)
LINK_TOOLS := $(MACHO_DYLIB_REMOVER)
POST_LINK = $(MACHO_DYLIB_REMOVER) $@ && codesign --force --sign - $@
TARGET_CFLAGS := -target arm64-apple-macos11 -isysroot $(MACOS_SDKROOT) \
	-DAVIFDEC_AARCH64_NEON=1
LDFLAGS := \
	$(TARGET_CFLAGS) -nostdlib -lSystem -Wl,-no_fixup_chains \
	-Wl,-platform_version,macos,11.0,11.0 \
	-Wl,-e,_start -Wl,-dead_strip \
	-Wl,-no_function_starts -Wl,-adhoc_codesign
else
$(error Unsupported macOS architecture '$(ARCH)'; currently only arm64 is supported)
endif
else
$(error Unsupported operating system '$(OS)')
endif

CFLAGS := \
	$(TARGET_CFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
	-ffreestanding -fno-builtin -fno-stack-protector \
	-fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -fPIE -MMD -MP -nostdinc \
	-DNEWOS_DISABLE_STACK_GUARD_INIT \
	-Isrc -Isrc/shared -Isrc/decoder -Isrc/codec \
	-I$(PLATFORM_DIR) -I$(ARCH_DIR)
HOST_TEST_CFLAGS := \
	-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-Isrc -Isrc/shared -Isrc/decoder -Isrc/codec
FUZZ_CFLAGS := \
	-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g \
	-fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined,integer \
	-fno-sanitize=unsigned-integer-overflow,implicit-integer-sign-change \
	-fno-sanitize=implicit-integer-truncation,unsigned-shift-base \
	-Isrc -Isrc/shared -Isrc/decoder -Isrc/codec
CODEC_C_SOURCES := \
	src/codec/av1_cdf.c src/codec/av1_symbol.c src/codec/av1_coeff.c \
	src/codec/av1_dsp.c src/codec/av1_intra.c src/codec/av1_predict.c \
	src/codec/av1_recon.c src/codec/av1_tile_cdf.c
DECODER_C_SOURCES := \
	src/decoder/bmff.c src/decoder/avif.c src/decoder/avif_parse.c \
	src/decoder/avif_properties_internal.c src/decoder/avif_sequence.c \
	src/decoder/avif_rgb.c src/decoder/avif_sato.c src/decoder/png.c \
	src/decoder/av1.c src/decoder/av1_bitstream.c \
	src/decoder/av1_frame_header.c \
	src/decoder/av1_copy.c src/decoder/av1_metadata.c \
	src/decoder/av1_parse.c src/decoder/av1_profile.c \
	src/decoder/av1_reference.c src/decoder/av1_film_grain.c \
	src/decoder/av1_inter.c src/decoder/av1_inter_predict.c \
	src/decoder/av1_warp.c src/decoder/av1_partition.c \
	src/decoder/av1_tile.c src/decoder/av1_tile_mode.c \
	src/decoder/av1_tile_inter_mode.c \
	src/decoder/av1_tile_inter_mv.c src/decoder/av1_tile_restoration.c \
	src/decoder/av1_tile_palette.c src/decoder/av1_block.c \
	src/decoder/av1_filter.c src/decoder/av1_cdef.c \
	src/decoder/av1_superres.c src/decoder/av1_restoration_filter.c
CORE_C_SOURCES := src/base.c $(CODEC_C_SOURCES) $(DECODER_C_SOURCES)
ENCODER_MODULE_C_SOURCES := src/encoder/avifenc.c src/encoder/write.c \
	src/encoder/avif_write.c src/encoder/av1_write.c \
	src/encoder/av1_symbol_write.c src/encoder/av1_tile_write.c \
	src/encoder/av1_tile_intra.c src/encoder/av1_tile_palette.c \
	src/encoder/av1_tile_partition.c src/encoder/av1_transform_forward.c \
	src/encoder/av1_transform_write.c
IMAGE_INPUT_C_SOURCES := src/encoder/cli/image_input.c \
	src/encoder/cli/image_input_png.c src/encoder/cli/image_input_jpeg.c
ENCODER_CORE_C_SOURCES := $(ENCODER_MODULE_C_SOURCES) src/base.c \
	src/codec/av1_cdf.c src/codec/av1_symbol.c src/codec/av1_coeff.c \
	src/codec/av1_intra.c src/codec/av1_predict.c src/codec/av1_dsp.c \
	src/codec/av1_recon.c src/codec/av1_tile_cdf.c
WASM_C_SOURCES := $(CORE_C_SOURCES) $(ENCODER_MODULE_C_SOURCES) \
	$(IMAGE_INPUT_C_SOURCES)
ENCODER_TEST_C_SOURCES := $(ENCODER_MODULE_C_SOURCES) $(CORE_C_SOURCES)
FREESTANDING_HEADERS := $(wildcard src/shared/*.h)
CORE_HEADERS := src/avifdec.h src/base.h \
	$(wildcard src/codec/*.h src/codec/*.inc \
	src/decoder/*.h src/decoder/*.inc)
ENCODER_HEADERS := $(wildcard src/encoder/*.h src/encoder/cli/*.h)
IMAGE_INPUT_HEADERS := $(wildcard src/encoder/cli/image_input*.h) src/base.h
PLATFORM_HEADERS := src/platform/platform.h src/task_pool.h \
	$(wildcard $(PLATFORM_DIR)/*.h $(ARCH_DIR)/*.h)
ENCODER_TEST_HEADERS := $(CORE_HEADERS) $(ENCODER_HEADERS) \
	$(FREESTANDING_HEADERS)
ENCODER_C_SOURCES := src/encoder/main.c $(ENCODER_CORE_C_SOURCES) \
	$(IMAGE_INPUT_C_SOURCES) src/task_pool.c $(PLATFORM_DIR)/thread.c \
	$(PLATFORM_DIR)/io.c
RUNTIME_C_SOURCES := src/task_pool.c $(PLATFORM_DIR)/thread.c
C_SOURCES := src/decoder/main.c src/decoder/png_write.c \
	$(CORE_C_SOURCES) $(RUNTIME_C_SOURCES) $(PLATFORM_DIR)/io.c
SOURCES := $(C_SOURCES) $(ARCH_SOURCES)
OBJECTS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.o))
OBJECTS := $(OBJECTS:.S=.o)
CORE_OBJECTS := $(addprefix $(BUILD_DIR)/,$(CORE_C_SOURCES:.c=.o))
ENCODER_CORE_OBJECTS := \
	$(addprefix $(BUILD_DIR)/,$(ENCODER_CORE_C_SOURCES:.c=.o))
ENCODER_MODULE_OBJECTS := \
	$(addprefix $(BUILD_DIR)/,$(ENCODER_MODULE_C_SOURCES:.c=.o))
ARCH_OBJECTS := $(addprefix $(BUILD_DIR)/,$(ARCH_SOURCES:.S=.o))
ENCODER_OBJECTS := $(addprefix $(BUILD_DIR)/,$(ENCODER_C_SOURCES:.c=.o)) \
	$(ARCH_OBJECTS)
COLD_OBJECTS := \
	$(BUILD_DIR)/src/decoder/av1.o \
	$(BUILD_DIR)/src/decoder/av1_frame_header.o \
	$(BUILD_DIR)/src/decoder/av1_metadata.o \
	$(BUILD_DIR)/src/decoder/avif.o \
	$(BUILD_DIR)/src/decoder/avif_parse.o \
	$(BUILD_DIR)/src/decoder/avif_sequence.o \
	$(BUILD_DIR)/src/decoder/bmff.o \
	$(BUILD_DIR)/src/decoder/main.o \
	$(BUILD_DIR)/src/decoder/png_write.o \
	$(BUILD_DIR)/$(PLATFORM_DIR)/io.o \
	$(BUILD_DIR)/$(PLATFORM_DIR)/thread.o
STRICT_UNIT_OBJECTS := $(BUILD_DIR)/tests/unit.o $(CORE_OBJECTS) $(ARCH_OBJECTS)
ENCODER_STRICT_UNIT_OBJECTS := $(BUILD_DIR)/tests/encoder_unit.o \
	$(ENCODER_MODULE_OBJECTS) $(CORE_OBJECTS) $(ARCH_OBJECTS)
IMAGE_INPUT_STRICT_UNIT_OBJECTS := \
	$(BUILD_DIR)/tests/image_input_unit.o \
	$(addprefix $(BUILD_DIR)/,$(IMAGE_INPUT_C_SOURCES:.c=.o)) \
	$(BUILD_DIR)/src/base.o $(ARCH_OBJECTS)
OBU_TRACE_OBJECTS := $(BUILD_DIR)/tests/obu_trace.o $(CORE_OBJECTS) \
	$(BUILD_DIR)/$(PLATFORM_DIR)/io.o $(ARCH_OBJECTS)
THREAD_UNIT_OBJECTS := $(BUILD_DIR)/tests/threading.o \
	$(BUILD_DIR)/src/task_pool.o $(BUILD_DIR)/src/base.o \
	$(BUILD_DIR)/$(PLATFORM_DIR)/thread.o \
	$(BUILD_DIR)/$(PLATFORM_DIR)/io.o $(ARCH_OBJECTS)
DEPENDENCIES := $(OBJECTS:.o=.d) $(STRICT_UNIT_OBJECTS:.o=.d) \
	$(ENCODER_STRICT_UNIT_OBJECTS:.o=.d) $(OBU_TRACE_OBJECTS:.o=.d) \
	$(IMAGE_INPUT_STRICT_UNIT_OBJECTS:.o=.d) \
	$(THREAD_UNIT_OBJECTS:.o=.d) $(ENCODER_OBJECTS:.o=.d)

$(COLD_OBJECTS): CFLAGS += -Os

-include $(DEPENDENCIES)

.PHONY: clean encoder test test-encoder test-all wasm fuzz fuzz-seeds \
	fuzz-smoke fuzz-campaign fuzz-differential encoder-fuzz \
	encoder-fuzz-seeds encoder-fuzz-smoke encoder-fuzz-campaign \
	encoder-benchmark encoder-benchmark-json encoder-scorecard

# Strip symbol/relocation metadata from the release decoder binary only;
# the custom static-pie startup only needs the dynamic relocation section,
# not the symbol table, and this leaves the debug-oriented test binaries
# below (unit, obu-trace, thread-unit) unstripped for debuggability.
$(TARGET): LDFLAGS += -Wl,-s

$(TARGET): $(OBJECTS) $(CORE_HEADERS) $(PLATFORM_HEADERS) \
		$(FREESTANDING_HEADERS) $(LINK_TOOLS) Makefile
	@mkdir -p $(@D)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(POST_LINK)

$(ENCODER_TARGET): $(ENCODER_OBJECTS) $(ENCODER_HEADERS) $(CORE_HEADERS) \
		$(PLATFORM_HEADERS) $(FREESTANDING_HEADERS) $(LINK_TOOLS) Makefile
	@mkdir -p $(@D)
	$(CC) $(ENCODER_OBJECTS) $(LDFLAGS) -o $@
	$(POST_LINK)

encoder: $(ENCODER_TARGET)

$(STRICT_UNIT): $(STRICT_UNIT_OBJECTS) $(CORE_HEADERS) \
		$(FREESTANDING_HEADERS) $(LINK_TOOLS) Makefile
	@mkdir -p $(@D)
	$(CC) $(STRICT_UNIT_OBJECTS) $(LDFLAGS) -o $@
	$(POST_LINK)

$(ENCODER_STRICT_UNIT): $(ENCODER_STRICT_UNIT_OBJECTS) \
		$(ENCODER_TEST_HEADERS) $(LINK_TOOLS) Makefile
	@mkdir -p $(@D)
	$(CC) $(ENCODER_STRICT_UNIT_OBJECTS) $(LDFLAGS) -o $@
	$(POST_LINK)

$(IMAGE_INPUT_STRICT_UNIT): $(IMAGE_INPUT_STRICT_UNIT_OBJECTS) \
		$(IMAGE_INPUT_HEADERS) $(FREESTANDING_HEADERS) $(LINK_TOOLS) Makefile
	@mkdir -p $(@D)
	$(CC) $(IMAGE_INPUT_STRICT_UNIT_OBJECTS) $(LDFLAGS) -o $@
	$(POST_LINK)

$(OBU_TRACE): $(OBU_TRACE_OBJECTS) $(CORE_HEADERS) $(PLATFORM_HEADERS) \
		$(FREESTANDING_HEADERS) $(LINK_TOOLS) Makefile
	@mkdir -p $(@D)
	$(CC) $(OBU_TRACE_OBJECTS) $(LDFLAGS) -o $@
	$(POST_LINK)

$(THREAD_UNIT): $(THREAD_UNIT_OBJECTS) $(PLATFORM_HEADERS) \
		$(FREESTANDING_HEADERS) $(LINK_TOOLS) Makefile
	@mkdir -p $(@D)
	$(CC) $(THREAD_UNIT_OBJECTS) $(LDFLAGS) -o $@
	$(POST_LINK)

$(MACHO_DYLIB_REMOVER): tools/macho_dylib_remover.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 $< -o $@

$(HOST_UNIT): tests/unit.c $(CORE_C_SOURCES) $(CORE_HEADERS) Makefile
	@mkdir -p $(@D)
	$(CC) $(HOST_TEST_CFLAGS) tests/unit.c $(CORE_C_SOURCES) -o $@

$(ENCODER_HOST_UNIT): tests/encoder_unit.c $(ENCODER_TEST_C_SOURCES) \
		$(ENCODER_TEST_HEADERS) Makefile
	@mkdir -p $(@D)
	$(CC) $(HOST_TEST_CFLAGS) tests/encoder_unit.c \
		$(ENCODER_TEST_C_SOURCES) -o $@

$(ENCODER_PARALLEL_UNIT): tests/encoder_parallel.c \
		$(ENCODER_TEST_C_SOURCES) $(ENCODER_TEST_HEADERS) Makefile
	@mkdir -p $(@D)
	$(CC) $(HOST_TEST_CFLAGS) tests/encoder_parallel.c \
		$(ENCODER_TEST_C_SOURCES) -pthread -o $@

$(IMAGE_INPUT_HOST_UNIT): tests/image_input_unit.c $(IMAGE_INPUT_C_SOURCES) \
		$(IMAGE_INPUT_HEADERS) src/base.c Makefile
	@mkdir -p $(@D)
	$(CC) $(HOST_TEST_CFLAGS) tests/image_input_unit.c \
		$(IMAGE_INPUT_C_SOURCES) src/base.c -o $@

$(ENCODER_BENCHMARK): tests/encoder_benchmark.c \
		$(ENCODER_TEST_C_SOURCES) $(IMAGE_INPUT_C_SOURCES) \
		$(ENCODER_TEST_HEADERS) Makefile
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
		-Isrc -Isrc/shared -Isrc/decoder -Isrc/codec \
		tests/encoder_benchmark.c \
		$(ENCODER_TEST_C_SOURCES) $(IMAGE_INPUT_C_SOURCES) \
		-lm -pthread -o $@

encoder-benchmark: $(ENCODER_BENCHMARK)
	$(ENCODER_BENCHMARK) --human --iterations $(BENCHMARK_ITERATIONS)

encoder-benchmark-json: $(ENCODER_BENCHMARK)
	$(ENCODER_BENCHMARK) --json --iterations $(BENCHMARK_ITERATIONS)

encoder-scorecard: $(ENCODER_BENCHMARK) tests/encoder-scorecard.sh \
		$(ENCODER_SCORECARD_BASELINE)
	sh tests/encoder-scorecard.sh $(ENCODER_BENCHMARK) \
		$(ENCODER_SCORECARD_BASELINE)

$(FUZZ_TARGET): tests/fuzz.c $(CORE_C_SOURCES) $(CORE_HEADERS) Makefile
	@mkdir -p $(@D)
	$(CC) $(FUZZ_CFLAGS) tests/fuzz.c $(CORE_C_SOURCES) -o $@

fuzz: $(FUZZ_TARGET)

$(ENCODER_FUZZ_TARGET): tests/encoder_fuzz.c $(ENCODER_TEST_C_SOURCES) \
		$(ENCODER_TEST_HEADERS) Makefile
	@mkdir -p $(@D)
	$(CC) $(FUZZ_CFLAGS) tests/encoder_fuzz.c \
		$(ENCODER_TEST_C_SOURCES) -o $@

encoder-fuzz: $(ENCODER_FUZZ_TARGET)

fuzz-seeds:
	sh tests/fuzz-seeds.sh $(FUZZ_CORPUS)

fuzz-smoke: $(FUZZ_TARGET) fuzz-seeds
	UBSAN_OPTIONS=halt_on_error=1 $(FUZZ_TARGET) \
		-runs=1000 -max_len=2097152 \
		-dict=tests/avif.dict -artifact_prefix=$(FUZZ_BUILD_DIR)/ \
		$(FUZZ_CORPUS)

fuzz-campaign: $(FUZZ_TARGET) fuzz-seeds
	UBSAN_OPTIONS=halt_on_error=1 $(FUZZ_TARGET) \
		-max_total_time=$${FUZZ_SECONDS:-3600} -timeout=30 \
		-max_len=2097152 -rss_limit_mb=4096 \
		-dict=tests/avif.dict -artifact_prefix=$(FUZZ_BUILD_DIR)/ \
		$(FUZZ_CORPUS)

fuzz-differential: $(TARGET) fuzz-seeds
	sh tests/fuzz-differential.sh $(TARGET) $(FUZZ_CORPUS)

encoder-fuzz-seeds:
	sh tests/encoder-fuzz-seeds.sh $(ENCODER_FUZZ_CORPUS)

encoder-fuzz-smoke: $(ENCODER_FUZZ_TARGET) encoder-fuzz-seeds
	UBSAN_OPTIONS=halt_on_error=1 $(ENCODER_FUZZ_TARGET) \
		-runs=1000 -max_len=4096 \
		-artifact_prefix=$(FUZZ_BUILD_DIR)/ \
		$(ENCODER_FUZZ_CORPUS)

encoder-fuzz-campaign: $(ENCODER_FUZZ_TARGET) encoder-fuzz-seeds
	UBSAN_OPTIONS=halt_on_error=1 $(ENCODER_FUZZ_TARGET) \
		-max_total_time=$${FUZZ_SECONDS:-3600} -timeout=30 \
		-max_len=4096 -rss_limit_mb=2048 \
		-artifact_prefix=$(FUZZ_BUILD_DIR)/ \
		$(ENCODER_FUZZ_CORPUS)

$(WASM_LOADER): wasm/avif_wasm.c $(WASM_C_SOURCES) src/avifdec.h \
		$(ENCODER_TEST_HEADERS) Makefile
	@mkdir -p $(@D)
	emcc -O2 -std=c11 -Wall -Wextra -Wpedantic -Werror \
		-Isrc -Isrc/shared -Isrc/decoder -Isrc/codec \
		wasm/avif_wasm.c $(WASM_C_SOURCES) --no-entry \
		-sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createAvifDecoder \
		-sENVIRONMENT=web,worker -sFILESYSTEM=0 -sALLOW_MEMORY_GROWTH=1 \
		-sINITIAL_MEMORY=16777216 -sMAXIMUM_MEMORY=1073741824 \
		-sSTACK_SIZE=8388608 -sSTACK_OVERFLOW_CHECK=2 \
		-sEXPORTED_FUNCTIONS='["_malloc","_free","_avif_wasm_decode","_avif_wasm_encode","_avif_wasm_reset","_avif_wasm_pixel_pointer","_avif_wasm_pixel_bytes","_avif_wasm_width","_avif_wasm_height","_avif_wasm_source_width","_avif_wasm_source_height","_avif_wasm_bit_depth","_avif_wasm_has_alpha","_avif_wasm_stage","_avif_wasm_error_offset","_avif_wasm_error_context","_avif_wasm_encoded_pointer","_avif_wasm_encoded_bytes","_avif_wasm_encoded_width","_avif_wasm_encoded_height","_avif_wasm_encoder_stage","_avif_wasm_encoder_error_context"]' \
		-sEXPORTED_RUNTIME_METHODS='["HEAPU8"]' -o $@

$(WASM_BUILD_DIR)/%: wasm/%
	@mkdir -p $(@D)
	cp $< $@

wasm: $(WASM_LOADER) $(addprefix $(WASM_BUILD_DIR)/,$(WASM_ASSETS))
	@test -f $(WASM_BINARY)
	@printf '%s\n' 'WASM viewer built in $(WASM_BUILD_DIR)'

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(GENERATED_CHECK): tools/generate-av1-coeff-tables.pl \
		tools/generate-av1-palette-tables.pl \
		tools/generate-av1-quant-tables.pl \
		tools/generate-av1-warp-tables.pl \
		tools/generate-av1-film-grain-table.pl $(AV1_SPEC) \
		$(COEFF_TABLES) $(PALETTE_TABLES) $(QUANT_TABLES) $(WARP_TABLES) \
		$(FILM_GRAIN_TABLE)
	@mkdir -p build/generated-check
	perl tools/generate-av1-coeff-tables.pl $(AV1_SPEC) $(COEFF_TABLES_CHECK)
	cmp $(COEFF_TABLES) $(COEFF_TABLES_CHECK)
	perl tools/generate-av1-palette-tables.pl $(AV1_SPEC) $(PALETTE_TABLES_CHECK)
	cmp $(PALETTE_TABLES) $(PALETTE_TABLES_CHECK)
	perl tools/generate-av1-quant-tables.pl $(AV1_SPEC) $(QUANT_TABLES_CHECK)
	cmp $(QUANT_TABLES) $(QUANT_TABLES_CHECK)
	perl tools/generate-av1-warp-tables.pl $(AV1_SPEC) $(WARP_TABLES_CHECK)
	cmp $(WARP_TABLES) $(WARP_TABLES_CHECK)
	perl tools/generate-av1-film-grain-table.pl $(AV1_SPEC) \
		$(FILM_GRAIN_TABLE_CHECK)
	cmp $(FILM_GRAIN_TABLE) $(FILM_GRAIN_TABLE_CHECK)
	@touch $@

# Self-contained decoder and encoder suite. Uses only the compiler, coreutils,
# and checked-in fixtures/corpus; it needs no codec, image, or other
# third-party tools, so `make test` runs anywhere the project itself builds.
test: test-encoder $(TARGET) $(STRICT_UNIT) $(HOST_UNIT) $(OBU_TRACE) \
		$(THREAD_UNIT)
	$(STRICT_UNIT)
	$(HOST_UNIT)
	$(THREAD_UNIT)
	sh tests/smoke.sh $(TARGET)
	sh tests/features.sh $(TARGET)
	sh tests/corpus.sh $(TARGET)

test-encoder: $(TARGET) $(ENCODER_TARGET) $(ENCODER_STRICT_UNIT) \
		$(ENCODER_HOST_UNIT) $(ENCODER_PARALLEL_UNIT) \
		$(IMAGE_INPUT_STRICT_UNIT) $(IMAGE_INPUT_HOST_UNIT) \
		$(ENCODER_BENCHMARK)
	$(ENCODER_STRICT_UNIT)
	$(ENCODER_HOST_UNIT)
	$(ENCODER_PARALLEL_UNIT)
	$(IMAGE_INPUT_STRICT_UNIT)
	$(IMAGE_INPUT_HOST_UNIT)
	sh tests/encoder.sh $(ENCODER_TARGET) $(TARGET)
	sh tests/encoder-scorecard.sh $(ENCODER_BENCHMARK) \
		$(ENCODER_SCORECARD_BASELINE)

# Full suite: the self-contained tests above plus the reference and
# differential comparisons. These additionally require ffmpeg, ffprobe,
# libavif (avifenc/avifdec), aom (aomenc/aomdec), ImageMagick (magick), and perl;
# tests/reference-block.sh also builds libaom through git and cmake. The
# generated-table reproduction checks run when the ignored docs/av1.html
# specification file is present.
test-all: test $(TEST_GENERATED_CHECK)
	@if test -z "$(TEST_GENERATED_CHECK)"; then \
		printf '%s\n' 'Skipping generated-table reproduction checks: docs/av1.html is unavailable.'; \
	fi
	sh tests/differential.sh $(TARGET)
	sh tests/encoder-reference.sh
	sh tests/encoder-interoperability.sh $(ENCODER_TARGET) $(TARGET)
	sh tests/reference.sh $(TARGET)
	sh tests/presentation.sh $(TARGET)
	sh tests/sequence.sh $(TARGET)
	sh tests/reference-block.sh $(TARGET) $(OBU_TRACE)

clean:
	rm -rf build