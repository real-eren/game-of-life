// ! Assumes little-endian; if you have a big-endian cpu, you just need to reverse
// the byte order on all the hex literals related to rendering.

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#if defined(__MSC_VER)
#define restrict __restrict
#include <windows.h>
#define uint uint_fast32_t
void sleep(uint seconds, uint nanoseconds) {
    Sleep(seconds * 1000 + nanoseconds / 1000000);
}
#else
#define uint uint_fast32_t
// assume POSIX
void sleep(uint seconds, uint nanoseconds) {
    struct timespec ts = {.tv_sec = seconds, .tv_nsec = nanoseconds};
    nanosleep(&ts, &ts);
}
#endif

uint64_t rotl5(const uint64_t value) {
    return (value << 5) | (value >> (sizeof(value) * 8 - 5));
}
uint64_t fxhash(uint64_t h, uint64_t w) {
    return (rotl5(h) ^ w) * 0x517cc1b727220a95;
}

// Simulates 1 generation ahead from `src_buffer`, writing the results into `dst_buffer`.
// 1 = live, 0 = dead.
// rules: a cell is alive if, in a radius of 1, (3 live neighbors) | (4 live neighbors & already alive).
// indexing does wrap-around.
// This is extremely naive perf-wise, but I'm aiming for brevity.
void do_gen_naive(char *restrict const dst_buffer, const char *restrict const src_buffer, uint height, uint width) {
    signed long int h = (signed long int)height;
    signed long int w = (signed long int)width;
    for (signed long int y = 0; y < h; ++y) {
        for (signed long int x = 0; x < w; ++x) {
            char live_count = 0;
            for (signed long int dy = -1; dy < 2; ++dy)
                for (signed long int dx = -1; dx < 2; ++dx)
                    live_count += src_buffer[(dy + y + h) % h * w + (dx + x + w) % w];
            dst_buffer[y * w + x] = ((live_count == 3) | ((live_count == 4) & src_buffer[y * w + x])) != 0;
        }
    }
}

// (LSB,MSB) => old_state->new_state => color (decimal).
// (0,0) => dead->dead => black (40).
// (0,1) => dead->live => green (42).
// (1,0) => live->dead => red (41).
// (1,1) => live->live => white (47).
const uint16_t color_tokens_4bit[4] = {0x3034, 0x3134, 0x3234, 0x3734};
const uint16_t bw_tokens_4bit[4] = {0x3034, 0x3734, 0x3034, 0x3734};
const uint16_t *active_tokens_4bit = color_tokens_4bit;

// Records the current state, with special colors for changes since the previous state,
// using ANSI 4 bit colors.
// `draw_buffer` has layout "\033[__m " per cell.
// Each row ends in "\033[00m\n" (also 6 bytes long)
void fill_draw_buffer_4bit_color(uint16_t *restrict draw_buffer, const char *restrict active_buffer,
                                 const char *restrict prev_buffer, uint height, uint width) {
    const char *restrict abp = active_buffer;
    const char *restrict pbp = prev_buffer;
    draw_buffer += 1; // point to color code section
    for (uint y = 0; y < height; ++y, draw_buffer += 3)
        for (uint x = 0; x < width; ++x, ++abp, ++pbp, draw_buffer += 3)
            *draw_buffer = active_tokens_4bit[((*abp) << 1) | *pbp];
}

// https://en.wikipedia.org/wiki/C_signal_handling#Example_usage
volatile sig_atomic_t status = 0;
static void catch_function(int signo) {
    status = signo;
}

// clear screen, reset cursors, reset terminal (erase history)
// https://stackoverflow.com/questions/2347770/how-do-you-clear-the-console-screen-in-c
const char clear_code[] = "\e[1;1H\e[2J\ec";
const char green_color_code[] = "\e[38;2;020;220;000m";
const char reset_font_code[] = "\e[0m";

const char usage_msg[] = "cmd expects 3 arguments, all positive integers:\n\
{height} {width} {max_FPS}\n\
Example: '{cmd} 25 50 5 => 20 tall, 50 wide, 5 max FPS'\n\
Passing 0 for max_FPS results in an uncapped framerate\n\
There is also an optional flag '-bw' or '--bw' that can be passed first.\n\
Example: '{cmd} --bw 25 50 10'\n\
This will disable the red and green colors, only displaying cells in black and white\n";

int main(int argc, char *argv[]) {
    if (signal(SIGINT, catch_function) == SIG_ERR) {
        fputs("An error occurred while setting a signal handler.\n", stderr);
        return 1;
    }
    if (argc < 4 || (argc > 1 && (strncasecmp(argv[1], "-h", 2) == 0 || strncasecmp(argv[1], "--help", 6) == 0))) {
        printf(usage_msg);
        return 0;
    }
    if (argc >= 5 && (strncasecmp("-bw", argv[1], 4) == 0 || strncasecmp("--bw", argv[1], 5) == 0)) {
        active_tokens_4bit = bw_tokens_4bit;
        argv++;
    }
    char *field_names[3] = {"height", "width", "max_FPS"};
    uint vals[3] = {0, 0, 0};
    uint min_vals[3] = {1, 1, 0};
    uint max_vals[3] = {2000, 2000, 4800};
    for (int i = 0; i < 3; ++i) {
        char *end;
        vals[i] = strtoul(argv[i + 1], &end, 10);
        if (end == argv[i + 1] || *end != '\0' || errno == ERANGE || vals[i] < min_vals[i] || max_vals[i] < vals[i]) {
            fprintf(stderr, "bad input: arg #%d, '%s', expects an int between %lu and %lu.\n\
try --help for more info\n",
                    i + 1, field_names[i], min_vals[i], max_vals[i]);
            return 1;
        }
    }

    uint height = vals[0];
    uint width = vals[1];
    uint max_fps = vals[2];
    uint sleep_seconds = max_fps ? (1 / max_fps) : 0;
    uint sleep_ns = max_fps ? (1000000000 / max_fps % 1000000000) : 0;

    uint buffer_area = (height * width + 7) & ~7u; // pad to 8 bytes
    // width + 1 for newline.
    // with 4bit colors, 6 bytes per cell and newline.
    uint draw_buffer_area = 3 * (width + 1) * height;

    uint16_t *const draw_buffer = malloc(draw_buffer_area * sizeof(uint16_t));
    char *front_buffer = malloc(buffer_area * sizeof(char));
    char *back_buffer = malloc(buffer_area * sizeof(char));
    if (!(back_buffer && front_buffer && draw_buffer)) {
        fprintf(stderr, "malloc failed, aborting\n");
        return 1;
    }

    { // randomize cells 8 at at time
        srand(time(NULL));
        uint num_words = buffer_area / 8;
        uint64_t h = (uint64_t)rand();
        char *abp = front_buffer;
        for (uint i = 0; i < num_words; ++i, abp += 8) {
            h = fxhash(h, i);
            *((uint64_t *)abp) = h & 0x0101010101010101; // keep first bit of each byte
        }
    }

    memcpy(back_buffer, front_buffer, buffer_area); // so that initial frame is B/W
    { // initialize escape codes (these won't get re-written every frame, only the color codes inside are)
        uint16_t *dbp = draw_buffer;
        for (uint y = 0; y < height; ++y, dbp += 3) {
            for (uint x = 0; x < width; ++x, dbp += 3) {
                dbp[0] = 0x5B1B; // \033[
                dbp[1] = 0x3030; // 00
                dbp[2] = 0x206D; // 'm '
            }
            dbp[0] = 0x5B1B; // \033[
            dbp[1] = 0x3030; // 00
            dbp[2] = 0x0A6D; // m\n
        }
    }

    uint gen = 0;
    while (gen++ < 50000) {
        if (status == SIGINT) {
            // fix the colors if we get interrupted
            fwrite(reset_font_code, sizeof(char), sizeof(reset_font_code) - 1, stdout);
            fflush(stdout);
            break;
        }
        fill_draw_buffer_4bit_color(draw_buffer, front_buffer, back_buffer, height, width);
        { // "draw" frame
            // we could easily pad draw_buffer with the constant escape codes and avoid 2 sys calls, but w/e
            fwrite(clear_code, sizeof(char), sizeof(clear_code) - 1, stdout);
            fwrite(draw_buffer, sizeof(uint16_t), draw_buffer_area, stdout);
            fwrite(reset_font_code, sizeof(char), sizeof(reset_font_code) - 1, stdout);
            fprintf(stdout, "gen: %lu\n", gen);
            fflush(stdout);
        }
        { // rotate buffers
            char *tmp = front_buffer;
            front_buffer = back_buffer;
            back_buffer = tmp;
        }
        do_gen_naive(front_buffer, back_buffer, height, width);
        if (sleep_seconds | sleep_ns)
            sleep(sleep_seconds, sleep_ns);
    }

    return 0;
}
