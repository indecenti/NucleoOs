#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAX_META_PATH 128
#define MAX_META_TEXT 64

struct TrackMeta {
    char path[MAX_META_PATH];
    char title[MAX_META_TEXT];
    char artist[MAX_META_TEXT];
    char genre[MAX_META_TEXT];
    uint32_t plays;
    bool fav;
};

// Initialize the DB (starts background indexing)
void music_db_init(void);

// Query functions (returns an array of populated TrackMeta, caller frees)
// If query is NULL, returns all matching the filter type.
// Types: 0=All, 1=Genre, 2=Artist, 3=Favorites, 4=Most Played
int music_db_search(const char *query, int filter_type, struct TrackMeta **out_results);

// Get unique list of genres or artists (returns array of strings, caller frees)
int music_db_get_unique(int type, char ***out_list); // 1=Genre, 2=Artist

// Update play count or favorite status
void music_db_add_play(const char *path);
void music_db_set_fav(const char *path, bool fav);
bool music_db_is_fav(const char *path);
