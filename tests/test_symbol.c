#include <stdio.h>
#include <string.h>

#include "../src/m3b-av1-decode/av1_symbol.h"

#define CHECK(cond)                            \
    do {                                       \
        if (!(cond)) {                         \
            fprintf(stderr, "FAIL: %s\n", #cond); \
            return 1;                          \
        }                                      \
    } while (0)

static int test_bool_all_zero(void) {
    const uint8_t data[] = {0x00, 0x00};
    Av1SymbolDecoder sd;
    char err[256] = {0};
    CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));

    uint32_t b;
    CHECK(av1_symbol_read_bool(&sd, &b, err, sizeof(err)));
    CHECK(b == 0);

    uint32_t lit;
    CHECK(av1_symbol_read_literal(&sd, 4, &lit, err, sizeof(err)));
    CHECK(lit == 0);
    return 0;
}

static int test_bool_all_one(void) {
    const uint8_t data[] = {0xFF, 0xFF};
    Av1SymbolDecoder sd;
    char err[256] = {0};
    CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));

    uint32_t b;
    CHECK(av1_symbol_read_bool(&sd, &b, err, sizeof(err)));
    CHECK(b == 1);

    // A 1-bit literal is exactly one read_bool.
    uint32_t lit;
    CHECK(av1_symbol_read_literal(&sd, 1, &lit, err, sizeof(err)));
    CHECK(lit == 1);
    return 0;
}

static int test_exit_symbol_trailing_bits_ok(void) {
    // For sz=2, init_symbol() reads 15 bits, then exit_symbol() (with no decoding)
    // will compute trailingBitPosition from the *current* position (15 bits read),
    // so trailingBitPosition==0 and then it skips the remaining bit to reach byte alignment.
    // That requires bit0 == 1 and bits 1..15 == 0.
    const uint8_t data[] = {0x80, 0x00};
    Av1SymbolDecoder sd;
    char err[256] = {0};
    CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));
    CHECK(av1_symbol_exit(&sd, err, sizeof(err)));
    return 0;
}

static int test_read_symbol_kat_3sym(void) {
    // Known-answer test with a simple 3-symbol CDF.
    // cdf values represent cumulative distribution; last symbol entry must be 1<<15.
    // We choose init_symbol() inputs by selecting the initial SymbolValue via the 15-bit buf:
    //   SymbolValue = 0x7FFF ^ buf
    // so we can place SymbolValue into a known region.

    // CDF for 3 symbols: roughly 50% / 25% / 25%
    // Layout is length N+1, with cdf[N] holding the adaptation count.
    uint16_t cdf_base[4];
    cdf_base[0] = 16384;
    cdf_base[1] = 24576;
    cdf_base[2] = 32768;
    cdf_base[3] = 0;

    // Case A: SymbolValue high => expect symbol 0.
    {
        const uint8_t data[] = {0x00, 0x00}; // buf=0 => SymbolValue=0x7FFF
        Av1SymbolDecoder sd;
        char err[256] = {0};
        uint16_t cdf[4];
        memcpy(cdf, cdf_base, sizeof(cdf));
        CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));
        uint32_t sym;
        CHECK(av1_symbol_read_symbol(&sd, cdf, 3, &sym, err, sizeof(err)));
        CHECK(sym == 0);
    }

    // Case B: SymbolValue in middle band => expect symbol 1.
    // Choose SymbolValue=10000 => buf = 0x7FFF ^ 10000 = 0x58EF, packed into first 15 bits.
    {
        const uint8_t data[] = {0xB1, 0xDE};
        Av1SymbolDecoder sd;
        char err[256] = {0};
        uint16_t cdf[4];
        memcpy(cdf, cdf_base, sizeof(cdf));
        CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));
        uint32_t sym;
        CHECK(av1_symbol_read_symbol(&sd, cdf, 3, &sym, err, sizeof(err)));
        CHECK(sym == 1);
    }

    // Case C: SymbolValue low => expect symbol 2.
    // Choose SymbolValue=5000 => buf = 0x7FFF ^ 5000 = 0x6C77.
    {
        const uint8_t data[] = {0xD8, 0xEE};
        Av1SymbolDecoder sd;
        char err[256] = {0};
        uint16_t cdf[4];
        memcpy(cdf, cdf_base, sizeof(cdf));
        CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));
        uint32_t sym;
        CHECK(av1_symbol_read_symbol(&sd, cdf, 3, &sym, err, sizeof(err)));
        CHECK(sym == 2);
    }

    return 0;
}

static int test_read_symbol_cdf_update_kat(void) {
    // Known-answer test for the CDF update step after decoding symbol 0 once.
    // With cdf_count=0, N=3 => rate = 3 + 0 + 0 + min(floorlog2(3),2)=4.
    // For symbol=0:
    //   cdf[0] moves toward 1<<15 by (32768-16384)>>4 = 1024 => 17408
    //   cdf[1] moves toward 1<<15 by (32768-24576)>>4 = 512  => 25088
    //   cdf[3] increments to 1

    const uint8_t data[] = {0x00, 0x00}; // drives symbol 0 for the CDF used here
    Av1SymbolDecoder sd;
    char err[256] = {0};
    CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));

    uint16_t cdf[4];
    cdf[0] = 16384;
    cdf[1] = 24576;
    cdf[2] = 32768;
    cdf[3] = 0;

    uint32_t sym;
    CHECK(av1_symbol_read_symbol(&sd, cdf, 3, &sym, err, sizeof(err)));
    CHECK(sym == 0);
    CHECK(cdf[0] == 17408);
    CHECK(cdf[1] == 25088);
    CHECK(cdf[2] == 32768);
    CHECK(cdf[3] == 1);
    return 0;
}

static int test_read_symbol_kat_2sym(void) {
    // Two-symbol CDF behaves like a biased coin. With our chosen init_symbol inputs,
    // we can force SymbolValue to extreme high/low and expect symbols 0/1.
    uint16_t cdf[3];
    cdf[0] = 1u << 14;
    cdf[1] = 1u << 15;
    cdf[2] = 0;

    // High SymbolValue => expect 0
    {
        const uint8_t data[] = {0x00, 0x00};
        Av1SymbolDecoder sd;
        char err[256] = {0};
        uint16_t cdf1[3];
        memcpy(cdf1, cdf, sizeof(cdf1));
        CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));
        uint32_t sym;
        CHECK(av1_symbol_read_symbol(&sd, cdf1, 2, &sym, err, sizeof(err)));
        CHECK(sym == 0);
    }

    // Low SymbolValue => expect 1
    {
        // Pick SymbolValue=0 => buf = 0x7FFF, packed into first 15 bits => 0xFF,0xFE.
        const uint8_t data[] = {0xFF, 0xFE};
        Av1SymbolDecoder sd;
        char err[256] = {0};
        uint16_t cdf1[3];
        memcpy(cdf1, cdf, sizeof(cdf1));
        CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));
        uint32_t sym;
        CHECK(av1_symbol_read_symbol(&sd, cdf1, 2, &sym, err, sizeof(err)));
        CHECK(sym == 1);
    }
    return 0;
}

static int test_exit_symbol_trailing_bits_fail(void) {
    // Same scenario as the ok test, but with trailing bit 0.
    const uint8_t data[] = {0x00, 0x00};
    Av1SymbolDecoder sd;
    char err[256] = {0};
    CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));
    CHECK(!av1_symbol_exit(&sd, err, sizeof(err)));
    return 0;
}

static int test_check_trailing_bits_ok(void) {
    // Smallest valid payload: trailing bit '1' at the start of the only byte.
    const uint8_t data1[] = {0x80};
    char err[256] = {0};
    CHECK(av1_symbol_check_trailing_bits(data1, sizeof(data1), err, sizeof(err)));

    // Valid: trailing '1' as the very last bit (no padding zeros needed).
    const uint8_t data2[] = {0x00, 0x01};
    memset(err, 0, sizeof(err));
    CHECK(av1_symbol_check_trailing_bits(data2, sizeof(data2), err, sizeof(err)));
    return 0;
}

static int test_check_trailing_bits_fail_when_last15_all_zero(void) {
    // Last 15 bits are all zero => no valid trailing marker.
    const uint8_t data[] = {0x80, 0x00, 0x00, 0x00};
    char err[256] = {0};
    CHECK(!av1_symbol_check_trailing_bits(data, sizeof(data), err, sizeof(err)));
    return 0;
}

static int test_disable_cdf_update_invariant(void) {
    // When disable_cdf_update is set, read_symbol must not modify the CDF (including the count).
    const uint8_t data[] = {0x00, 0x00};
    Av1SymbolDecoder sd;
    char err[256] = {0};
    CHECK(av1_symbol_init(&sd, data, sizeof(data), true, err, sizeof(err)));

    uint16_t cdf[4] = {16384, 24576, 32768, 7};
    uint16_t before[4];
    memcpy(before, cdf, sizeof(before));

    uint32_t sym;
    CHECK(av1_symbol_read_symbol(&sd, cdf, 3, &sym, err, sizeof(err)));
    CHECK(memcmp(cdf, before, sizeof(cdf)) == 0);
    return 0;
}

static int test_cdf_count_saturates_at_32(void) {
    // With sz=1, SymbolMaxBits starts negative, so the decoder uses implicit padding bits.
    // This lets us decode many symbols and verify the CDF count saturates at 32.
    const uint8_t data[] = {0x00};
    Av1SymbolDecoder sd;
    char err[256] = {0};
    CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));

    uint16_t cdf[3] = {1u << 14, 1u << 15, 0};
    for (int i = 0; i < 100; i++) {
        uint32_t sym;
        CHECK(av1_symbol_read_symbol(&sd, cdf, 2, &sym, err, sizeof(err)));
    }
    CHECK(cdf[2] == 32);
    return 0;
}

static int test_reject_invalid_cdf(void) {
    const uint8_t data[] = {0x00, 0x00};
    Av1SymbolDecoder sd;
    char err[256] = {0};
    CHECK(av1_symbol_init(&sd, data, sizeof(data), false, err, sizeof(err)));

    // cdf[n-1] must be 1<<15
    uint16_t bad[4] = {100, 200, 300, 0};
    uint32_t sym;
    CHECK(!av1_symbol_read_symbol(&sd, bad, 3, &sym, err, sizeof(err)));
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_bool_all_zero();
    rc |= test_bool_all_one();
    rc |= test_exit_symbol_trailing_bits_ok();
    rc |= test_exit_symbol_trailing_bits_fail();
    rc |= test_check_trailing_bits_ok();
    rc |= test_check_trailing_bits_fail_when_last15_all_zero();
    rc |= test_read_symbol_kat_3sym();
    rc |= test_read_symbol_cdf_update_kat();
    rc |= test_read_symbol_kat_2sym();
    rc |= test_disable_cdf_update_invariant();
    rc |= test_cdf_count_saturates_at_32();
    rc |= test_reject_invalid_cdf();
    if (rc == 0) {
        printf("symbol decoder tests: ok\n");
    }
    return rc;
}
