/* KallistiOS ##version##

   naomibin.c
   Copyright (C) 2020 Lawrence Sebald

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>

typedef struct naomi_hdr {
    char signature[16];
    char developer[32];
    char region_title[8][32];
} naomi_hdr_t;

typedef struct naomi_segment {
    uint32_t rom;
    uint32_t ram;
    uint32_t size;
} naomi_segment_t;

static void print_header(const naomi_hdr_t *hdr) {
    int i;

    printf("Platform Signature: %.16s\n", hdr->signature);
    printf("Developer: %.32s\n", hdr->developer);

    for(i = 0; i < 8; ++i) {
        printf("Region title %d: %.32s\n", i + 1, hdr->region_title[i]);
    }
}

static naomi_segment_t *read_segments(FILE *fp, int *num_segments) {
    int addr = 0x0360;
    int ns = 0;
    naomi_segment_t seg;

    fseek(fp, addr, SEEK_SET);

    while(addr < 0x0420) {
        fread(&seg, sizeof(naomi_segment_t), 1, fp);

        if(seg.size == 0 || (seg.rom & 0x80000000))
            break;

        printf("Segment %d\n", ns + 1);
        printf("ROM Address: %08" PRIx32 "\n", seg.rom);
        printf("RAM Address: %08" PRIx32 "\n", seg.ram);
        printf("Length: %" PRIu32 "\n", seg.size);
        addr += 12;
        ++ns;
    }

    *num_segments = ns;

    return NULL;
}

static int read_entry(FILE *fp, uint32_t *entry, uint32_t *entry2) {
    fseek(fp, 0x0420, SEEK_SET);

    fread(entry, sizeof(uint32_t), 1, fp);
    fread(entry2, sizeof(uint32_t), 1, fp);

    printf("Entry point: %08" PRIx32 "\n", *entry);
    printf("Reset point: %08" PRIx32 "\n", *entry2);

    return 0;
}

static int read_interrupts(FILE *fp) {
    uint32_t vec[22];
    int i;

    fseek(fp, 0x0130, SEEK_SET);

    for(i = 0; i < 22; ++i) {
        fread(vec + i, sizeof(uint32_t), 1, fp);
        printf("Vector %d: %08" PRIx32 "\n", i, vec[i]);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    FILE *fp;
    naomi_hdr_t hdr;
    int tmp;
    uint32_t entry, reset;

    if(argc != 2) {
        fprintf(stderr, "Usage: %s filename\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    fp = fopen(argv[1], "rb");
    if(!fp) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    if(fread(&hdr, sizeof(naomi_hdr_t), 1, fp) != 1) {
        perror("Error reading file");
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    print_header(&hdr);
    read_segments(fp, &tmp);
    read_entry(fp, &entry, &reset);
    read_interrupts(fp);

    fclose(fp);
    return 0;
}
