#ifndef AVIFDEC_BASE_H
#define AVIFDEC_BASE_H

#include "avifdec.h"

int avifdec_size_add(size_t left, size_t right, size_t *result);
int avifdec_size_multiply(size_t left, size_t right, size_t *result);
int avifdec_size_align(size_t value, size_t alignment, size_t *result);

void avifdec_memory_copy(void *destination, const void *source, size_t count);
void avifdec_memory_fill(void *destination, unsigned char value, size_t count);
int avifdec_memory_compare(const void *left, const void *right, size_t count);

uint16_t avifdec_load_u16be(const unsigned char *bytes);
uint32_t avifdec_load_u32be(const unsigned char *bytes);
uint64_t avifdec_load_u64be(const unsigned char *bytes);

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t position;
    size_t base_offset;
    AvifdecStatus status;
} AvifdecByteReader;

void avifdec_byte_reader_init(AvifdecByteReader *reader,
                              const void *data,
                              size_t size,
                              size_t base_offset);
size_t avifdec_byte_reader_offset(const AvifdecByteReader *reader);
size_t avifdec_byte_reader_remaining(const AvifdecByteReader *reader);
const unsigned char *avifdec_byte_reader_take(AvifdecByteReader *reader, size_t count);
uint8_t avifdec_byte_reader_u8(AvifdecByteReader *reader);
uint16_t avifdec_byte_reader_u16be(AvifdecByteReader *reader);
uint32_t avifdec_byte_reader_u32be(AvifdecByteReader *reader);
uint64_t avifdec_byte_reader_u64be(AvifdecByteReader *reader);
AvifdecStatus avifdec_byte_reader_skip(AvifdecByteReader *reader, size_t count);
AvifdecStatus avifdec_byte_reader_subreader(AvifdecByteReader *reader,
                                            size_t count,
                                            AvifdecByteReader *subreader);

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t bit_position;
    size_t base_offset;
    AvifdecStatus status;
} AvifdecBitReader;

void avifdec_bit_reader_init(AvifdecBitReader *reader,
                             const void *data,
                             size_t size,
                             size_t base_offset);
size_t avifdec_bit_reader_offset(const AvifdecBitReader *reader);
uint32_t avifdec_bit_reader_read(AvifdecBitReader *reader, unsigned int bit_count);
AvifdecStatus avifdec_bit_reader_skip(AvifdecBitReader *reader, size_t bit_count);
AvifdecStatus avifdec_bit_reader_align(AvifdecBitReader *reader);

typedef struct {
    unsigned char *data;
    size_t size;
    size_t used;
    AvifdecStatus status;
    int sizing_only;
} AvifdecArena;

void avifdec_arena_init(AvifdecArena *arena, void *data, size_t size);
void avifdec_arena_init_sizing(AvifdecArena *arena);
void *avifdec_arena_allocate(AvifdecArena *arena, size_t size, size_t alignment);
size_t avifdec_arena_required(const AvifdecArena *arena);

#endif