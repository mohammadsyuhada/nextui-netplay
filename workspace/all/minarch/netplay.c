/*
 * NextUI Netplay Module
 * Simplified implementation based on RetroArch netplay concepts
 *
 * Key design:
 * - Lockstep synchronization: both devices must have same inputs before advancing
 * - Frame buffer: circular buffer storing input history
 * - Host = Player 1, Client = Player 2 (always)
 * - Both devices run identical emulation with identical inputs
 */

#include "netplay.h"
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

// Protocol constants (internal)
#define NP_PROTOCOL_MAGIC   0x4E585550  // "NXUP" - NextUI Protocol
#define NP_DISCOVERY_QUERY  0x4E584451  // "NXDQ" - NextUI Discovery Query
#define NP_DISCOVERY_RESP   0x4E584452  // "NXDR" - NextUI Discovery Response

// Optimization: Discovery broadcast interval (microseconds)
#define DISCOVERY_BROADCAST_INTERVAL_US 500000  // 500ms

// Network commands
enum {
    CMD_INPUT      = 0x01,  // Input data for a frame
    CMD_STATE_REQ  = 0x02,  // Request state transfer
    CMD_STATE_HDR  = 0x03,  // State header (size)
    CMD_STATE_DATA = 0x04,  // State data chunk
    CMD_STATE_ACK  = 0x05,  // State received OK
    CMD_PING       = 0x06,
    CMD_PONG       = 0x07,
    CMD_DISCONNECT = 0x08,
    CMD_READY      = 0x09,  // Ready to play
    CMD_PAUSE      = 0x0A,  // Player paused (menu opened)
    CMD_RESUME     = 0x0B,  // Player resumed (menu closed)
};

// Frame input entry
typedef struct {
    uint32_t frame;
    uint16_t p1_input;  // Host input (always Player 1)
    uint16_t p2_input;  // Client input (always Player 2)
    bool have_p1;
    bool have_p2;
} FrameInput;

// Packet header
typedef struct __attribute__((packed)) {
    uint8_t  cmd;
    uint32_t frame;
    uint16_t size;
} PacketHeader;

// Input packet
typedef struct __attribute__((packed)) {
    uint16_t input;
} InputPacket;

// Discovery packet
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t protocol_version;
    uint32_t game_crc;
    uint16_t port;
    char game_name[NETPLAY_MAX_GAME_NAME];
} DiscoveryPacket;

// Main netplay state
static struct {
    NetplayMode mode;
    NetplayState state;

    // Sockets
    int tcp_fd;         // Main TCP connection
    int listen_fd;      // Server listen socket
    int udp_fd;         // Discovery UDP socket

    // Connection info
    char local_ip[16];
    char remote_ip[16];
    uint16_t port;

    // Game info
    char game_name[NETPLAY_MAX_GAME_NAME];
    uint32_t game_crc;

    // Frame synchronization
    uint32_t self_frame;        // Our current frame
    uint32_t run_frame;         // Frame we're executing
    uint32_t other_frame;       // Last frame with complete input

    // Circular frame buffer
    FrameInput frame_buffer[NETPLAY_FRAME_BUFFER_SIZE];

    // Local input for current frame
    uint16_t local_input;

    // State sync flags
    bool needs_state_sync;
    bool state_sync_complete;

    // Discovery
    NetplayHostInfo discovered_hosts[NETPLAY_MAX_HOSTS];
    int num_hosts;
    bool discovery_active;

    // Threading
    pthread_t listen_thread;
    pthread_mutex_t mutex;
    volatile bool running;

    // Status
    char status_msg[128];
    int stall_frames;

    // Optimization: Cached audio silence state (updated per frame)
    volatile bool audio_should_silence;

    // Hotspot mode
    bool using_hotspot;

    // Pause state (for menu)
    bool local_paused;   // We have paused (menu open)
    bool remote_paused;  // Remote player has paused

} np = {0};

// Forward declarations
static bool send_packet(uint8_t cmd, uint32_t frame, const void* data, uint16_t size);
static bool recv_packet(PacketHeader* hdr, void* data, uint16_t max_size, int timeout_ms);
static void* listen_thread_func(void* arg);
static FrameInput* get_frame_slot(uint32_t frame);
static void init_frame_buffer(void);

//////////////////////////////////////////////////////////////////////////////
// Initialization
//////////////////////////////////////////////////////////////////////////////

void Netplay_init(void) {
    memset(&np, 0, sizeof(np));
    np.mode = NETPLAY_OFF;
    np.state = NETPLAY_STATE_IDLE;
    np.tcp_fd = -1;
    np.listen_fd = -1;
    np.udp_fd = -1;
    np.port = NETPLAY_DEFAULT_PORT;
    pthread_mutex_init(&np.mutex, NULL);
    NET_getLocalIP(np.local_ip, sizeof(np.local_ip));
    snprintf(np.status_msg, sizeof(np.status_msg), "Netplay ready");
}

void Netplay_quit(void) {
    Netplay_disconnect();
    Netplay_stopHost();
    Netplay_stopDiscovery();
    pthread_mutex_destroy(&np.mutex);
}

//////////////////////////////////////////////////////////////////////////////
// Helper Functions (extracted for code reuse)
//////////////////////////////////////////////////////////////////////////////

static FrameInput* get_frame_slot(uint32_t frame) {
    return &np.frame_buffer[frame & NETPLAY_FRAME_MASK];
}

static void init_frame_slot(uint32_t frame) {
    FrameInput* slot = get_frame_slot(frame);
    slot->frame = frame;
    slot->p1_input = 0;
    slot->p2_input = 0;
    slot->have_p1 = false;
    slot->have_p2 = false;
}

// Optimization: Extracted duplicate frame buffer initialization
static void init_frame_buffer(void) {
    for (int i = 0; i < NETPLAY_FRAME_BUFFER_SIZE; i++) {
        init_frame_slot(i);
    }
}

//////////////////////////////////////////////////////////////////////////////
// Host Mode
//////////////////////////////////////////////////////////////////////////////

int Netplay_startHost(const char* game_name, uint32_t game_crc) {
    if (np.mode != NETPLAY_OFF) {
        return -1;
    }

    // Create TCP listen socket using shared utility
    np.listen_fd = NET_createListenSocket(np.port, np.status_msg, sizeof(np.status_msg));
    if (np.listen_fd < 0) {
        return -1;
    }

    // Create UDP socket for discovery broadcasts
    np.udp_fd = NET_createBroadcastSocket();

    strncpy(np.game_name, game_name, NETPLAY_MAX_GAME_NAME - 1);
    np.game_crc = game_crc;

    // Start listen thread
    np.running = true;
    pthread_create(&np.listen_thread, NULL, listen_thread_func, NULL);

    np.mode = NETPLAY_HOST;
    np.state = NETPLAY_STATE_WAITING;
    np.needs_state_sync = true;

    snprintf(np.status_msg, sizeof(np.status_msg), "Hosting on %s:%d", np.local_ip, np.port);
    return 0;
}

int Netplay_startHostWithHotspot(const char* game_name, uint32_t game_crc) {
    if (np.mode != NETPLAY_OFF) {
        return -1;
    }

    // Generate SSID with random 4-character code using shared utility
    char ssid[33];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    NET_HotspotConfig hotspot_cfg = {
        .prefix = NETPLAY_HOTSPOT_SSID_PREFIX,
        .seed = (unsigned int)(tv.tv_usec ^ tv.tv_sec ^ game_crc)
    };
    NET_generateHotspotSSID(ssid, sizeof(ssid), &hotspot_cfg);

    const char* pass = PLAT_getHotspotPassword();

    snprintf(np.status_msg, sizeof(np.status_msg), "Starting hotspot...");

    if (PLAT_startHotspot(ssid, pass) != 0) {
        snprintf(np.status_msg, sizeof(np.status_msg), "Failed to start hotspot");
        return -1;
    }

    np.using_hotspot = true;

    // Use hotspot IP instead of WiFi IP
    strncpy(np.local_ip, PLAT_getHotspotIP(), sizeof(np.local_ip) - 1);

    // Create TCP listen socket using shared utility
    np.listen_fd = NET_createListenSocket(np.port, np.status_msg, sizeof(np.status_msg));
    if (np.listen_fd < 0) {
        PLAT_stopHotspot();
        np.using_hotspot = false;
        return -1;
    }

    // Create UDP socket for discovery broadcasts
    np.udp_fd = NET_createBroadcastSocket();

    strncpy(np.game_name, game_name, NETPLAY_MAX_GAME_NAME - 1);
    np.game_crc = game_crc;

    // Start listen thread
    np.running = true;
    pthread_create(&np.listen_thread, NULL, listen_thread_func, NULL);

    np.mode = NETPLAY_HOST;
    np.state = NETPLAY_STATE_WAITING;
    np.needs_state_sync = true;

    snprintf(np.status_msg, sizeof(np.status_msg), "Hotspot: %s | IP: %s", ssid, np.local_ip);
    return 0;
}

int Netplay_stopHost(void) {
    if (np.mode != NETPLAY_HOST) return -1;

    np.running = false;
    if (np.listen_thread) {
        pthread_cancel(np.listen_thread);
        pthread_join(np.listen_thread, NULL);
        np.listen_thread = 0;
    }

    if (np.listen_fd >= 0) {
        close(np.listen_fd);
        np.listen_fd = -1;
    }
    if (np.udp_fd >= 0) {
        close(np.udp_fd);
        np.udp_fd = -1;
    }

    Netplay_disconnect();

    // Stop hotspot if it was started
    if (np.using_hotspot) {
        PLAT_stopHotspot();
        np.using_hotspot = false;
    }

    np.mode = NETPLAY_OFF;
    np.state = NETPLAY_STATE_IDLE;
    snprintf(np.status_msg, sizeof(np.status_msg), "Netplay ready");
    return 0;
}

static void* listen_thread_func(void* arg) {
    (void)arg;

    // Use shared broadcast timer for rate limiting
    NET_BroadcastTimer broadcast_timer;
    NET_initBroadcastTimer(&broadcast_timer, DISCOVERY_BROADCAST_INTERVAL_US);

    while (np.running && np.listen_fd >= 0) {
        // Rate-limited discovery broadcast using shared timer
        if (np.udp_fd >= 0 && np.state == NETPLAY_STATE_WAITING) {
            if (NET_shouldBroadcast(&broadcast_timer)) {
                DiscoveryPacket disc = {0};
                disc.magic = htonl(NP_DISCOVERY_RESP);
                disc.protocol_version = htonl(NETPLAY_PROTOCOL_VERSION);
                disc.game_crc = htonl(np.game_crc);
                disc.port = htons(np.port);
                strncpy(disc.game_name, np.game_name, NETPLAY_MAX_GAME_NAME - 1);

                struct sockaddr_in bcast = {0};
                bcast.sin_family = AF_INET;
                bcast.sin_addr.s_addr = INADDR_BROADCAST;
                bcast.sin_port = htons(NETPLAY_DISCOVERY_PORT);

                sendto(np.udp_fd, &disc, sizeof(disc), 0,
                       (struct sockaddr*)&bcast, sizeof(bcast));
            }
        }

        // Check for incoming connection (only accept when waiting)
        if (np.state == NETPLAY_STATE_WAITING) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(np.listen_fd, &fds);

            struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};  // 100ms timeout
            if (select(np.listen_fd + 1, &fds, NULL, NULL, &tv) > 0) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);

                int fd = accept(np.listen_fd, (struct sockaddr*)&client_addr, &len);
                if (fd >= 0) {
                    pthread_mutex_lock(&np.mutex);

                    // Double-check we're still waiting (state could have changed)
                    if (np.state != NETPLAY_STATE_WAITING) {
                        close(fd);
                        pthread_mutex_unlock(&np.mutex);
                        continue;
                    }

                    // Configure TCP socket using shared utility (default: 64KB buffers)
                    NET_configureTCPSocket(fd, NULL);

                    np.tcp_fd = fd;
                    inet_ntop(AF_INET, &client_addr.sin_addr, np.remote_ip, sizeof(np.remote_ip));

                    np.state = NETPLAY_STATE_SYNCING;
                    np.needs_state_sync = true;
                    np.self_frame = 0;
                    np.run_frame = 0;
                    np.other_frame = 0;

                    init_frame_buffer();

                    snprintf(np.status_msg, sizeof(np.status_msg), "Client connected: %s", np.remote_ip);
                    pthread_mutex_unlock(&np.mutex);
                }
            }
        } else {
            // Not waiting, just sleep briefly to avoid busy loop
            usleep(50000);  // 50ms (reduced from 100ms)
        }
    }

    return NULL;
}

//////////////////////////////////////////////////////////////////////////////
// Client Mode
//////////////////////////////////////////////////////////////////////////////

int Netplay_connectToHost(const char* ip, uint16_t port) {
    if (np.mode != NETPLAY_OFF) {
        return -1;
    }

    np.tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (np.tcp_fd < 0) {
        snprintf(np.status_msg, sizeof(np.status_msg), "Socket creation failed");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(np.tcp_fd);
        np.tcp_fd = -1;
        snprintf(np.status_msg, sizeof(np.status_msg), "Invalid IP address");
        return -1;
    }

    np.state = NETPLAY_STATE_CONNECTING;
    snprintf(np.status_msg, sizeof(np.status_msg), "Connecting to %s:%d...", ip, port);

    // Connect with timeout
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(np.tcp_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(np.tcp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(np.tcp_fd);
        np.tcp_fd = -1;
        np.state = NETPLAY_STATE_ERROR;
        snprintf(np.status_msg, sizeof(np.status_msg), "Connection failed");
        return -1;
    }

    // Configure TCP socket using shared utility (default: 64KB buffers)
    NET_configureTCPSocket(np.tcp_fd, NULL);

    strncpy(np.remote_ip, ip, sizeof(np.remote_ip) - 1);
    np.port = port;
    np.mode = NETPLAY_CLIENT;
    np.state = NETPLAY_STATE_SYNCING;
    np.needs_state_sync = true;

    np.self_frame = 0;
    np.run_frame = 0;
    np.other_frame = 0;

    init_frame_buffer();

    snprintf(np.status_msg, sizeof(np.status_msg), "Connected to %s", ip);
    return 0;
}

void Netplay_disconnect(void) {
    if (np.tcp_fd >= 0) {
        send_packet(CMD_DISCONNECT, 0, NULL, 0);
        close(np.tcp_fd);
        np.tcp_fd = -1;
    }

    // Update cached audio state
    np.audio_should_silence = false;

    // Reset pause state
    np.local_paused = false;
    np.remote_paused = false;

    if (np.mode == NETPLAY_CLIENT) {
        np.mode = NETPLAY_OFF;
        np.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(np.status_msg, sizeof(np.status_msg), "Disconnected");
    } else if (np.mode == NETPLAY_HOST) {
        // Host stays in host mode, reset to waiting for new client
        np.state = NETPLAY_STATE_WAITING;
        np.needs_state_sync = true;
        np.stall_frames = 0;
        snprintf(np.status_msg, sizeof(np.status_msg), "Client left, waiting on %s:%d", np.local_ip, np.port);
    } else {
        np.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(np.status_msg, sizeof(np.status_msg), "Disconnected");
    }
}

//////////////////////////////////////////////////////////////////////////////
// Discovery
//////////////////////////////////////////////////////////////////////////////

int Netplay_startDiscovery(void) {
    if (np.discovery_active) return 0;

    np.udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (np.udp_fd < 0) return -1;

    int opt = 1;
    setsockopt(np.udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(NETPLAY_DISCOVERY_PORT);

    if (bind(np.udp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(np.udp_fd);
        np.udp_fd = -1;
        return -1;
    }

    // Set non-blocking
    int flags = fcntl(np.udp_fd, F_GETFL, 0);
    fcntl(np.udp_fd, F_SETFL, flags | O_NONBLOCK);

    np.num_hosts = 0;
    np.discovery_active = true;
    return 0;
}

void Netplay_stopDiscovery(void) {
    if (!np.discovery_active) return;

    if (np.udp_fd >= 0 && np.mode == NETPLAY_OFF) {
        close(np.udp_fd);
        np.udp_fd = -1;
    }

    np.discovery_active = false;
}

int Netplay_getDiscoveredHosts(NetplayHostInfo* hosts, int max_hosts) {
    if (!np.discovery_active || np.udp_fd < 0) return 0;

    // Poll for discovery responses
    DiscoveryPacket pkt;
    struct sockaddr_in sender;
    socklen_t len = sizeof(sender);

    while (recvfrom(np.udp_fd, &pkt, sizeof(pkt), 0,
                    (struct sockaddr*)&sender, &len) == sizeof(pkt)) {
        if (ntohl(pkt.magic) != NP_DISCOVERY_RESP) continue;

        char ip[16];
        inet_ntop(AF_INET, &sender.sin_addr, ip, sizeof(ip));

        // Check if already in list
        bool found = false;
        for (int i = 0; i < np.num_hosts; i++) {
            if (strcmp(np.discovered_hosts[i].host_ip, ip) == 0) {
                found = true;
                break;
            }
        }

        if (!found && np.num_hosts < NETPLAY_MAX_HOSTS) {
            NetplayHostInfo* h = &np.discovered_hosts[np.num_hosts];
            strncpy(h->game_name, pkt.game_name, NETPLAY_MAX_GAME_NAME - 1);
            strncpy(h->host_ip, ip, sizeof(h->host_ip) - 1);
            h->port = ntohs(pkt.port);
            h->game_crc = ntohl(pkt.game_crc);
            np.num_hosts++;
        }
    }

    int count = (np.num_hosts < max_hosts) ? np.num_hosts : max_hosts;
    memcpy(hosts, np.discovered_hosts, count * sizeof(NetplayHostInfo));
    return count;
}

//////////////////////////////////////////////////////////////////////////////
// Frame Synchronization (Core Netplay Logic)
//////////////////////////////////////////////////////////////////////////////

bool Netplay_preFrame(void) {
    if (!Netplay_isConnected()) return true;

    pthread_mutex_lock(&np.mutex);

    // Get slot for execution (current run frame)
    FrameInput* run_slot = get_frame_slot(np.run_frame);

    // Get slot for input sending (ahead by latency frames)
    FrameInput* input_slot = get_frame_slot(np.self_frame);
    if (input_slot->frame != np.self_frame) {
        init_frame_slot(np.self_frame);
        input_slot->frame = np.self_frame;
    }

    // Store and send our input for the FUTURE frame (np.self_frame)
    if (np.mode == NETPLAY_HOST) {
        if (!input_slot->have_p1) {
            input_slot->p1_input = np.local_input;
            input_slot->have_p1 = true;
            InputPacket pkt = { .input = htons(np.local_input) };
            send_packet(CMD_INPUT, np.self_frame, &pkt, sizeof(pkt));
        }
    } else {
        if (!input_slot->have_p2) {
            input_slot->p2_input = np.local_input;
            input_slot->have_p2 = true;
            InputPacket pkt = { .input = htons(np.local_input) };
            send_packet(CMD_INPUT, np.self_frame, &pkt, sizeof(pkt));
        }
    }

    // Try to receive remote input - always process available packets
    int timeout_ms = 16;   // ~1 frame at 60fps
    int max_attempts = 10; // ~160ms total
    int attempts = 0;

    while (attempts < max_attempts) {
        // Check if we already have both inputs for the run frame
        run_slot = get_frame_slot(np.run_frame);
        if (run_slot->have_p1 && run_slot->have_p2) {
            break;  // Got both inputs, proceed
        }

        // Release lock during blocking network operation
        pthread_mutex_unlock(&np.mutex);

        // Try to receive remote input
        PacketHeader hdr;
        InputPacket remote_pkt;
        bool received = recv_packet(&hdr, &remote_pkt, sizeof(remote_pkt), timeout_ms);

        // Re-acquire lock for frame buffer access
        pthread_mutex_lock(&np.mutex);

        // Check if disconnected during recv
        if (np.state == NETPLAY_STATE_DISCONNECTED) {
            np.audio_should_silence = false;
            pthread_mutex_unlock(&np.mutex);
            return false;
        }

        if (received) {
            if (hdr.cmd == CMD_INPUT) {
                FrameInput* remote_slot = get_frame_slot(hdr.frame);
                uint16_t remote_input = ntohs(remote_pkt.input);

                // Store remote input in appropriate slot
                if (np.mode == NETPLAY_HOST) {
                    remote_slot->p2_input = remote_input;
                    remote_slot->have_p2 = true;
                } else {
                    remote_slot->p1_input = remote_input;
                    remote_slot->have_p1 = true;
                }
            } else if (hdr.cmd == CMD_DISCONNECT) {
                np.state = NETPLAY_STATE_DISCONNECTED;
                np.audio_should_silence = false;
                pthread_mutex_unlock(&np.mutex);
                return false;
            } else if (hdr.cmd == CMD_PAUSE) {
                np.remote_paused = true;
                np.state = NETPLAY_STATE_PAUSED;
                snprintf(np.status_msg, sizeof(np.status_msg), "Remote player paused");
            } else if (hdr.cmd == CMD_RESUME) {
                np.remote_paused = false;
                if (!np.local_paused) {
                    np.state = NETPLAY_STATE_PLAYING;
                    snprintf(np.status_msg, sizeof(np.status_msg), "Netplay active");
                }
            }
        }
        attempts++;
    }

    // Final check - do we have both inputs for run_frame?
    run_slot = get_frame_slot(np.run_frame);
    if (!run_slot->have_p1 || !run_slot->have_p2) {
        np.stall_frames++;
        // Skip timeout when either player is paused (menu open)
        if (np.stall_frames > 30 && !np.local_paused && !np.remote_paused) {
            snprintf(np.status_msg, sizeof(np.status_msg), "Connection timeout");
            np.state = NETPLAY_STATE_DISCONNECTED;
            np.audio_should_silence = false;
            pthread_mutex_unlock(&np.mutex);
            return false;
        }
        np.state = NETPLAY_STATE_STALLED;
        np.audio_should_silence = true;
        pthread_mutex_unlock(&np.mutex);
        return false;
    }

    np.stall_frames = 0;
    np.audio_should_silence = false;
    np.state = NETPLAY_STATE_PLAYING;
    pthread_mutex_unlock(&np.mutex);
    return true;
}

uint16_t Netplay_getInputState(unsigned port) {
    if (!Netplay_isConnected()) return 0;

    pthread_mutex_lock(&np.mutex);
    FrameInput* slot = get_frame_slot(np.run_frame);
    uint16_t input = (port == 0) ? slot->p1_input : slot->p2_input;
    pthread_mutex_unlock(&np.mutex);

    return input;
}

void Netplay_setLocalInput(uint16_t input) {
    np.local_input = input;
}

void Netplay_postFrame(void) {
    if (!Netplay_isConnected()) return;

    pthread_mutex_lock(&np.mutex);
    np.run_frame++;
    np.self_frame++;
    pthread_mutex_unlock(&np.mutex);
}

bool Netplay_shouldStall(void) {
    return np.state == NETPLAY_STATE_STALLED;
}

// Optimization: Uses cached value instead of checking state each call
bool Netplay_shouldSilenceAudio(void) {
    return np.audio_should_silence;
}

//////////////////////////////////////////////////////////////////////////////
// State Synchronization
//////////////////////////////////////////////////////////////////////////////

int Netplay_sendState(const void* data, size_t size) {
    if (!Netplay_isConnected() || !data || size == 0) return -1;

    // Send state header
    uint32_t state_size = (uint32_t)size;
    state_size = htonl(state_size);
    if (!send_packet(CMD_STATE_HDR, 0, &state_size, sizeof(state_size))) {
        return -1;
    }

    // Send state data in chunks
    const uint8_t* ptr = (const uint8_t*)data;
    size_t remaining = size;

    while (remaining > 0) {
        size_t chunk = (remaining > 4096) ? 4096 : remaining;
        ssize_t sent = send(np.tcp_fd, ptr, chunk, MSG_NOSIGNAL);
        if (sent <= 0) {
            return -1;
        }
        ptr += sent;
        remaining -= sent;
    }

    // Wait for client ACK
    PacketHeader hdr;
    if (!recv_packet(&hdr, NULL, 0, 10000) || hdr.cmd != CMD_STATE_ACK) {
        return -1;
    }

    // Send READY signal
    if (!send_packet(CMD_READY, 0, NULL, 0)) {
        return -1;
    }

    return 0;
}

int Netplay_receiveState(void* data, size_t size) {
    if (!Netplay_isConnected() || !data || size == 0) return -1;

    // Receive state header
    PacketHeader hdr;
    uint32_t state_size;

    if (!recv_packet(&hdr, &state_size, sizeof(state_size), 10000) ||
        hdr.cmd != CMD_STATE_HDR) {
        return -1;
    }

    state_size = ntohl(state_size);

    if (state_size != size) {
        snprintf(np.status_msg, sizeof(np.status_msg),
                 "State size mismatch: %u vs %zu", state_size, size);
        return -1;
    }

    // Receive state data
    uint8_t* ptr = (uint8_t*)data;
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t received = recv(np.tcp_fd, ptr, remaining, 0);
        if (received <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        ptr += received;
        remaining -= received;
    }

    // Send ACK to host
    if (!send_packet(CMD_STATE_ACK, 0, NULL, 0)) {
        return -1;
    }

    // Wait for READY signal from host
    if (!recv_packet(&hdr, NULL, 0, 10000) || hdr.cmd != CMD_READY) {
        return -1;
    }

    return 0;
}

bool Netplay_needsStateSync(void) {
    return np.needs_state_sync && np.state == NETPLAY_STATE_SYNCING;
}

void Netplay_completeStateSync(void) {
    pthread_mutex_lock(&np.mutex);
    np.needs_state_sync = false;
    np.state_sync_complete = true;
    np.state = NETPLAY_STATE_PLAYING;

    // Pre-fill latency buffer frames with neutral input
    for (int i = 0; i < NETPLAY_INPUT_LATENCY_FRAMES; i++) {
        FrameInput* slot = get_frame_slot(i);
        slot->frame = i;
        slot->p1_input = 0;
        slot->p2_input = 0;
        slot->have_p1 = true;
        slot->have_p2 = true;
    }

    np.run_frame = 0;
    np.self_frame = NETPLAY_INPUT_LATENCY_FRAMES;
    np.stall_frames = 0;
    np.audio_should_silence = false;

    snprintf(np.status_msg, sizeof(np.status_msg), "Netplay active");
    pthread_mutex_unlock(&np.mutex);
}

//////////////////////////////////////////////////////////////////////////////
// Status Functions
//////////////////////////////////////////////////////////////////////////////

NetplayMode Netplay_getMode(void) { return np.mode; }
NetplayState Netplay_getState(void) { return np.state; }
bool Netplay_isUsingHotspot(void) { return np.using_hotspot; }

bool Netplay_isConnected(void) {
    return np.tcp_fd >= 0 &&
           (np.state == NETPLAY_STATE_SYNCING ||
            np.state == NETPLAY_STATE_PLAYING ||
            np.state == NETPLAY_STATE_STALLED ||
            np.state == NETPLAY_STATE_PAUSED);
}

bool Netplay_isActive(void) {
    return np.state == NETPLAY_STATE_PLAYING;
}

const char* Netplay_getStatusMessage(void) { return np.status_msg; }
const char* Netplay_getLocalIP(void) { return np.local_ip; }

void Netplay_clearLocalIP(void) {
    strcpy(np.local_ip, "N/A");
}

uint32_t Netplay_getFrameCount(void) { return np.run_frame; }

bool Netplay_hasNetworkConnection(void) {
    // Re-check local IP in case network state changed
    NET_getLocalIP(np.local_ip, sizeof(np.local_ip));
    // If IP is 0.0.0.0, no network connection
    return strcmp(np.local_ip, "0.0.0.0") != 0;
}

void Netplay_update(void) {
    // Check for connection errors
    if (np.tcp_fd >= 0) {
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(np.tcp_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            Netplay_disconnect();
        }
    }
}

//////////////////////////////////////////////////////////////////////////////
// Pause/Resume for Menu
//////////////////////////////////////////////////////////////////////////////

void Netplay_pause(void) {
    if (!Netplay_isConnected()) return;

    pthread_mutex_lock(&np.mutex);
    np.local_paused = true;
    send_packet(CMD_PAUSE, 0, NULL, 0);
    np.state = NETPLAY_STATE_PAUSED;
    snprintf(np.status_msg, sizeof(np.status_msg), "Paused");
    pthread_mutex_unlock(&np.mutex);
}

void Netplay_resume(void) {
    if (!Netplay_isConnected()) return;

    pthread_mutex_lock(&np.mutex);
    np.local_paused = false;
    send_packet(CMD_RESUME, 0, NULL, 0);

    // Only resume to PLAYING if remote is also not paused
    if (!np.remote_paused) {
        np.state = NETPLAY_STATE_PLAYING;
        np.stall_frames = 0;
        snprintf(np.status_msg, sizeof(np.status_msg), "Netplay active");
    } else {
        snprintf(np.status_msg, sizeof(np.status_msg), "Waiting for remote...");
    }
    pthread_mutex_unlock(&np.mutex);
}

void Netplay_pollWhilePaused(void) {
    if (!Netplay_isConnected()) return;

    // Just check if connection is still alive, don't consume any packets
    // Input packets will be processed by Netplay_preFrame() when we resume

    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(np.tcp_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        pthread_mutex_lock(&np.mutex);
        np.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(np.status_msg, sizeof(np.status_msg), "Connection lost");
        close(np.tcp_fd);
        np.tcp_fd = -1;
        pthread_mutex_unlock(&np.mutex);
    }
}

bool Netplay_isPaused(void) {
    return np.local_paused || np.remote_paused;
}

//////////////////////////////////////////////////////////////////////////////
// Network Helper Functions
//////////////////////////////////////////////////////////////////////////////

static bool send_packet(uint8_t cmd, uint32_t frame, const void* data, uint16_t size) {
    if (np.tcp_fd < 0) return false;

    PacketHeader hdr = {
        .cmd = cmd,
        .frame = htonl(frame),
        .size = htons(size)
    };

    if (send(np.tcp_fd, &hdr, sizeof(hdr), MSG_NOSIGNAL) != sizeof(hdr)) {
        return false;
    }

    if (size > 0 && data) {
        if (send(np.tcp_fd, data, size, MSG_NOSIGNAL) != size) {
            return false;
        }
    }

    return true;
}

static bool recv_packet(PacketHeader* hdr, void* data, uint16_t max_size, int timeout_ms) {
    if (np.tcp_fd < 0) return false;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(np.tcp_fd, &fds);

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };

    if (select(np.tcp_fd + 1, &fds, NULL, NULL, &tv) <= 0) {
        return false;  // Timeout or error
    }

    ssize_t ret = recv(np.tcp_fd, hdr, sizeof(*hdr), 0);
    if (ret == 0) {
        // Connection closed by remote end
        np.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(np.status_msg, sizeof(np.status_msg), "Remote disconnected");
        close(np.tcp_fd);
        np.tcp_fd = -1;
        return false;
    }
    if (ret < 0 || ret != sizeof(*hdr)) {
        // Error or partial read
        if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
            np.state = NETPLAY_STATE_DISCONNECTED;
            snprintf(np.status_msg, sizeof(np.status_msg), "Connection lost");
            close(np.tcp_fd);
            np.tcp_fd = -1;
        }
        return false;
    }

    hdr->frame = ntohl(hdr->frame);
    hdr->size = ntohs(hdr->size);

    if (hdr->size > 0 && data && hdr->size <= max_size) {
        ret = recv(np.tcp_fd, data, hdr->size, 0);
        if (ret == 0) {
            np.state = NETPLAY_STATE_DISCONNECTED;
            close(np.tcp_fd);
            np.tcp_fd = -1;
            return false;
        }
        if (ret != hdr->size) {
            return false;
        }
    }

    return true;
}
