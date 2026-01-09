#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// C-only generated-vector verifier.
//
// Goals:
// - No Python, no external tools (no avifdec).
// - Use only our built CLIs plus the generated manifest.
// - Keep checks deterministic and spec-driven where possible.
//
// Checks per generated AVIF (from testFiles/generated/manifest.txt):
// - m0: avif_boxdump does not crash
// - m1: avif_metadump does not crash + pixi matches bit depth/channels derived from av1C
// - m2: avif_extract_av1 succeeds
// - m3a: av1_parse succeeds
// - m3b-step1: av1_framehdr succeeds and reports dimensions matching manifest

#define MAX_OUT (128u * 1024u)

typedef struct {
    int exit_code;
    char *out;      // malloc'd
    size_t out_len; // bytes
} RunResult;

static bool g_saw_tx_mode_select = false;
static bool g_saw_tx_mode_select_tx_depth = false;
static bool g_saw_tx_mode_select_tx_size = false;
static bool g_saw_block0_tx_type = false;
static bool g_saw_block0_txb_skip = false;
static bool g_saw_block0_tx1_txb_skip = false;
static bool g_saw_block0_eob_pt = false;
static bool g_saw_block0_eob = false;
static bool g_saw_block0_coeff_base_eob = false;
static bool g_saw_block0_coeff_base = false;
static bool g_saw_block0_coeff_br = false;
static bool g_saw_block0_dc_sign = false;
static bool g_saw_block1_txb_skip = false;
static bool g_saw_block1_txb_skip_ctx_nonzero = false;
static bool g_saw_block1_eob_pt = false;
static bool g_saw_block1_eob = false;
static bool g_saw_block1_coeff_base_eob = false;
static bool g_saw_block1_coeff_base = false;

static void rr_free(RunResult *r) {
    if (!r) {
        return;
    }
    free(r->out);
    r->out = NULL;
    r->out_len = 0;
    r->exit_code = -1;
}

static bool append_bytes(char **buf, size_t *len, size_t *cap, const char *src, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t new_cap = (*cap == 0) ? 4096 : *cap;
        while (*len + n + 1 > new_cap) {
            new_cap *= 2;
            if (new_cap > MAX_OUT) {
                new_cap = MAX_OUT;
                break;
            }
        }
        if (*len + n + 1 > new_cap) {
            // Would exceed MAX_OUT.
            n = (new_cap > *len + 1) ? (new_cap - *len - 1) : 0;
        }
        if (new_cap != *cap) {
            char *p = (char *)realloc(*buf, new_cap);
            if (!p) {
                return false;
            }
            *buf = p;
            *cap = new_cap;
        }
    }
    if (n) {
        memcpy(*buf + *len, src, n);
        *len += n;
        (*buf)[*len] = 0;
    } else if (*buf) {
        (*buf)[*len] = 0;
    }
    return true;
}

static bool files_equal(const char *a_path, const char *b_path, char *err, size_t err_cap) {
    FILE *a = fopen(a_path, "rb");
    if (!a) {
        snprintf(err, err_cap, "open %s failed: %s", a_path, strerror(errno));
        return false;
    }
    FILE *b = fopen(b_path, "rb");
    if (!b) {
        snprintf(err, err_cap, "open %s failed: %s", b_path, strerror(errno));
        fclose(a);
        return false;
    }

    bool same = true;
    uint8_t buf_a[64 * 1024];
    uint8_t buf_b[64 * 1024];

    for (;;) {
        size_t na = fread(buf_a, 1, sizeof(buf_a), a);
        size_t nb = fread(buf_b, 1, sizeof(buf_b), b);

        if (na != nb || memcmp(buf_a, buf_b, na) != 0) {
            same = false;
            break;
        }
        if (na == 0) {
            break;
        }
    }

    if (ferror(a) || ferror(b)) {
        snprintf(err, err_cap, "file read error");
        same = false;
    }

    fclose(a);
    fclose(b);
    if (!same && err[0] == 0) {
        snprintf(err, err_cap, "files differ");
    }
    return same;
}

#if defined(_WIN32)
static bool run_capture(char *const argv[], RunResult *out) {
    (void)argv;
    (void)out;
    fprintf(stderr, "Windows is not supported by this verifier yet.\n");
    return false;
}
#else
static bool run_capture(char *const argv[], RunResult *out) {
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        // Child
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execv(argv[0], argv);
        _exit(127);
    }

    // Parent
    close(pipefd[1]);
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    char tmp[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (!append_bytes(&buf, &len, &cap, tmp, (size_t)n)) {
            free(buf);
            buf = NULL;
            len = 0;
            cap = 0;
            break;
        }
        if (cap >= MAX_OUT && len + 1 >= cap) {
            // Truncated
            break;
        }
    }
    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        rr_free(out);
        free(buf);
        return false;
    }

    int code = 1;
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        code = 128 + WTERMSIG(status);
    }

    out->exit_code = code;
    out->out = buf ? buf : strdup("");
    out->out_len = buf ? len : 0;
    return out->out != NULL;
}
#endif

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    return s;
}

static bool parse_u32_after(const char *hay, const char *needle, uint32_t *out) {
    const char *p = strstr(hay, needle);
    if (!p) {
        return false;
    }
    p += strlen(needle);
    p = skip_ws(p);
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p || v > 0xFFFFFFFFu) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool str_contains(const char *hay, const char *needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

static bool parse_av1c_fields(const char *text,
                             uint32_t *profile,
                             uint32_t *hb,
                             uint32_t *tb,
                             uint32_t *mono) {
    // Example token in avif_metadump output:
    // av1C(profile=1 level=0 tier=0 hb=0 tb=0 mono=0 subsamp=00)
    const char *p = strstr(text, "av1C(");
    if (!p) {
        return false;
    }
    if (!parse_u32_after(p, "profile=", profile)) {
        return false;
    }
    if (!parse_u32_after(p, "hb=", hb)) {
        return false;
    }
    if (!parse_u32_after(p, "tb=", tb)) {
        return false;
    }
    if (!parse_u32_after(p, "mono=", mono)) {
        return false;
    }
    return true;
}

static uint32_t av1c_bit_depth(uint32_t profile, uint32_t hb, uint32_t tb) {
    if (!hb) {
        return 8;
    }
    if (profile == 2) {
        return tb ? 12 : 10;
    }
    // profile 0/1
    return 10;
}

static bool parse_pixi(const char *text, uint32_t *channels, uint32_t depths[4], size_t *depth_count) {
    // Example:
    // pixi_channels=3 pixi_depths=8,8,8
    const char *p = strstr(text, "pixi_channels=");
    if (!p) {
        return false;
    }
    if (!parse_u32_after(p, "pixi_channels=", channels)) {
        return false;
    }

    const char *q = strstr(p, "pixi_depths=");
    if (!q) {
        return false;
    }
    q += strlen("pixi_depths=");

    size_t n = 0;
    while (*q && *q != '\n' && *q != ' ' && n < 4) {
        char *end = NULL;
        unsigned long v = strtoul(q, &end, 10);
        if (end == q || v > 0xFFFFFFFFu) {
            break;
        }
        depths[n++] = (uint32_t)v;
        q = end;
        if (*q == ',') {
            q++;
        } else {
            break;
        }
    }

    if (n == 0) {
        return false;
    }
    *depth_count = n;
    return true;
}

static bool parse_framehdr_dims(const char *text, uint32_t *w, uint32_t *h) {
    // Prefer upscaled_width, but accept frame_width.
    uint32_t uw = 0;
    uint32_t fw = 0;
    uint32_t fh = 0;
    bool has_h = parse_u32_after(text, "frame_height=", &fh);
    bool has_uw = parse_u32_after(text, "upscaled_width=", &uw);
    bool has_fw = parse_u32_after(text, "frame_width=", &fw);
    if (!has_h || (!has_uw && !has_fw)) {
        return false;
    }
    *w = has_uw ? uw : fw;
    *h = fh;
    return true;
}

static void build_avif_path(char *dst, size_t cap, const char *base, const char *preset) {
    if (strcmp(preset, "lossless") == 0) {
        snprintf(dst, cap, "testFiles/generated/avif/%s.avif", base);
    } else {
        snprintf(dst, cap, "testFiles/generated/avif/%s__%s.avif", base, preset);
    }
}

static int verify_one(const char *base, const char *preset, uint32_t want_w, uint32_t want_h, unsigned index) {
    char avif_path[512];
    build_avif_path(avif_path, sizeof(avif_path), base, preset);

    char tmp_av1[512];
    snprintf(tmp_av1, sizeof(tmp_av1), "build/_tmp_verify_%d_%u.av1", (int)getpid(), index);

    char tmp_av1_m1[512];
    snprintf(tmp_av1_m1, sizeof(tmp_av1_m1), "build/_tmp_verify_%d_%u.m1.av1", (int)getpid(), index);

    int failures = 0;

    // m0: box walker should not crash.
    {
        char *argv[] = {"./build/avif_boxdump", "--max-depth", "2", avif_path, NULL};
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run avif_boxdump\n", avif_path);
            return 1;
        }
        if (rr.exit_code != 0) {
            fprintf(stderr, "%s: m0 failed (exit=%d)\n", avif_path, rr.exit_code);
            failures++;
        }
        rr_free(&rr);
    }

    // m1: metadump should not crash; pixi must match av1C-derived bit depth + channels.
    {
        char *argv[] = {"./build/avif_metadump", (char *)avif_path, NULL};
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run avif_metadump\n", avif_path);
            return 1;
        }
        if (rr.exit_code != 0) {
            fprintf(stderr, "%s: m1 failed (exit=%d)\n", avif_path, rr.exit_code);
            failures++;
        } else {
            uint32_t profile = 0, hb = 0, tb = 0, mono = 0;
            uint32_t pixi_channels = 0;
            uint32_t pixi_depths[4] = {0};
            size_t pixi_n = 0;
            bool has_av1c = parse_av1c_fields(rr.out, &profile, &hb, &tb, &mono);
            bool has_pixi = parse_pixi(rr.out, &pixi_channels, pixi_depths, &pixi_n);
            if (!has_av1c || !has_pixi) {
                fprintf(stderr, "%s: m1 parse missing av1C or pixi in output\n", avif_path);
                failures++;
            } else {
                uint32_t want_depth = av1c_bit_depth(profile, hb, tb);
                uint32_t want_channels = mono ? 1u : 3u;
                if (pixi_channels != want_channels) {
                    fprintf(stderr,
                            "%s: pixi_channels mismatch (pixi=%u, av1C-derived=%u)\n",
                            avif_path,
                            pixi_channels,
                            want_channels);
                    failures++;
                }
                // Some encoders may emit depths for fewer channels; enforce that the ones present match.
                for (size_t i = 0; i < pixi_n; i++) {
                    if (pixi_depths[i] != want_depth) {
                        fprintf(stderr,
                                "%s: pixi_depths[%zu] mismatch (pixi=%u, av1C-derived=%u)\n",
                                avif_path,
                                i,
                                pixi_depths[i],
                                want_depth);
                        failures++;
                        break;
                    }
                }
            }
        }
        rr_free(&rr);
    }

    // m1: extraction should produce byte-identical AV1 sample for generated vectors.
    {
        char *argv[] = {"./build/avif_metadump", "--extract-primary", tmp_av1_m1, (char *)avif_path, NULL};
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run avif_metadump --extract-primary\n", avif_path);
            return 1;
        }
        if (rr.exit_code != 0) {
            fprintf(stderr, "%s: m1 extract failed (exit=%d)\n", avif_path, rr.exit_code);
            failures++;
        }
        rr_free(&rr);
    }

    // m2: extract primary av1 sample.
    {
        char *argv[] = {"./build/avif_extract_av1", (char *)avif_path, tmp_av1, NULL};
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run avif_extract_av1\n", avif_path);
            return 1;
        }
        if (rr.exit_code != 0) {
            fprintf(stderr, "%s: m2 extract failed (exit=%d)\n", avif_path, rr.exit_code);
            failures++;
        }
        rr_free(&rr);
    }

    // m1 vs m2: extracted bytes must match for generated vectors.
    {
        char cmp_err[256] = {0};
        if (!files_equal(tmp_av1_m1, tmp_av1, cmp_err, sizeof(cmp_err))) {
            fprintf(stderr, "%s: m1 vs m2 extract mismatch: %s\n", avif_path, cmp_err);
            failures++;
        }
    }

    // m3a: parse should succeed.
    {
        char *argv[] = {"./build/av1_parse", tmp_av1, NULL};
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run av1_parse\n", avif_path);
            return 1;
        }
        if (rr.exit_code != 0) {
            fprintf(stderr, "%s: m3a parse failed (exit=%d)\n", avif_path, rr.exit_code);
            failures++;
        }
        rr_free(&rr);
    }

    // m3b-step1: frame header should succeed and dimensions should match manifest.
    {
        char *argv[] = {"./build/av1_framehdr", "--check-tile-trailingbits-strict", tmp_av1, NULL};
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run av1_framehdr\n", avif_path);
            return 1;
        }
        if (rr.exit_code != 0) {
            fprintf(stderr, "%s: m3b-step1 failed (exit=%d)\n", avif_path, rr.exit_code);
            failures++;
        } else {
            uint32_t got_w = 0, got_h = 0;
            if (!parse_framehdr_dims(rr.out, &got_w, &got_h)) {
                fprintf(stderr, "%s: failed to parse av1_framehdr dimensions\n", avif_path);
                failures++;
            } else if (got_w != want_w || got_h != want_h) {
                fprintf(stderr,
                        "%s: dimension mismatch (m3b=%ux%u, manifest=%ux%u)\n",
                        avif_path,
                        got_w,
                        got_h,
                        want_w,
                        want_h);
                failures++;
            }
        }
        rr_free(&rr);
    }

    // m3b tile-syntax probe (smoke): should not crash on generated vectors.
    // Note: expected to report UNSUPPORTED until full decode is implemented.
    {
        char *argv[] = {"./build/av1_framehdr", tmp_av1, "--decode-tile-syntax", NULL};
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run av1_framehdr --decode-tile-syntax\n", avif_path);
            return 1;
        }
        if (rr.exit_code != 0) {
            fprintf(stderr, "%s: m3b decode-tile-syntax smoke failed (exit=%d)\n", avif_path, rr.exit_code);
            failures++;
        } else {
            // Test 1: ensure the probe actually ran and reported its expected (currently
            // unsupported) state.
            if (!str_contains(rr.out, "decode-tile-syntax")) {
                fprintf(stderr, "%s: m3b decode-tile-syntax produced no probe output\n", avif_path);
                failures++;
            }
            if (!str_contains(rr.out, "decode-tile-syntax UNSUPPORTED")) {
                fprintf(stderr, "%s: m3b decode-tile-syntax did not report UNSUPPORTED as expected\n", avif_path);
                failures++;
            }

            // Test 2: assert we reached the partition traversal milestone (root partition
            // decision is reported) and intentionally stopped early.
            if (!str_contains(rr.out, "root_part=") || !str_contains(rr.out, "stopped=")) {
                fprintf(stderr, "%s: m3b decode-tile-syntax missing required markers (root_part/stopped)\n", avif_path);
                failures++;
            }

            // Test 3 (suite-level): ensure we exercise TxMode==TX_MODE_SELECT in at
            // least one generated vector, and that the probe reports concrete tx_depth
            // + tx_size for it.
            if (str_contains(rr.out, "tx_mode=2")) {
                g_saw_tx_mode_select = true;
                if (str_contains(rr.out, "tx_depth=") && !str_contains(rr.out, "tx_depth=n/a")) {
                    g_saw_tx_mode_select_tx_depth = true;
                }
                if (str_contains(rr.out, "tx_size=") && !str_contains(rr.out, "tx_size=n/a")) {
                    g_saw_tx_mode_select_tx_size = true;
                }
            }

            // Test 4 (suite-level): ensure we decode the first coeff-related symbol
            // (all_zero / txb_skip) for at least one generated vector.
            if (str_contains(rr.out, "txb_skip=") && !str_contains(rr.out, "txb_skip=n/a")) {
                g_saw_block0_txb_skip = true;
            }

            // Test 4b (suite-level): ensure we also decode txb_skip for a second luma
            // transform block within block0 at least once.
            if (str_contains(rr.out, "block0_tx1_txb_skip=") && !str_contains(rr.out, "block0_tx1_txb_skip=n/a")) {
                g_saw_block0_tx1_txb_skip = true;
            }

            // Test 4c (suite-level): ensure we decode txb_skip for a *second* block
            // in at least one generated vector (exercises persistent coeff contexts).
            if (str_contains(rr.out, "block1_txb_skip=") && !str_contains(rr.out, "block1_txb_skip=n/a")) {
                g_saw_block1_txb_skip = true;
                uint32_t ctx = 0;
                if (parse_u32_after(rr.out, "block1_txb_skip_ctx=", &ctx) && ctx != 0u) {
                    g_saw_block1_txb_skip_ctx_nonzero = true;
                }
            }

            // Test 4d (suite-level): ensure we reach the next coeff milestone for block1
            // (eob_pt) at least once.
            if (str_contains(rr.out, "block1_eob_pt=") && !str_contains(rr.out, "block1_eob_pt=n/a")) {
                g_saw_block1_eob_pt = true;
            }

            // Test 4e (suite-level): ensure we also derive block1 eob at least once.
            if (str_contains(rr.out, "block1_eob=") && !str_contains(rr.out, "block1_eob=n/a")) {
                g_saw_block1_eob = true;
            }

            // Test 4f (suite-level): ensure we decode block1 coeff_base_eob at least once.
            if (str_contains(rr.out, "block1_coeff_base_eob=") && !str_contains(rr.out, "block1_coeff_base_eob=n/a")) {
                g_saw_block1_coeff_base_eob = true;
            }

            // Test 4g (suite-level): attempt to reach the next milestone (block1 coeff_base).
            // This may depend on eob>=2 occurring in block1 in the generated corpus.
            if (str_contains(rr.out, "block1_coeff_base=") && !str_contains(rr.out, "block1_coeff_base=n/a")) {
                g_saw_block1_coeff_base = true;
            }

            // Test 4b (suite-level): ensure we decode transform_type (tx_type) for at
            // least one vector (requires tx set allows it and base_q_idx>0).
            if (str_contains(rr.out, "tx_type=") && !str_contains(rr.out, "tx_type=n/a")) {
                g_saw_block0_tx_type = true;
            }

            // Test 5 (suite-level): ensure we decode the next coeff symbol (eob_pt)
            // for at least one generated vector.
            if (str_contains(rr.out, "eob_pt=") && !str_contains(rr.out, "eob_pt=n/a")) {
                g_saw_block0_eob_pt = true;
            }

            // Test 6 (suite-level): ensure we decode the derived eob for at least
            // one generated vector.
            if (str_contains(rr.out, "eob=") && !str_contains(rr.out, "eob=n/a")) {
                g_saw_block0_eob = true;
            }

            // Test 7 (suite-level): ensure we decode coeff_base_eob (last non-zero
            // coeff base level) for at least one generated vector.
            if (str_contains(rr.out, "coeff_base_eob=") && !str_contains(rr.out, "coeff_base_eob=n/a")) {
                g_saw_block0_coeff_base_eob = true;
            }

            // Test 8 (suite-level): ensure we decode at least one coeff_base symbol
            // (currently implemented for the minimal eob==2 case).
            if (str_contains(rr.out, "coeff_base=") && !str_contains(rr.out, "coeff_base=n/a")) {
                g_saw_block0_coeff_base = true;
            }

            // Test 9 (suite-level): ensure we decode coeff_br for at least one vector.
            if (str_contains(rr.out, "coeff_br=") && !str_contains(rr.out, "coeff_br=n/a")) {
                g_saw_block0_coeff_br = true;
            }

            // Test 10 (suite-level): ensure we decode dc_sign for at least one vector.
            if (str_contains(rr.out, "dc_sign=") && !str_contains(rr.out, "dc_sign=n/a")) {
                g_saw_block0_dc_sign = true;
            }
        }
        rr_free(&rr);
    }

    (void)remove(tmp_av1);
    (void)remove(tmp_av1_m1);

    return failures ? 1 : 0;
}

int main(void) {
    FILE *f = fopen("testFiles/generated/manifest.txt", "rb");
    if (!f) {
        fprintf(stderr, "failed to open testFiles/generated/manifest.txt: %s\n", strerror(errno));
        return 1;
    }

    unsigned total = 0;
    unsigned failed = 0;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            continue;
        }
        char base[128] = {0};
        char preset[32] = {0};
        uint32_t w = 0, h = 0;
        char sha_src[80] = {0}, sha_avif[80] = {0}, sha_oracle[80] = {0};

        int n = sscanf(line, "%127s %31s %u %u %79s %79s %79s", base, preset, &w, &h, sha_src, sha_avif, sha_oracle);
        if (n == EOF || n == 0) {
            continue;
        }
        if (n != 7) {
            fprintf(stderr, "malformed manifest line: %s", line);
            fclose(f);
            return 1;
        }

        int ok = verify_one(base, preset, w, h, total);
        total++;
        if (ok != 0) {
            failed++;
        }
    }

    fclose(f);

    printf("generated vectors checked: %u\n", total);
    printf("passed: %u\n", total - failed);
    printf("failures: %u\n", failed);

    if (!g_saw_tx_mode_select) {
        fprintf(stderr, "expected at least one vector with tx_mode=2 (TX_MODE_SELECT), but saw none\n");
        return 1;
    }
    if (!g_saw_tx_mode_select_tx_depth) {
        fprintf(stderr, "expected at least one tx_mode=2 vector with decoded tx_depth, but saw none\n");
        return 1;
    }
    if (!g_saw_tx_mode_select_tx_size) {
        fprintf(stderr, "expected at least one tx_mode=2 vector with derived tx_size, but saw none\n");
        return 1;
    }
    if (!g_saw_block0_txb_skip) {
        fprintf(stderr, "expected at least one vector with decoded block0 txb_skip (all_zero), but saw none\n");
        return 1;
    }
    if (!g_saw_block0_tx1_txb_skip) {
        fprintf(stderr, "expected at least one vector with decoded block0_tx1_txb_skip (all_zero for 2nd tx block), but saw none\n");
        return 1;
    }
    if (!g_saw_block1_txb_skip) {
        fprintf(stderr, "expected at least one vector with decoded block1 txb_skip (all_zero), but saw none\n");
        return 1;
    }

    if (!g_saw_block1_eob_pt) {
        fprintf(stderr, "expected at least one vector with decoded block1 eob_pt, but saw none\n");
        return 1;
    }

    if (!g_saw_block1_eob) {
        fprintf(stderr, "expected at least one vector with decoded block1 eob, but saw none\n");
        return 1;
    }

    if (!g_saw_block1_coeff_base_eob) {
        fprintf(stderr, "expected at least one vector with decoded block1 coeff_base_eob, but saw none\n");
        return 1;
    }

    if (!g_saw_block1_coeff_base) {
        fprintf(stderr, "warning: did not observe block1_coeff_base in generated vectors\n");
    }

    // Soft check: we'd like to see a non-zero block1 txb_skip context at least
    // once, but allow a fully-zero corpus.
    if (!g_saw_block1_txb_skip_ctx_nonzero) {
        fprintf(stderr, "warning: did not observe a non-zero block1_txb_skip_ctx in generated vectors\n");
    }
    if (!g_saw_block0_tx_type) {
        fprintf(stderr, "expected at least one vector with decoded block0 tx_type (transform_type), but saw none\n");
        return 1;
    }
    if (!g_saw_block0_eob_pt) {
        fprintf(stderr, "expected at least one vector with decoded block0 eob_pt, but saw none\n");
        return 1;
    }
    if (!g_saw_block0_eob) {
        fprintf(stderr, "expected at least one vector with decoded block0 eob, but saw none\n");
        return 1;
    }
    if (!g_saw_block0_coeff_base_eob) {
        fprintf(stderr, "expected at least one vector with decoded block0 coeff_base_eob, but saw none\n");
        return 1;
    }

    if (!g_saw_block0_coeff_base) {
        fprintf(stderr, "expected at least one vector with decoded block0 coeff_base, but saw none\n");
        return 1;
    }

    if (!g_saw_block0_coeff_br) {
        fprintf(stderr, "expected at least one vector with decoded block0 coeff_br, but saw none\n");
        return 1;
    }

    if (!g_saw_block0_dc_sign) {
        fprintf(stderr, "expected at least one vector with decoded block0 dc_sign, but saw none\n");
        return 1;
    }

    return failed ? 1 : 0;
}
