#ifndef __UI_H__
#define __UI_H__

#include <stdbool.h>
#include <SDL2/SDL.h>
#include "defines.h"
#include "api.h"
#include "fileops.h"
#include "selfupdate.h"

// Menu items
typedef enum {
    MENU_TOGGLE = 0,        // Enable/Disable based on state
    MENU_SUPPORTED,         // Supported platforms/cores
    MENU_ABOUT,             // About page
    MENU_ITEM_COUNT
} MenuItem;

// Extern declarations for compatibility info (defined in netplay.c)
extern char compatible_version[64];
extern char compatible_commit[32];
extern bool using_compatible_version;

// Initialize UI
void UI_init(void);

// Cleanup UI
void UI_quit(void);

// Render main menu
void UI_renderMenu(SDL_Surface* screen, int show_setting, int selected,
                   NetplayState state, bool version_supported);

// Render supported platforms/cores screen
void UI_renderSupported(SDL_Surface* screen, int show_setting, int scroll_offset);

// Render about screen
void UI_renderAbout(SDL_Surface* screen, int show_setting);

// Render error screen
void UI_renderError(SDL_Surface* screen, int show_setting, const char* error);

// Render confirmation dialog
void UI_renderConfirm(SDL_Surface* screen, int show_setting,
                      const char* title, const char* message);

// Render update progress screen
void UI_renderUpdateProgress(SDL_Surface* screen, int show_setting);

// Get menu item label based on current state
const char* UI_getMenuLabel(MenuItem item, NetplayState state);

#endif
