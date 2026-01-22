/*
 * NextUI Minarch Header
 * Shared types and declarations for minarch menu system
 */

#ifndef MINARCH_H
#define MINARCH_H

// Menu callback result codes
enum {
    MENU_CALLBACK_NOP = 0,
    MENU_CALLBACK_EXIT = 1,
    MENU_CALLBACK_NEXT_ITEM = 2,
};

// Forward declaration for opaque pointer
typedef struct OptionList OptionList;

// Core option list accessor
OptionList* minarch_getCoreOptionList(void);

// Generic option functions
char* OptionList_getOptionValue(OptionList* list, const char* key);
void OptionList_setOptionValue(OptionList* list, const char* key, const char* value);

// Force core to process option changes immediately
// Runs one frame with video output suppressed to trigger check_variables()
void minarch_forceCoreOptionUpdate(void);

// Reset the core (reinitializes game state including serial mode)
void minarch_resetCore(void);

// Save current config to file (preserves option changes before core reset)
void minarch_saveConfig(void);

// Reload the game to reinitialize core state (including serial mode)
void minarch_reloadGame(void);

// Deferred reload - use this from menu callbacks to avoid segfaults
// The actual reload happens after the menu closes
void minarch_requestReload(void);
int minarch_hasPendingReload(void);
void minarch_doPendingReload(void);

#endif /* MINARCH_H */
