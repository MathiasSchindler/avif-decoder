CC ?= cc

ARCH := $(shell uname -m)
BUILD_DIR := build/$(ARCH)
TARGET := $(BUILD_DIR)/avifdec
STRICT_UNIT := $(BUILD_DIR)/unit
HOST_UNIT := build/host/unit
OBU_TRACE := $(BUILD_DIR)/obu-trace
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

ifeq ($(ARCH),x86_64)
ARCH_DIR := src/arch/x86_64/linux
ARCH_SOURCES := $(ARCH_DIR)/crt0.S $(ARCH_DIR)/syscall_stubs.S
else
$(error Unsupported architecture '$(ARCH)'; currently only x86_64 Linux is wired into the build)
endif

CFLAGS := \
	-std=c11 -Wall -Wextra -Wpedantic -Werror -Os \
	-ffreestanding -fno-builtin -fno-stack-protector \
	-fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -fPIE -MMD -MP -nostdinc \
	-DNEWOS_DISABLE_STACK_GUARD_INIT \
	-Isrc -Isrc/shared -Isrc/platform/linux -I$(ARCH_DIR)
HOST_TEST_CFLAGS := \
	-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-Isrc -Isrc/shared
LDFLAGS := \
	-nostdlib -static-pie \
	-Wl,--gc-sections -Wl,--build-id=none -Wl,-e,_start

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
C_SOURCES := src/main.c $(CORE_C_SOURCES) src/platform/linux/io.c
SOURCES := $(C_SOURCES) $(ARCH_SOURCES)
OBJECTS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.o))
OBJECTS := $(OBJECTS:.S=.o)
CORE_OBJECTS := $(addprefix $(BUILD_DIR)/,$(CORE_C_SOURCES:.c=.o))
ARCH_OBJECTS := $(addprefix $(BUILD_DIR)/,$(ARCH_SOURCES:.S=.o))
STRICT_UNIT_OBJECTS := $(BUILD_DIR)/tests/unit.o $(CORE_OBJECTS) $(ARCH_OBJECTS)
OBU_TRACE_OBJECTS := $(BUILD_DIR)/tests/obu_trace.o $(CORE_OBJECTS) \
	$(BUILD_DIR)/src/platform/linux/io.o $(ARCH_OBJECTS)
DEPENDENCIES := $(OBJECTS:.o=.d) $(STRICT_UNIT_OBJECTS:.o=.d) \
	$(OBU_TRACE_OBJECTS:.o=.d)

-include $(DEPENDENCIES)

.PHONY: clean test

$(TARGET): $(OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(STRICT_UNIT): $(STRICT_UNIT_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(STRICT_UNIT_OBJECTS) $(LDFLAGS) -o $@

$(OBU_TRACE): $(OBU_TRACE_OBJECTS)
	@mkdir -p $(@D)
	$(CC) $(OBU_TRACE_OBJECTS) $(LDFLAGS) -o $@

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
		tools/generate-av1-film-grain-table.pl docs/av1.html \
		$(COEFF_TABLES) $(PALETTE_TABLES) $(QUANT_TABLES) $(WARP_TABLES) \
		$(FILM_GRAIN_TABLE)
	@mkdir -p build/generated-check
	perl tools/generate-av1-coeff-tables.pl docs/av1.html $(COEFF_TABLES_CHECK)
	cmp $(COEFF_TABLES) $(COEFF_TABLES_CHECK)
	perl tools/generate-av1-palette-tables.pl docs/av1.html $(PALETTE_TABLES_CHECK)
	cmp $(PALETTE_TABLES) $(PALETTE_TABLES_CHECK)
	perl tools/generate-av1-quant-tables.pl docs/av1.html $(QUANT_TABLES_CHECK)
	cmp $(QUANT_TABLES) $(QUANT_TABLES_CHECK)
	perl tools/generate-av1-warp-tables.pl docs/av1.html $(WARP_TABLES_CHECK)
	cmp $(WARP_TABLES) $(WARP_TABLES_CHECK)
	perl tools/generate-av1-film-grain-table.pl docs/av1.html \
		$(FILM_GRAIN_TABLE_CHECK)
	cmp $(FILM_GRAIN_TABLE) $(FILM_GRAIN_TABLE_CHECK)
	@touch $@

test: $(GENERATED_CHECK) $(TARGET) $(STRICT_UNIT) $(HOST_UNIT) $(OBU_TRACE)
	$(STRICT_UNIT)
	$(HOST_UNIT)
	sh tests/smoke.sh $(TARGET)
	sh tests/features.sh $(TARGET)
	sh tests/corpus.sh $(TARGET)
	sh tests/reference.sh $(TARGET)
	sh tests/presentation.sh $(TARGET)
	sh tests/sequence.sh $(TARGET)
	sh tests/reference-block.sh $(TARGET) $(OBU_TRACE)

clean:
	rm -rf build