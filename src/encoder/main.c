#include "encoder/avifenc.h"
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

static int write_text(int fd, const char *text) {
    return write_bytes(fd, text, text_length(text));
}

static void write_usage(int fd) {
    (void)write_text(
        fd,
        "usage: avifenc [--quantizer 0..255] WIDTH HEIGHT INPUT.yuv OUTPUT.avif\n"
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
    uint32_t quantizer = AVIFENC_DEFAULT_QUANTIZER;
    int argument = 1;

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
    if (argument + 1 < argc && text_equal(argv[argument], "--quantizer")) {
        if (!text_to_u32(argv[argument + 1], &quantizer) ||
            quantizer > UINT16_MAX) {
            (void)write_text(2, "avifenc: invalid quantizer\n");
            return 2;
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
    status = avifenc_query(&image, &options, &requirements, &error);
    if (status != AVIFENC_OK) {
        (void)write_error(status, &error);
        return 2;
    }

    (void)argv[argument + 2];
    (void)argv[argument + 3];
    status = AVIFENC_UNSUPPORTED;
    error.status = status;
    error.context = AVIFENC_CONTEXT_IMPLEMENTATION;
    error.required_size = requirements.output_capacity_required;
    error.provided_size = 0U;
    (void)write_error(status, &error);
    return 3;
}