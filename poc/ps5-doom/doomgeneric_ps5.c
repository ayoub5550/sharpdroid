/*
 * doomgeneric for PS5 native apps (ps5-native-app-boilerplate runtime).
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The whole platform layer: a CPU-drawn VideoOut frame (two tiled 1920x1080 RGBA8 buffers in
 * direct memory), the pad, and the clock. Doom renders 640x400 XRGB into DG_ScreenBuffer; each
 * frame is scaled 3x2.7 into the back buffer and flipped.
 *
 * Built to run on a PS5 and, the reason it exists, as a free test title for the SharpEmu
 * emulator: with no input the game plays its attract-mode demos, which is real gameplay.
 */

#include "doom/doomgeneric.h" /* src/doom/ in the boilerplate tree: doomgeneric copied by build.sh */
#include "doom/doomkeys.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t *physical_address);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t physical_address, size_t alignment);
int sceKernelSendNotificationRequest(uint32_t device, void *request, size_t size, int blocking);
int sceKernelUsleep(uint32_t microseconds);
uint64_t sceKernelGetProcessTime(void);
int sceSystemServiceHideSplashScreen(void);
int sceVideoOutOpen(int32_t user_id, int32_t bus_type, int32_t index, const void *param);
int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
int sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t flip_mode,
                          int64_t flip_argument);
int sceVideoOutWaitVblank(int32_t handle);
void sceVideoOutSetBufferAttribute2(void *attribute, uint64_t pixel_format, uint32_t tiling_mode,
                                    uint32_t width, uint32_t height, uint64_t option,
                                    uint32_t dcc_control, uint64_t dcc_clear_color);
int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set_index, int32_t buffer_index_start,
                                void *buffers, int32_t buffer_count, void *attribute,
                                int32_t category, void *option);
int sceUserServiceInitialize(void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void *param);
int scePadReadState(int32_t handle, void *data);

enum
{
    frame_width = 1920,
    frame_height = 1080,
    frame_bytes = 0x1000000,
    memory_alignment = 0x200000,
    memory_type_wc_garlic = 3,
    map_protection = 0x33,
};
static const uint64_t pixel_format_rgba8_srgb = 0x8000000022000000ull;

struct video_buffer
{
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
};

static int32_t video = -1;
static uint8_t *frames[2];
static int64_t frame_number = 1;
static int32_t pad = -1;
static uint32_t previous_buttons;
static uint8_t previous_stick_keys;

/* Pixel (x, y) lives at row_base[y] + column_base[x] + (row_swizzle[y] ^ column_swizzle[x]):
 * 128x128-pixel 64 KiB blocks, row-major, with the 64KB_R_X swizzle inside a block. */
static uint32_t row_base[frame_height], row_swizzle[frame_height];
static uint32_t column_base[frame_width], column_swizzle[frame_width];
static uint16_t source_row[frame_height], source_column[frame_width];

static struct
{
    uint8_t reserved[45];
    char message[3075];
} notification;

static void notify(const char *message)
{
    snprintf(notification.message, sizeof notification.message, "%s", message);
    (void)sceKernelSendNotificationRequest(0, &notification, sizeof notification, 0);
    printf("[doom-ps5] %s\n", message);
}

static void halt(const char *message)
{
    notify(message);
    for (;;)
        (void)sceKernelUsleep(1000000);
}

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
    (void)sceSystemServiceHideSplashScreen();

    video = sceVideoOutOpen(0xff, 0, 0, NULL);
    if (video < 0)
        halt("DOOM: sceVideoOutOpen failed");

    const size_t memory_bytes = (size_t)frame_bytes * 2;
    const size_t pool_size = sceKernelGetDirectMemorySize();
    if (pool_size < memory_bytes)
        halt("DOOM: insufficient direct memory");

    int64_t physical_address = 0;
    if (sceKernelAllocateDirectMemory(0, (int64_t)pool_size, memory_bytes, memory_alignment,
                                      memory_type_wc_garlic, &physical_address) < 0)
        halt("DOOM: direct-memory allocation failed");

    void *mapped = NULL;
    if (sceKernelMapDirectMemory(&mapped, memory_bytes, map_protection, 0, physical_address,
                                 memory_alignment) < 0)
        halt("DOOM: direct-memory mapping failed");

    frames[0] = (uint8_t *)mapped;
    frames[1] = (uint8_t *)mapped + frame_bytes;
    memset(mapped, 0, memory_bytes);

    struct video_buffer buffers[2] = {{frames[0], NULL, NULL, NULL}, {frames[1], NULL, NULL, NULL}};
    uint8_t attribute[80];
    memset(attribute, 0, sizeof attribute);
    (void)sceVideoOutSetFlipRate(video, 0);
    sceVideoOutSetBufferAttribute2(attribute, pixel_format_rgba8_srgb, 0, frame_width, frame_height,
                                   0, 0, 0);
    if (sceVideoOutRegisterBuffers2(video, 0, 0, buffers, 2, attribute, 0, NULL) < 0)
        halt("DOOM: buffer registration failed");

    int32_t user = -1;
    if (sceUserServiceInitialize(NULL) >= 0 && sceUserServiceGetInitialUser(&user) >= 0 &&
        scePadInit() >= 0)
        pad = scePadOpen(user, 0, 0, NULL);

    notify("DOOM: video ready");
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
    __asm__ volatile("sfence" ::: "memory");

    if (sceVideoOutSubmitFlip(video, back, 1, frame_number) < 0)
        (void)sceKernelUsleep(4000);
    ++frame_number;
}

void DG_SleepMs(uint32_t ms)
{
    (void)sceKernelUsleep(ms * 1000u);
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(sceKernelGetProcessTime() / 1000u);
}

/* Pad -> Doom keys. Bits are the PS4/PS5 pad button mask. */
static const struct
{
    uint32_t button;
    unsigned char key;
} pad_keys[] = {
    {0x00000010u, KEY_UPARROW},    {0x00000040u, KEY_DOWNARROW}, {0x00000080u, KEY_LEFTARROW},
    {0x00000020u, KEY_RIGHTARROW}, {0x00004000u, KEY_ENTER},     {0x00008000u, KEY_USE},
    {0x00000200u, KEY_FIRE},       {0x00000100u, KEY_RSHIFT},    {0x00000400u, KEY_STRAFE_L},
    {0x00000800u, KEY_STRAFE_R},   {0x00000008u, KEY_ESCAPE},    {0x00001000u, 'y'},
    {0x00002000u, KEY_BACKSPACE},
};
static const unsigned char stick_keys[4] = {KEY_UPARROW, KEY_DOWNARROW, KEY_LEFTARROW, KEY_RIGHTARROW};

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (pad < 0)
        return 0;

    uint8_t state[256];
    memset(state, 0, sizeof state);
    if (scePadReadState(pad, state) < 0)
        return 0;

    uint32_t buttons;
    memcpy(&buttons, state, sizeof buttons);
    const uint32_t changed = buttons ^ previous_buttons;
    for (size_t i = 0; i < sizeof pad_keys / sizeof pad_keys[0]; ++i)
    {
        if (changed & pad_keys[i].button)
        {
            previous_buttons ^= pad_keys[i].button;
            *pressed = (buttons & pad_keys[i].button) != 0;
            *key = pad_keys[i].key;
            return 1;
        }
    }

    const uint8_t lx = state[4], ly = state[5];
    const uint8_t stick = (uint8_t)((ly < 64) | ((ly > 192) << 1) | ((lx < 64) << 2) | ((lx > 192) << 3));
    const uint8_t stick_changed = stick ^ previous_stick_keys;
    for (int i = 0; i < 4; ++i)
    {
        if (stick_changed & (1u << i))
        {
            previous_stick_keys ^= (uint8_t)(1u << i);
            *pressed = (stick >> i) & 1;
            *key = stick_keys[i];
            return 1;
        }
    }
    return 0;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

/* Extra command-line arguments from /app0/assets/args.txt (whitespace separated), so one build
 * can be the attract-mode demo or a benchmark ("-timedemo demo1") by its data alone. */
static int load_arguments(char **arguments, int count, int capacity)
{
    static char text[512];
    FILE *file = fopen("/app0/assets/args.txt", "r");
    if (file == NULL)
        return count;
    const size_t length = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[length] = '\0';
    for (char *cursor = text; *cursor != '\0' && count < capacity - 1;)
    {
        while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t')
            *cursor++ = '\0';
        if (*cursor == '\0')
            break;
        arguments[count++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\n' && *cursor != '\r' && *cursor != '\t')
            ++cursor;
    }
    arguments[count] = NULL;
    return count;
}

int main(void)
{
    static char *arguments[16] = {"doom", "-iwad", "/app0/assets/doom1.wad", NULL};
    const int count = load_arguments(arguments, 3, 16);
    notify("DOOM: starting");
    doomgeneric_Create(count, arguments);
    for (;;)
        doomgeneric_Tick();
}
