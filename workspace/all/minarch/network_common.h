/*
 * NextUI Network Common Module
 * Shared networking utilities for netplay and gbalink
 */

#ifndef NETWORK_COMMON_H
#define NETWORK_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>

// Configuration for TCP socket setup
typedef struct {
    int buffer_size;           // SO_SNDBUF/SO_RCVBUF size (bytes)
    int recv_timeout_us;       // SO_RCVTIMEO in microseconds (0 = none)
    bool enable_keepalive;     // SO_KEEPALIVE
} NET_TCPConfig;

// Configuration for hotspot SSID generation
typedef struct {
    const char* prefix;        // e.g., "Netplay-" or "GBALink-"
    uint32_t seed;             // Random seed (typically game_crc ^ time)
} NET_HotspotConfig;

// Rate-limited broadcast timer
typedef struct {
    struct timeval last_broadcast;
    int interval_us;
} NET_BroadcastTimer;

//////////////////////////////////////////////////////////////////////////////
// IP Address Utilities
//////////////////////////////////////////////////////////////////////////////

/**
 * Get the local IP address (preferring wlan interfaces)
 * @param ip_out Buffer to receive IP string
 * @param ip_size Size of ip_out buffer (should be at least 16 bytes)
 */
void NET_getLocalIP(char* ip_out, size_t ip_size);

//////////////////////////////////////////////////////////////////////////////
// TCP Socket Configuration
//////////////////////////////////////////////////////////////////////////////

/**
 * Configure TCP socket with common options (TCP_NODELAY, buffer sizes, etc.)
 * @param fd Socket file descriptor
 * @param config Configuration options (NULL for default: 64KB buffers, no timeout)
 */
void NET_configureTCPSocket(int fd, const NET_TCPConfig* config);

//////////////////////////////////////////////////////////////////////////////
// Server Socket Creation
//////////////////////////////////////////////////////////////////////////////

/**
 * Create a listening TCP socket bound to a port
 * @param port Port number to bind to
 * @param error_msg Buffer to receive error message on failure (can be NULL)
 * @param error_size Size of error_msg buffer
 * @return Socket fd on success, -1 on failure
 */
int NET_createListenSocket(uint16_t port, char* error_msg, size_t error_size);

/**
 * Create a UDP socket for broadcasting
 * @return Socket fd on success, -1 on failure
 */
int NET_createBroadcastSocket(void);

//////////////////////////////////////////////////////////////////////////////
// Hotspot Utilities
//////////////////////////////////////////////////////////////////////////////

/**
 * Generate a hotspot SSID with random suffix
 * Format: "{prefix}XXXX" where XXXX is a random 4-character code
 * @param ssid_out Buffer to receive SSID string
 * @param ssid_size Size of ssid_out buffer (should be at least 33 bytes)
 * @param config Hotspot configuration (prefix and random seed)
 */
void NET_generateHotspotSSID(char* ssid_out, size_t ssid_size, const NET_HotspotConfig* config);

//////////////////////////////////////////////////////////////////////////////
// Broadcast Timer
//////////////////////////////////////////////////////////////////////////////

/**
 * Initialize a broadcast timer for rate-limiting
 * @param timer Timer to initialize
 * @param interval_us Minimum interval between broadcasts in microseconds
 */
void NET_initBroadcastTimer(NET_BroadcastTimer* timer, int interval_us);

/**
 * Check if enough time has passed to broadcast again
 * Updates the timer's last_broadcast time if returning true
 * @param timer Timer to check
 * @return true if should broadcast, false if too soon
 */
bool NET_shouldBroadcast(NET_BroadcastTimer* timer);

#endif /* NETWORK_COMMON_H */
