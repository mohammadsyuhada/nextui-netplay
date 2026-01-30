#include "ui.h"

#include <stdio.h>
#include <string.h>

#include <SDL2/SDL_image.h>

#include "defines.h"
#include "api.h"
#include "selfupdate.h"

// Embedded QR code for About page
#include "qr_code_data.h"

// Supported cores/platforms data
typedef struct {
    const char* core_name;
    const char* platforms;
} SupportedCore;

static const SupportedCore supported_cores[] = {
    {"FBNeo", "FBN"},
    {"FCEUmm", "FC, FDS"},
    {"Snes9x/Supafaust", "SFC, SUPA"},
    {"PicoDrive", "MD, SMS"},
    {"PCSX-ReARMed", "PS"},
    {"gpSP", "GBA"},
    {"Gambatte", "GB, GBC"}
};
#define SUPPORTED_CORE_COUNT (sizeof(supported_cores) / sizeof(supported_cores[0]))

void UI_init(void) {
    // Nothing to initialize for now
}

void UI_quit(void) {
    // Nothing to cleanup for now
}

// Render screen header (title pill + hardware status)
static void render_header(SDL_Surface* screen, const char* title, int show_setting) {
    int hw = screen->w;
    char truncated[256];

    int title_width = GFX_truncateText(font.large, title, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE)});

    SDL_Surface* title_text = TTF_RenderUTF8_Blended(font.large, truncated, COLOR_GRAY);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING) + SCALE1(BUTTON_PADDING), SCALE1(PADDING + 4)});
        SDL_FreeSurface(title_text);
    }

    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }
}

// Get menu item label based on current state
const char* UI_getMenuLabel(MenuItem item, NetplayState state) {
    static char about_label[64];  // Static buffer for dynamic About label

    switch (item) {
        case MENU_TOGGLE:
            return (state == NETPLAY_STATE_ENABLED) ? "Disable Netplay" : "Enable Netplay";
        case MENU_SUPPORTED:
            return "Supported Cores";
        case MENU_ABOUT: {
            const SelfUpdateStatus* status = SelfUpdate_getStatus();
            if (status->update_available) {
                snprintf(about_label, sizeof(about_label), "About (Update Available)");
                return about_label;
            }
            return "About";
        }
        default:
            return "";
    }
}

void UI_renderMenu(SDL_Surface* screen, int show_setting, int selected,
                   NetplayState state, bool version_supported) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;

    render_header(screen, "Netplay", show_setting);

    // Menu list starting position (minimal gap after title)
    int menu_y = SCALE1(PADDING + PILL_SIZE + 4);
    int item_h = SCALE1(PILL_SIZE + 4);  // Tighter spacing between menu items
    int max_width = hw - SCALE1(PADDING * 2);

    // Render menu items
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        bool is_selected = (i == selected);
        const char* label = UI_getMenuLabel(i, state);
        char truncated[256];

        // Disable toggle option if version not supported
        bool disabled = (i == MENU_TOGGLE && !version_supported && state != NETPLAY_STATE_ENABLED);

        // Calculate pill width using font.large (matching Music Player style)
        int text_w, text_h;
        TTF_SizeUTF8(font.large, label, &text_w, &text_h);
        int pill_w = text_w + SCALE1(BUTTON_PADDING * 2);
        if (pill_w > max_width) pill_w = max_width;

        // Draw pill background only for selected item (theme-aware)
        SDL_Rect pill_rect = {SCALE1(PADDING), menu_y + i * item_h, pill_w, SCALE1(PILL_SIZE)};
        if (is_selected) {
            GFX_blitPillColor(ASSET_WHITE_PILL, screen, &pill_rect, THEME_COLOR1, RGB_WHITE);
        }
        // No background for unselected items

        // Truncate text if needed
        GFX_truncateText(font.large, label, truncated, pill_w - SCALE1(BUTTON_PADDING * 2), 0);

        // Draw text with theme colors
        SDL_Color text_color;
        if (disabled) {
            text_color = (SDL_Color){100, 100, 100, 255};  // Gray for disabled
        } else if (is_selected) {
            text_color = uintToColour(THEME_COLOR5_255);  // Theme color for selected
        } else {
            text_color = uintToColour(THEME_COLOR4_255);  // Theme color for unselected
        }

        SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font.large, truncated, text_color);
        if (text_surf) {
            int text_y = menu_y + i * item_h + (SCALE1(PILL_SIZE) - text_surf->h) / 2;
            SDL_BlitSurface(text_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), text_y, 0, 0});
            SDL_FreeSurface(text_surf);
        }
    }

    // Show unsupported version message below menu if version not supported
    if (!version_supported && state != NETPLAY_STATE_ENABLED) {
        int msg_y = menu_y + MENU_ITEM_COUNT * item_h + SCALE1(12);
        SDL_Color warn_color = {255, 180, 100, 255};  // Orange/warning color
        SDL_Surface* warn1 = TTF_RenderUTF8_Blended(font.small, "Your NextUI version is not supported.", warn_color);
        if (warn1) {
            SDL_BlitSurface(warn1, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), msg_y, 0, 0});
            SDL_FreeSurface(warn1);
        }
        SDL_Surface* warn2 = TTF_RenderUTF8_Blended(font.small, "Please update to the latest version.", warn_color);
        if (warn2) {
            SDL_BlitSurface(warn2, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), msg_y + SCALE1(16), 0, 0});
            SDL_FreeSurface(warn2);
        }
    }

    // Button hints
    if (selected == MENU_TOGGLE && !version_supported && state != NETPLAY_STATE_ENABLED) {
        GFX_blitButtonGroup((char*[]){"B", "EXIT", NULL}, 1, screen, 1);
    } else {
        GFX_blitButtonGroup((char*[]){"B", "EXIT", "A", "SELECT", NULL}, 1, screen, 1);
    }

    if (show_setting) {
        GFX_blitHardwareHints(screen, show_setting);
    }

    GFX_flip(screen);
}

void UI_renderSupported(SDL_Surface* screen, int show_setting, int scroll_offset) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;

    render_header(screen, "Supported Cores", show_setting);

    // List area
    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int line_h = SCALE1(22);
    int max_lines = (hh - list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN * 2)) / line_h;

    // Render cores list
    int visible_start = scroll_offset;
    int visible_end = scroll_offset + max_lines;
    if (visible_end > (int)SUPPORTED_CORE_COUNT) visible_end = SUPPORTED_CORE_COUNT;

    for (int i = visible_start; i < visible_end; i++) {
        int y = list_y + (i - visible_start) * line_h;
        char line[256];
        snprintf(line, sizeof(line), "%s - %s", supported_cores[i].core_name, supported_cores[i].platforms);

        SDL_Surface* text = TTF_RenderUTF8_Blended(font.small, line, COLOR_WHITE);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), y, 0, 0});
            SDL_FreeSurface(text);
        }
    }

    // Note below the list
    int note_y = list_y + (visible_end - visible_start) * line_h + SCALE1(12);
    SDL_Color note_color = {150, 150, 150, 255};
    SDL_Surface* note_text = TTF_RenderUTF8_Blended(font.tiny, "Other systems supported by these cores", note_color);
    if (note_text) {
        SDL_BlitSurface(note_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), note_y, 0, 0});
        SDL_FreeSurface(note_text);
    }
    SDL_Surface* note_text2 = TTF_RenderUTF8_Blended(font.tiny, "may also have netplay capabilities.", note_color);
    if (note_text2) {
        SDL_BlitSurface(note_text2, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), note_y + SCALE1(14), 0, 0});
        SDL_FreeSurface(note_text2);
    }

    // Scroll indicators
    int center_x = hw / 2 - SCALE1(12);
    if (scroll_offset > 0) {
        GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){center_x, SCALE1(PADDING + PILL_SIZE)});
    }
    if (visible_end < (int)SUPPORTED_CORE_COUNT) {
        GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){center_x, hh - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE)});
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);

    if (show_setting) {
        GFX_blitHardwareHints(screen, show_setting);
    }

    GFX_flip(screen);
}

void UI_renderAbout(SDL_Surface* screen, int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;

    render_header(screen, "About", show_setting);

    // App name with version
    const char* version = SelfUpdate_getVersion();
    char app_name[128];
    snprintf(app_name, sizeof(app_name), "Netplay (%s)", version);

    SDL_Surface* name_text = TTF_RenderUTF8_Blended(font.large, app_name, COLOR_WHITE);
    if (name_text) {
        SDL_BlitSurface(name_text, NULL, screen, &(SDL_Rect){(hw - name_text->w) / 2, SCALE1(PADDING * 3 + PILL_SIZE)});
        SDL_FreeSurface(name_text);
    }

    // Tagline
    int info_y = SCALE1(PADDING * 3 + PILL_SIZE + 30);
    const char* tagline1 = "Multiplayer gaming over WiFi";
    const char* tagline2 = "for your handheld.";

    SDL_Surface* tag1 = TTF_RenderUTF8_Blended(font.small, tagline1, COLOR_WHITE);
    if (tag1) {
        SDL_BlitSurface(tag1, NULL, screen, &(SDL_Rect){(hw - tag1->w) / 2, info_y});
        SDL_FreeSurface(tag1);
    }
    SDL_Surface* tag2 = TTF_RenderUTF8_Blended(font.small, tagline2, COLOR_WHITE);
    if (tag2) {
        SDL_BlitSurface(tag2, NULL, screen, &(SDL_Rect){(hw - tag2->w) / 2, info_y + SCALE1(18)});
        SDL_FreeSurface(tag2);
    }

    // Show update status
    const SelfUpdateStatus* status = SelfUpdate_getStatus();
    SelfUpdateState state = status->state;
    int status_y = info_y + SCALE1(40);

    if (status->update_available) {
        char update_msg[128];
        snprintf(update_msg, sizeof(update_msg), "Update available: %s", status->latest_version);
        SDL_Surface* update_text = TTF_RenderUTF8_Blended(font.small, update_msg, (SDL_Color){100, 255, 100, 255});
        if (update_text) {
            SDL_BlitSurface(update_text, NULL, screen, &(SDL_Rect){(hw - update_text->w) / 2, status_y});
            SDL_FreeSurface(update_text);
        }
    } else if (state == SELFUPDATE_STATE_CHECKING) {
        SDL_Surface* check_text = TTF_RenderUTF8_Blended(font.small, "Checking for updates...", (SDL_Color){200, 200, 200, 255});
        if (check_text) {
            SDL_BlitSurface(check_text, NULL, screen, &(SDL_Rect){(hw - check_text->w) / 2, status_y});
            SDL_FreeSurface(check_text);
        }
    } else if (state == SELFUPDATE_STATE_ERROR) {
        const char* err = strlen(status->error_message) > 0 ? status->error_message : "Update check failed";
        SDL_Surface* err_text = TTF_RenderUTF8_Blended(font.small, err, (SDL_Color){255, 100, 100, 255});
        if (err_text) {
            SDL_BlitSurface(err_text, NULL, screen, &(SDL_Rect){(hw - err_text->w) / 2, status_y});
            SDL_FreeSurface(err_text);
        }
    } else if (state == SELFUPDATE_STATE_IDLE && !status->update_available && strlen(status->latest_version) > 0) {
        // Check completed, no update (latest_version is set when check completes)
        SDL_Surface* uptodate_text = TTF_RenderUTF8_Blended(font.small, "You're up to date", (SDL_Color){150, 150, 150, 255});
        if (uptodate_text) {
            SDL_BlitSurface(uptodate_text, NULL, screen, &(SDL_Rect){(hw - uptodate_text->w) / 2, status_y});
            SDL_FreeSurface(uptodate_text);
        }
    }

    // GitHub QR Code
    SDL_RWops* rw = SDL_RWFromConstMem(qr_code_png, qr_code_png_len);
    if (rw) {
        SDL_Surface* qr_surface = IMG_Load_RW(rw, 1);
        if (qr_surface) {
            int qr_size = SCALE1(75);
            SDL_Rect src_rect = {0, 0, qr_surface->w, qr_surface->h};
            SDL_Rect dst_rect = {(hw - qr_size) / 2, hh - SCALE1(PILL_SIZE + PADDING * 2) - qr_size, qr_size, qr_size};
            SDL_BlitScaled(qr_surface, &src_rect, screen, &dst_rect);
            SDL_FreeSurface(qr_surface);
        }
    }

    // Button hints
    if (status->update_available) {
        GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "UPDATE",  NULL}, 1, screen, 1);
    } else {
        GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
    }

    if (show_setting) {
        GFX_blitHardwareHints(screen, show_setting);
    }

    GFX_flip(screen);
}

void UI_renderUpdateProgress(SDL_Surface* screen, int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;

    render_header(screen, "App Update", show_setting);

    const SelfUpdateStatus* status = SelfUpdate_getStatus();
    SelfUpdateState state = status->state;

    // Version info
    char ver_str[128];
    if (strlen(status->latest_version) > 0) {
        snprintf(ver_str, sizeof(ver_str), "%s  ->  %s", status->current_version, status->latest_version);
    } else {
        snprintf(ver_str, sizeof(ver_str), "%s", status->current_version);
    }

    int ver_y = SCALE1(PADDING * 3 + 35);
    SDL_Surface* ver_text = TTF_RenderUTF8_Blended(font.medium, ver_str, COLOR_GRAY);
    if (ver_text) {
        SDL_BlitSurface(ver_text, NULL, screen, &(SDL_Rect){(hw - ver_text->w) / 2, ver_y});
        SDL_FreeSurface(ver_text);
    }

    // Progress bar (during active update)
    if (state == SELFUPDATE_STATE_DOWNLOADING || state == SELFUPDATE_STATE_EXTRACTING ||
        state == SELFUPDATE_STATE_APPLYING) {
        int bar_w = hw - SCALE1(PADDING * 8);
        int bar_h = SCALE1(8);
        int bar_x = SCALE1(PADDING * 4);
        int bar_y = hh / 2;

        // Background
        SDL_Rect bg_rect = {bar_x, bar_y, bar_w, bar_h};
        SDL_FillRect(screen, &bg_rect, SDL_MapRGB(screen->format, 64, 64, 64));

        // Progress
        int prog_w = (bar_w * status->progress_percent) / 100;
        SDL_Rect prog_rect = {bar_x, bar_y, prog_w, bar_h};
        SDL_FillRect(screen, &prog_rect, SDL_MapRGB(screen->format, 255, 255, 255));
    }

    // Status message
    const char* status_msg = status->status_message;
    if (state == SELFUPDATE_STATE_ERROR && strlen(status->error_message) > 0) {
        status_msg = status->error_message;
    }

    SDL_Color status_color = COLOR_WHITE;
    if (state == SELFUPDATE_STATE_ERROR) {
        status_color = (SDL_Color){255, 100, 100, 255};
    } else if (state == SELFUPDATE_STATE_COMPLETED) {
        status_color = (SDL_Color){100, 255, 100, 255};
    }

    if (strlen(status_msg) > 0) {
        SDL_Surface* status_text = TTF_RenderUTF8_Blended(font.small, status_msg, status_color);
        if (status_text) {
            SDL_BlitSurface(status_text, NULL, screen, &(SDL_Rect){(hw - status_text->w) / 2, hh / 2 + SCALE1(30)});
            SDL_FreeSurface(status_text);
        }
    }

    // Button hints
    if (state == SELFUPDATE_STATE_COMPLETED) {
        GFX_blitButtonGroup((char*[]){"A", "RESTART", NULL}, 1, screen, 1);
    } else if (state == SELFUPDATE_STATE_DOWNLOADING) {
        GFX_blitButtonGroup((char*[]){"B", "CANCEL", NULL}, 1, screen, 1);
    } else {
        GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
    }

    if (show_setting) {
        GFX_blitHardwareHints(screen, show_setting);
    }

    GFX_flip(screen);
}

void UI_renderError(SDL_Surface* screen, int show_setting, const char* error) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;

    render_header(screen, "Error", show_setting);

    // Center content
    int center_y = hh / 2 - SCALE1(10);

    SDL_Color error_color = {255, 100, 100, 255};

    // Error message
    if (error) {
        char wrapped[512];
        strncpy(wrapped, error, sizeof(wrapped) - 1);
        wrapped[sizeof(wrapped) - 1] = '\0';
        GFX_wrapText(font.medium, wrapped, hw - SCALE1(PADDING * 4), 3);

        SDL_Surface* text_surf = TTF_RenderUTF8_Blended_Wrapped(font.medium, wrapped, error_color, hw - SCALE1(PADDING * 4));
        if (text_surf) {
            int x = (hw - text_surf->w) / 2;
            SDL_BlitSurface(text_surf, NULL, screen, &(SDL_Rect){x, center_y, 0, 0});
            SDL_FreeSurface(text_surf);
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);

    if (show_setting) {
        GFX_blitHardwareHints(screen, show_setting);
    }

    GFX_flip(screen);
}

void UI_renderConfirm(SDL_Surface* screen, int show_setting,
                      const char* title, const char* message) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;

    render_header(screen, title, show_setting);

    // Top-aligned content (below header)
    int content_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);

    SDL_Color text_color = COLOR_LIGHT_TEXT;

    // Message
    if (message) {
        char wrapped[512];
        strncpy(wrapped, message, sizeof(wrapped) - 1);
        wrapped[sizeof(wrapped) - 1] = '\0';
        GFX_wrapText(font.medium, wrapped, hw - SCALE1(PADDING * 4), 6);

        SDL_Surface* text_surf = TTF_RenderUTF8_Blended_Wrapped(font.medium, wrapped, text_color, hw - SCALE1(PADDING * 4));
        if (text_surf) {
            SDL_BlitSurface(text_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), content_y, 0, 0});
            SDL_FreeSurface(text_surf);
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"B", "CANCEL", "A", "CONFIRM", NULL}, 1, screen, 1);

    if (show_setting) {
        GFX_blitHardwareHints(screen, show_setting);
    }

    GFX_flip(screen);
}
