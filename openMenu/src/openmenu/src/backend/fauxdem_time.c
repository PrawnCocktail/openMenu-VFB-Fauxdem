/*
 * File: fauxdem_time.c
 * Fauxdem Time Sync: sets the console clock from the time a Fauxdem in the
 * modem port gets over NTP, without dialing.
 *
 * The console writes the knock "FXTIME" to modem register 0x02. A Fauxdem
 * then shows its answer until the release write (0x00 to 0x02), a modem
 * reset or 2 seconds, and puts the registers back:
 *   0x04, 0x05  'T', 'M', written last
 *   0x06        protocol version
 *   0x07        1 once the Fauxdem has NTP time, 0 before
 *   0x11-0x14   UTC in Unix seconds, little-endian
 *   0x15        CRC-8 (poly 0x07) of 0x06, 0x07 and 0x11-0x14
 * A modem, an adapter or an empty port never shows the signature, so the lookup ends.
 */

#include <stdint.h>
#include <time.h>

#include <arch/timer.h>
#include <dc/g2bus.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include <openmenu_savefile.h>
#include <openmenu_settings.h>

#include "backend/fauxdem_bus.h"
#include "backend/fauxdem_time.h"

#define MODEM_REG(r) (0xa0600400 + ((r) << 2))

#define REG_KNOCK      0x02
#define REG_SIG0       0x04
#define REG_SIG1       0x05
#define REG_VERSION    0x06
#define REG_STATUS     0x07
#define REG_UTC        0x11
#define REG_CHECK      0x15
#define KNOCK          "FXTIME"
#define RELEASE        0x00
#define SIG0           'T'
#define SIG1           'M'
#define PROTO_VERSION  1
#define STATUS_SYNCED  1

#define SIG_WAIT_MS    500
#define FIND_TRIES     5 /* The Fauxdem powers up with the console and can miss the first knocks. */
#define FIND_GAP_MS    1000
#define POLL_MS        1000
#define SYNC_WAIT_MS   45000 /* Ethernet link, DHCP, DNS and NTP after a cold start. */
#define SLEEP_STEP_MS  50
#define UTC_FLOOR      1767225600 /* 2026-01-01. Anything earlier is not a real time. */

typedef enum sync_state { SYNC_IDLE = 0, SYNC_RUNNING, SYNC_DONE, SYNC_FAILED } sync_state_t;

static mutex_t sync_mutex = MUTEX_INITIALIZER;
static sync_state_t sync_state = SYNC_IDLE;
static time_t sync_utc = 0;                
static kthread_t* sync_thread = NULL;      
static volatile int abort_requested = 0;
static int started = 0; /* one lookup per boot */

static void
set_state(sync_state_t state, time_t utc) {
    mutex_lock(&sync_mutex);
    sync_state = state;
    sync_utc = utc;
    mutex_unlock(&sync_mutex);
}

static void
pause_ms(int ms) {
    for (int waited = 0; waited < ms && !abort_requested; waited += SLEEP_STEP_MS) {
        thd_sleep(SLEEP_STEP_MS);
    }
}

static int
signed_on(void) {
    return g2_read_8(MODEM_REG(REG_SIG0)) == SIG0 && g2_read_8(MODEM_REG(REG_SIG1)) == SIG1;
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

/* out: version, status, UTC bytes. Returns 1 if they check out. */
static int
read_answer(uint8_t out[6]) {
    static const uint8_t regs[6] = {REG_VERSION, REG_STATUS, REG_UTC, REG_UTC + 1, REG_UTC + 2, REG_UTC + 3};
    uint8_t check;

    for (int i = 0; i < 6; i++) {
        out[i] = g2_read_8(MODEM_REG(regs[i]));
    }
    check = g2_read_8(MODEM_REG(REG_CHECK));
    /* The signature still up afterwards means the registers were not put
     * back part way through. */
    return crc8(out, 6) == check && signed_on();
}

static void*
sync_main(void* param) {
    uint64_t give_up = timer_ms_gettime64() + SYNC_WAIT_MS;
    uint8_t answer[6];
    time_t utc = 0;
    int shown = 0;
    int ok = 0;

    (void)param;
    for (int i = 0; i < FIND_TRIES && !shown && !abort_requested; i++) {
        if (i) {
            pause_ms(FIND_GAP_MS);
        }
        shown = knock();
    }
    if (!shown) {
        set_state(SYNC_FAILED, 0);
        return NULL;
    }
    for (;;) {
        if (shown) {
            int valid = read_answer(answer);

            release();
            if (valid && answer[0] != PROTO_VERSION) {
                break;
            }
            if (valid && answer[1] == STATUS_SYNCED) {
                utc = (time_t)((uint32_t)answer[2] | (uint32_t)answer[3] << 8 | (uint32_t)answer[4] << 16
                               | (uint32_t)answer[5] << 24);
                ok = utc >= UTC_FLOOR;
                break;
            }
        }
        if (abort_requested || timer_ms_gettime64() >= give_up) {
            break;
        }
        pause_ms(POLL_MS);
        shown = !abort_requested && knock();
    }
    set_state(ok ? SYNC_DONE : SYNC_FAILED, utc);
    return NULL;
}

static void
join_thread(void) {
    kthread_t* thread;

    mutex_lock(&sync_mutex);
    thread = sync_thread;
    sync_thread = NULL;
    mutex_unlock(&sync_mutex);
    if (thread != NULL) {
        thd_join(thread, NULL);
    }
}

void
fauxdem_time_tick(void) {
    sync_state_t state;
    time_t utc;
    int setting = sf_online_time_sync[0];

    if (!started) {
        started = 1;
        if (setting == ONLINE_TIME_SYNC_OFF) {
            return;
        }
        /* Held across the create so an abort cannot miss the pointer. */
        mutex_lock(&sync_mutex);
        sync_thread = thd_create(false, sync_main, NULL);
        sync_state = sync_thread != NULL ? SYNC_RUNNING : SYNC_IDLE;
        mutex_unlock(&sync_mutex);
        return;
    }

    mutex_lock(&sync_mutex);
    state = sync_state;
    utc = sync_utc;
    mutex_unlock(&sync_mutex);
    if (state != SYNC_DONE && state != SYNC_FAILED) {
        return;
    }
    join_thread();
    set_state(SYNC_IDLE, 0);
    if (state == SYNC_DONE && setting != ONLINE_TIME_SYNC_OFF) {
        set_rtc_and_syscfg(utc + (time_t)online_time_sync_minutes(setting) * 60);
    }
}

void
fauxdem_time_abort(void) {
    abort_requested = 1;
    started = 1;
    join_thread();
    set_state(SYNC_IDLE, 0);
}
