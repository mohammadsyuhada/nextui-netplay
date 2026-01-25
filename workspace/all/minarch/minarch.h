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

// Save current config to file
void minarch_saveConfig(void);

// Reload game to apply option changes (e.g., gpSP serial mode)
// Unloads and reloads ROM so core re-reads options during load_game()
void minarch_reloadGame(void);

#endif /* MINARCH_H */
