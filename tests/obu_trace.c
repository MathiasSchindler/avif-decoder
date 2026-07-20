#include "av1.h"
#include "base.h"
#include "platform/platform.h"
#include <stddef.h>
#include <stdint.h>

#define OBU_MAX_INPUT_SIZE (64U * 1024U * 1024U)

static size_t text_length(const char *text) {
    size_t length = 0U;

    while (text[length] != '\0') ++length;
    return length;
}

static int write_bytes(int fd, const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t written = 0U;

    while (written < length) {
        long result = platform_write(fd, bytes + written, length - written);

        if (result <= 0) return -1;
        written += (size_t)result;
    }
    return 0;
}

static int write_text(int fd, const char *text) {
    return write_bytes(fd, text, text_length(text));
}

static int write_unsigned(int fd, size_t value) {
    char reversed[32];
    char output[32];
    size_t count = 0U;
    size_t index;

    do {
        reversed[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    for (index = 0U; index < count; ++index) {
        output[index] = reversed[count - index - 1U];
    }
    return write_bytes(fd, output, count);
}

static int write_hex_u64(int fd, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char output[18];
    unsigned int index;

    output[0] = '0';
    output[1] = 'x';
    for (index = 0U; index < 16U; ++index) {
        output[2U + index] =
            digits[(value >> ((15U - index) * 4U)) & 15U];
    }
    return write_bytes(fd, output, sizeof(output));
}

static int read_exact(int fd, unsigned char *data, size_t size) {
    size_t received = 0U;

    while (received < size) {
        long result = platform_read(fd, data + received, size - received);

        if (result <= 0) return -1;
        received += (size_t)result;
    }
    return 0;
}

static int parse_size(const char *text, size_t *value) {
    size_t parsed = 0U;
    size_t index = 0U;

    if (text == 0 || value == 0 || text[0] == '\0') return -1;
    while (text[index] != '\0') {
        unsigned int digit;

        if (text[index] < '0' || text[index] > '9') return -1;
        digit = (unsigned int)(text[index] - '0');
        if (parsed > (SIZE_MAX - digit) / 10U) return -1;
        parsed = parsed * 10U + digit;
        ++index;
    }
    *value = parsed;
    return 0;
}

static int load_file(const char *path,
                     unsigned char **data_out,
                     size_t *size_out) {
    long long end;
    unsigned char *data;
    int fd = platform_open_read(path);

    if (fd < 0) return -1;
    end = platform_seek(fd, 0, PLATFORM_SEEK_END);
    if (end <= 0 ||
        (unsigned long long)end > (unsigned long long)SIZE_MAX ||
        (unsigned long long)end > OBU_MAX_INPUT_SIZE ||
        platform_seek(fd, 0, PLATFORM_SEEK_SET) != 0) {
        (void)platform_close(fd);
        return -1;
    }
    data = (unsigned char *)platform_allocate_pages((size_t)end);
    if (data == 0 || read_exact(fd, data, (size_t)end) != 0) {
        if (data != 0) (void)platform_free_pages(data, (size_t)end);
        (void)platform_close(fd);
        return -1;
    }
    (void)platform_close(fd);
    *data_out = data;
    *size_out = (size_t)end;
    return 0;
}

static void print_value(const char *name, size_t value) {
    (void)write_text(1, name);
    (void)write_text(1, "=");
    (void)write_unsigned(1, value);
    (void)write_text(1, "\n");
}

static void print_checksum(const char *name, uint64_t value) {
    (void)write_text(1, name);
    (void)write_text(1, "=");
    (void)write_hex_u64(1, value);
    (void)write_text(1, "\n");
}

int main(int argc, char **argv) {
    unsigned char *data = 0;
    void *workspace = 0;
    size_t size = 0U;
    AvifdecSpan span;
    AvifdecImageInfo info = { 0 };
    AvifdecEntropyTrace trace = { 0 };
    AvifdecError error;
    AvifdecStatus status;
    AvifdecLimits limits = { 0 };
    const AvifdecLimits *limits_pointer = 0;
    const char *path;

    if (argc == 2) {
        path = argv[1];
    } else if (argc == 4 &&
               text_length(argv[1]) == text_length("--max-frames") &&
               avifdec_memory_compare(
                   argv[1], "--max-frames",
                   text_length("--max-frames")) == 0 &&
               parse_size(argv[2], &limits.max_frames) == 0) {
        path = argv[3];
        limits_pointer = &limits;
    } else {
        (void)write_text(
            2, "usage: obu-trace [--max-frames COUNT] INPUT.obu\n");
        return 2;
    }
    if (load_file(path, &data, &size) != 0) {
        (void)write_text(2, "obu-trace: cannot read input\n");
        return 1;
    }
    span.data = data;
    span.size = size;
    span.file_offset = 0U;
    status = avifdec_av1_query(
        &span, 1U, limits_pointer, &info, &error);
    if (status == AVIFDEC_OK) {
        workspace = platform_allocate_pages(info.workspace_required);
        if (workspace == 0) status = AVIFDEC_OUT_OF_MEMORY;
    }
    if (status == AVIFDEC_OK) {
        status = avifdec_av1_trace(
            &span, 1U, limits_pointer, &info, workspace,
            info.workspace_required, &trace, &error);
    }
    if (status != AVIFDEC_OK) {
        (void)write_text(2, "obu-trace: ");
        (void)write_text(2, avifdec_status_string(status));
        (void)write_text(2, " at ");
        (void)write_unsigned(2, error.offset);
        (void)write_text(2, " context ");
        (void)write_hex_u64(2, error.context);
        (void)write_text(2, "\n");
        print_value("frames", trace.frame_count);
        print_value("blocks", trace.block_count);
        print_value("inter_blocks", trace.inter_block_count);
        if (workspace != 0) {
            (void)platform_free_pages(workspace, info.workspace_required);
        }
        (void)platform_free_pages(data, size);
        return 1;
    }
    print_value("width", info.width);
    print_value("height", info.height);
    print_value("workspace_required", info.workspace_required);
    print_value("frames", trace.frame_count);
    print_value("show_existing_frames", trace.show_existing_frame_count);
    print_value("blocks", trace.block_count);
    print_value("inter_blocks", trace.inter_block_count);
    print_value("compound_blocks", trace.compound_block_count);
    print_checksum("reference_state_checksum",
                   trace.reference_state_checksum);
    print_checksum("mode_checksum", trace.mode_checksum);
    print_checksum("inter_mode_checksum", trace.inter_mode_checksum);
    print_checksum("mv_stack_checksum", trace.mv_stack_checksum);
    print_checksum("mv_checksum", trace.mv_checksum);
    print_checksum("predictor_checksum", trace.predictor_checksum);
    print_checksum("quantized_checksum", trace.quantized_checksum);
    print_checksum("dequantized_checksum", trace.dequantized_checksum);
    print_checksum("residual_checksum", trace.residual_checksum);
    print_checksum("reconstruction_checksum", trace.reconstruction_checksum);
    print_checksum("deblocked_checksum", trace.deblocked_checksum);
    print_checksum("cdef_checksum", trace.cdef_checksum);
    print_checksum("superres_checksum", trace.superres_checksum);
    print_checksum("restoration_checksum", trace.restoration_checksum);
    (void)platform_free_pages(workspace, info.workspace_required);
    (void)platform_free_pages(data, size);
    return 0;
}
