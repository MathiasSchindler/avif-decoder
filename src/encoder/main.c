#include "encoder/avifenc.h"
#include "base.h"
#include "platform.h"
#include <stddef.h>
#include <stdint.h>

static size_t text_length(const char *text) {
    size_t length = 0U;

    while (text[length] != '\0') ++length;
    return length;
}

static int text_equal(const char *left, const char *right) {
    size_t index = 0U;

    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return 0;
        ++index;
    }
    return left[index] == right[index];
}

static int text_to_u32(const char *text, uint32_t *value) {
    uint32_t result = 0U;
    size_t index = 0U;

    if (text == 0 || value == 0 || text[0] == '\0') return 0;
    while (text[index] != '\0') {
        uint32_t digit;

        if (text[index] < '0' || text[index] > '9') return 0;
        digit = (uint32_t)(text[index] - '0');
        if (result > (UINT32_MAX - digit) / 10U) return 0;
        result = result * 10U + digit;
        ++index;
    }
    *value = result;
    return 1;
}

static int write_bytes(int fd, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t written = 0U;

    while (written < size) {
        long result = platform_write(fd, bytes + written, size - written);

        if (result <= 0) return -1;
        written += (size_t)result;
    }
    return 0;
}

static int read_bytes(int fd, void *data, size_t size) {
    unsigned char *bytes = (unsigned char *)data;
    size_t received = 0U;

    while (received < size) {
        long result = platform_read(fd, bytes + received, size - received);

        if (result <= 0) return -1;
        received += (size_t)result;
    }
    return 0;
}

static int write_text(int fd, const char *text) {
    return write_bytes(fd, text, text_length(text));
}

static void write_usage(int fd) {
    (void)write_text(
        fd,
        "usage: avifenc [--quantizer 1..255] [--speed 0..2] "
        "WIDTH HEIGHT INPUT.yuv OUTPUT.avif\n"
        "       avifenc --help\n"
        "       avifenc --version\n");
}

static int write_error(AvifencStatus status, const AvifencError *error) {
    if (write_text(2, "avifenc: ") != 0 ||
        write_text(2, avifenc_status_string(status)) != 0 ||
        write_text(2, ": ") != 0 ||
        write_text(2, avifenc_error_context_string(error->context)) != 0 ||
        write_text(2, "\n") != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    static const uint8_t placeholder = 0U;
    AvifencImage image = { 0 };
    AvifencOptions options;
    AvifencRequirements requirements;
    AvifencError error;
    AvifencStatus status;
    unsigned char *input = 0;
    void *workspace = 0;
    unsigned char *output = 0;
    size_t luma_size;
    size_t chroma_size;
    size_t input_size;
    size_t output_written = 0U;
    uint32_t quantizer = AVIFENC_DEFAULT_QUANTIZER;
    uint32_t speed = AVIFENC_DEFAULT_SPEED;
    int input_fd = -1;
    int output_fd = -1;
    int argument = 1;
    int result = 3;

    if (argc == 2 && text_equal(argv[1], "--help")) {
        write_usage(1);
        return 0;
    }
    if (argc == 2 && text_equal(argv[1], "--version")) {
        (void)write_text(1, "avifenc ");
        (void)write_text(1, avifenc_version_string());
        (void)write_text(1, "\n");
        return 0;
    }
    while (argument + 1 < argc) {
        if (text_equal(argv[argument], "--quantizer")) {
            if (!text_to_u32(argv[argument + 1], &quantizer) ||
                quantizer > UINT16_MAX) {
                (void)write_text(2, "avifenc: invalid quantizer\n");
                return 2;
            }
        } else if (text_equal(argv[argument], "--speed")) {
            if (!text_to_u32(argv[argument + 1], &speed) ||
                speed > UINT8_MAX) {
                (void)write_text(2, "avifenc: invalid speed\n");
                return 2;
            }
        } else {
            break;
        }
        argument += 2;
    }
    if (argc - argument != 4 ||
        !text_to_u32(argv[argument], &image.width) ||
        !text_to_u32(argv[argument + 1], &image.height)) {
        write_usage(2);
        return 2;
    }

    image.planes[0] = &placeholder;
    image.planes[1] = &placeholder;
    image.planes[2] = &placeholder;
    image.strides[0] = image.width;
    image.strides[1] = image.width / 2U;
    image.strides[2] = image.width / 2U;
    image.color.color_primaries = 1U;
    image.color.transfer_characteristics = 1U;
    image.color.matrix_coefficients = 1U;
    avifenc_options_default(&options);
    options.quantizer = (uint16_t)quantizer;
    options.speed = (uint8_t)speed;
    status = avifenc_query(&image, &options, &requirements, &error);
    if (status != AVIFENC_OK) {
        (void)write_error(status, &error);
        return 2;
    }
    if (!avifdec_size_multiply(image.width, image.height, &luma_size) ||
        !avifdec_size_multiply(
            image.width / 2U, image.height / 2U, &chroma_size) ||
        chroma_size > (SIZE_MAX - luma_size) / 2U) {
        (void)write_text(2, "avifenc: input dimensions overflow\n");
        return 2;
    }
    input_size = luma_size + 2U * chroma_size;
    if (input_size == 0U) {
        (void)write_text(2, "avifenc: invalid dimensions\n");
        return 2;
    }
    input = (unsigned char *)platform_allocate_pages(input_size);
    if (input == 0) {
        (void)write_text(2, "avifenc: out of memory: input\n");
        return 3;
    }
    input_fd = platform_open_read(argv[argument + 2]);
    if (input_fd < 0 || read_bytes(input_fd, input, input_size) != 0) {
        (void)write_text(2, "avifenc: failed to read complete YUV input\n");
        result = 4;
        goto cleanup;
    }
    {
        unsigned char trailing;
        long extra = platform_read(input_fd, &trailing, 1U);

        if (extra != 0) {
            (void)write_text(2, "avifenc: YUV input has trailing data\n");
            result = 4;
            goto cleanup;
        }
    }
    if (platform_close(input_fd) != 0) {
        input_fd = -1;
        (void)write_text(2, "avifenc: failed to close YUV input\n");
        result = 4;
        goto cleanup;
    }
    input_fd = -1;
    image.planes[0] = input;
    image.planes[1] = input + luma_size;
    image.planes[2] = input + luma_size + chroma_size;
    workspace = platform_allocate_pages(requirements.workspace_required);
    output = (unsigned char *)platform_allocate_pages(
        requirements.output_capacity_required);
    if (workspace == 0 || output == 0) {
        (void)write_text(2, "avifenc: out of memory: encode buffers\n");
        goto cleanup;
    }
    status = avifenc_encode(
        &image, &options, workspace, requirements.workspace_required,
        output, requirements.output_capacity_required,
        &output_written, &error);
    if (status != AVIFENC_OK) {
        (void)write_error(status, &error);
        goto cleanup;
    }
    output_fd = platform_open_write(argv[argument + 3], 0644U);
    if (output_fd < 0 || write_bytes(output_fd, output, output_written) != 0) {
        (void)write_text(2, "avifenc: failed to write AVIF output\n");
        result = 4;
        goto cleanup;
    }
    if (platform_close(output_fd) != 0) {
        output_fd = -1;
        (void)write_text(2, "avifenc: failed to close AVIF output\n");
        result = 4;
        goto cleanup;
    }
    output_fd = -1;
    result = 0;

cleanup:
    if (input_fd >= 0) (void)platform_close(input_fd);
    if (output_fd >= 0) (void)platform_close(output_fd);
    if (output != 0) {
        (void)platform_free_pages(
            output, requirements.output_capacity_required);
    }
    if (workspace != 0) {
        (void)platform_free_pages(
            workspace, requirements.workspace_required);
    }
    (void)platform_free_pages(input, input_size);
    return result;
}