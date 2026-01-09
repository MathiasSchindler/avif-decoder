#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *out) {
    fprintf(out,
            "Usage: avif_boxdump [--max-depth N] <file.avif>\n"
            "\n"
            "Dumps ISO-BMFF/HEIF box structure (sizes, offsets, types).\n"
            "m0 goal: robust container walking, not full semantics.\n");
}

static bool read_exact(FILE *f, void *buf, size_t n) {
    return fread(buf, 1, n, f) == n;
}

static uint32_t read_u32_be_from_buf(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static uint64_t read_u64_be_from_buf(const uint8_t b[8]) {
    return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) | ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
           ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) | ((uint64_t)b[6] << 8) | (uint64_t)b[7];
}

static bool file_seek(FILE *f, uint64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

static bool file_tell(FILE *f, uint64_t *out) {
#if defined(_WIN32)
    long long p = _ftelli64(f);
    if (p < 0) {
        return false;
    }
    *out = (uint64_t)p;
    return true;
#else
    off_t p = ftello(f);
    if (p < 0) {
        return false;
    }
    *out = (uint64_t)p;
    return true;
#endif
}

static bool get_file_size(FILE *f, uint64_t *out_size) {
    uint64_t cur;
    if (!file_tell(f, &cur)) {
        return false;
    }
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        return false;
    }
    long long end = _ftelli64(f);
    if (end < 0) {
        return false;
    }
    *out_size = (uint64_t)end;
    return _fseeki64(f, (long long)cur, SEEK_SET) == 0;
#else
    if (fseeko(f, 0, SEEK_END) != 0) {
        return false;
    }
    off_t end = ftello(f);
    if (end < 0) {
        return false;
    }
    *out_size = (uint64_t)end;
    return fseeko(f, (off_t)cur, SEEK_SET) == 0;
#endif
}

static void indent_print(int depth) {
    for (int i = 0; i < depth; i++) {
        fputs("  ", stdout);
    }
}

static bool type_equals(const char t[4], const char *lit4) {
    return memcmp(t, lit4, 4) == 0;
}

static bool is_container_box(const char type[4]) {
    // Only recurse into boxes whose payload is a simple list of child boxes.
    // Many ISOBMFF/HEIF boxes contain non-box fields (counts/arrays) before any children
    // (e.g. iinf, dref, stsd), and blindly recursing will mis-parse.
    static const char *const containers[] = {
        // Common ISOBMFF container structure.
        "moov", "trak", "mdia", "minf", "stbl", "edts", "udta",
        // Fragmented MP4 containers (may appear in sequences).
        "moof", "traf",
        // HEIF/AVIF.
        "meta", "iprp", "ipco",
    };
    for (size_t i = 0; i < sizeof(containers) / sizeof(containers[0]); i++) {
        if (type_equals(type, containers[i])) {
            return true;
        }
    }
    return false;
}

typedef struct {
    uint64_t offset;
    uint64_t size;
    char type[4];
    bool has_uuid;
    uint8_t uuid[16];
    uint64_t header_size;
    bool is_fullbox;
    uint8_t version;
    uint32_t flags;
} Box;

static void print_type(const char type[4]) {
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)type[i];
        if (c >= 32 && c <= 126) {
            fputc(c, stdout);
        } else {
            fputc('.', stdout);
        }
    }
}

static bool read_box_header(FILE *f, uint64_t file_size, uint64_t parent_end, Box *out_box, char *err, size_t err_cap) {
    memset(out_box, 0, sizeof(*out_box));

    uint64_t start;
    if (!file_tell(f, &start)) {
        snprintf(err, err_cap, "ftell failed: %s", strerror(errno));
        return false;
    }

    if (start + 8 > parent_end || start + 8 > file_size) {
        snprintf(err, err_cap, "truncated box header at offset=%" PRIu64, start);
        return false;
    }

    uint8_t header8[8];
    if (!read_exact(f, header8, sizeof(header8))) {
        snprintf(err, err_cap, "read failed at offset=%" PRIu64 ": %s", start, strerror(errno));
        return false;
    }

    uint32_t size32 = read_u32_be_from_buf(header8);
    memcpy(out_box->type, header8 + 4, 4);
    out_box->offset = start;
    out_box->header_size = 8;

    uint64_t box_size = 0;
    if (size32 == 0) {
        // Extends to end of file/parent.
        box_size = parent_end - start;
    } else if (size32 == 1) {
        if (start + 16 > parent_end || start + 16 > file_size) {
            snprintf(err, err_cap, "truncated largesize at offset=%" PRIu64, start);
            return false;
        }
        uint8_t size8[8];
        if (!read_exact(f, size8, sizeof(size8))) {
            snprintf(err, err_cap, "read largesize failed at offset=%" PRIu64, start);
            return false;
        }
        box_size = read_u64_be_from_buf(size8);
        out_box->header_size = 16;
    } else {
        box_size = (uint64_t)size32;
    }

    if (box_size < out_box->header_size) {
        snprintf(err, err_cap, "invalid box size=%" PRIu64 " < header_size=%" PRIu64 " at offset=%" PRIu64,
                 box_size, out_box->header_size, start);
        return false;
    }

    if (start + box_size > parent_end || start + box_size > file_size) {
        snprintf(err, err_cap, "box overruns parent/file: offset=%" PRIu64 " size=%" PRIu64, start, box_size);
        return false;
    }

    out_box->size = box_size;

    if (type_equals(out_box->type, "uuid")) {
        if (out_box->header_size + 16 > out_box->size) {
            snprintf(err, err_cap, "uuid box too small at offset=%" PRIu64, start);
            return false;
        }
        if (!read_exact(f, out_box->uuid, 16)) {
            snprintf(err, err_cap, "read uuid failed at offset=%" PRIu64, start);
            return false;
        }
        out_box->has_uuid = true;
        out_box->header_size += 16;
    }

    // meta is a FullBox in HEIF/ISOBMFF context (version/flags) before children.
    if (type_equals(out_box->type, "meta")) {
        if (out_box->header_size + 4 > out_box->size) {
            snprintf(err, err_cap, "meta box too small for FullBox fields at offset=%" PRIu64, start);
            return false;
        }
        uint8_t vf[4];
        if (!read_exact(f, vf, 4)) {
            snprintf(err, err_cap, "read meta FullBox failed at offset=%" PRIu64, start);
            return false;
        }
        out_box->is_fullbox = true;
        out_box->version = vf[0];
        out_box->flags = ((uint32_t)vf[1] << 16) | ((uint32_t)vf[2] << 8) | (uint32_t)vf[3];
        out_box->header_size += 4;
    }

    return true;
}

static bool dump_boxes(FILE *f, uint64_t file_size, uint64_t start_off, uint64_t end_off, int depth, int max_depth) {
    if (!file_seek(f, start_off)) {
        fprintf(stderr, "seek failed to offset=%" PRIu64 ": %s\n", start_off, strerror(errno));
        return false;
    }

    uint64_t cursor = start_off;
    while (cursor < end_off) {
        if (!file_seek(f, cursor)) {
            fprintf(stderr, "seek failed to offset=%" PRIu64 ": %s\n", cursor, strerror(errno));
            return false;
        }

        Box box;
        char err[256];
        if (!read_box_header(f, file_size, end_off, &box, err, sizeof(err))) {
            indent_print(depth);
            fprintf(stderr, "ERROR: %s\n", err);
            return false;
        }

        indent_print(depth);
        printf("[%" PRIu64 "+%" PRIu64 "] ", box.offset, box.size);
        print_type(box.type);
        if (box.has_uuid) {
            printf(" uuid=");
            for (int i = 0; i < 16; i++) {
                printf("%02x", (unsigned)box.uuid[i]);
            }
        }
        if (box.is_fullbox) {
            printf(" v=%u flags=0x%06" PRIx32, (unsigned)box.version, box.flags);
        }
        putchar('\n');

        uint64_t payload_off = box.offset + box.header_size;
        uint64_t payload_end = box.offset + box.size;

        if (depth < max_depth && is_container_box(box.type)) {
            if (payload_off < payload_end) {
                if (!dump_boxes(f, file_size, payload_off, payload_end, depth + 1, max_depth)) {
                    return false;
                }
            }
        }

        cursor = box.offset + box.size;
    }

    return true;
}

int main(int argc, char **argv) {
    int max_depth = 64;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--max-depth") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--max-depth requires an argument\n");
                return 2;
            }
            max_depth = atoi(argv[++i]);
            if (max_depth < 0) {
                fprintf(stderr, "invalid --max-depth\n");
                return 2;
            }
            continue;
        }
        if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!path) {
        print_usage(stderr);
        return 2;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        return 1;
    }

    uint64_t file_size;
    if (!get_file_size(f, &file_size)) {
        fprintf(stderr, "failed to get file size: %s\n", strerror(errno));
        fclose(f);
        return 1;
    }

    bool ok = dump_boxes(f, file_size, 0, file_size, 0, max_depth);
    fclose(f);
    return ok ? 0 : 1;
}
