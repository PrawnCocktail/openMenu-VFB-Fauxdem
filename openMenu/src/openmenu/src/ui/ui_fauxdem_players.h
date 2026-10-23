/*
 * File: ui_fauxdem_players.h
 * Project: ui
 * The Fauxdem player list, shown over any screen while X and Y are held.
 */

#pragma once

#include <stdint.h>

#include "ui/common.h"

struct theme_color;

/* Main loop, before the screen sees the input. With a Fauxdem found, a lone
 * X or Y waits a few frames to see whether the other joins it; X and Y
 * together open the list until either is let go. Without one the input
 * passes untouched. */
enum control fauxdem_players_input(enum control input);

/* Last thing each screen draws. Does nothing while the list is closed. */
void draw_fauxdem_players_overlay(struct theme_color* colors, uint32_t title_color, int ui_mode);
