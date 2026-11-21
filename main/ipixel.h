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
// Write one command to fa02. Returns 0 on success, a NimBLE error otherwise.
int ipixel_write(const uint8_t *data, size_t len);
