#ifndef AVIFENC_WRITE_H
#define AVIFENC_WRITE_H

#include "encoder/avifenc.h"

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t position;
    AvifencStatus status;
    int sizing_only;
} AvifencByteWriter;

void avifenc_byte_writer_init(AvifencByteWriter *writer,
                              void *data,
                              size_t capacity);
void avifenc_byte_writer_init_sizing(AvifencByteWriter *writer);
size_t avifenc_byte_writer_size(const AvifencByteWriter *writer);
AvifencStatus avifenc_byte_writer_write(AvifencByteWriter *writer,
                                        const void *data,
                                        size_t size);
AvifencStatus avifenc_byte_writer_u8(AvifencByteWriter *writer,
                                     uint8_t value);
AvifencStatus avifenc_byte_writer_u16be(AvifencByteWriter *writer,
                                        uint16_t value);
AvifencStatus avifenc_byte_writer_u32be(AvifencByteWriter *writer,
                                        uint32_t value);
AvifencStatus avifenc_byte_writer_u64be(AvifencByteWriter *writer,
                                        uint64_t value);
AvifencStatus avifenc_byte_writer_leb128(AvifencByteWriter *writer,
                                         size_t value);
AvifencStatus avifenc_byte_writer_reserve(AvifencByteWriter *writer,
                                          size_t size,
                                          size_t *offset);
AvifencStatus avifenc_byte_writer_patch_u8(AvifencByteWriter *writer,
                                           size_t offset,
                                           uint8_t value);
AvifencStatus avifenc_byte_writer_patch_u16be(AvifencByteWriter *writer,
                                              size_t offset,
                                              uint16_t value);
AvifencStatus avifenc_byte_writer_patch_u32be(AvifencByteWriter *writer,
                                              size_t offset,
                                              uint32_t value);
AvifencStatus avifenc_byte_writer_patch_u64be(AvifencByteWriter *writer,
                                              size_t offset,
                                              uint64_t value);

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t bit_position;
    AvifencStatus status;
    int sizing_only;
} AvifencBitWriter;

void avifenc_bit_writer_init(AvifencBitWriter *writer,
                             void *data,
                             size_t capacity);
void avifenc_bit_writer_init_sizing(AvifencBitWriter *writer);
size_t avifenc_bit_writer_bits(const AvifencBitWriter *writer);
size_t avifenc_bit_writer_bytes(const AvifencBitWriter *writer);
AvifencStatus avifenc_bit_writer_write(AvifencBitWriter *writer,
                                       uint64_t value,
                                       unsigned int bit_count);
AvifencStatus avifenc_bit_writer_align(AvifencBitWriter *writer);
AvifencStatus avifenc_bit_writer_uvlc(AvifencBitWriter *writer,
                                      uint32_t value);
AvifencStatus avifenc_bit_writer_ns(AvifencBitWriter *writer,
                                    uint32_t value,
                                    uint32_t symbol_count);

#endif