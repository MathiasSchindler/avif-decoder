#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MACHO_MAGIC_64 0xfeedfacfU
#define MACHO_HEADER_64_SIZE 32U
#define MACHO_LC_LOAD_DYLIB 0x0cU
#define MACHO_LIBSYSTEM_PATH "/usr/lib/libSystem.B.dylib"

static uint32_t read_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8U) |
        ((uint32_t)bytes[2] << 16U) |
        ((uint32_t)bytes[3] << 24U);
}

static void write_u32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static int remove_libsystem_command(unsigned char *image, size_t image_size) {
    uint32_t command_count;
    uint32_t commands_size;
    size_t command_offset = MACHO_HEADER_64_SIZE;
    uint32_t index;

    if (image_size < MACHO_HEADER_64_SIZE || read_u32(image) != MACHO_MAGIC_64) {
        return -1;
    }
    command_count = read_u32(image + 16U);
    commands_size = read_u32(image + 20U);
    if ((size_t)commands_size > image_size - MACHO_HEADER_64_SIZE) {
        return -1;
    }

    for (index = 0U; index < command_count; ++index) {
        uint32_t command;
        uint32_t command_size;

        if (command_offset + 8U > MACHO_HEADER_64_SIZE + commands_size) {
            return -1;
        }
        command = read_u32(image + command_offset);
        command_size = read_u32(image + command_offset + 4U);
        if (command_size < 8U || command_offset + command_size >
                MACHO_HEADER_64_SIZE + commands_size) {
            return -1;
        }
        if (command == MACHO_LC_LOAD_DYLIB) {
            uint32_t name_offset;
            const char *name;
            size_t remaining;
            size_t tail_size;

            if (command_size < 24U) return -1;
            name_offset = read_u32(image + command_offset + 8U);
            if (name_offset >= command_size) return -1;
            name = (const char *)(image + command_offset + name_offset);
            remaining = command_size - name_offset;
            if (memchr(name, '\0', remaining) == 0 ||
                    strcmp(name, MACHO_LIBSYSTEM_PATH) != 0) {
                return -1;
            }

            tail_size = MACHO_HEADER_64_SIZE + commands_size -
                (command_offset + command_size);
            memmove(
                image + command_offset,
                image + command_offset + command_size,
                tail_size
            );
            memset(
                image + MACHO_HEADER_64_SIZE + commands_size - command_size,
                0,
                command_size
            );
            write_u32(image + 16U, command_count - 1U);
            write_u32(image + 20U, commands_size - command_size);
            return 0;
        }
        command_offset += command_size;
    }
    return -1;
}

int main(int argc, char **argv) {
    FILE *file;
    unsigned char *image;
    long file_size;
    int result = 1;

    if (argc != 2) {
        fprintf(stderr, "usage: macho-dylib-remover FILE\n");
        return 2;
    }
    file = fopen(argv[1], "rb+");
    if (file == 0) {
        perror(argv[1]);
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
            fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "macho-dylib-remover: cannot size %s\n", argv[1]);
        fclose(file);
        return 1;
    }
    image = (unsigned char *)malloc((size_t)file_size);
    if (image == 0) {
        fprintf(stderr, "macho-dylib-remover: out of memory\n");
        fclose(file);
        return 1;
    }
    if (fread(image, 1U, (size_t)file_size, file) != (size_t)file_size ||
            remove_libsystem_command(image, (size_t)file_size) != 0 ||
            fseek(file, 0, SEEK_SET) != 0 ||
            fwrite(image, 1U, (size_t)file_size, file) != (size_t)file_size ||
            fflush(file) != 0) {
        fprintf(stderr, "macho-dylib-remover: cannot rewrite %s\n", argv[1]);
    } else {
        result = 0;
    }
    free(image);
    if (fclose(file) != 0) result = 1;
    return result;
}