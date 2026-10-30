// Statically linked, user-generated game code for the PS5 title.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gamelib.h"
#include "../game_sites.h"
#include "../rt.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef GAMECODE_CONSOLE
#include "../gamecode_link.h"

static GameLibTables tables = {GAMELIB_ABI, GAMELIB_ID, NULL, NULL, NULL, NULL, NULL};

void ps5_gamelib_set_image(const GameLinkImage *image) {
    tables.modules = image->game_modules;
    tables.nmodules = image->game_module_count;
    tables.version = image->game_version;
    tables.sites = image->game_sites;
    tables.nsites = image->game_site_count;
}
#define game_version (tables.version)
#define game_sites (tables.sites)
#define game_site_count (*tables.nsites)
#define game_module_count (*tables.nmodules)
#else
extern const GameLibModule game_modules[];
extern const uint32_t game_module_count;
extern const char game_version[];
extern const GameLibSite game_sites[];
extern const uint32_t game_site_count;
#endif

GameSites g_site;
#ifdef GAMECODE_CONSOLE
// This variant accepts only 1.9.0; keep the pointer stable while early HD work runs.
const char *g_game_version = "1.9.0";
#else
const char *g_game_version = "";
#endif
#ifndef GAMECODE_CONSOLE
static const GameLibTables tables = {
    GAMELIB_ABI, GAMELIB_ID, game_modules, &game_module_count,
    game_version, game_sites, &game_site_count
};
#endif
static bool loaded;

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    (void)game_dir;
    static const struct { const char *name; size_t offset; bool optional; } wanted[] = {
#define GAME_SITE_ENTRY(n) {#n, offsetof(GameSites, n), false},
#define GAME_SITE_OPTIONAL_ENTRY(n) {#n, offsetof(GameSites, n), true},
        GAME_SITES(GAME_SITE_ENTRY)
        GAME_SITES_OPTIONAL(GAME_SITE_OPTIONAL_ENTRY)
#undef GAME_SITE_ENTRY
#undef GAME_SITE_OPTIONAL_ENTRY
    };
#ifdef GAMECODE_CONSOLE
    if (!tables.modules || !tables.nmodules || !tables.version || !tables.sites || !tables.nsites) {
        if (why && why_size) snprintf(why, why_size, "game code layout symbols are missing");
        return false;
    }
#endif
    if (strcmp(game_version, "1.9.0")) {
        if (why && why_size) snprintf(why, why_size, "linked game code is not version 1.9.0");
        return false;
    }
    for (size_t i = 0; i < sizeof wanted / sizeof *wanted; i++) {
        uint32_t j = 0;
        while (j < game_site_count && strcmp(game_sites[j].name, wanted[i].name)) j++;
        if (j == game_site_count && !wanted[i].optional) {
            if (why && why_size) snprintf(why, why_size, "linked game code has no address for %s", wanted[i].name);
            return false;
        }
        *(uint64_t *)((char *)&g_site + wanted[i].offset) = j < game_site_count ? game_sites[j].addr : 0;
    }
#ifndef GAMECODE_CONSOLE
    g_game_version = game_version;
#endif
    loaded = true;
    return true;
}
const GameLibTables *gamelib_tables(void) { return loaded ? &tables : NULL; }
#ifdef GAMECODE_CONSOLE
bool gamelib_present(const char *game_dir) { (void)game_dir; return tables.nmodules && game_module_count != 0; }
#else
bool gamelib_present(const char *game_dir) { (void)game_dir; return game_module_count != 0; }
#endif
bool gamelib_compat(void) { return false; }
const char *gamelib_id(void) { return GAMELIB_ID; }
const char *gamelib_cpu_problem(void) { return NULL; } // PS5 is the fixed x86-64-v3 target.
bool gamelib_other_cpu(const char *game_dir) { (void)game_dir; return false; }
