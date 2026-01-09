#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

// C-only micro-benchmark harness.
//
// Intended usage:
// - Compare our pipeline stages across a corpus.
// - Optionally compare against avifdec if available.
//
// Defaults:
// - corpus root: testFiles
// - excludes testFiles/generated
// - repeat: 1
// - bench stages: m2 + m3b (m3a optional)

#define MAX_PATH 4096

typedef struct {
    int exit_code;
} RunStatus;

#if defined(_WIN32)
static int64_t now_ns(void) { return 0; }
static bool run_no_capture(char *const argv[], RunStatus *st) {
    (void)argv;
    (void)st;
    fprintf(stderr, "Windows not supported\n");
    return false;
}
#else
static int64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static bool run_silent(char *const argv[], RunStatus *st) {
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        FILE *dn = fopen("/dev/null", "wb");
        if (dn) {
            (void)dup2(fileno(dn), STDOUT_FILENO);
            (void)dup2(fileno(dn), STDERR_FILENO);
            fclose(dn);
        }

        // Use execv for explicit paths (./build/...), and execvp for PATH lookups (avifdec).
        if (strchr(argv[0], '/')) {
            execv(argv[0], argv);
        } else {
            execvp(argv[0], argv);
        }
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return false;
    }

    int code = 1;
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        code = 128 + WTERMSIG(status);
    }

    st->exit_code = code;
    return true;
}
#endif

static bool has_suffix(const char *s, const char *suffix) {
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    return ls >= lf && memcmp(s + (ls - lf), suffix, lf) == 0;
}

static bool contains_component_generated(const char *path) {
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
    unsigned files;
    unsigned extracted_ok;
    unsigned extracted_fail;
    unsigned framehdr_ok;
    unsigned framehdr_fail;

    int64_t ns_m2;
    int64_t ns_m3b;
    int64_t ns_avifdec_info;
    int64_t ns_avifdec_decode;
} Bench;

static int bench_one(const char *avif_path,
                     unsigned index,
                     unsigned repeat,
                     const char *avifdec_path,
                     bool do_avifdec_info,
                     bool do_avifdec_decode,
                     Bench *b) {
    b->files++;

    char tmp_av1[MAX_PATH];
#if defined(_WIN32)
    snprintf(tmp_av1, sizeof(tmp_av1), "build/_tmp_bench_%u.av1", index);
#else
    snprintf(tmp_av1, sizeof(tmp_av1), "build/_tmp_bench_%d_%u.av1", (int)getpid(), index);
#endif

    char tmp_png[MAX_PATH];
#if defined(_WIN32)
    snprintf(tmp_png, sizeof(tmp_png), "build/_tmp_bench_%u.png", index);
#else
    snprintf(tmp_png, sizeof(tmp_png), "build/_tmp_bench_%d_%u.png", (int)getpid(), index);
#endif

    // Repeat per-file to smooth noise.
    for (unsigned r = 0; r < repeat; r++) {
        if (avifdec_path && do_avifdec_info) {
            char *argv[] = {(char *)avifdec_path, "--info", (char *)avif_path, NULL};
            RunStatus st;
            int64_t t0 = now_ns();
            if (!run_silent(argv, &st)) {
                return 1;
            }
            int64_t t1 = now_ns();
            b->ns_avifdec_info += (t1 - t0);
            // Ignore avifdec failures in perf harness; report via exit status at end if needed.
            (void)st;
        }

        if (avifdec_path && do_avifdec_decode) {
            char *argv[] = {(char *)avifdec_path, (char *)avif_path, tmp_png, NULL};
            RunStatus st;
            int64_t t0 = now_ns();
            if (!run_silent(argv, &st)) {
                return 1;
            }
            int64_t t1 = now_ns();
            b->ns_avifdec_decode += (t1 - t0);
            (void)remove(tmp_png);
            (void)st;
        }

        // m2
        {
            char *argv[] = {"./build/avif_extract_av1", (char *)avif_path, tmp_av1, NULL};
            RunStatus st;
            int64_t t0 = now_ns();
            if (!run_silent(argv, &st)) {
                return 1;
            }
            int64_t t1 = now_ns();
            b->ns_m2 += (t1 - t0);
            if (st.exit_code != 0) {
                b->extracted_fail++;
                (void)remove(tmp_av1);
                return 0; // unsupported path, skip downstream
            }
        }

        // m3b framehdr
        {
            char *argv[] = {"./build/av1_framehdr", tmp_av1, NULL};
            RunStatus st;
            int64_t t0 = now_ns();
            if (!run_silent(argv, &st)) {
                return 1;
            }
            int64_t t1 = now_ns();
            b->ns_m3b += (t1 - t0);
            if (st.exit_code != 0) {
                b->framehdr_fail++;
                (void)remove(tmp_av1);
                return 0;
            }
        }

        (void)remove(tmp_av1);
    }

    b->extracted_ok++;
    b->framehdr_ok++;
    return 0;
}

static int walk_dir(const char *dir,
                    bool include_generated,
                    unsigned *io_index,
                    unsigned limit,
                    unsigned repeat,
                    const char *avifdec_path,
                    bool do_avifdec_info,
                    bool do_avifdec_decode,
                    Bench *b) {
#if defined(_WIN32)
    (void)dir;
    (void)include_generated;
    (void)io_index;
    (void)limit;
    (void)repeat;
    (void)b;
    fprintf(stderr, "Windows is not supported by this bench tool yet.\n");
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

        if (ent->d_type == DT_DIR) {
            if (!include_generated && contains_component_generated(path)) {
                continue;
            }
            if (walk_dir(path,
                         include_generated,
                         io_index,
                         limit,
                         repeat,
                         avifdec_path,
                         do_avifdec_info,
                         do_avifdec_decode,
                         b) != 0) {
                rc = 1;
            }
            continue;
        }

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
        if (bench_one(path, idx, repeat, avifdec_path, do_avifdec_info, do_avifdec_decode, b) != 0) {
            rc = 1;
        }
    }

    closedir(dp);
    return rc;
#endif
}

static void print_usage(FILE *out) {
    fprintf(out,
            "Usage: bench [--include-generated] [--limit N] [--repeat N] [--avifdec PATH] [--avifdec-info] [--avifdec-decode]\n\n"
            "Benchmarks m2 extraction and m3b framehdr across testFiles/**/*.avif.\n"
            "When --avifdec is provided, you can also time avifdec --info and/or decode.\n");
}

int main(int argc, char **argv) {
    bool include_generated = false;
    unsigned limit = 0;
    unsigned repeat = 1;
    const char *avifdec_path = NULL;
    bool do_avifdec_info = false;
    bool do_avifdec_decode = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--include-generated")) {
            include_generated = true;
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
        if (!strcmp(argv[i], "--repeat")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--repeat requires N\n");
                return 2;
            }
            repeat = (unsigned)strtoul(argv[++i], NULL, 10);
            if (repeat == 0) {
                repeat = 1;
            }
            continue;
        }
        if (!strcmp(argv[i], "--avifdec")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--avifdec requires PATH (or 'avifdec' to use PATH)\n");
                return 2;
            }
            avifdec_path = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--avifdec-info")) {
            do_avifdec_info = true;
            continue;
        }
        if (!strcmp(argv[i], "--avifdec-decode")) {
            do_avifdec_decode = true;
            continue;
        }
        fprintf(stderr, "unexpected arg: %s\n", argv[i]);
        return 2;
    }

    Bench b;
    memset(&b, 0, sizeof(b));

    unsigned index = 0;
    int rc = walk_dir("testFiles",
                      include_generated,
                      &index,
                      limit,
                      repeat,
                      avifdec_path,
                      do_avifdec_info,
                      do_avifdec_decode,
                      &b);

    double ms_m2 = (double)b.ns_m2 / 1e6;
    double ms_m3b = (double)b.ns_m3b / 1e6;
    double ms_avifdec_info = (double)b.ns_avifdec_info / 1e6;
    double ms_avifdec_decode = (double)b.ns_avifdec_decode / 1e6;

    printf("bench files visited: %u\n", b.files);
    printf("repeat: %u\n", repeat);
    printf("m2 extracted ok: %u\n", b.extracted_ok);
    printf("m2 extract failed/unsupported: %u\n", b.extracted_fail);
    printf("m3b ok: %u\n", b.framehdr_ok);
    printf("m3b failed: %u\n", b.framehdr_fail);
    printf("timing total: m2=%.2fms m3b=%.2fms\n", ms_m2, ms_m3b);
    if (avifdec_path && do_avifdec_info) {
        printf("timing total: avifdec --info=%.2fms\n", ms_avifdec_info);
    }
    if (avifdec_path && do_avifdec_decode) {
        printf("timing total: avifdec decode=%.2fms\n", ms_avifdec_decode);
    }
    if (b.files) {
        printf("timing per-file (avg): m2=%.3fms m3b=%.3fms\n", ms_m2 / (double)b.files, ms_m3b / (double)b.files);
        if (avifdec_path && do_avifdec_info) {
            printf("timing per-file (avg): avifdec --info=%.3fms\n", ms_avifdec_info / (double)b.files);
        }
        if (avifdec_path && do_avifdec_decode) {
            printf("timing per-file (avg): avifdec decode=%.3fms\n", ms_avifdec_decode / (double)b.files);
        }
    }

    return rc ? 1 : 0;
}
