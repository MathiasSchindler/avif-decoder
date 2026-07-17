CC ?= cc

MACHO_DYLIB_REMOVER := build/host/macho-dylib-remover
LINK_TOOLS :=
POST_LINK :=
TARGET_CFLAGS :=
OS := $(shell uname -s)
ARCH := $(shell uname -m)
BUILD_DIR := build/$(ARCH)
TARGET := $(BUILD_DIR)/avifdec
STRICT_UNIT := $(BUILD_DIR)/unit
HOST_UNIT := build/host/unit
OBU_TRACE := $(BUILD_DIR)/obu-trace
THREAD_UNIT := $(BUILD_DIR)/thread-unit
WASM_BUILD_DIR := build/wasm
WASM_LOADER := $(WASM_BUILD_DIR)/avif-decoder.js
WASM_BINARY := $(WASM_BUILD_DIR)/avif-decoder.wasm
WASM_ASSETS := index.html app.js decoder-worker.js styles.css
.DEFAULT_GOAL := $(TARGET)
COEFF_TABLES := src/av1_coeff_tables.inc
COEFF_TABLES_CHECK := build/generated-check/av1_coeff_tables.inc
PALETTE_TABLES := src/av1_palette_tables.inc
PALETTE_TABLES_CHECK := build/generated-check/av1_palette_tables.inc
QUANT_TABLES := src/av1_quant_tables.inc
QUANT_TABLES_CHECK := build/generated-check/av1_quant_tables.inc
WARP_TABLES := src/av1_warp_tables.inc
WARP_TABLES_CHECK := build/generated-check/av1_warp_tables.inc
FILM_GRAIN_TABLE := src/av1_film_grain_gaussian.inc
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
ARCH_SOURCES := $(ARCH_DIR)/crt0.S
PLATFORM_DIR := src/platform/macos
MACOS_SDKROOT := $(shell xcrun --sdk macosx --show-sdk-path)
LINK_TOOLS := $(MACHO_DYLIB_REMOVER)
POST_LINK = $(MACHO_DYLIB_REMOVER) $@ && codesign --force --sign - $@
TARGET_CFLAGS := -target arm64-apple-macos11 -isysroot $(MACOS_SDKROOT)
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
	-Isrc -Isrc/shared -I$(PLATFORM_DIR) -I$(ARCH_DIR)
HOST_TEST_CFLAGS := \
	-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-Isrc -Isrc/shared
CORE_C_SOURCES := \
	src/base.c src/bmff.c src/avif.c src/avif_sequence.c src/avif_rgb.c \
	src/avif_sato.c src/png.c \
	src/av1.c src/av1_bitstream.c src/av1_metadata.c src/av1_profile.c \
	src/av1_reference.c \
	src/av1_film_grain.c \
	src/av1_inter.c src/av1_inter_predict.c src/av1_warp.c \
	src/av1_symbol.c src/av1_partition.c src/av1_coeff.c \
	src/av1_recon.c src/av1_intra.c src/av1_predict.c \
	src/av1_tile.c src/av1_tile_cdf.c src/av1_tile_restoration.c \
	src/av1_tile_palette.c src/av1_block.c \
	src/av1_filter.c src/av1_cdef.c src/av1_superres.c \
	src/av1_restoration_filter.c
RUNTIME_C_SOURCES := src/task_pool.c $(PLATFORM_DIR)/thread.c
C_SOURCES := src/main.c $(CORE_C_SOURCES) $(RUNTIME_C_SOURCES) \
	$(PLATFORM_DIR)/io.c
SOURCES := $(C_SOURCES) $(ARCH_SOURCES)
OBJECTS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.o))
OBJECTS := $(OBJECTS:.S=.o)
CORE_OBJECTS := $(addprefix $(BUILD_DIR)/,$(CORE_C_SOURCES:.c=.o))
ARCH_OBJECTS := $(addprefix $(BUILD_DIR)/,$(ARCH_SOURCES:.S=.o))
COLD_OBJECTS := \
	$(BUILD_DIR)/src/av1.o \
	$(BUILD_DIR)/src/av1_metadata.o \
	$(BUILD_DIR)/src/avif.o \
	$(BUILD_DIR)/src/avif_sequence.o \
	$(BUILD_DIR)/src/bmff.o \
	$(BUILD_DIR)/src/main.o \
	$(BUILD_DIR)/$(PLATFORM_DIR)/io.o \
	$(BUILD_DIR)/$(PLATFORM_DIR)/thread.o
STRICT_UNIT_OBJECTS := $(BUILD_DIR)/tests/unit.o $(CORE_OBJECTS) $(ARCH_OBJECTS)
OBU_TRACE_OBJECTS := $(BUILD_DIR)/tests/obu_trace.o $(CORE_OBJECTS) \
	$(BUILD_DIR)/$(PLATFORM_DIR)/io.o $(ARCH_OBJECTS)
THREAD_UNIT_OBJECTS := $(BUILD_DIR)/tests/threading.o \
	$(BUILD_DIR)/src/task_pool.o $(BUILD_DIR)/src/base.o \
	$(BUILD_DIR)/$(PLATFORM_DIR)/thread.o \
	$(BUILD_DIR)/$(PLATFORM_DIR)/io.o $(ARCH_OBJECTS)
DEPENDENCIES := $(OBJECTS:.o=.d) $(STRICT_UNIT_OBJECTS:.o=.d) \
	$(OBU_TRACE_OBJECTS:.o=.d) $(THREAD_UNIT_OBJECTS:.o=.d)

$(COLD_OBJECTS): CFLAGS += -Os

-include $(DEPENDENCIES)

.PHONY: clean test wasm

# Strip symbol/relocation metadata from the release decoder binary only;
# the custom static-pie startup only needs the dynamic relocation section,
# not the symbol table, and this leaves the debug-oriented test binaries
# below (unit, obu-trace, thread-unit) unstripped for debuggability.
$(TARGET): LDFLAGS += -Wl,-s

$(TARGET): $(OBJECTS) $(LINK_TOOLS)
	@mkdir -p $(@D)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(POST_LINK)

$(STRICT_UNIT): $(STRICT_UNIT_OBJECTS) $(LINK_TOOLS)
	@mkdir -p $(@D)
	$(CC) $(STRICT_UNIT_OBJECTS) $(LDFLAGS) -o $@
	$(POST_LINK)

$(OBU_TRACE): $(OBU_TRACE_OBJECTS) $(LINK_TOOLS)
	@mkdir -p $(@D)
	$(CC) $(OBU_TRACE_OBJECTS) $(LDFLAGS) -o $@
	$(POST_LINK)

$(THREAD_UNIT): $(THREAD_UNIT_OBJECTS) $(LINK_TOOLS)
	@mkdir -p $(@D)
	$(CC) $(THREAD_UNIT_OBJECTS) $(LDFLAGS) -o $@
	$(POST_LINK)

$(MACHO_DYLIB_REMOVER): tools/macho_dylib_remover.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 $< -o $@

$(HOST_UNIT): tests/unit.c $(CORE_C_SOURCES) src/base.h src/bmff.h \
		src/av1.h src/av1_bitstream.h src/av1_metadata.h src/av1_profile.h \
		src/av1_film_grain.h src/av1_film_grain_gaussian.inc \
		src/av1_inter.h \
		src/av1_inter_predict.h src/av1_warp.h $(WARP_TABLES) \
		src/av1_symbol.h src/av1_partition.h src/av1_coeff.h \
		src/av1_coeff_defaults.inc $(COEFF_TABLES) src/av1_recon.h \
		$(QUANT_TABLES) src/av1_intra.h src/av1_intra_defaults.inc \
		src/av1_predict.h src/av1_tile.h src/av1_tile_internal.h \
		src/av1_filter.h src/avifdec.h src/png.h
	@mkdir -p $(@D)
	$(CC) $(HOST_TEST_CFLAGS) tests/unit.c $(CORE_C_SOURCES) -o $@

$(WASM_LOADER): wasm/avif_wasm.c $(CORE_C_SOURCES) src/avifdec.h
	@mkdir -p $(@D)
	emcc -O2 -std=c11 -Wall -Wextra -Wpedantic -Werror -Isrc \
		wasm/avif_wasm.c $(CORE_C_SOURCES) --no-entry \
		-sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createAvifDecoder \
		-sENVIRONMENT=web,worker -sFILESYSTEM=0 -sALLOW_MEMORY_GROWTH=1 \
		-sINITIAL_MEMORY=16777216 -sMAXIMUM_MEMORY=1073741824 \
		-sSTACK_SIZE=8388608 -sSTACK_OVERFLOW_CHECK=2 \
		-sEXPORTED_FUNCTIONS='["_malloc","_free","_avif_wasm_decode","_avif_wasm_reset","_avif_wasm_pixel_pointer","_avif_wasm_pixel_bytes","_avif_wasm_width","_avif_wasm_height","_avif_wasm_source_width","_avif_wasm_source_height","_avif_wasm_bit_depth","_avif_wasm_has_alpha","_avif_wasm_stage","_avif_wasm_error_offset","_avif_wasm_error_context"]' \
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

test: $(TEST_GENERATED_CHECK) $(TARGET) $(STRICT_UNIT) $(HOST_UNIT) \
		$(OBU_TRACE) $(THREAD_UNIT)
	@if test -z "$(TEST_GENERATED_CHECK)"; then \
		printf '%s\n' 'Skipping generated-table reproduction checks: docs/av1.html is unavailable.'; \
	fi
	$(STRICT_UNIT)
	$(HOST_UNIT)
	$(THREAD_UNIT)
	sh tests/smoke.sh $(TARGET)
	sh tests/features.sh $(TARGET)
	sh tests/corpus.sh $(TARGET)
	sh tests/differential.sh $(TARGET)
	sh tests/reference.sh $(TARGET)
	sh tests/presentation.sh $(TARGET)
	sh tests/sequence.sh $(TARGET)
	sh tests/reference-block.sh $(TARGET) $(OBU_TRACE)

clean:
	rm -rf build