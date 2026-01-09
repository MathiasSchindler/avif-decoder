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

// Differential metadata test against avifdec (libavif reference CLI).
//
// This is intentionally NOT a pixel-level decode comparison yet.
// It verifies that our interpretation of AVIF/AV1 metadata matches avifdec --info:
// - resolution
// - bit depth
// - chroma format (YUV420/422/444 or YUV400)
//
// It uses the deterministic generated manifest: testFiles/generated/manifest.txt

#define MAX_OUT (128u * 1024u)

typedef struct {
    int exit_code;
    char *out;      // malloc'd
    size_t out_len; // bytes
} RunResult;

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

#if defined(_WIN32)
static bool run_capture(char *const argv[], RunResult *out) {
    (void)argv;
    (void)out;
    fprintf(stderr, "Windows is not supported by this test yet.\n");
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
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execv(argv[0], argv);
        _exit(127);
    }

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

static bool parse_u32_after(const char *hay, const char *needle, uint32_t *out) {
    const char *p = strstr(hay, needle);
    if (!p) {
        return false;
    }
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p || v > 0xFFFFFFFFu) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parse_av1c_fields(const char *text,
                             uint32_t *profile,
                             uint32_t *hb,
                             uint32_t *tb,
                             uint32_t *mono,
                             uint32_t *subsamp_x,
                             uint32_t *subsamp_y) {
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

    const char *q = strstr(p, "subsamp=");
    if (!q) {
        return false;
    }
    q += strlen("subsamp=");
    // Expect two digits: 00, 10, 11.
    if (q[0] < '0' || q[0] > '1' || q[1] < '0' || q[1] > '1') {
        return false;
    }
    *subsamp_x = (uint32_t)(q[0] - '0');
    *subsamp_y = (uint32_t)(q[1] - '0');
    return true;
}

static uint32_t av1c_bit_depth(uint32_t profile, uint32_t hb, uint32_t tb) {
    if (!hb) {
        return 8;
    }
    if (profile == 2) {
        return tb ? 12 : 10;
    }
    return 10;
}

static const char *av1c_format_string(uint32_t mono, uint32_t subsamp_x, uint32_t subsamp_y) {
    if (mono) {
        return "YUV400";
    }
    if (subsamp_x == 0 && subsamp_y == 0) {
        return "YUV444";
    }
    if (subsamp_x == 1 && subsamp_y == 0) {
        return "YUV422";
    }
    if (subsamp_x == 1 && subsamp_y == 1) {
        return "YUV420";
    }
    return NULL;
}

static bool parse_avifdec_info(const char *text,
                              uint32_t *w,
                              uint32_t *h,
                              uint32_t *depth,
                              char *format,
                              size_t format_cap) {
    // Examples:
    //  * Resolution     : 16x16
    //  * Bit Depth      : 8
    //  * Format         : YUV444

    const char *p = strstr(text, "* Resolution");
    if (!p) {
        return false;
    }
    const char *colon = strchr(p, ':');
    if (!colon) {
        return false;
    }
    colon++;
    while (*colon == ' ' || *colon == '\t') {
        colon++;
    }
    unsigned ww = 0, hh = 0;
    if (sscanf(colon, "%ux%u", &ww, &hh) != 2) {
        return false;
    }

    uint32_t dd = 0;
    if (!parse_u32_after(text, "* Bit Depth", &dd)) {
        // parse_u32_after expects exact needle; use fallback:
        if (!parse_u32_after(text, "* Bit Depth      :", &dd) && !parse_u32_after(text, "Bit Depth", &dd)) {
            return false;
        }
    }

    const char *f = strstr(text, "* Format");
    if (!f) {
        return false;
    }
    const char *fcolon = strchr(f, ':');
    if (!fcolon) {
        return false;
    }
    fcolon++;
    while (*fcolon == ' ' || *fcolon == '\t') {
        fcolon++;
    }
    size_t n = 0;
    while (fcolon[n] && fcolon[n] != '\n' && fcolon[n] != '\r' && n + 1 < format_cap) {
        format[n] = fcolon[n];
        n++;
    }
    format[n] = 0;

    *w = (uint32_t)ww;
    *h = (uint32_t)hh;
    *depth = dd;
    return true;
}

static void build_avif_path(char *dst, size_t cap, const char *base, const char *preset) {
    if (strcmp(preset, "lossless") == 0) {
        snprintf(dst, cap, "testFiles/generated/avif/%s.avif", base);
    } else {
        snprintf(dst, cap, "testFiles/generated/avif/%s__%s.avif", base, preset);
    }
}

static int verify_one(const char *base, const char *preset, uint32_t want_w, uint32_t want_h) {
    char avif_path[512];
    build_avif_path(avif_path, sizeof(avif_path), base, preset);

    // 1) Extract AV1C-derived depth/format from our metadump.
    uint32_t profile = 0, hb = 0, tb = 0, mono = 0, sx = 0, sy = 0;
    uint32_t want_depth = 0;
    const char *want_fmt = NULL;
    {
        char *argv[] = {"./build/avif_metadump", (char *)avif_path, NULL};
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run avif_metadump\n", avif_path);
            return 1;
        }
        if (rr.exit_code != 0) {
            fprintf(stderr, "%s: avif_metadump failed (exit=%d)\n", avif_path, rr.exit_code);
            rr_free(&rr);
            return 1;
        }
        if (!parse_av1c_fields(rr.out, &profile, &hb, &tb, &mono, &sx, &sy)) {
            fprintf(stderr, "%s: failed to parse av1C fields from avif_metadump output\n", avif_path);
            rr_free(&rr);
            return 1;
        }
        rr_free(&rr);

        want_depth = av1c_bit_depth(profile, hb, tb);
        want_fmt = av1c_format_string(mono, sx, sy);
        if (!want_fmt) {
            fprintf(stderr, "%s: unsupported subsampling combo from av1C subsamp=%u%u\n", avif_path, sx, sy);
            return 1;
        }
    }

    // 2) Parse avifdec --info.
    {
        // Use execv, so find avifdec via PATH by shelling through /usr/bin/env.
        char *argv[] = {"/usr/bin/env", "avifdec", "--info", (char *)avif_path, NULL};
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run avifdec --info\n", avif_path);
            return 1;
        }
        if (rr.exit_code != 0) {
            fprintf(stderr, "%s: avifdec --info failed (exit=%d)\n", avif_path, rr.exit_code);
            if (rr.out && rr.out_len) {
                fprintf(stderr, "  out: %.200s\n", rr.out);
            }
            rr_free(&rr);
            return 1;
        }

        uint32_t got_w = 0, got_h = 0, got_depth = 0;
        char got_fmt[32];
        if (!parse_avifdec_info(rr.out, &got_w, &got_h, &got_depth, got_fmt, sizeof(got_fmt))) {
            fprintf(stderr, "%s: failed to parse avifdec --info output\n", avif_path);
            rr_free(&rr);
            return 1;
        }
        rr_free(&rr);

        int failures = 0;
        if (got_w != want_w || got_h != want_h) {
            fprintf(stderr,
                    "%s: resolution mismatch (avifdec=%ux%u, manifest=%ux%u)\n",
                    avif_path,
                    got_w,
                    got_h,
                    want_w,
                    want_h);
            failures++;
        }
        if (got_depth != want_depth) {
            fprintf(stderr,
                    "%s: bit depth mismatch (avifdec=%u, av1C-derived=%u)\n",
                    avif_path,
                    got_depth,
                    want_depth);
            failures++;
        }
        if (strcmp(got_fmt, want_fmt) != 0) {
            fprintf(stderr,
                    "%s: format mismatch (avifdec=%s, av1C-derived=%s)\n",
                    avif_path,
                    got_fmt,
                    want_fmt);
            failures++;
        }
        return failures ? 1 : 0;
    }
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

        (void)sha_src;
        (void)sha_avif;
        (void)sha_oracle;

        int ok = verify_one(base, preset, w, h);
        total++;
        if (ok != 0) {
            failed++;
        }
    }

    fclose(f);

    printf("avifdec info vectors checked: %u\n", total);
    printf("passed: %u\n", total - failed);
    printf("failures: %u\n", failed);

    return failed ? 1 : 0;
}
