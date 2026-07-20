#ifndef AVIFDEC_CLI_PNG_WRITE_H
#define AVIFDEC_CLI_PNG_WRITE_H

#include "avifdec.h"

AvifdecStatus cli_convert_rgb_rows_parallel(
    const AvifdecExecutor *executor,
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    AvifdecRgbImage *rgb,
    uint32_t first_row,
    uint32_t row_count,
    AvifdecError *error);

AvifdecStatus cli_write_png_file(
    const char *path,
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecExecutor *executor,
    AvifdecError *error);

#endif
