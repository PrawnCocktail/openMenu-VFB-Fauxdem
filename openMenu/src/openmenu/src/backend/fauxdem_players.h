/*
 * File: fauxdem_players.h
 * Fauxdem Players: the Dreamcast Now player list without dialing.
 */

#pragma once

#include <stdint.h>

#define FAUXDEM_PLAYER_MAX 128

typedef struct fauxdem_player {
    char country[3];
    char name[32];
    char title[64];
} fauxdem_player_t;

typedef struct fauxdem_players_info {
    int have_list;   
    int failed;      
    int busy;        
    int no_internet;
    int total;      
    int count;       
    int generation; 
    uint64_t updated_ms;
} fauxdem_players_info_t;

void fauxdem_players_tick(void);

void fauxdem_players_abort(void);

int fauxdem_players_available(void);

void fauxdem_players_set_showing(int showing);

int fauxdem_players_copy(fauxdem_player_t* out, int max, fauxdem_players_info_t* info);

void fauxdem_players_status(fauxdem_players_info_t* info);
