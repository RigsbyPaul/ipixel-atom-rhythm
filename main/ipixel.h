#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    IPIXEL_SCANNING,
    IPIXEL_CONNECTING,
    IPIXEL_READY,
} ipixel_state_t;

// A panel's BLE address (NimBLE ble_addr_t layout: type + 6 bytes).
typedef struct {
    uint8_t type;
    uint8_t val[6];
} ipixel_addr_t;

// Called (from the BLE task) when a panel is found by scanning, so it can be saved.
typedef void (*ipixel_found_cb)(const ipixel_addr_t *addr);

// Start NimBLE and keep connected to the panel. If known is not NULL, connect to it
// directly and only scan after 3 failed attempts.
void ipixel_start(const ipixel_addr_t *known, ipixel_found_cb on_found);
ipixel_state_t ipixel_state(void);
// True once the write characteristic (fa02) has been found.
bool ipixel_ready(void);
// Panel size read from its time-sync reply at connect; false until known.
bool ipixel_panel_size(int *width, int *height);
// Called (from the BLE task) when the connected panel reports its size.
typedef void (*ipixel_size_cb)(const ipixel_addr_t *addr, int width, int height);
void ipixel_on_size(ipixel_size_cb cb);
// Address of the connected panel; false when not connected.
bool ipixel_connected_addr(ipixel_addr_t *addr);
// Use a size from elsewhere (e.g. saved earlier) until the panel reports one.
void ipixel_set_panel_size(int width, int height);
// Ask the panel for its size again (time sync; sets the panel clock to 00:00).
void ipixel_request_panel_size(void);
// Write one command to fa02. Returns 0 on success, a NimBLE error otherwise.
int ipixel_write(const uint8_t *data, size_t len);
// Write with response and wait for the panel to confirm (falls back to
// ipixel_write if the panel only offers write without response). Blocks; don't
// call from the BLE task.
int ipixel_write_confirmed(const uint8_t *data, size_t len);
// Write without response, waiting for NimBLE buffers instead of failing when
// they run out (big blocks). Blocks; don't call from the BLE task.
int ipixel_write_paced(const uint8_t *data, size_t len);
// Frame acks (05 00 <type> 00 <status>): arm before writing, then wait.
// Returns the status (0 invalid, 1 next frame, 2 no space, 3 complete) or -1
// on timeout.
void ipixel_ack_arm(void);
int ipixel_ack_wait(int timeout_ms);
