#include "encoder/cli/image_input.h"

#include "base.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static unsigned char workspace[210000];

static int test_png(void) {
    static const unsigned char png[] = {
        0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
        0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
        0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x01U,
        0x08U, 0x02U, 0x00U, 0x00U, 0x00U, 0x7bU, 0x40U, 0xe8U,
        0xddU, 0x00U, 0x00U, 0x00U, 0x12U, 0x49U, 0x44U, 0x41U,
        0x54U, 0x78U, 0x01U, 0x01U, 0x07U, 0x00U, 0xf8U, 0xffU,
        0x00U, 0xffU, 0x00U, 0x00U, 0x00U, 0xffU, 0x00U, 0x07U,
        0xffU, 0x01U, 0xffU, 0xc5U, 0x0eU, 0xe2U, 0x6aU, 0x00U,
        0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4eU, 0x44U, 0xaeU,
        0x42U, 0x60U, 0x82U
    };
            static const unsigned char palette_png[] = {
            0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
            0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
            0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x01U,
            0x08U, 0x03U, 0x00U, 0x00U, 0x00U, 0xc3U, 0xfcU, 0x8fU,
            0xb8U, 0x00U, 0x00U, 0x00U, 0x06U, 0x50U, 0x4cU, 0x54U,
            0x45U, 0x0aU, 0x14U, 0x1eU, 0xc8U, 0xd2U, 0xdcU, 0x82U,
            0x8dU, 0x75U, 0xddU, 0x00U, 0x00U, 0x00U, 0x02U, 0x74U,
            0x52U, 0x4eU, 0x53U, 0x00U, 0xffU, 0x5bU, 0x91U, 0x22U,
            0xb5U, 0x00U, 0x00U, 0x00U, 0x0bU, 0x49U, 0x44U, 0x41U,
            0x54U, 0x78U, 0xdaU, 0x63U, 0x60U, 0x60U, 0x04U, 0x00U,
            0x00U, 0x04U, 0x00U, 0x02U, 0x2cU, 0xdeU, 0x48U, 0xadU,
            0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4eU, 0x44U,
            0xaeU, 0x42U, 0x60U, 0x82U
            };
    unsigned char corrupted[sizeof(png)];
    unsigned char output[6] = { 0U, 0U, 0U, 0U, 0U, 0U };
            unsigned char palette_output[6] = { 0U, 0U, 0U, 0U, 0U, 0U };
    ImageInputInfo info;

    CHECK(image_input_query(png, sizeof(png), &info) == IMAGE_INPUT_OK);
    CHECK(info.format == IMAGE_INPUT_FORMAT_PNG && info.width == 2U &&
          info.height == 1U && info.rgb_stride == 6U &&
          info.output_size == sizeof(output) &&
          info.workspace_size <= sizeof(workspace));
    CHECK(image_input_decode(png, sizeof(png), workspace,
                             info.workspace_size - 1U, output,
                             sizeof(output), &info) ==
          IMAGE_INPUT_WORKSPACE_TOO_SMALL);
    CHECK(image_input_decode(png, sizeof(png), workspace,
                             sizeof(workspace), output,
                             sizeof(output) - 1U, &info) ==
          IMAGE_INPUT_OUTPUT_TOO_SMALL);
    CHECK(image_input_decode(png, sizeof(png), workspace,
                             sizeof(workspace), output,
                             sizeof(output), &info) == IMAGE_INPUT_OK);
    CHECK(output[0] == 255U && output[1] == 0U && output[2] == 0U &&
          output[3] == 0U && output[4] == 255U && output[5] == 0U);
    avifdec_memory_copy(corrupted, png, sizeof(png));
    corrupted[24] ^= 1U;
    CHECK(image_input_query(corrupted, sizeof(corrupted), &info) ==
          IMAGE_INPUT_INVALID_DATA);
    CHECK(image_input_query(png, sizeof(png) - 1U, &info) ==
          IMAGE_INPUT_TRUNCATED);
    CHECK(image_input_decode(palette_png, sizeof(palette_png), workspace,
                             sizeof(workspace), palette_output,
                             sizeof(palette_output), &info) ==
          IMAGE_INPUT_OK);
    CHECK(palette_output[0] == 10U && palette_output[1] == 20U &&
          palette_output[2] == 30U && palette_output[3] == 200U &&
          palette_output[4] == 210U && palette_output[5] == 220U);
    return 0;
}

static int test_jpeg(void) {
    static const unsigned char jpeg[] = {
        0xffU, 0xd8U,
        0xffU, 0xdbU, 0x00U, 0x43U, 0x00U,
        1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
        1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
        1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
        1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
        1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
        1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
        1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
        1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
        0xffU, 0xc0U, 0x00U, 0x0bU, 0x08U, 0x00U, 0x01U,
        0x00U, 0x01U, 0x01U, 0x01U, 0x11U, 0x00U,
        0xffU, 0xc4U, 0x00U, 0x14U, 0x00U,
        1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0xffU, 0xc4U, 0x00U, 0x14U, 0x10U,
        1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0xffU, 0xdaU, 0x00U, 0x08U, 0x01U, 0x01U, 0x00U,
        0x00U, 0x3fU, 0x00U, 0x3fU, 0xffU, 0xd9U
    };
            unsigned char progressive[sizeof(jpeg)];
    unsigned char output[3] = { 0U, 0U, 0U };
    ImageInputInfo info;
            size_t index;

    CHECK(image_input_query(jpeg, sizeof(jpeg), &info) == IMAGE_INPUT_OK);
    CHECK(info.format == IMAGE_INPUT_FORMAT_JPEG && info.width == 1U &&
          info.height == 1U && info.rgb_stride == 3U &&
          info.output_size == sizeof(output) &&
          info.workspace_size <= sizeof(workspace));
    CHECK(image_input_decode(jpeg, sizeof(jpeg), workspace,
                             sizeof(workspace), output,
                             sizeof(output), &info) == IMAGE_INPUT_OK);
    CHECK(output[0] == 128U && output[1] == 128U && output[2] == 128U);
    CHECK(image_input_decode(jpeg, sizeof(jpeg) - 1U, workspace,
                             sizeof(workspace), output,
                             sizeof(output), &info) ==
          IMAGE_INPUT_TRUNCATED);
      avifdec_memory_copy(progressive, jpeg, sizeof(jpeg));
      for (index = 1U; index < sizeof(progressive); ++index) {
            if (progressive[index - 1U] == 0xffU &&
                  progressive[index] == 0xc0U) {
                  progressive[index] = 0xc2U;
                  break;
            }
      }
      CHECK(index < sizeof(progressive));
      CHECK(image_input_query(progressive, sizeof(progressive), &info) ==
              IMAGE_INPUT_UNSUPPORTED);
    return 0;
}

int main(void) {
    int result = test_png();

    if (result != 0) return result;
    return test_jpeg();
}