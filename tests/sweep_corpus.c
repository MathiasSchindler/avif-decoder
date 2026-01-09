#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// C-only third-party corpus sweep.
//
// Goal: replicate the spirit of tools/sweep_m2.py / sweep_m3a.py / sweep_m3b.py
// without Python or external tools.
//
// By default, this sweeps testFiles/** excluding testFiles/generated/**.
//
// Stages:
// - m2: avif_extract_av1
// - m3a: av1_parse (on extracted .av1)
// - m3b: av1_framehdr (on extracted .av1)
//
// Output: counts + a short list of failures/unsupported reasons.

#define MAX_PATH 4096
#define MAX_OUT (128u * 1024u)

typedef enum {
    STAGE_M2 = 2,
    STAGE_M3A = 3,
    STAGE_M3B = 4,
} Stage;

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
    fprintf(stderr, "Windows is not supported by this sweep tool yet.\n");
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

static bool has_suffix(const char *s, const char *suffix) {
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    return ls >= lf && memcmp(s + (ls - lf), suffix, lf) == 0;
}

static bool contains_component_generated(const char *path) {
    // Simple substring match is sufficient for our repo layout.
    return strstr(path, "/generated/") != NULL || strstr(path, "testFiles/generated/") != NULL;
}

static void join_path(char *dst, size_t cap, const char *a, const char *b) {
    if (!a || !*a) {
        snprintf(dst, cap, "%s", b);
        return;
    }
    size_t la = strlen(a);
    if (la && a[la - 1] == '/') {
        snprintf(dst, cap, "%s%s", a, b);
    } else {
        snprintf(dst, cap, "%s/%s", a, b);
    }
}

typedef struct {
    unsigned total_avif;
    unsigned extracted_ok;
    unsigned extracted_fail;
    unsigned stage_ok;
    unsigned stage_fail;
    unsigned stage_skip;

    // Optional m3b diagnostics.
    unsigned m3b_trailingbits_files_with_failures;
    unsigned m3b_trailingbits_failed_tiles;

    unsigned m3b_exitprobe_files_with_failures;
    unsigned m3b_exitprobe_failed_tiles;
    unsigned m3b_exitprobe_readbool_failed_tiles;

    // Optional m3b tile stats (parsed from av1_framehdr stdout).
    unsigned m3b_tilestats_files;
    unsigned m3b_tilestats_total_tiles;
    uint64_t m3b_tilestats_min_tile_size;
    uint64_t m3b_tilestats_max_tile_size;
    char m3b_tilestats_min_tile_file[MAX_PATH];

    unsigned m3b_tilestats_bucket_lt_256;
    unsigned m3b_tilestats_bucket_lt_1024;
    unsigned m3b_tilestats_bucket_lt_4096;
    unsigned m3b_tilestats_bucket_lt_16384;
    unsigned m3b_tilestats_bucket_lt_65536;
    unsigned m3b_tilestats_bucket_ge_65536;

    unsigned m3b_tilestats_tiles_1;
    unsigned m3b_tilestats_tiles_2_4;
    unsigned m3b_tilestats_tiles_5_16;
    unsigned m3b_tilestats_tiles_gt_16;
} Stats;

// Optional m3b diagnostics toggles (kept global for simplicity in this test tool).
static bool g_m3b_check_trailingbits = false;
static bool g_m3b_check_trailingbits_strict = false;
static bool g_m3b_exit_probe = false;
static bool g_m3b_exit_probe_strict = false;
static unsigned g_m3b_consume_bools = 0;
static bool g_m3b_tile_stats = false;

static void print_usage(FILE *out) {
    fprintf(out,
            "Usage: sweep_corpus [--stage m2|m3a|m3b] [--include-generated] [--limit N]\n"
            "                   [--m3b-check-trailingbits] [--m3b-check-trailingbits-strict]\n"
            "                   [--m3b-exit-probe] [--m3b-exit-probe-strict] [--m3b-consume-bools N]\n\n"
            "                   [--m3b-tile-stats]\n\n"
            "Sweeps testFiles/**/*.avif and runs our CLIs. Default excludes testFiles/generated.\n");
}

static void tilestats_bucket_add(Stats *st, uint64_t tile_size) {
    if (tile_size < 256) {
        st->m3b_tilestats_bucket_lt_256++;
    } else if (tile_size < 1024) {
        st->m3b_tilestats_bucket_lt_1024++;
    } else if (tile_size < 4096) {
        st->m3b_tilestats_bucket_lt_4096++;
    } else if (tile_size < 16384) {
        st->m3b_tilestats_bucket_lt_16384++;
    } else if (tile_size < 65536) {
        st->m3b_tilestats_bucket_lt_65536++;
    } else {
        st->m3b_tilestats_bucket_ge_65536++;
    }
}

static void tilestats_tiles_per_file_add(Stats *st, unsigned tiles_in_file) {
    if (tiles_in_file <= 1) {
        st->m3b_tilestats_tiles_1++;
    } else if (tiles_in_file <= 4) {
        st->m3b_tilestats_tiles_2_4++;
    } else if (tiles_in_file <= 16) {
        st->m3b_tilestats_tiles_5_16++;
    } else {
        st->m3b_tilestats_tiles_gt_16++;
    }
}

static void tilestats_parse_m3b_output(const char *avif_path, const RunResult *rr, Stats *st) {
    if (!rr || !rr->out) {
        return;
    }
    // Parse lines like:
    //   "tile[3] r0 c1: off=... size=1234"
    const char *p = rr->out;
    unsigned tiles_in_file = 0;
    while ((p = strstr(p, " size=")) != NULL) {
        uint64_t sz = 0;
        if (sscanf(p, " size=%" SCNu64, &sz) == 1) {
            tiles_in_file++;
            st->m3b_tilestats_total_tiles++;

            if (st->m3b_tilestats_files == 0 && st->m3b_tilestats_total_tiles == 1) {
                st->m3b_tilestats_min_tile_size = sz;
                st->m3b_tilestats_max_tile_size = sz;
                snprintf(st->m3b_tilestats_min_tile_file, sizeof(st->m3b_tilestats_min_tile_file), "%s", avif_path);
            } else {
                if (sz < st->m3b_tilestats_min_tile_size) {
                    st->m3b_tilestats_min_tile_size = sz;
                    snprintf(st->m3b_tilestats_min_tile_file, sizeof(st->m3b_tilestats_min_tile_file), "%s", avif_path);
                }
                if (sz > st->m3b_tilestats_max_tile_size) {
                    st->m3b_tilestats_max_tile_size = sz;
                }
            }

            tilestats_bucket_add(st, sz);
        }
        p += 6; // advance past " size=" to avoid infinite loops
    }

    st->m3b_tilestats_files++;
    tilestats_tiles_per_file_add(st, tiles_in_file);
}

static unsigned count_substr(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) {
        return 0;
    }
    unsigned n = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

static Stage parse_stage(const char *s) {
    if (strcmp(s, "m2") == 0) {
        return STAGE_M2;
    }
    if (strcmp(s, "m3a") == 0) {
        return STAGE_M3A;
    }
    if (strcmp(s, "m3b") == 0) {
        return STAGE_M3B;
    }
    return 0;
}

static void summarize_one_failure(const char *avif_path, const char *label, const RunResult *rr) {
    fprintf(stderr, "%s: %s failed (exit=%d)\n", avif_path, label, rr->exit_code);
    if (rr->out && rr->out_len) {
        // Print only first line-ish.
        const char *p = rr->out;
        size_t n = 0;
        while (p[n] && p[n] != '\n' && n < 200) {
            n++;
        }
        fprintf(stderr, "  out: %.*s\n", (int)n, p);
    }
}

static int sweep_one(const char *avif_path, Stage stage, unsigned index, Stats *st) {
    st->total_avif++;

    char tmp_av1[MAX_PATH];
#if defined(_WIN32)
    snprintf(tmp_av1, sizeof(tmp_av1), "build/_tmp_sweep_%u.av1", index);
#else
    snprintf(tmp_av1, sizeof(tmp_av1), "build/_tmp_sweep_%d_%u.av1", (int)getpid(), index);
#endif

    // m2 extract
    {
        char *argv[] = {"./build/avif_extract_av1", (char *)avif_path, tmp_av1, NULL};
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run avif_extract_av1\n", avif_path);
            return 1;
        }
        if (rr.exit_code != 0) {
            st->extracted_fail++;
            st->stage_skip++;
            // For most non-simple paths (grid/derived/etc) this is expected.
            rr_free(&rr);
            (void)remove(tmp_av1);
            return 0;
        }
        st->extracted_ok++;
        rr_free(&rr);
    }

    int rc = 0;
    if (stage == STAGE_M2) {
        st->stage_ok++;
        goto done;
    }

    if (stage == STAGE_M3A) {
        char *argv[] = {"./build/av1_parse", tmp_av1, NULL};
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run av1_parse\n", avif_path);
            rc = 1;
        } else if (rr.exit_code != 0) {
            st->stage_fail++;
            summarize_one_failure(avif_path, "m3a", &rr);
        } else {
            st->stage_ok++;
        }
        rr_free(&rr);
        goto done;
    }

    if (stage == STAGE_M3B) {
        // Optional: push toward real-world tile boundary validation.
        // If enabled, we ask av1_framehdr to check entropy trailing bits per tile and/or
        // run init/exit_symbol probing (optionally consuming N bool symbols first).

        char consume_buf[32];
        char *argv[12];
        int ai = 0;
        argv[ai++] = "./build/av1_framehdr";

        if (g_m3b_check_trailingbits) {
            argv[ai++] = g_m3b_check_trailingbits_strict ? "--check-tile-trailingbits-strict" : "--check-tile-trailingbits";
        }

        if (g_m3b_exit_probe) {
            argv[ai++] = g_m3b_exit_probe_strict ? "--check-tile-trailing-strict" : "--check-tile-trailing";
            if (g_m3b_consume_bools) {
                argv[ai++] = "--tile-consume-bools";
                snprintf(consume_buf, sizeof(consume_buf), "%u", g_m3b_consume_bools);
                argv[ai++] = consume_buf;
            }
        }

        argv[ai++] = tmp_av1;
        argv[ai] = NULL;
        RunResult rr;
        if (!run_capture(argv, &rr)) {
            fprintf(stderr, "%s: failed to run av1_framehdr\n", avif_path);
            rc = 1;
        } else if (rr.exit_code != 0) {
            st->stage_fail++;
            summarize_one_failure(avif_path, "m3b", &rr);
        } else {
            st->stage_ok++;

            if (g_m3b_check_trailingbits) {
                unsigned fails = count_substr(rr.out, "trailing-bits check FAILED");
                if (fails) {
                    st->m3b_trailingbits_files_with_failures++;
                    st->m3b_trailingbits_failed_tiles += fails;
                }
            }

            if (g_m3b_exit_probe) {
                unsigned exit_fails = count_substr(rr.out, "exit_symbol probe FAILED");
                unsigned rb_fails = count_substr(rr.out, "read_bool(");
                if (exit_fails || rb_fails) {
                    st->m3b_exitprobe_files_with_failures++;
                    st->m3b_exitprobe_failed_tiles += exit_fails;
                    st->m3b_exitprobe_readbool_failed_tiles += rb_fails;
                }
            }

            if (g_m3b_tile_stats) {
                tilestats_parse_m3b_output(avif_path, &rr, st);
            }
        }
        rr_free(&rr);
        goto done;
    }

done:
    (void)remove(tmp_av1);
    return rc;
}

static int walk_dir(const char *dir,
                    Stage stage,
                    bool include_generated,
                    unsigned *io_index,
                    unsigned limit,
                    Stats *st) {
#if defined(_WIN32)
    (void)dir;
    (void)stage;
    (void)include_generated;
    (void)io_index;
    (void)limit;
    (void)st;
    fprintf(stderr, "Windows is not supported by this sweep tool yet.\n");
    return 1;
#else
    DIR *dp = opendir(dir);
    if (!dp) {
        fprintf(stderr, "failed to open dir %s: %s\n", dir, strerror(errno));
        return 1;
    }

    int rc = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char path[MAX_PATH];
        join_path(path, sizeof(path), dir, ent->d_name);

        // Recurse into subdirs.
        if (ent->d_type == DT_DIR) {
            if (!include_generated && contains_component_generated(path)) {
                continue;
            }
            if (walk_dir(path, stage, include_generated, io_index, limit, st) != 0) {
                rc = 1;
            }
            continue;
        }

        // Some filesystems report DT_UNKNOWN; treat as file and filter by suffix.
        if (!has_suffix(ent->d_name, ".avif")) {
            continue;
        }
        if (!include_generated && contains_component_generated(path)) {
            continue;
        }

        if (limit && *io_index >= limit) {
            break;
        }

        unsigned idx = (*io_index)++;
        if (sweep_one(path, stage, idx, st) != 0) {
            rc = 1;
        }
    }

    closedir(dp);
    return rc;
#endif
}

int main(int argc, char **argv) {
    Stage stage = STAGE_M3B;
    bool include_generated = false;
    unsigned limit = 0;

    bool m3b_check_trailingbits = false;
    bool m3b_check_trailingbits_strict = false;
    bool m3b_exit_probe = false;
    bool m3b_exit_probe_strict = false;
    unsigned m3b_consume_bools = 0;
    bool m3b_tile_stats = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--include-generated")) {
            include_generated = true;
            continue;
        }
        if (!strcmp(argv[i], "--stage")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--stage requires m2|m3a|m3b\n");
                return 2;
            }
            stage = parse_stage(argv[++i]);
            if (!stage) {
                fprintf(stderr, "invalid --stage value\n");
                return 2;
            }
            continue;
        }
        if (!strcmp(argv[i], "--limit")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--limit requires N\n");
                return 2;
            }
            limit = (unsigned)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(argv[i], "--m3b-check-trailingbits")) {
            m3b_check_trailingbits = true;
            continue;
        }
        if (!strcmp(argv[i], "--m3b-check-trailingbits-strict")) {
            m3b_check_trailingbits = true;
            m3b_check_trailingbits_strict = true;
            continue;
        }
        if (!strcmp(argv[i], "--m3b-exit-probe")) {
            m3b_exit_probe = true;
            continue;
        }
        if (!strcmp(argv[i], "--m3b-exit-probe-strict")) {
            m3b_exit_probe = true;
            m3b_exit_probe_strict = true;
            continue;
        }
        if (!strcmp(argv[i], "--m3b-consume-bools")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--m3b-consume-bools requires N\n");
                return 2;
            }
            unsigned long v = strtoul(argv[++i], NULL, 10);
            if (v > 1000000ul) {
                fprintf(stderr, "invalid --m3b-consume-bools value\n");
                return 2;
            }
            m3b_consume_bools = (unsigned)v;
            continue;
        }
        if (!strcmp(argv[i], "--m3b-tile-stats")) {
            m3b_tile_stats = true;
            continue;
        }
        fprintf(stderr, "unexpected arg: %s\n", argv[i]);
        return 2;
    }

    // Publish the chosen settings.
    g_m3b_check_trailingbits = m3b_check_trailingbits;
    g_m3b_check_trailingbits_strict = m3b_check_trailingbits_strict;
    g_m3b_exit_probe = m3b_exit_probe;
    g_m3b_exit_probe_strict = m3b_exit_probe_strict;
    g_m3b_consume_bools = m3b_consume_bools;
    g_m3b_tile_stats = m3b_tile_stats;

    Stats st;
    memset(&st, 0, sizeof(st));

    unsigned index = 0;
    int rc = walk_dir("testFiles", stage, include_generated, &index, limit, &st);

    printf("sweep stage: %s\n", stage == STAGE_M2 ? "m2" : (stage == STAGE_M3A ? "m3a" : "m3b"));
    printf("avif files visited: %u\n", st.total_avif);
    printf("m2 extracted ok: %u\n", st.extracted_ok);
    printf("m2 extract failed/unsupported: %u\n", st.extracted_fail);
    if (stage != STAGE_M2) {
        printf("stage ok: %u\n", st.stage_ok);
        printf("stage failed: %u\n", st.stage_fail);
        printf("stage skipped (no extract): %u\n", st.stage_skip);

        if (stage == STAGE_M3B && m3b_check_trailingbits) {
            printf("m3b trailingbits: files with failures: %u\n", st.m3b_trailingbits_files_with_failures);
            printf("m3b trailingbits: failed tiles: %u\n", st.m3b_trailingbits_failed_tiles);
        }

        if (stage == STAGE_M3B && m3b_exit_probe) {
            printf("m3b exit-probe: consume bools: %u\n", m3b_consume_bools);
            printf("m3b exit-probe: files with failures: %u\n", st.m3b_exitprobe_files_with_failures);
            printf("m3b exit-probe: exit_symbol failed tiles: %u\n", st.m3b_exitprobe_failed_tiles);
            printf("m3b exit-probe: read_bool failed tiles: %u\n", st.m3b_exitprobe_readbool_failed_tiles);
        }

        if (stage == STAGE_M3B && m3b_tile_stats) {
            printf("m3b tile-stats: files scanned: %u\n", st.m3b_tilestats_files);
            printf("m3b tile-stats: total tiles: %u\n", st.m3b_tilestats_total_tiles);
            if (st.m3b_tilestats_total_tiles) {
                printf("m3b tile-stats: min tile size: %" PRIu64 " (file: %s)\n",
                       st.m3b_tilestats_min_tile_size,
                       st.m3b_tilestats_min_tile_file[0] ? st.m3b_tilestats_min_tile_file : "(unknown)");
                printf("m3b tile-stats: max tile size: %" PRIu64 "\n", st.m3b_tilestats_max_tile_size);
            }

            printf("m3b tile-stats: tile size buckets (#tiles)\n");
            printf("  <256: %u\n", st.m3b_tilestats_bucket_lt_256);
            printf("  <1KiB: %u\n", st.m3b_tilestats_bucket_lt_1024);
            printf("  <4KiB: %u\n", st.m3b_tilestats_bucket_lt_4096);
            printf("  <16KiB: %u\n", st.m3b_tilestats_bucket_lt_16384);
            printf("  <64KiB: %u\n", st.m3b_tilestats_bucket_lt_65536);
            printf("  >=64KiB: %u\n", st.m3b_tilestats_bucket_ge_65536);

            printf("m3b tile-stats: tiles per file (#files)\n");
            printf("  1: %u\n", st.m3b_tilestats_tiles_1);
            printf("  2-4: %u\n", st.m3b_tilestats_tiles_2_4);
            printf("  5-16: %u\n", st.m3b_tilestats_tiles_5_16);
            printf("  >16: %u\n", st.m3b_tilestats_tiles_gt_16);
        }
    }

    return rc ? 1 : 0;
}
