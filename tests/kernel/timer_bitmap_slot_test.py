"""Production timer slot helpers against an independent linear occupancy model."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'kernel/time/hrtimer.c').read_text()
helpers = source[source.index('static uint32_t timer_find_free_slot_locked('):
                 source.index('static void timer_global_lock(')]
program = r'''
#include <kernel/timer.h>
#include <kernel/bitmap.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define TIMER_SLOT_WORDS ((TIMER_OBJECT_LIMIT + 63U) / 64U)
_Static_assert(TIMER_OBJECT_LIMIT == 128U, "exercise both complete words");
static uint64_t g_timer_occupied[TIMER_SLOT_WORDS];
''' + helpers + r'''
static unsigned cases;
static void check(void) {
    uint64_t saved[TIMER_SLOT_WORDS];
    memcpy(saved, g_timer_occupied, sizeof(saved));
    unsigned first = TIMER_OBJECT_LIMIT, limit = 0;
    for (unsigned i = 0; i < TIMER_OBJECT_LIMIT; ++i) {
        bool occupied = (saved[i / 64] >> (i % 64)) & 1U;
        if (occupied) limit = i + 1;
        else if (first == TIMER_OBJECT_LIMIT) first = i;
    }
    assert(timer_find_free_slot_locked() == first);
    assert(timer_registered_limit_locked() == limit);
    assert(!memcmp(saved, g_timer_occupied, sizeof(saved)));
    ++cases;
}
static uint64_t random_word(uint64_t *state) {
    *state ^= *state << 13; *state ^= *state >> 7; *state ^= *state << 17;
    return *state;
}
int main(void) {
    for (unsigned length = 0; length <= TIMER_OBJECT_LIMIT; ++length) {
        bitmap_zero(g_timer_occupied, TIMER_OBJECT_LIMIT);
        for (unsigned i = 0; i < length; ++i) bitmap_set_bit(g_timer_occupied, i);
        check();
        for (unsigned word = 0; word < TIMER_SLOT_WORDS; ++word)
            g_timer_occupied[word] = ~g_timer_occupied[word];
        check();
    }
    for (unsigned i = 0; i < TIMER_OBJECT_LIMIT; ++i) {
        bitmap_zero(g_timer_occupied, TIMER_OBJECT_LIMIT);
        bitmap_set_bit(g_timer_occupied, i); check();
        for (unsigned word = 0; word < TIMER_SLOT_WORDS; ++word)
            g_timer_occupied[word] = ~g_timer_occupied[word];
        check();
    }
    const unsigned offsets[] = {0, 48, 56, 64, 112};
    for (unsigned offset = 0; offset < sizeof(offsets)/sizeof(offsets[0]); ++offset)
    for (unsigned mask = 0; mask < 65536U; ++mask) {
        bitmap_zero(g_timer_occupied, TIMER_OBJECT_LIMIT);
        for (unsigned bit = 0; bit < 16; ++bit)
            if (mask & (1U << bit)) bitmap_set_bit(g_timer_occupied, offsets[offset] + bit);
        check();
        for (unsigned word = 0; word < TIMER_SLOT_WORDS; ++word)
            g_timer_occupied[word] = ~g_timer_occupied[word];
        check();
    }
    uint64_t state = 0xa317ca652b8964efULL;
    for (unsigned i = 0; i < 1000000U; ++i) {
        for (unsigned word = 0; word < TIMER_SLOT_WORDS; ++word)
            g_timer_occupied[word] = random_word(&state);
        check();
    }
    printf("PASS timer bitmap: %u linear-model cases, boundary 63/64, read-only masks\n", cases);
}
'''
with tempfile.TemporaryDirectory(prefix='timer-bitmap-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'include'), '-fsanitize=address,undefined',
                    '-fno-pie', '-no-pie', '-x', 'c', '-', '-o', binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True, timeout=60)
