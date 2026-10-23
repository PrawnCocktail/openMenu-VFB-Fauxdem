/*
 * File: ui_fauxdem_players.c
 * Project: ui
 * The Fauxdem player list, shown over any screen while X and Y are held.
 */

#include <stdio.h>
#include <string.h>

#include <arch/timer.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#include <openmenu_settings.h>
#include "backend/fauxdem_players.h"
#include "ui/draw_prototypes.h"
#include "ui/font_prototypes.h"
#include "ui/theme_manager.h"
#include "ui/ui_fauxdem_players.h"

#define WAIT_FRAMES   8 /* how long a lone X or Y waits for the other */
#define QUIET_FRAMES  2 /* X shows for a frame after it's let go */
#define REPEAT_FIRST  15
#define REPEAT_NEXT   4
#define ROWS_BITMAP   14
#define ROWS_BMF      13

enum mode { M_IDLE, M_WAIT, M_PASS, M_OPEN, M_QUIET };

static enum mode mode = M_IDLE;
static enum control waiting = NONE; /* the press held back in M_WAIT */
static int frames;
static int up_frames;
static enum control repeat_dir = NONE;
static int repeat_count;

static fauxdem_player_t list[FAUXDEM_PLAYER_MAX];
static fauxdem_players_info_t info;
static int list_count;
static int list_generation = -1;
static int scroll;

/* X and Y as any controller holds them now. */
static void
held_xy(int* x, int* y) {
    *x = *y = 0;
    for (int i = 0;; i++) {
        maple_device_t* dev = maple_enum_type(i, MAPLE_FUNC_CONTROLLER);
        cont_state_t* st;

        if (dev == NULL) {
            break;
        }
        st = (cont_state_t*)maple_dev_status(dev);
        if (st == NULL) {
            continue;
        }
        *x |= (st->buttons & CONT_X) != 0;
        *y |= (st->buttons & CONT_Y) != 0;
    }
}

static int
visible_rows(void) {
    return sf_ui[0] == UI_SCROLL || sf_ui[0] == UI_FOLDERS ? ROWS_BITMAP : ROWS_BMF;
}

static void
scroll_by(int step) {
    int max = list_count - visible_rows();

    scroll += step;
    if (scroll > max) {
        scroll = max;
    }
    if (scroll < 0) {
        scroll = 0;
    }
}

/* Up and down move a row, held they repeat; the triggers move a page. */
static void
navigate(enum control input) {
    int step = 0;

    if (input == UP || input == DOWN) {
        if (input != repeat_dir) {
            repeat_count = REPEAT_FIRST;
            step = 1;
        } else if (--repeat_count <= 0) {
            repeat_count = REPEAT_NEXT;
            step = 1;
        }
        repeat_dir = input;
        scroll_by(input == UP ? -step : step);
        return;
    }
    if ((input == TRIG_L || input == TRIG_R) && input != repeat_dir) {
        scroll_by(input == TRIG_L ? -visible_rows() : visible_rows());
    }
    repeat_dir = input;
}

static void
open_list(void) {
    mode = M_OPEN;
    scroll = 0;
    repeat_dir = NONE;
    fauxdem_players_set_showing(1);
}

/* X and Y are let through again once both have been up for a moment. */
static int
quiet_over(int x, int y) {
    up_frames = x || y ? 0 : up_frames + 1;
    return up_frames >= QUIET_FRAMES;
}

enum control
fauxdem_players_input(enum control input) {
    int x, y;

    if (!fauxdem_players_available()) {
        if (mode == M_OPEN) {
            fauxdem_players_set_showing(0);
        }
        mode = M_IDLE;
        return input;
    }
    held_xy(&x, &y);
    switch (mode) {
        case M_OPEN:
            if (x && y) {
                navigate(input);
                return NONE;
            }
            fauxdem_players_set_showing(0);
            mode = M_QUIET;
            up_frames = 0;
            return NONE;
        case M_QUIET:
            if (quiet_over(x, y)) {
                mode = M_IDLE;
            }
            return input == X || input == Y ? NONE : input;
        case M_PASS:
            /* The press went through: the rest of it does too. */
            if (quiet_over(x, y)) {
                mode = M_IDLE;
            }
            return input;
        case M_WAIT:
            if (x && y) {
                open_list();
                return NONE;
            }
            if (!(waiting == X ? x : y) || ++frames >= WAIT_FRAMES) {
                mode = M_PASS;
                up_frames = 0;
                return waiting;
            }
            return input == X || input == Y ? NONE : input;
        case M_IDLE:
        default:
            if (x && y) {
                open_list();
                return NONE;
            }
            if (input == X || input == Y) {
                mode = M_WAIT;
                waiting = input;
                frames = 0;
                return NONE;
            }
            return input;
    }
}

/* Cut to max characters with "..." when longer. */
static void
cut(char* out, size_t out_len, const char* text, int max) {
    if ((int)strlen(text) > max) {
        snprintf(out, out_len, "%.*s...", max - 3, text);
    } else {
        snprintf(out, out_len, "%s", text);
    }
}

static void
summary_line(char* out, size_t out_len) {
    char when[24];
    uint64_t now = timer_ms_gettime64();
    unsigned age = now > info.updated_ms ? (unsigned)((now - info.updated_ms) / 1000) : 0;

    if (!info.have_list) {
        if (info.no_internet) {
            snprintf(out, out_len, "The Fauxdem isn't online yet.");
        } else if (info.failed && !info.busy) {
            snprintf(out, out_len, "Couldn't reach dreamcast.online.");
        } else {
            snprintf(out, out_len, "Getting the player list...");
        }
        return;
    }
    if (age < 60) {
        snprintf(when, sizeof(when), "%us ago", age);
    } else if (age < 3600) {
        snprintf(when, sizeof(when), "%um ago", age / 60);
    } else {
        snprintf(when, sizeof(when), "%uh ago", age / 3600);
    }
    if (info.failed) {
        snprintf(out, out_len, "Refresh failed, list from %s", when);
    } else if (info.total == 1) {
        snprintf(out, out_len, "1 player online (%s)", when);
    } else if (info.total == 0) {
        snprintf(out, out_len, "No players online (%s)", when);
    } else {
        snprintf(out, out_len, "%d players online (%s)", info.total, when);
    }
}

/* A string wider than max_width is drawn smaller to fit. */
static void
draw_fit(int x, int y, uint32_t color, const char* text, int max_width) {
    float w = font_bmf_text_width(text);

    if (w > (float)max_width) {
        font_bmf_set_height(24.0f * (float)max_width / w);
        font_bmf_draw(x, y, color, text);
        font_bmf_set_height(24.0f);
    } else {
        font_bmf_draw(x, y, color, text);
    }
}

/* The settings menu's box: a border, the background and, for the bitmap font
 * screens, a header strip. */
static void
draw_box(int x, int y, int width, int height, const struct theme_color* colors, int bitmap) {
    draw_draw_quad(x - 2, y - 2, width + 4, height + 4, colors->menu_bkg_border_color);
    draw_draw_quad(x, y, width, height, colors->menu_bkg_color);
    if (bitmap) {
        draw_draw_quad(x, y, width, 20, colors->menu_bkg_border_color);
    }
}

static void
draw_scrollbar(int x, int top, int rows, int line_height, const struct theme_color* colors) {
    const int track_h = rows * line_height;
    int thumb_h = track_h * rows / list_count;
    int max = list_count - rows;

    if (thumb_h < 16) {
        thumb_h = 16;
    }
    draw_draw_quad(x, top, 6, track_h, colors->menu_bkg_border_color);
    draw_draw_quad(x + 1, top + 1 + (track_h - 2 - thumb_h) * scroll / max, 4, thumb_h, colors->menu_highlight_color);
}

void
draw_fauxdem_players_overlay(struct theme_color* colors, uint32_t title_color, int ui_mode) {
    const int bitmap = ui_mode == UI_SCROLL || ui_mode == UI_FOLDERS;
    const int max_rows = bitmap ? ROWS_BITMAP : ROWS_BMF;
    fauxdem_players_info_t fresh;
    char summary[64];
    int rows;

    if (mode != M_OPEN) {
        return;
    }
    fauxdem_players_status(&fresh);
    if (fresh.generation != list_generation) {
        list_count = fauxdem_players_copy(list, FAUXDEM_PLAYER_MAX, &fresh);
        list_generation = fresh.generation;
        scroll_by(0);
    }
    info = fresh;
    summary_line(summary, sizeof(summary));
    rows = list_count < max_rows ? list_count : max_rows;
    z_set_cond(225.0f);

    if (bitmap) {
        const int line_height = 24;
        const int width = 408;
        const int pad = 8;
        const int height = 20 + pad + line_height + (rows ? pad + 2 + pad + rows * line_height : 0) + pad;
        const int x = (640 - width) / 2;
        const int y = (480 - height) / 2;
        const int x_item = x + 8;
        const int sep_y = y + 20 + pad + line_height;
        const int list_top = sep_y + 2 + pad;
        char line[64];
        char title[32];
        char name[24];

        draw_box(x, y, width, height, colors, 1);
        if (rows) {
            draw_draw_quad(x, sep_y, width, 2, colors->menu_bkg_border_color);
        }
        if (list_count > rows) {
            draw_scrollbar(x + width - 12, list_top, rows, line_height, colors);
        }
        font_bmp_begin_draw();
        font_bmp_set_color(title_color);
        font_bmp_draw_main(x + width / 2 - (14 * 8 / 2), y + 2, "Online Players");
        font_bmp_set_color(colors->menu_text_color);
        font_bmp_draw_main(x_item, y + 20 + pad + 4, summary);
        for (int i = 0; i < rows; i++) {
            const fauxdem_player_t* p = &list[scroll + i];
            const int title_max = list_count > rows ? 23 : 24;

            cut(title, sizeof(title), p->title[0] ? p->title : "(Idle)", title_max);
            cut(name, sizeof(name), p->name, 20);
            snprintf(line, sizeof(line), "%-2s %-*s %s", p->country, title_max, title, name);
            font_bmp_draw_main(x_item, list_top + i * line_height + 4, line);
        }
    } else {
        const int line_height = 26;
        const int width = 484;
        const int title_height = 28;
        const int pad = 8;
        const int height = title_height + pad + line_height + (rows ? pad + 2 + pad + rows * line_height : 0) + pad;
        const int x = (640 - width) / 2;
        const int y = (480 - height) / 2;
        const int x_item = x + 4;
        const int sep_y = y + title_height + pad + line_height;
        const int list_top = sep_y + 2 + pad;
        const uint32_t text = colors->menu_text_color;

        draw_box(x, y, width, height, colors, 0);
        if (rows) {
            draw_draw_quad(x, sep_y, width, 2, colors->menu_bkg_border_color);
        }
        if (list_count > rows) {
            draw_scrollbar(x + width - 12, list_top, rows, line_height, colors);
        }
        font_bmf_begin_draw();
        font_bmf_set_height(24.0f);
        font_bmf_draw(x_item, y + 2, title_color, "Online Players");
        draw_fit(x_item, y + title_height + pad, text, summary, width - 8);
        for (int i = 0; i < rows; i++) {
            const fauxdem_player_t* p = &list[scroll + i];
            const int row_y = list_top + i * line_height;

            /* The BMF drawer reads past an empty string, so a missing country
             * draws nothing in its column. */
            if (p->country[0] != '\0') {
                font_bmf_draw(x_item, row_y, text, p->country);
            }
            draw_fit(x_item + 48, row_y, text, p->title[0] ? p->title : "(Idle)", 190);
            /* The name column stops 8 px short of the scrollbar track. */
            draw_fit(x_item + 248, row_y, text, p->name, width - 272);
        }
        font_bmf_set_height_default();
    }
}
