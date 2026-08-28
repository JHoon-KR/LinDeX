#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_dimension(const char *text, uint32_t *value) {
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > 8192ul)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

int main(int argc, char **argv) {
    FILE *input;
    uint8_t *frame;
    uint32_t width;
    uint32_t height;
    uint64_t frame_bytes;
    unsigned int poc = 0;

    if (argc != 4 || parse_dimension(argv[2], &width) < 0 ||
        parse_dimension(argv[3], &height) < 0 || (width & 1u) != 0 ||
        (height & 1u) != 0) {
        fprintf(stderr, "usage: %s NV12 WIDTH HEIGHT\n", argv[0]);
        return 2;
    }
    frame_bytes = (uint64_t)width * height * 3u / 2u;
    if (frame_bytes > SIZE_MAX) return 2;
    input = fopen(argv[1], "rb");
    if (input == NULL) {
        perror("fopen");
        return 1;
    }
    frame = malloc((size_t)frame_bytes);
    if (frame == NULL) {
        fclose(input);
        return 1;
    }
    for (;;) {
        size_t read_size = fread(frame, 1, (size_t)frame_bytes, input);
        uint64_t hash = UINT64_C(1469598103934665603);
        size_t i;
        if (read_size == 0 && feof(input)) break;
        if (read_size != frame_bytes) {
            fprintf(stderr, "partial NV12 frame: %zu of %" PRIu64 " bytes\n",
                    read_size, frame_bytes);
            free(frame);
            fclose(input);
            return 1;
        }
        for (i = 0; i < read_size; ++i) {
            hash ^= frame[i];
            hash *= UINT64_C(1099511628211);
        }
        printf("poc=%u fnv1a64=%016" PRIx64 " bytes=%" PRIu64 "\n",
               poc++, hash, frame_bytes);
    }
    free(frame);
    if (ferror(input) || fclose(input) != 0) return 1;
    return poc == 0 ? 1 : 0;
}
