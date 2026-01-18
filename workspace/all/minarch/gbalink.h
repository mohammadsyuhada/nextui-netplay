/*
 * NextUI GBA Link Module
 * Implements GBA Wireless Adapter (RFU) emulation over WiFi using libretro netpacket interface
 *
 * This module bridges gpSP's built-in RFU emulation with network transport,
 * enabling Pokemon trading, battles (Union Room), and other wireless features.
 *
 * This is separate from netplay (input sync) - it provides wireless adapter
 * communication for GBA games via gpSP's complete RFU implementation.
 */

#ifndef GBALINK_H
#define GBALINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "libretro-common/include/libretro.h"

#define GBALINK_DEFAULT_PORT 55437
#define GBALINK_DISCOVERY_PORT 55438
#define GBALINK_MAGIC "GBLK"
#define GBALINK_PROTOCOL_VERSION 1
#define GBALINK_MAX_GAME_NAME 64
#define GBALINK_MAX_HOSTS 8

typedef enum {
    GBALINK_OFF = 0,
    GBALINK_HOST,
    GBALINK_CLIENT
} GBALinkMode;

typedef enum {
    GBALINK_CONN_WIFI = 0,    // Use existing WiFi network
    GBALINK_CONN_HOTSPOT      // Create/connect to hotspot
} GBALinkConnMethod;

typedef enum {
    GBALINK_STATE_IDLE = 0,
    GBALINK_STATE_WAITING,      // Host waiting for client
    GBALINK_STATE_CONNECTING,   // Client connecting to host
    GBALINK_STATE_CONNECTED,    // Connected and ready for SIO packets
    GBALINK_STATE_DISCONNECTED,
    GBALINK_STATE_ERROR
} GBALinkState;

typedef struct {
    char game_name[GBALINK_MAX_GAME_NAME];
    char host_ip[16];
    uint16_t port;
    uint32_t game_crc;
} GBALinkHostInfo;

// Initialize/cleanup
void GBALink_init(void);
void GBALink_quit(void);

// Connection management
int GBALink_startHost(const char* game_name, uint32_t game_crc);
int GBALink_startHostWithHotspot(const char* game_name, uint32_t game_crc);
int GBALink_stopHost(void);
int GBALink_connectToHost(const char* ip, uint16_t port);
void GBALink_disconnect(void);

// Hotspot mode
bool GBALink_isUsingHotspot(void);
void GBALink_setConnectionMethod(GBALinkConnMethod method);
GBALinkConnMethod GBALink_getConnectionMethod(void);

// Status queries
GBALinkMode GBALink_getMode(void);
GBALinkState GBALink_getState(void);
bool GBALink_isConnected(void);
const char* GBALink_getStatusMessage(void);
const char* GBALink_getLocalIP(void);
bool GBALink_hasNetworkConnection(void);

// IP management - call after WiFi network changes
void GBALink_refreshLocalIP(void);
void GBALink_clearLocalIP(void);

// Host discovery (for client)
int GBALink_startDiscovery(void);
void GBALink_stopDiscovery(void);
int GBALink_getDiscoveredHosts(GBALinkHostInfo* hosts, int max_hosts);

// Netpacket interface callbacks for core
// These are called when the core registers its netpacket interface
void GBALink_onNetpacketStart(uint16_t client_id,
                               retro_netpacket_send_t send_fn,
                               retro_netpacket_poll_receive_t poll_receive_fn);
void GBALink_onNetpacketReceive(const void* buf, size_t len, uint16_t client_id);
void GBALink_onNetpacketStop(void);
void GBALink_onNetpacketPoll(void);
bool GBALink_onNetpacketConnected(uint16_t client_id);
void GBALink_onNetpacketDisconnected(uint16_t client_id);

// Called by frontend to provide send/poll functions to core
// These wrap the network transport layer
void GBALink_sendPacket(int flags, const void* buf, size_t len, uint16_t client_id);
void GBALink_pollReceive(void);

// Update function (call periodically for connection handling)
void GBALink_update(void);

// Check if core supports GBA Link (has netpacket interface)
bool GBALink_coreSupportsLink(void);

// Called by minarch when core registers netpacket interface
void GBALink_setCoreSupport(bool supported);

// Get pending received packet for delivery to core
// Returns true if a packet is available, fills buf/len/client_id
bool GBALink_getPendingPacket(void** buf, size_t* len, uint16_t* client_id);
void GBALink_consumePendingPacket(void);

// Callbacks from gbalink to minarch (defined in minarch.c)
// Called when GBA Link connection state changes
extern void GBALink_notifyConnected(int is_host);
extern void GBALink_notifyDisconnected(void);

#endif /* GBALINK_H */
