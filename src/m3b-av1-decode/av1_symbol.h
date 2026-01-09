#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Minimal AV1 Symbol decoder (entropy decoder) implementation based directly on the
// AV1 bitstream specification (init_symbol/read_symbol/read_bool/read_literal).
//
// This is intentionally small and self-contained to support incremental m3b work.

typedef struct {
    const uint8_t *data;
    size_t size;
    uint64_t bitpos;
} Av1BitReader;

typedef struct {
    Av1BitReader br;

    uint32_t symbol_value;
    uint32_t symbol_range;
    int32_t symbol_max_bits;

    // If true, suppresses adaptive CDF updates in av1_symbol_read_symbol().
    bool disable_cdf_update;
} Av1SymbolDecoder;

bool av1_symbol_init(Av1SymbolDecoder *sd,
                     const uint8_t *data,
                     size_t size_bytes,
                     bool disable_cdf_update,
                     char *err,
                     size_t err_cap);

// Reads a symbol in [0..N-1] from a CDF of length N+1.
// Conformance expectation: N > 1 and cdf[N-1] == (1<<15).
bool av1_symbol_read_symbol(Av1SymbolDecoder *sd,
                            uint16_t *cdf,
                            size_t n,
                            uint32_t *out_symbol,
                            char *err,
                            size_t err_cap);

bool av1_symbol_read_bool(Av1SymbolDecoder *sd, uint32_t *out_bit, char *err, size_t err_cap);

bool av1_symbol_read_literal(Av1SymbolDecoder *sd, unsigned n, uint32_t *out, char *err, size_t err_cap);

// Validates/consumes trailing bits at the end of a tile payload.
// This models the AV1 spec's exit_symbol() process.
bool av1_symbol_exit(Av1SymbolDecoder *sd, char *err, size_t err_cap);

// Checks AV1 entropy-coded trailing bits condition on a complete tile payload buffer.
// This is a lightweight, syntax-independent validation: the last 15 bits of the buffer
// must contain at least one '1' bit, and all bits after the last '1' (to the end of the
// buffer) must be zero.
bool av1_symbol_check_trailing_bits(const uint8_t *data, size_t size_bytes, char *err, size_t err_cap);
