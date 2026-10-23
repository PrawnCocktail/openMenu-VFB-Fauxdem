/*
 * File: fauxdem_bus.c
 * Project: openmenu
 * One knock at a time on the Fauxdem's register bus.
 */

#include <kos/mutex.h>

#include "backend/fauxdem_bus.h"

static mutex_t bus_mutex = MUTEX_INITIALIZER;

void
fauxdem_bus_lock(void) {
    mutex_lock(&bus_mutex);
}

void
fauxdem_bus_unlock(void) {
    mutex_unlock(&bus_mutex);
}
