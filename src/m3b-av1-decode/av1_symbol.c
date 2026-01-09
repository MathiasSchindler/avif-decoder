#include "av1_symbol.h"

#include <stdio.h>
#include <string.h>

enum {
    EC_PROB_SHIFT = 6,
    EC_MIN_PROB = 4,
};

static uint32_t floor_log2_u32(uint32_t n) {
    uint32_t r = 0;
    while (n >= 2) {
        n >>= 1;
        r++;
    }
    return r;
}

static bool br_read_bit(Av1BitReader *br, uint32_t *out) {
    if ((br->bitpos >> 3) >= br->size) {
        return false;
    }
    uint8_t byte = br->data[br->bitpos >> 3];
    unsigned shift = 7u - (unsigned)(br->bitpos & 7u);
    *out = (byte >> shift) & 1u;
    br->bitpos++;
    return true;
}

static bool br_read_bits(Av1BitReader *br, unsigned n, uint32_t *out) {
    if (n == 0) {
        *out = 0;
        return true;
    }
    if (n > 32) {
        return false;
    }
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t b;
        if (!br_read_bit(br, &b)) {
            return false;
        }
        v = (v << 1) | b;
    }
    *out = v;
    return true;
}

static bool get_bit_at(const uint8_t *data, size_t size, uint64_t bitpos, uint32_t *out) {
    uint64_t byte_index = bitpos >> 3;
    if (byte_index >= size) {
        return false;
    }
    uint8_t byte = data[byte_index];
    unsigned shift = 7u - (unsigned)(bitpos & 7u);
    *out = (byte >> shift) & 1u;
    return true;
}

bool av1_symbol_init(Av1SymbolDecoder *sd,
                     const uint8_t *data,
                     size_t size_bytes,
                     bool disable_cdf_update,
                     char *err,
                     size_t err_cap) {
    if (!sd || (!data && size_bytes)) {
        if (err && err_cap) {
            snprintf(err, err_cap, "invalid args");
        }
        return false;
    }

    memset(sd, 0, sizeof(*sd));
    sd->br.data = data;
    sd->br.size = size_bytes;
    sd->br.bitpos = 0;
    sd->disable_cdf_update = disable_cdf_update;

    // init_symbol(sz)
    uint32_t numBits = (uint32_t)((size_bytes * 8u) < 15u ? (size_bytes * 8u) : 15u);

    uint32_t buf = 0;
    if (!br_read_bits(&sd->br, (unsigned)numBits, &buf)) {
        if (err && err_cap) {
            snprintf(err, err_cap, "truncated init_symbol buf");
        }
        return false;
    }

    uint32_t paddedBuf = buf << (15u - numBits);
    sd->symbol_value = ((1u << 15) - 1u) ^ paddedBuf;
    sd->symbol_range = 1u << 15;
    sd->symbol_max_bits = (int32_t)(8u * (uint32_t)size_bytes) - 15;

    return true;
}

bool av1_symbol_read_symbol(Av1SymbolDecoder *sd,
                            uint16_t *cdf,
                            size_t n,
                            uint32_t *out_symbol,
                            char *err,
                            size_t err_cap) {
    if (!sd || !cdf || !out_symbol) {
        if (err && err_cap) {
            snprintf(err, err_cap, "invalid args");
        }
        return false;
    }
    if (n <= 1) {
        if (err && err_cap) {
            snprintf(err, err_cap, "invalid cdf size");
        }
        return false;
    }
    if (cdf[n - 1] != (1u << 15)) {
        if (err && err_cap) {
            snprintf(err, err_cap, "cdf[n-1] must equal 1<<15");
        }
        return false;
    }

    uint32_t cur = sd->symbol_range;
    uint32_t prev = 0;
    int32_t symbol = -1;

    for (;;) {
        symbol++;
        if ((size_t)symbol >= n) {
            if (err && err_cap) {
                snprintf(err, err_cap, "symbol decode failed (cdf walk overflow)");
            }
            return false;
        }
        prev = cur;

        uint32_t f = (1u << 15) - (uint32_t)cdf[symbol];
        uint32_t t = ((sd->symbol_range >> 8) * (f >> EC_PROB_SHIFT)) >> (7 - EC_PROB_SHIFT);
        t += (uint32_t)EC_MIN_PROB * (uint32_t)(n - (size_t)symbol - 1u);
        cur = t;

        if (sd->symbol_value >= cur) {
            break;
        }
    }

    sd->symbol_range = prev - cur;
    sd->symbol_value -= cur;

    // Renormalization
    if (sd->symbol_range == 0) {
        if (err && err_cap) {
            snprintf(err, err_cap, "invalid symbol_range=0");
        }
        return false;
    }

    uint32_t bits = 15u - floor_log2_u32(sd->symbol_range);
    if (bits > 15u) {
        if (err && err_cap) {
            snprintf(err, err_cap, "invalid renorm bits");
        }
        return false;
    }

    sd->symbol_range <<= bits;

    int32_t maxReadable = sd->symbol_max_bits;
    if (maxReadable < 0) {
        maxReadable = 0;
    }

    uint32_t numBits = bits;
    if (numBits > (uint32_t)maxReadable) {
        numBits = (uint32_t)maxReadable;
    }

    uint32_t newData = 0;
    if (numBits) {
        if (!br_read_bits(&sd->br, (unsigned)numBits, &newData)) {
            if (err && err_cap) {
                snprintf(err, err_cap, "truncated symbol renorm bits");
            }
            return false;
        }
    }

    uint32_t paddedData = newData << (bits - numBits);
    sd->symbol_value = paddedData ^ ((((sd->symbol_value + 1u) << bits) - 1u));
    sd->symbol_max_bits -= (int32_t)bits;

    // CDF update
    if (!sd->disable_cdf_update) {
        uint16_t count = cdf[n];
        uint32_t rate = 3u + (count > 15u) + (count > 31u);
        uint32_t lg = floor_log2_u32((uint32_t)n);
        rate += (lg < 2u) ? lg : 2u;

        uint32_t tmp = 0;
        for (size_t i = 0; i + 1 < n; i++) {
            if ((int32_t)i == symbol) {
                tmp = 1u << 15;
            }
            uint32_t ci = cdf[i];
            if (tmp < ci) {
                ci -= (ci - tmp) >> rate;
            } else {
                ci += (tmp - ci) >> rate;
            }
            if (ci > (1u << 15)) {
                ci = 1u << 15;
            }
            cdf[i] = (uint16_t)ci;
        }
        if (cdf[n] < 32u) {
            cdf[n] = (uint16_t)(cdf[n] + 1u);
        }
    }

    *out_symbol = (uint32_t)symbol;
    return true;
}

bool av1_symbol_read_bool(Av1SymbolDecoder *sd, uint32_t *out_bit, char *err, size_t err_cap) {
    // Spec: constructs a fresh CDF each call.
    uint16_t cdf[3];
    cdf[0] = 1u << 14;
    cdf[1] = 1u << 15;
    cdf[2] = 0;

    // Per spec note: we can omit CDF update here.
    bool saved = sd->disable_cdf_update;
    sd->disable_cdf_update = true;

    uint32_t sym = 0;
    bool ok = av1_symbol_read_symbol(sd, cdf, 2, &sym, err, err_cap);

    sd->disable_cdf_update = saved;

    if (!ok) {
        return false;
    }
    *out_bit = sym;
    return true;
}

bool av1_symbol_read_literal(Av1SymbolDecoder *sd, unsigned n, uint32_t *out, char *err, size_t err_cap) {
    if (!sd || !out) {
        if (err && err_cap) {
            snprintf(err, err_cap, "invalid args");
        }
        return false;
    }
    if (n > 32) {
        if (err && err_cap) {
            snprintf(err, err_cap, "unsupported literal width");
        }
        return false;
    }

    uint32_t x = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t b;
        if (!av1_symbol_read_bool(sd, &b, err, err_cap)) {
            return false;
        }
        x = (x << 1) | (b & 1u);
    }
    *out = x;
    return true;
}

bool av1_symbol_exit(Av1SymbolDecoder *sd, char *err, size_t err_cap) {
    if (!sd) {
        if (err && err_cap) {
            snprintf(err, err_cap, "invalid args");
        }
        return false;
    }

    // Spec conformance requirement.
    if (sd->symbol_max_bits < -14) {
        if (err && err_cap) {
            snprintf(err, err_cap, "SymbolMaxBits < -14 at exit (%d)", sd->symbol_max_bits);
        }
        return false;
    }

    // trailingBitPosition = get_position() - Min(15, SymbolMaxBits+15)
    int32_t smb_plus_15 = sd->symbol_max_bits + 15;
    uint32_t minv = 15u;
    if (smb_plus_15 < 15) {
        minv = (smb_plus_15 < 0) ? 0u : (uint32_t)smb_plus_15;
    }

    uint64_t cur_pos = sd->br.bitpos;
    if (cur_pos < (uint64_t)minv) {
        if (err && err_cap) {
            snprintf(err, err_cap, "invalid trailingBitPosition (underflow)");
        }
        return false;
    }
    uint64_t trailingBitPosition = cur_pos - (uint64_t)minv;

    // Advance bitstream position by Max(0, SymbolMaxBits).
    if (sd->symbol_max_bits > 0) {
        sd->br.bitpos += (uint64_t)sd->symbol_max_bits;
    }

    // paddingEndPosition is current position (byte-aligned per spec; enforce here).
    uint64_t paddingEndPosition = sd->br.bitpos;
    if ((paddingEndPosition & 7u) != 0) {
        if (err && err_cap) {
            snprintf(err, err_cap, "exit_symbol ended unaligned");
        }
        return false;
    }

    uint64_t totalBits = (uint64_t)sd->br.size * 8u;
    if (paddingEndPosition > totalBits) {
        // Padding zero bits beyond the end are allowed only when SymbolMaxBits went negative.
        // For our current use (no real tile decode), treat this as unsupported.
        if (err && err_cap) {
            snprintf(err, err_cap, "exit_symbol advanced beyond end of buffer");
        }
        return false;
    }

    // Check trailing bits: bit at trailingBitPosition == 1; bits after until paddingEndPosition == 0.
    uint32_t b;
    if (!get_bit_at(sd->br.data, sd->br.size, trailingBitPosition, &b)) {
        if (err && err_cap) {
            snprintf(err, err_cap, "trailingBitPosition out of range");
        }
        return false;
    }
    if (b != 1u) {
        if (err && err_cap) {
            snprintf(err, err_cap, "trailing bit not 1");
        }
        return false;
    }
    for (uint64_t pos = trailingBitPosition + 1; pos < paddingEndPosition; pos++) {
        if (!get_bit_at(sd->br.data, sd->br.size, pos, &b)) {
            if (err && err_cap) {
                snprintf(err, err_cap, "trailing padding bit out of range");
            }
            return false;
        }
        if (b != 0u) {
            if (err && err_cap) {
                snprintf(err, err_cap, "nonzero trailing padding bit");
            }
            return false;
        }
    }

    return true;
}

bool av1_symbol_check_trailing_bits(const uint8_t *data, size_t size_bytes, char *err, size_t err_cap) {
    if (!data || size_bytes == 0) {
        if (err && err_cap) {
            snprintf(err, err_cap, "invalid args");
        }
        return false;
    }

    uint64_t totalBits = (uint64_t)size_bytes * 8u;
    uint64_t start = 0;
    if (totalBits > 15u) {
        start = totalBits - 15u;
    }

    // Find the last '1' bit within the last 15 bits.
    uint64_t last_one = UINT64_MAX;
    for (uint64_t pos = start; pos < totalBits; pos++) {
        uint64_t byte_index = pos >> 3;
        uint8_t byte = data[byte_index];
        unsigned shift = 7u - (unsigned)(pos & 7u);
        uint32_t b = (byte >> shift) & 1u;
        if (b) {
            last_one = pos;
        }
    }

    if (last_one == UINT64_MAX) {
        if (err && err_cap) {
            snprintf(err, err_cap, "missing trailing '1' bit in last 15 bits");
        }
        return false;
    }

    // All bits after the last '1' must be zero.
    for (uint64_t pos = last_one + 1; pos < totalBits; pos++) {
        uint64_t byte_index = pos >> 3;
        uint8_t byte = data[byte_index];
        unsigned shift = 7u - (unsigned)(pos & 7u);
        uint32_t b = (byte >> shift) & 1u;
        if (b) {
            if (err && err_cap) {
                snprintf(err, err_cap, "nonzero padding bit after trailing '1'");
            }
            return false;
        }
    }

    return true;
}
