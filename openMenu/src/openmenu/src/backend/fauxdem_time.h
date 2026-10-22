/*
 * File: fauxdem_time.h
 * Fauxdem Time Sync: sets the console clock from the Fauxdem.
 */

#pragma once

/* Once per frame from the main loop. The first call starts the background
 * lookup when Online Time Sync is on, and a later one applies its answer. */
void fauxdem_time_tick(void);

/* Stops a lookup in progress and waits for its thread, at most about 60 ms.
 * Call it before anything else drives the modem port. */
void fauxdem_time_abort(void);
