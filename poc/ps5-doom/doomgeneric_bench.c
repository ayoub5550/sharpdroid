/* Host twin of doomgeneric_ps5.c for a native baseline: the same 640x400 -> 1920x1080 tiled
 * RGBA conversion per frame, the host clock, no display. SPDX-License-Identifier: GPL-2.0-or-later */
#include "doomgeneric.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
enum { frame_width = 1920, frame_height = 1080, frame_bytes = 0x1000000 };
static uint8_t *frames[2];
static int64_t frame_number = 1;
static uint32_t row_base[frame_height], row_swizzle[frame_height];
static uint32_t column_base[frame_width], column_swizzle[frame_width];
static uint16_t source_row[frame_height], source_column[frame_width];

static void build_tables(void)
{
    const uint32_t blocks_per_row = (frame_width + 127u) >> 7;
    for (uint32_t y = 0; y < frame_height; ++y)
    {
        row_base[y] = ((y >> 7) * blocks_per_row) << 16;
        row_swizzle[y] = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^
                         ((y << 8) & 0x4000u);
        source_row[y] = (uint16_t)((y * DOOMGENERIC_RESY) / frame_height);
    }
    for (uint32_t x = 0; x < frame_width; ++x)
    {
        column_base[x] = (x >> 7) << 16;
        column_swizzle[x] = ((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^
                            ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);
        source_column[x] = (uint16_t)((x * DOOMGENERIC_RESX) / frame_width);
    }
}


void DG_Init(void)
{
    build_tables();
    frames[0] = calloc(1, frame_bytes);
    frames[1] = calloc(1, frame_bytes);
}
void DG_DrawFrame(void)
{
    const int back = (int)(frame_number & 1) ^ 1;
    uint8_t *destination = frames[back];
    const uint32_t *source = DG_ScreenBuffer;

    for (uint32_t y = 0; y < frame_height; ++y)
    {
        const uint32_t *line = source + (size_t)source_row[y] * DOOMGENERIC_RESX;
        uint8_t *row = destination + row_base[y];
        const uint32_t swizzle = row_swizzle[y];
        for (uint32_t x = 0; x < frame_width; ++x)
        {
            const uint32_t p = line[source_column[x]];
            /* XRGB8888 -> RGBA8 in memory (R first). */
            const uint32_t rgba = 0xff000000u | ((p & 0xffu) << 16) | (p & 0xff00u) | ((p >> 16) & 0xffu);
            *(uint32_t *)(row + column_base[x] + (swizzle ^ column_swizzle[x])) = rgba;
        }
    }
    ++frame_number;
}


void DG_SleepMs(uint32_t ms) { usleep(ms * 1000u); }
uint32_t DG_GetTicksMs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}
int DG_GetKey(int *pressed, unsigned char *key) { (void)pressed; (void)key; return 0; }
void DG_SetWindowTitle(const char *title) { (void)title; }
int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);
    for (;;)
        doomgeneric_Tick();
}
