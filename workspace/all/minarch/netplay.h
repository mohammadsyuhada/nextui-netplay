/*
 * NextUI Netplay Module
 * Based on RetroArch netplay architecture
 * Implements frame-synchronized multiplayer over WiFi
 */

#ifndef NETPLAY_H
#define NETPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define NETPLAY_DEFAULT_PORT 55435
#define NETPLAY_DISCOVERY_PORT 55436
#define NETPLAY_MAGIC "NXNP"
#define NETPLAY_PROTOCOL_VERSION 2
#define NETPLAY_MAX_GAME_NAME 64
#define NETPLAY_MAX_HOSTS 8

// Frame buffer for rollback (power of 2 for efficient wraparound)
#define NETPLAY_FRAME_BUFFER_SIZE 64
#define NETPLAY_FRAME_MASK (NETPLAY_FRAME_BUFFER_SIZE - 1)

// Hotspot SSID prefix for Netplay
#define NETPLAY_HOTSPOT_SSID_PREFIX "Netplay-"

typedef enum {
    NETPLAY_CONN_WIFI = 0,
    NETPLAY_CONN_HOTSPOT
} NetplayConnMethod;

// Input latency frames (to hide network jitter)
#define NETPLAY_INPUT_LATENCY_FRAMES 2

typedef enum {
    NETPLAY_OFF = 0,
    NETPLAY_HOST,
    NETPLAY_CLIENT
} NetplayMode;

typedef enum {
    NETPLAY_STATE_IDLE = 0,
    NETPLAY_STATE_WAITING,      // Host waiting for client
    NETPLAY_STATE_CONNECTING,   // Client connecting to host
    NETPLAY_STATE_SYNCING,      // Exchanging initial state
    NETPLAY_STATE_PLAYING,      // Active gameplay
    NETPLAY_STATE_STALLED,      // Waiting for remote input
    NETPLAY_STATE_PAUSED,       // Local or remote player has paused (menu open)
    NETPLAY_STATE_DISCONNECTED,
    NETPLAY_STATE_ERROR
} NetplayState;

typedef struct {
    char game_name[NETPLAY_MAX_GAME_NAME];
    char host_ip[16];
    uint16_t port;
    uint32_t game_crc;
} NetplayHostInfo;

// Initialize/cleanup
void Netplay_init(void);
void Netplay_quit(void);

// Connection management
int Netplay_startHost(const char* game_name, uint32_t game_crc);
int Netplay_startHostWithHotspot(const char* game_name, uint32_t game_crc);
int Netplay_stopHost(void);
int Netplay_connectToHost(const char* ip, uint16_t port);
void Netplay_disconnect(void);

// Hotspot mode
bool Netplay_isUsingHotspot(void);

// Status queries
NetplayMode Netplay_getMode(void);
NetplayState Netplay_getState(void);
bool Netplay_isConnected(void);
bool Netplay_isActive(void);
const char* Netplay_getStatusMessage(void);
const char* Netplay_getLocalIP(void);
void Netplay_clearLocalIP(void);
bool Netplay_hasNetworkConnection(void);

// Host discovery (for client)
int Netplay_startDiscovery(void);
void Netplay_stopDiscovery(void);
int Netplay_getDiscoveredHosts(NetplayHostInfo* hosts, int max_hosts);

// Frame synchronization (RetroArch-style)
// Called at the start of each frame - handles network polling and sync
bool Netplay_preFrame(void);

// Get inputs for a specific player (called by input_state_callback)
uint16_t Netplay_getInputState(unsigned port);

// Set local player's input for current frame
void Netplay_setLocalInput(uint16_t input);

// Called at end of each frame - sends data, advances frame counter
void Netplay_postFrame(void);

// Check if we should skip this frame (stalled waiting for input)
bool Netplay_shouldStall(void);

// Audio control - returns true if audio should be silenced (during stall)
bool Netplay_shouldSilenceAudio(void);

// State synchronization
int Netplay_sendState(const void* data, size_t size);
int Netplay_receiveState(void* data, size_t size);
bool Netplay_needsStateSync(void);
void Netplay_completeStateSync(void);

// Frame info
uint32_t Netplay_getFrameCount(void);

// Update function (call periodically for connection handling)
void Netplay_update(void);

// Pause/resume for menu (keeps connection alive)
void Netplay_pause(void);           // Called when entering menu
void Netplay_resume(void);          // Called when exiting menu
void Netplay_pollWhilePaused(void); // Call periodically during menu to maintain connection
bool Netplay_isPaused(void);        // Check if paused

#endif /* NETPLAY_H */
