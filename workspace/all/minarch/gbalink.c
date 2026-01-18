/*
 * NextUI GBA Link Module
 * Implements GBA Wireless Adapter (RFU) emulation over WiFi
 *
 * This module provides a transport layer for the libretro netpacket interface,
 * allowing gpSP to use its built-in Wireless Adapter (RFU) emulation over TCP.
 *
 * gpSP has complete RFU support (rfu.c - 937 lines) that handles the complex
 * wireless adapter protocol used by Pokemon games for trading and battles.
 *
 * Unlike netplay (input synchronization), GBA Link:
 * - Uses gpSP's native RFU timing and protocol
 * - Each device runs its own save file and game state
 * - Only wireless adapter packets are exchanged (not inputs)
 *
 * Supported features via gpSP:
 * - Pokemon trading (FireRed/LeafGreen/Ruby/Sapphire/Emerald)
 * - Pokemon battles (Union Room)
 * - Up to 32 concurrent peers
 */

#include "gbalink.h"
#include "network_common.h"
#include "defines.h"  // Must come before api.h for BTN_ID_COUNT
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

// Protocol constants
#define GL_PROTOCOL_MAGIC   0x47424C4B  // "GBLK"
#define GL_DISCOVERY_QUERY  0x47424451  // "GBDQ" - GBA Link Discovery Query
#define GL_DISCOVERY_RESP   0x47424452  // "GBDR" - GBA Link Discovery Response

// Discovery broadcast interval
#define DISCOVERY_BROADCAST_INTERVAL_US 500000  // 500ms

// Network commands
enum {
    CMD_SIO_DATA   = 0x01,  // SIO packet data from core
    CMD_PING       = 0x02,
    CMD_PONG       = 0x03,
    CMD_DISCONNECT = 0x04,
    CMD_READY      = 0x05,  // Signal ready for SIO exchange
    CMD_HEARTBEAT  = 0x06,  // Keepalive during idle periods
};

// Heartbeat interval - RFU protocol requires host to send data so clients can respond
// 500ms interval keeps connection alive without excessive overhead
// (100ms was too aggressive and could overwhelm slow receivers)
#define HEARTBEAT_INTERVAL_MS 500

// Packet header for TCP communication
typedef struct __attribute__((packed)) {
    uint8_t  cmd;
    uint16_t size;
    uint16_t client_id;  // Source client ID
} PacketHeader;

// Discovery packet
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t protocol_version;
    uint32_t game_crc;
    uint16_t port;
    char game_name[GBALINK_MAX_GAME_NAME];
} DiscoveryPacket;

// Receive buffer for incoming packets
// GBA wireless packets vary in size: trades ~32 bytes, battles can be larger
// 4096 bytes provides safe headroom while still reducing memory significantly
#define RECV_BUFFER_SIZE 4096
typedef struct {
    uint8_t data[RECV_BUFFER_SIZE];
    size_t len;
    uint16_t client_id;
} ReceivedPacket;


// Pending packet queue - needs enough slots to handle burst traffic during
// trade/battle setup. 64 slots with 4KB buffers = 256KB (acceptable for RFU reliability)
#define MAX_PENDING_PACKETS 64

// Main GBA Link state
static struct {
    GBALinkMode mode;
    GBALinkState state;

    // Sockets
    int tcp_fd;         // Main TCP connection
    int listen_fd;      // Server listen socket
    int udp_fd;         // Discovery UDP socket

    // Connection info
    char local_ip[16];
    char remote_ip[16];
    uint16_t port;

    // Hotspot mode
    GBALinkConnMethod conn_method;
    bool using_hotspot;           // True if hotspot was started for this session
    bool connected_to_hotspot;    // True if client connected to hotspot WiFi

    // Game info
    char game_name[GBALINK_MAX_GAME_NAME];
    uint32_t game_crc;

    // Netpacket interface (from core)
    bool core_registered;
    uint16_t local_client_id;
    retro_netpacket_send_t core_send_fn;        // Stored but we provide our own to core
    retro_netpacket_poll_receive_t core_poll_fn;

    // Receive buffer for delivering to core
    ReceivedPacket pending_packets[MAX_PENDING_PACKETS];
    int pending_count;
    int pending_read_idx;
    int pending_write_idx;

    // Discovery
    GBALinkHostInfo discovered_hosts[GBALINK_MAX_HOSTS];
    int num_hosts;
    bool discovery_active;

    // Threading
    pthread_t listen_thread;
    pthread_mutex_t mutex;
    volatile bool running;

    // Status
    char status_msg[128];

    // Core support flag
    bool has_netpacket_support;

    // Streaming receive buffer for handling partial TCP reads
    // Uses read/write indices to avoid memmove on every packet
    uint8_t stream_buf[RECV_BUFFER_SIZE + sizeof(PacketHeader)];
    size_t stream_buf_read_idx;   // Where to read next packet from
    size_t stream_buf_write_idx;  // Where to write incoming data

    // Heartbeat/keepalive tracking - critical for RFU protocol
    // The host must send data (even dummy) so clients can respond
    struct timeval last_packet_sent;
    struct timeval last_packet_received;

    // Deferred connection notification (listen thread sets, main thread processes)
    // Required because core callbacks must be called from main thread
    volatile bool pending_host_connected;
} gl = {0};

// Forward declarations
static bool send_packet(uint8_t cmd, const void* data, uint16_t size, uint16_t client_id);
static bool recv_packet(PacketHeader* hdr, void* data, uint16_t max_size, int timeout_ms);
static void* listen_thread_func(void* arg);
static void GBALink_sendHeartbeatIfNeeded(void);

//////////////////////////////////////////////////////////////////////////////
// Initialization
//////////////////////////////////////////////////////////////////////////////

void GBALink_init(void) {
    memset(&gl, 0, sizeof(gl));
    gl.mode = GBALINK_OFF;
    gl.state = GBALINK_STATE_IDLE;
    gl.tcp_fd = -1;
    gl.listen_fd = -1;
    gl.udp_fd = -1;
    gl.port = GBALINK_DEFAULT_PORT;
    pthread_mutex_init(&gl.mutex, NULL);
    NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    snprintf(gl.status_msg, sizeof(gl.status_msg), "GBA Link ready");
}

void GBALink_quit(void) {
    GBALink_disconnect();
    GBALink_stopHost();
    GBALink_stopDiscovery();
    pthread_mutex_destroy(&gl.mutex);
}

//////////////////////////////////////////////////////////////////////////////
// Helper Functions
//////////////////////////////////////////////////////////////////////////////

// GBALink-specific TCP configuration:
// - 128KB buffers for burst traffic during Union Room
// - 1ms recv timeout for RFU sub-frame timing
// - Keepalive enabled for dead connection detection
static const NET_TCPConfig GBALINK_TCP_CONFIG = {
    .buffer_size = 131072,      // 128KB (larger than netplay's 64KB)
    .recv_timeout_us = 1000,    // 1ms timeout for RFU timing
    .enable_keepalive = true
};

//////////////////////////////////////////////////////////////////////////////
// Host Mode
//////////////////////////////////////////////////////////////////////////////

int GBALink_startHost(const char* game_name, uint32_t game_crc) {
    if (gl.mode != GBALINK_OFF) {
        return -1;
    }

    // Create TCP listen socket using shared utility
    gl.listen_fd = NET_createListenSocket(gl.port, gl.status_msg, sizeof(gl.status_msg));
    if (gl.listen_fd < 0) {
        return -1;
    }

    // Create UDP socket for discovery broadcasts
    gl.udp_fd = NET_createBroadcastSocket();

    strncpy(gl.game_name, game_name, GBALINK_MAX_GAME_NAME - 1);
    gl.game_crc = game_crc;

    // Start listen thread
    gl.running = true;
    pthread_create(&gl.listen_thread, NULL, listen_thread_func, NULL);

    gl.mode = GBALINK_HOST;
    gl.state = GBALINK_STATE_WAITING;
    gl.local_client_id = 0;  // Host is always client 0

    snprintf(gl.status_msg, sizeof(gl.status_msg), "Hosting on %s:%d", gl.local_ip, gl.port);
    return 0;
}

int GBALink_startHostWithHotspot(const char* game_name, uint32_t game_crc) {
    if (gl.mode != GBALINK_OFF) {
        return -1;
    }

    // Generate SSID with random 4-character code using shared utility
    char ssid[33];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    NET_HotspotConfig hotspot_cfg = {
        .prefix = PLAT_getHotspotSSIDPrefix(),
        .seed = (unsigned int)(tv.tv_usec ^ tv.tv_sec ^ game_crc)
    };
    NET_generateHotspotSSID(ssid, sizeof(ssid), &hotspot_cfg);

    const char* pass = PLAT_getHotspotPassword();

    snprintf(gl.status_msg, sizeof(gl.status_msg), "Starting hotspot...");

    if (PLAT_startHotspot(ssid, pass) != 0) {
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Failed to start hotspot");
        return -1;
    }

    gl.using_hotspot = true;
    gl.conn_method = GBALINK_CONN_HOTSPOT;

    // Use hotspot IP instead of WiFi IP
    strncpy(gl.local_ip, PLAT_getHotspotIP(), sizeof(gl.local_ip) - 1);

    // Create TCP listen socket using shared utility
    gl.listen_fd = NET_createListenSocket(gl.port, gl.status_msg, sizeof(gl.status_msg));
    if (gl.listen_fd < 0) {
        PLAT_stopHotspot();
        gl.using_hotspot = false;
        return -1;
    }

    // Create UDP socket for discovery broadcasts
    gl.udp_fd = NET_createBroadcastSocket();

    strncpy(gl.game_name, game_name, GBALINK_MAX_GAME_NAME - 1);
    gl.game_crc = game_crc;

    // Start listen thread
    gl.running = true;
    pthread_create(&gl.listen_thread, NULL, listen_thread_func, NULL);

    gl.mode = GBALINK_HOST;
    gl.state = GBALINK_STATE_WAITING;
    gl.local_client_id = 0;

    snprintf(gl.status_msg, sizeof(gl.status_msg), "Hotspot: %s | IP: %s", ssid, gl.local_ip);
    return 0;
}

int GBALink_stopHost(void) {
    if (gl.mode != GBALINK_HOST) return -1;

    gl.running = false;

    // Close listen socket to unblock accept() in listen thread
    if (gl.listen_fd >= 0) {
        close(gl.listen_fd);
        gl.listen_fd = -1;
    }

    if (gl.listen_thread) {
        // Thread will exit via gl.running check - no pthread_cancel needed
        // (pthread_cancel can leave mutex/fd in undefined state)
        pthread_join(gl.listen_thread, NULL);
        gl.listen_thread = 0;
    }

    if (gl.udp_fd >= 0) {
        close(gl.udp_fd);
        gl.udp_fd = -1;
    }

    GBALink_disconnect();

    // Stop hotspot if it was started
    if (gl.using_hotspot) {
        PLAT_stopHotspot();
        gl.using_hotspot = false;
        // Clear local IP since hotspot is stopping
        // PLAT_stopHotspot will restore previous WiFi connection,
        // and the IP will be refreshed when needed
        strcpy(gl.local_ip, "0.0.0.0");
    }
    gl.mode = GBALINK_OFF;
    gl.state = GBALINK_STATE_IDLE;
    snprintf(gl.status_msg, sizeof(gl.status_msg), "GBA Link ready");
    return 0;
}

static void* listen_thread_func(void* arg) {
    (void)arg;

    // Use shared broadcast timer for rate limiting
    NET_BroadcastTimer broadcast_timer;
    NET_initBroadcastTimer(&broadcast_timer, DISCOVERY_BROADCAST_INTERVAL_US);

    while (gl.running && gl.listen_fd >= 0) {
        // Rate-limited discovery broadcast using shared timer
        if (gl.udp_fd >= 0 && gl.state == GBALINK_STATE_WAITING) {
            if (NET_shouldBroadcast(&broadcast_timer)) {
                DiscoveryPacket disc = {0};
                disc.magic = htonl(GL_DISCOVERY_RESP);
                disc.protocol_version = htonl(GBALINK_PROTOCOL_VERSION);
                disc.game_crc = htonl(gl.game_crc);
                disc.port = htons(gl.port);
                strncpy(disc.game_name, gl.game_name, GBALINK_MAX_GAME_NAME - 1);

                struct sockaddr_in bcast = {0};
                bcast.sin_family = AF_INET;
                bcast.sin_addr.s_addr = INADDR_BROADCAST;
                bcast.sin_port = htons(GBALINK_DISCOVERY_PORT);

                sendto(gl.udp_fd, &disc, sizeof(disc), 0,
                       (struct sockaddr*)&bcast, sizeof(bcast));
            }
        }

        // Check for incoming connection
        if (gl.state == GBALINK_STATE_WAITING && gl.listen_fd >= 0) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(gl.listen_fd, &fds);

            struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};  // 100ms timeout
            int sel_result = select(gl.listen_fd + 1, &fds, NULL, NULL, &tv);
            if (sel_result < 0 || !gl.running) break;  // Socket closed or stopping
            if (sel_result > 0) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);

                int fd = accept(gl.listen_fd, (struct sockaddr*)&client_addr, &len);
                if (fd >= 0) {
                    pthread_mutex_lock(&gl.mutex);

                    if (gl.state != GBALINK_STATE_WAITING) {
                        close(fd);
                        pthread_mutex_unlock(&gl.mutex);
                        continue;
                    }

                    // Configure TCP socket using GBALink-specific settings
                    NET_configureTCPSocket(fd, &GBALINK_TCP_CONFIG);

                    gl.tcp_fd = fd;
                    inet_ntop(AF_INET, &client_addr.sin_addr, gl.remote_ip, sizeof(gl.remote_ip));

                    gl.state = GBALINK_STATE_CONNECTED;
                    gl.pending_count = 0;
                    gl.pending_read_idx = 0;
                    gl.pending_write_idx = 0;
                    gl.stream_buf_read_idx = 0;
                    gl.stream_buf_write_idx = 0;
                    gettimeofday(&gl.last_packet_sent, NULL);
                    gettimeofday(&gl.last_packet_received, NULL);

                    snprintf(gl.status_msg, sizeof(gl.status_msg), "Client connected: %s", gl.remote_ip);

                    LOG_info("GBALink: HOST accepted client %s\n", gl.remote_ip);

                    // Wait for client's READY signal
                    LOG_info("GBALink: HOST waiting for client READY...\n");
                    bool client_ready = false;
                    for (int attempts = 0; attempts < 100 && gl.running; attempts++) {  // 5 second timeout
                        PacketHeader hdr;
                        uint8_t data[64];
                        bool got_packet = recv_packet(&hdr, data, sizeof(data), 50);
                        if (got_packet && hdr.cmd == CMD_READY) {
                            client_ready = true;
                            LOG_info("GBALink: HOST received client READY\n");
                            break;
                        }
                    }

                    if (!client_ready) {
                        LOG_error("GBALink: HOST timeout waiting for client READY\n");
                        close(gl.tcp_fd);
                        gl.tcp_fd = -1;
                        gl.state = GBALINK_STATE_WAITING;
                        pthread_mutex_unlock(&gl.mutex);
                        continue;
                    }

                    // Send READY back to client
                    send_packet(CMD_READY, NULL, 0, 0);
                    LOG_info("GBALink: HOST sent READY to client\n");

                    // Set flag for main thread to process (core callbacks must run on main thread)
                    gl.pending_host_connected = true;

                    pthread_mutex_unlock(&gl.mutex);
                }
            }
        } else {
            usleep(50000);  // 50ms
        }
    }

    return NULL;
}

//////////////////////////////////////////////////////////////////////////////
// Client Mode
//////////////////////////////////////////////////////////////////////////////

int GBALink_connectToHost(const char* ip, uint16_t port) {
    if (gl.mode != GBALINK_OFF) {
        return -1;
    }

    // Refresh local IP - important when client just connected to hotspot WiFi
    // This ensures we have the correct IP from the hotspot's DHCP server
    NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    LOG_info("GBALink: CLIENT local IP is %s\n", gl.local_ip);

    gl.tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (gl.tcp_fd < 0) {
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Socket creation failed");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(gl.tcp_fd);
        gl.tcp_fd = -1;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Invalid IP address");
        return -1;
    }

    gl.state = GBALINK_STATE_CONNECTING;
    snprintf(gl.status_msg, sizeof(gl.status_msg), "Connecting to %s:%d...", ip, port);

    // Connect with timeout
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(gl.tcp_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(gl.tcp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(gl.tcp_fd);
        gl.tcp_fd = -1;
        gl.state = GBALINK_STATE_ERROR;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Connection failed");
        return -1;
    }

    // Configure TCP socket using GBALink-specific settings
    NET_configureTCPSocket(gl.tcp_fd, &GBALINK_TCP_CONFIG);

    strncpy(gl.remote_ip, ip, sizeof(gl.remote_ip) - 1);
    gl.port = port;
    gl.mode = GBALINK_CLIENT;
    gl.state = GBALINK_STATE_CONNECTED;
    gl.local_client_id = 1;  // Client is always client 1

    gl.pending_count = 0;
    gl.pending_read_idx = 0;
    gl.pending_write_idx = 0;
    gl.stream_buf_read_idx = 0;
    gl.stream_buf_write_idx = 0;
    gettimeofday(&gl.last_packet_sent, NULL);
    gettimeofday(&gl.last_packet_received, NULL);

    snprintf(gl.status_msg, sizeof(gl.status_msg), "Connected to %s", ip);

    LOG_info("GBALink: CLIENT connected to host %s\n", ip);

    // Send READY signal to host and wait for host's READY
    pthread_mutex_lock(&gl.mutex);
    send_packet(CMD_READY, NULL, 0, gl.local_client_id);
    pthread_mutex_unlock(&gl.mutex);

    LOG_info("GBALink: CLIENT sent READY, waiting for host READY...\n");

    // Wait for host's READY signal (with timeout - 5 seconds = 100 x 50ms)
    bool host_ready = false;
    for (int attempts = 0; attempts < 100; attempts++) {
        PacketHeader hdr;
        uint8_t data[64];
        pthread_mutex_lock(&gl.mutex);
        bool got_packet = recv_packet(&hdr, data, sizeof(data), 50);
        pthread_mutex_unlock(&gl.mutex);

        if (got_packet && hdr.cmd == CMD_READY) {
            host_ready = true;
            LOG_info("GBALink: CLIENT received host READY\n");
            break;
        }
    }

    if (!host_ready) {
        LOG_error("GBALink: CLIENT timeout waiting for host READY\n");
        close(gl.tcp_fd);
        gl.tcp_fd = -1;
        gl.mode = GBALINK_OFF;
        gl.state = GBALINK_STATE_ERROR;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Host not responding");
        return -1;
    }

    // Now both sides are ready - notify minarch to start netpacket session
    GBALink_notifyConnected(0);

    return 0;
}

void GBALink_disconnect(void) {
    // Notify minarch to stop netpacket session
    GBALink_notifyDisconnected();

    pthread_mutex_lock(&gl.mutex);
    if (gl.tcp_fd >= 0) {
        send_packet(CMD_DISCONNECT, NULL, 0, 0);
        // send_packet re-acquires mutex, so we still hold it here
        close(gl.tcp_fd);
        gl.tcp_fd = -1;
    }

    if (gl.mode == GBALINK_CLIENT) {
        gl.mode = GBALINK_OFF;
        gl.state = GBALINK_STATE_DISCONNECTED;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Disconnected");
        // Clear local IP since client is disconnecting from hotspot network
        // The actual WiFi disconnection happens separately, but we clear here
        // so the UI shows no IP while disconnected
        strcpy(gl.local_ip, "0.0.0.0");
        gl.connected_to_hotspot = false;
    } else if (gl.mode == GBALINK_HOST) {
        gl.state = GBALINK_STATE_WAITING;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Client left, waiting on %s:%d", gl.local_ip, gl.port);
    } else {
        gl.state = GBALINK_STATE_DISCONNECTED;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Disconnected");
    }

    gl.pending_count = 0;
    gl.stream_buf_read_idx = 0;
    gl.stream_buf_write_idx = 0;
    pthread_mutex_unlock(&gl.mutex);
}

//////////////////////////////////////////////////////////////////////////////
// Discovery
//////////////////////////////////////////////////////////////////////////////

int GBALink_startDiscovery(void) {
    if (gl.discovery_active) return 0;

    gl.udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (gl.udp_fd < 0) return -1;

    int opt = 1;
    setsockopt(gl.udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(GBALINK_DISCOVERY_PORT);

    if (bind(gl.udp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(gl.udp_fd);
        gl.udp_fd = -1;
        return -1;
    }

    // Set non-blocking
    int flags = fcntl(gl.udp_fd, F_GETFL, 0);
    fcntl(gl.udp_fd, F_SETFL, flags | O_NONBLOCK);

    gl.num_hosts = 0;
    gl.discovery_active = true;
    return 0;
}

void GBALink_stopDiscovery(void) {
    if (!gl.discovery_active) return;

    if (gl.udp_fd >= 0 && gl.mode == GBALINK_OFF) {
        close(gl.udp_fd);
        gl.udp_fd = -1;
    }

    gl.discovery_active = false;
}

int GBALink_getDiscoveredHosts(GBALinkHostInfo* hosts, int max_hosts) {
    if (!gl.discovery_active || gl.udp_fd < 0) return 0;

    // Poll for discovery responses
    DiscoveryPacket pkt;
    struct sockaddr_in sender;
    socklen_t len = sizeof(sender);

    while (recvfrom(gl.udp_fd, &pkt, sizeof(pkt), 0,
                    (struct sockaddr*)&sender, &len) == sizeof(pkt)) {
        if (ntohl(pkt.magic) != GL_DISCOVERY_RESP) continue;

        char ip[16];
        inet_ntop(AF_INET, &sender.sin_addr, ip, sizeof(ip));

        // Check if already in list
        bool found = false;
        for (int i = 0; i < gl.num_hosts; i++) {
            if (strcmp(gl.discovered_hosts[i].host_ip, ip) == 0) {
                found = true;
                break;
            }
        }

        if (!found && gl.num_hosts < GBALINK_MAX_HOSTS) {
            GBALinkHostInfo* h = &gl.discovered_hosts[gl.num_hosts];
            strncpy(h->game_name, pkt.game_name, GBALINK_MAX_GAME_NAME - 1);
            strncpy(h->host_ip, ip, sizeof(h->host_ip) - 1);
            h->port = ntohs(pkt.port);
            h->game_crc = ntohl(pkt.game_crc);
            gl.num_hosts++;
        }
    }

    int count = (gl.num_hosts < max_hosts) ? gl.num_hosts : max_hosts;
    memcpy(hosts, gl.discovered_hosts, count * sizeof(GBALinkHostInfo));
    return count;
}

//////////////////////////////////////////////////////////////////////////////
// Netpacket Interface (called by frontend when core registers)
//////////////////////////////////////////////////////////////////////////////

void GBALink_onNetpacketStart(uint16_t client_id,
                               retro_netpacket_send_t send_fn,
                               retro_netpacket_poll_receive_t poll_receive_fn) {
    pthread_mutex_lock(&gl.mutex);
    gl.core_registered = true;
    gl.local_client_id = client_id;
    gl.core_send_fn = send_fn;
    gl.core_poll_fn = poll_receive_fn;
    pthread_mutex_unlock(&gl.mutex);
}

void GBALink_onNetpacketReceive(const void* buf, size_t len, uint16_t client_id) {
    // This is called when we receive data from network and deliver to core
    // (happens in pollReceive)
    (void)buf;
    (void)len;
    (void)client_id;
}

void GBALink_onNetpacketStop(void) {
    pthread_mutex_lock(&gl.mutex);
    gl.core_registered = false;
    gl.core_send_fn = NULL;
    gl.core_poll_fn = NULL;
    pthread_mutex_unlock(&gl.mutex);
}

void GBALink_onNetpacketPoll(void) {
    // Called by core each frame - check for incoming packets
    GBALink_pollReceive();
}

bool GBALink_onNetpacketConnected(uint16_t client_id) {
    // Accept all connections for now (could limit to 1 for 2-player link)
    (void)client_id;
    return true;
}

void GBALink_onNetpacketDisconnected(uint16_t client_id) {
    (void)client_id;
    // Handle player disconnect
}

//////////////////////////////////////////////////////////////////////////////
// Packet Sending (called by core via netpacket send function)
//////////////////////////////////////////////////////////////////////////////

void GBALink_sendPacket(int flags, const void* buf, size_t len, uint16_t client_id) {
    if (!GBALink_isConnected()) return;

    // Handle empty flush request (flush only, no data)
    // We use TCP_NODELAY so packets are already flushed immediately
    if (!buf || len == 0) return;

    // Send to remote via TCP
    pthread_mutex_lock(&gl.mutex);
    send_packet(CMD_SIO_DATA, buf, (uint16_t)len, client_id);
    // Update last_packet_sent to prevent unnecessary heartbeats during active communication
    gettimeofday(&gl.last_packet_sent, NULL);
    pthread_mutex_unlock(&gl.mutex);

    // FLUSH_HINT: Since we use TCP_NODELAY and send immediately,
    // packets are already flushed. No additional action needed.
    (void)flags;
}

// Limit packets per poll to prevent frame stalls during high traffic
// 64 packets allows same-frame delivery of packet bursts during trade/battle
// This prevents Pokemon Union Room trade failures caused by buffering packets until next frame
#define MAX_PACKETS_PER_POLL 64

// Send heartbeat packet if idle for too long (host only)
// Critical for RFU protocol: "the host must send data (even dummy) so clients can respond"
// Without this, clients timeout fatally (unrecoverable "communication error")
static void GBALink_sendHeartbeatIfNeeded(void) {
    // Only host sends heartbeats - clients respond to host packets
    if (gl.mode != GBALINK_HOST || !GBALink_isConnected()) return;

    struct timeval now;
    gettimeofday(&now, NULL);

    long elapsed_ms = (now.tv_sec - gl.last_packet_sent.tv_sec) * 1000 +
                      (now.tv_usec - gl.last_packet_sent.tv_usec) / 1000;

    if (elapsed_ms >= HEARTBEAT_INTERVAL_MS) {
        pthread_mutex_lock(&gl.mutex);
        send_packet(CMD_HEARTBEAT, NULL, 0, 0);
        gl.last_packet_sent = now;
        pthread_mutex_unlock(&gl.mutex);
    }
}

void GBALink_pollReceive(void) {
    if (!GBALink_isConnected()) return;

    // Send heartbeat if needed (host only, keeps clients alive)
    GBALink_sendHeartbeatIfNeeded();

    pthread_mutex_lock(&gl.mutex);

    // Check for incoming packets with short timeout
    PacketHeader hdr;
    uint8_t data[RECV_BUFFER_SIZE];
    uint16_t max_recv = (RECV_BUFFER_SIZE > 65535) ? 65535 : RECV_BUFFER_SIZE;
    int packets_this_poll = 0;

    while (packets_this_poll < MAX_PACKETS_PER_POLL && recv_packet(&hdr, data, max_recv, 0)) {
        if (hdr.cmd == CMD_SIO_DATA) {
            // Queue packet for delivery to core
            if (gl.pending_count < MAX_PENDING_PACKETS) {
                ReceivedPacket* pkt = &gl.pending_packets[gl.pending_write_idx];
                memcpy(pkt->data, data, hdr.size);
                pkt->len = hdr.size;
                pkt->client_id = hdr.client_id;
                gl.pending_write_idx = (gl.pending_write_idx + 1) % MAX_PENDING_PACKETS;
                gl.pending_count++;
            }
            packets_this_poll++;
        } else if (hdr.cmd == CMD_HEARTBEAT) {
            // Heartbeat received - update last_packet_received timestamp
            // (handled in recv_packet - just acknowledge here)
        } else if (hdr.cmd == CMD_DISCONNECT) {
            // Remote disconnected - clean up properly
            close(gl.tcp_fd);
            gl.tcp_fd = -1;
            gl.state = GBALINK_STATE_DISCONNECTED;
            snprintf(gl.status_msg, sizeof(gl.status_msg), "Remote disconnected");

            // For client, fully disconnect (host stays in waiting mode)
            if (gl.mode == GBALINK_CLIENT) {
                gl.mode = GBALINK_OFF;
                strcpy(gl.local_ip, "0.0.0.0");
                gl.connected_to_hotspot = false;
                // Notify minarch that connection is lost
                pthread_mutex_unlock(&gl.mutex);
                GBALink_notifyDisconnected();
                pthread_mutex_lock(&gl.mutex);
            } else if (gl.mode == GBALINK_HOST) {
                // Host goes back to waiting state
                gl.state = GBALINK_STATE_WAITING;
                snprintf(gl.status_msg, sizeof(gl.status_msg), "Client left, waiting...");
            }
            break;
        }
    }

    pthread_mutex_unlock(&gl.mutex);
}

//////////////////////////////////////////////////////////////////////////////
// Status Functions
//////////////////////////////////////////////////////////////////////////////

GBALinkMode GBALink_getMode(void) { return gl.mode; }
GBALinkState GBALink_getState(void) { return gl.state; }

bool GBALink_isConnected(void) {
    return gl.tcp_fd >= 0 && gl.state == GBALINK_STATE_CONNECTED;
}

const char* GBALink_getStatusMessage(void) { return gl.status_msg; }
const char* GBALink_getLocalIP(void) { return gl.local_ip; }

bool GBALink_isUsingHotspot(void) { return gl.using_hotspot; }
void GBALink_setConnectionMethod(GBALinkConnMethod method) { gl.conn_method = method; }
GBALinkConnMethod GBALink_getConnectionMethod(void) { return gl.conn_method; }

bool GBALink_hasNetworkConnection(void) {
    NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    return strcmp(gl.local_ip, "0.0.0.0") != 0;
}

void GBALink_refreshLocalIP(void) {
    NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    LOG_info("GBALink: refreshed local IP to %s\n", gl.local_ip);
}

void GBALink_clearLocalIP(void) {
    strcpy(gl.local_ip, "0.0.0.0");
    LOG_info("GBALink: cleared local IP\n");
}

void GBALink_update(void) {
    // Process pending host connection notification (must run on main thread)
    // The listen thread sets this flag, we process it here to ensure
    // core callbacks are called from the main thread
    if (gl.pending_host_connected) {
        gl.pending_host_connected = false;
        GBALink_notifyConnected(1);  // We are host
    }

    // Check for connection errors via socket error state
    if (gl.tcp_fd >= 0) {
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(gl.tcp_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            GBALink_disconnect();
        }
    }
}

bool GBALink_coreSupportsLink(void) {
    return gl.has_netpacket_support;
}

// Called by minarch when core registers netpacket interface
void GBALink_setCoreSupport(bool supported) {
    gl.has_netpacket_support = supported;
}

bool GBALink_getPendingPacket(void** buf, size_t* len, uint16_t* client_id) {
    pthread_mutex_lock(&gl.mutex);
    if (gl.pending_count == 0) {
        pthread_mutex_unlock(&gl.mutex);
        return false;
    }
    ReceivedPacket* pkt = &gl.pending_packets[gl.pending_read_idx];
    *buf = pkt->data;
    *len = pkt->len;
    *client_id = pkt->client_id;
    pthread_mutex_unlock(&gl.mutex);
    return true;
}

void GBALink_consumePendingPacket(void) {
    pthread_mutex_lock(&gl.mutex);
    if (gl.pending_count > 0) {
        gl.pending_read_idx = (gl.pending_read_idx + 1) % MAX_PENDING_PACKETS;
        gl.pending_count--;
    }
    pthread_mutex_unlock(&gl.mutex);
}

//////////////////////////////////////////////////////////////////////////////
// Network Helper Functions
//////////////////////////////////////////////////////////////////////////////

// Forward declaration for drain function
static void drain_receive_buffer(void);

// Send all bytes with retry logic for reliability
// Retries up to 500ms total to handle WiFi latency and buffer pressure
// Critical: RFU protocol breaks if packets are dropped - must deliver all packets
// Returns false only on real errors or extended blocking
// NOTE: This function does NOT hold the mutex - caller must NOT hold mutex either
static bool send_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = buf;
    int total_wait_us = 0;
    const int max_wait_us = 500000;  // 500ms max total wait - prefer stalling over dropping

    while (len > 0) {
        ssize_t sent = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) {
            p += sent;
            len -= sent;
            total_wait_us = 0;  // Reset wait time on successful send
        } else if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full - wait briefly and retry
                if (total_wait_us >= max_wait_us) {
                    return false;
                }

                // CRITICAL: While waiting for send buffer to clear, drain our receive buffer
                // This prevents deadlock where both sides are waiting to send but neither is receiving
                // Safe to call without mutex since socket ops are thread-safe
                drain_receive_buffer();

                usleep(1000);  // 1ms
                total_wait_us += 1000;
            } else {
                // Real error (connection closed, broken pipe, etc.)
                return false;
            }
        } else {
            // sent == 0 should not happen with TCP
            return false;
        }
    }
    return true;
}

// Drain receive buffer without holding mutex - used during send retries to prevent deadlock
// This is a lightweight version that just reads from socket to kernel buffer
static void drain_receive_buffer(void) {
    int fd;
    pthread_mutex_lock(&gl.mutex);
    fd = gl.tcp_fd;
    pthread_mutex_unlock(&gl.mutex);

    if (fd < 0) return;

    // Check if data is available
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = {0, 0};  // Non-blocking

    if (select(fd + 1, &fds, NULL, NULL, &tv) > 0) {
        // Read data into stream buffer (under mutex)
        pthread_mutex_lock(&gl.mutex);
        if (gl.tcp_fd >= 0) {
            size_t space = sizeof(gl.stream_buf) - gl.stream_buf_write_idx;
            if (space > 0) {
                ssize_t ret = recv(gl.tcp_fd, gl.stream_buf + gl.stream_buf_write_idx, space, MSG_DONTWAIT);
                if (ret > 0) {
                    gl.stream_buf_write_idx += ret;
                }
            }
        }
        pthread_mutex_unlock(&gl.mutex);
    }
}

// Send packet - NOTE: caller must hold mutex for shared state, but we release during I/O
static bool send_packet(uint8_t cmd, const void* data, uint16_t size, uint16_t client_id) {
    if (gl.tcp_fd < 0) return false;

    // Get fd before releasing mutex
    int fd = gl.tcp_fd;

    PacketHeader hdr = {
        .cmd = cmd,
        .size = htons(size),
        .client_id = htons(client_id)
    };

    // Release mutex during actual I/O to allow receive processing
    pthread_mutex_unlock(&gl.mutex);

    bool ok = send_all(fd, &hdr, sizeof(hdr));
    if (ok && size > 0 && data) {
        ok = send_all(fd, data, size);
    }

    // Re-acquire mutex before returning
    pthread_mutex_lock(&gl.mutex);

    if (!ok) {
        return false;
    }

    return true;
}

static bool recv_packet(PacketHeader* hdr, void* data, uint16_t max_size, int timeout_ms) {
    if (gl.tcp_fd < 0) return false;

    // Calculate available data in buffer
    size_t available = gl.stream_buf_write_idx - gl.stream_buf_read_idx;

    // Try to read more data into our stream buffer (non-blocking)
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(gl.tcp_fd, &fds);

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };

    // Only try to recv if there's data available (non-blocking check)
    if (select(gl.tcp_fd + 1, &fds, NULL, NULL, &tv) > 0) {
        // Check if we need to compact the buffer (lazy compaction)
        // Compact when write index is near the end AND there's data to compact
        // Use 1024 threshold to ensure room for larger battle packets
        size_t space_at_end = sizeof(gl.stream_buf) - gl.stream_buf_write_idx;
        if (space_at_end < 1024 && gl.stream_buf_read_idx > 0) {
            // Compact: move unread data to the beginning
            if (available > 0) {
                memmove(gl.stream_buf, gl.stream_buf + gl.stream_buf_read_idx, available);
            }
            gl.stream_buf_read_idx = 0;
            gl.stream_buf_write_idx = available;
            space_at_end = sizeof(gl.stream_buf) - gl.stream_buf_write_idx;
        }

        if (space_at_end > 0) {
            ssize_t ret = recv(gl.tcp_fd, gl.stream_buf + gl.stream_buf_write_idx, space_at_end, MSG_DONTWAIT);
            if (ret == 0) {
                // Connection closed by remote
                close(gl.tcp_fd);
                gl.tcp_fd = -1;
                gl.state = GBALINK_STATE_DISCONNECTED;
                snprintf(gl.status_msg, sizeof(gl.status_msg), "Remote disconnected");
                // For client, fully disconnect
                if (gl.mode == GBALINK_CLIENT) {
                    gl.mode = GBALINK_OFF;
                    strcpy(gl.local_ip, "0.0.0.0");
                    gl.connected_to_hotspot = false;
                    GBALink_notifyDisconnected();
                }
                return false;
            }
            if (ret < 0) {
                if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
                    close(gl.tcp_fd);
                    gl.tcp_fd = -1;
                    gl.state = GBALINK_STATE_DISCONNECTED;
                    snprintf(gl.status_msg, sizeof(gl.status_msg), "Connection lost");
                    // For client, fully disconnect
                    if (gl.mode == GBALINK_CLIENT) {
                        gl.mode = GBALINK_OFF;
                        strcpy(gl.local_ip, "0.0.0.0");
                        gl.connected_to_hotspot = false;
                        GBALink_notifyDisconnected();
                    }
                    return false;
                }
                // EAGAIN/EWOULDBLOCK is ok, just no data right now
            } else {
                gl.stream_buf_write_idx += ret;
                available += ret;
            }
        }
    }

    // Check if we have a complete header
    if (available < sizeof(PacketHeader)) {
        return false;  // Need more data
    }

    // Parse header from buffer
    PacketHeader* buf_hdr = (PacketHeader*)(gl.stream_buf + gl.stream_buf_read_idx);
    hdr->cmd = buf_hdr->cmd;
    hdr->size = ntohs(buf_hdr->size);
    hdr->client_id = ntohs(buf_hdr->client_id);

    // Validate size
    if (hdr->size > max_size) {
        // Invalid packet size - protocol error, reset buffer
        gl.stream_buf_read_idx = 0;
        gl.stream_buf_write_idx = 0;
        return false;
    }

    // Check if we have complete packet (header + payload)
    size_t total_size = sizeof(PacketHeader) + hdr->size;
    if (available < total_size) {
        return false;  // Need more data
    }

    // Copy payload to output
    if (hdr->size > 0 && data) {
        memcpy(data, gl.stream_buf + gl.stream_buf_read_idx + sizeof(PacketHeader), hdr->size);
    }

    // Advance read index instead of memmove - O(1) instead of O(n)
    gl.stream_buf_read_idx += total_size;

    // If buffer is now empty, reset indices to avoid accumulating offset
    if (gl.stream_buf_read_idx == gl.stream_buf_write_idx) {
        gl.stream_buf_read_idx = 0;
        gl.stream_buf_write_idx = 0;
    }

    return true;
}
