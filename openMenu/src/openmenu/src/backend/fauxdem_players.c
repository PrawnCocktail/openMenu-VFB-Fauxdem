/*
 * File: fauxdem_players.c
 * Fauxdem Players: the Dreamcast Now player list from a Fauxdem.
 *
 * The console writes the knock "FXLIST" to modem register 0x02. A Fauxdem
 * then shows its answer until the release write (0x00 to 0x02), a modem
 * reset or 2 seconds, and puts the registers back:
 *   0x04, 0x05  'P', 'L', written last
 *   0x06        protocol version
 *   0x07        flags: 1 list there, 2 last fetch failed, 4 fetching, 8 no internet
 *   0x08-0x09   list length in bytes
 *   0x0A-0x0B   list generation, +1 for every list
 *   0x0C-0x0D   seconds since the list arrived
 *   0x0E-0x0F   online players in the feed
 *   0x11-0x14   CRC-32 of the list
 *   0x15        CRC-8 (poly 0x07) of 0x06-0x0F and 0x11-0x14
 * Two-byte values are little-endian. Each read of 0x01 then gives the next
 * byte of the list: a line per online player, country, tab, name, tab, game.
 * The Fauxdem fetches only while it is asked, so the worker asks every few
 * seconds. A modem, an adapter or an empty port never shows the signature.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <arch/timer.h>
#include <dc/g2bus.h>
#include <kos/mutex.h>
#include <kos/thread.h>

#include "backend/fauxdem_bus.h"
#include "backend/fauxdem_players.h"

#define MODEM_REG(r) (0xa0600400 + ((r) << 2))

#define REG_STREAM    0x01
#define REG_KNOCK     0x02
#define REG_SIG0      0x04
#define REG_SIG1      0x05
#define REG_VERSION   0x06
#define REG_CRC       0x11
#define REG_CHECK     0x15
#define KNOCK         "FXLIST"
#define RELEASE       0x00
#define SIG0          'P'
#define SIG1          'L'
#define PROTO_VERSION 1
#define FLAG_HAVE     0x01
#define FLAG_FAILED   0x02
#define FLAG_BUSY     0x04
#define FLAG_NO_WAN   0x08
#define TEXT_MAX      8192

#define SIG_WAIT_MS   500
#define FIND_TRIES    5 
#define FIND_GAP_MS   1000
#define POLL_MS       5000
#define SHOWING_MS    1000
#define SLEEP_STEP_MS 50

static mutex_t list_mutex = MUTEX_INITIALIZER;
static fauxdem_player_t players[FAUXDEM_PLAYER_MAX];
static fauxdem_players_info_t status;               
static mutex_t worker_mutex = MUTEX_INITIALIZER;
static kthread_t* worker = NULL; 
static volatile int abort_requested = 0;
static volatile int available = 0;
static volatile int showing = 0;
static int started = 0;

/* The worker's own. */
static char text[TEXT_MAX];
static fauxdem_player_t parsed[FAUXDEM_PLAYER_MAX];
static int last_generation = -1;

static void
pause_ms(int ms) {
    for (int waited = 0; waited < ms && !abort_requested; waited += SLEEP_STEP_MS) {
        if (showing && waited >= SHOWING_MS) {
            return;
        }
        thd_sleep(SLEEP_STEP_MS);
    }
}

static uint8_t
reg(int r) {
    return g2_read_8(MODEM_REG(r));
}

static int
signed_on(void) {
    return reg(REG_SIG0) == SIG0 && reg(REG_SIG1) == SIG1;
}

static void
release(void) {
    g2_write_8(MODEM_REG(REG_KNOCK), RELEASE);
    fauxdem_bus_unlock();
}

static int
knock(void) {
    uint64_t deadline;

    fauxdem_bus_lock();
    for (const char* p = KNOCK; *p; p++) {
        g2_write_8(MODEM_REG(REG_KNOCK), (uint8_t)*p);
    }
    deadline = timer_ms_gettime64() + SIG_WAIT_MS;
    while (timer_ms_gettime64() < deadline) {
        if (abort_requested) {
            release();
            return 0;
        }
        if (signed_on()) {
            return 1;
        }
        thd_sleep(10);
    }
    fauxdem_bus_unlock();
    return 0;
}

static uint8_t
crc8(const uint8_t* p, int n) {
    uint8_t crc = 0;

    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc & 0x80) ? (uint8_t)(crc << 1 ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t
crc32(const char* p, int n) {
    uint32_t crc = ~0u;

    while (n--) {
        crc ^= (uint8_t)*p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
        }
    }
    return ~crc;
}

static int
player_order(const void* left, const void* right) {
    const fauxdem_player_t* a = left;
    const fauxdem_player_t* b = right;

    if ((a->country[0] == '\0') != (b->country[0] == '\0')) {
        return a->country[0] == '\0' ? 1 : -1;
    }
    if (strcmp(a->country, b->country) != 0) {
        return strcmp(a->country, b->country);
    }
    if ((a->title[0] == '\0') != (b->title[0] == '\0')) {
        return a->title[0] == '\0' ? 1 : -1;
    }
    if (strcasecmp(a->title, b->title) != 0) {
        return strcasecmp(a->title, b->title);
    }
    return strcasecmp(a->name, b->name);
}

static void
copy_field(char* out, size_t out_len, const char* start, const char* end) {
    size_t n = (size_t)(end - start);

    if (n > out_len - 1) {
        n = out_len - 1;
    }
    memcpy(out, start, n);
    out[n] = '\0';
}

static int
parse_text(int len) {
    const char* p = text;
    const char* end = text + len;
    int count = 0;

    while (p < end && count < FAUXDEM_PLAYER_MAX) {
        const char* line_end = memchr(p, '\n', (size_t)(end - p));
        const char* tab1;
        const char* tab2;

        if (line_end == NULL) {
            break;
        }
        tab1 = memchr(p, '\t', (size_t)(line_end - p));
        tab2 = tab1 != NULL ? memchr(tab1 + 1, '\t', (size_t)(line_end - tab1 - 1)) : NULL;
        if (tab2 != NULL) {
            fauxdem_player_t* out = &parsed[count++];
            copy_field(out->country, sizeof(out->country), p, tab1);
            copy_field(out->name, sizeof(out->name), tab1 + 1, tab2);
            copy_field(out->title, sizeof(out->title), tab2 + 1, line_end);
        }
        p = line_end + 1;
    }
    qsort(parsed, (size_t)count, sizeof(parsed[0]), player_order);
    return count;
}

static int
ask(void) {
    static const uint8_t regs[14] = {0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
                                     0x0D, 0x0E, 0x0F, 0x11, 0x12, 0x13, 0x14};
    uint8_t a[14];
    int flags, len, generation, age, total;
    uint32_t crc;
    int count = -1;

    if (!knock()) {
        return 0;
    }
    for (int i = 0; i < 14; i++) {
        a[i] = reg(regs[i]);
    }
    /* The signature still up afterwards means the registers were not put back
     * part way through. */
    if (crc8(a, 14) != reg(REG_CHECK) || !signed_on() || a[0] != PROTO_VERSION) {
        release();
        return 1;
    }
    flags = a[1];
    len = a[2] | a[3] << 8;
    generation = a[4] | a[5] << 8;
    age = a[6] | a[7] << 8;
    total = a[8] | a[9] << 8;
    crc = (uint32_t)a[10] | (uint32_t)a[11] << 8 | (uint32_t)a[12] << 16 | (uint32_t)a[13] << 24;
    if ((flags & FLAG_HAVE) && generation != last_generation && len <= TEXT_MAX) {
        for (int i = 0; i < len; i++) {
            text[i] = (char)reg(REG_STREAM);
        }
        if (crc32(text, len) == crc && signed_on()) {
            count = parse_text(len);
            last_generation = generation;
        }
    }
    release();

    mutex_lock(&list_mutex);
    if (count >= 0) {
        memcpy(players, parsed, sizeof(players[0]) * (size_t)count);
        status.count = count;
        status.total = total;
        status.have_list = 1;
        status.generation++;
    }
    if (flags & FLAG_HAVE) {
        status.updated_ms = timer_ms_gettime64() - (uint64_t)age * 1000;
    }
    status.failed = (flags & FLAG_FAILED) != 0;
    status.busy = (flags & FLAG_BUSY) != 0;
    status.no_internet = (flags & FLAG_NO_WAN) != 0;
    mutex_unlock(&list_mutex);
    return 1;
}

static void*
worker_main(void* param) {
    (void)param;
    for (int i = 0; i < FIND_TRIES && !available && !abort_requested; i++) {
        if (i) {
            pause_ms(FIND_GAP_MS);
        }
        available = ask();
    }
    while (available && !abort_requested) {
        pause_ms(POLL_MS);
        if (!abort_requested) {
            ask();
        }
    }
    return NULL;
}

void
fauxdem_players_tick(void) {
    /* Held across the create so an abort cannot miss the pointer. */
    mutex_lock(&worker_mutex);
    if (!started) {
        started = 1;
        worker = thd_create(false, worker_main, NULL);
    }
    mutex_unlock(&worker_mutex);
}

void
fauxdem_players_abort(void) {
    kthread_t* thread;

    abort_requested = 1;
    available = 0;
    mutex_lock(&worker_mutex);
    started = 1;
    thread = worker;
    worker = NULL;
    mutex_unlock(&worker_mutex);
    if (thread != NULL) {
        thd_join(thread, NULL);
    }
    available = 0; /* the worker may have set it while stopping */
}

int
fauxdem_players_available(void) {
    return available;
}

void
fauxdem_players_set_showing(int on) {
    showing = on;
}

int
fauxdem_players_copy(fauxdem_player_t* out, int max, fauxdem_players_info_t* info) {
    int count;

    mutex_lock(&list_mutex);
    count = status.count < max ? status.count : max;
    memcpy(out, players, sizeof(players[0]) * (size_t)count);
    *info = status;
    mutex_unlock(&list_mutex);
    return count;
}

void
fauxdem_players_status(fauxdem_players_info_t* info) {
    mutex_lock(&list_mutex);
    *info = status;
    mutex_unlock(&list_mutex);
}
