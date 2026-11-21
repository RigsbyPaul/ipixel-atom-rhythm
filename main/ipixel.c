// NimBLE central for the iPixel LED panel.
// Scans for a name starting "LED_BLE_", connects, finds the fa02 write
// characteristic and keeps the link up. See ipixel-ble device.py.
#include "ipixel.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ipixel";

#define NAME_PREFIX "LED_BLE_"
#define UUID_WRITE 0xFA02
#define UUID_NOTIFY 0xFA03

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_write_handle;
static bool s_write_no_rsp;
static volatile bool s_ready;
static uint8_t s_own_addr_type;
static volatile ipixel_state_t s_state = IPIXEL_SCANNING;
static ble_addr_t s_known;
static bool s_have_known;
static int s_direct_failures;
static ipixel_found_cb s_on_found;
static uint16_t s_notify_handle, s_cccd_handle;
static volatile int s_width, s_height;  // 0 until the panel has told us
static volatile uint8_t s_panel_type;

// Panel size by the device-type byte of the time-sync reply (ipixel-ble PANEL_SIZES).
static const struct { uint8_t type, w, h; } PANEL_SIZES[] = {
    {128, 64, 64}, {129, 32, 32}, {130, 32, 16}, {131, 64, 16}, {132, 96, 16},
    {133, 64, 20}, {134, 128, 32}, {135, 144, 16}, {136, 192, 16}, {137, 48, 24},
    {138, 64, 32}, {139, 96, 32}, {140, 128, 32}, {141, 96, 32}, {142, 160, 32},
    {143, 192, 32}, {144, 0, 32}, {145, 0, 32}, {146, 0, 32}, {147, 0, 32},
};  // 144..147 are 256..448 wide: too wide for single-byte x, treated as unknown

static void start_scan(void);
static void reconnect(void);

// Accept a 16-bit UUID in either its short or full 128-bit (base UUID) form.
static bool uuid_is16(const ble_uuid_t *u, uint16_t value)
{
    if (u->type == BLE_UUID_TYPE_16) {
        return BLE_UUID16(u)->value == value;
    }
    if (u->type == BLE_UUID_TYPE_128) {
        static const uint8_t base[16] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                         0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        const uint8_t *v = BLE_UUID128(u)->value;
        return memcmp(v, base, 12) == 0 && v[12] == (value & 0xff) && v[13] == (value >> 8) &&
               v[14] == 0 && v[15] == 0;
    }
    return false;
}

bool ipixel_ready(void) { return s_ready; }

ipixel_state_t ipixel_state(void) { return s_state; }

bool ipixel_panel_size(int *w, int *h)
{
    *w = s_width;
    *h = s_height;
    return s_width > 0 && s_height > 0;
}

int ipixel_write(const uint8_t *data, size_t len)
{
    if (!s_ready) {
        return BLE_HS_ENOTCONN;
    }
    // Split into ATT-MTU-sized writes, like ipixel-ble (chunk = MTU - 3).
    size_t chunk = ble_att_mtu(s_conn) - 3;
    if (chunk < 20) chunk = 20;
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        int rc = s_write_no_rsp
                     ? ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, data + off, n)
                     : ble_gattc_write_flat(s_conn, s_write_handle, data + off, n, NULL, NULL);
        if (rc != 0) return rc;
    }
    return 0;
}

static void set_ready(void)
{
    s_ready = true;
    s_state = IPIXEL_READY;
    s_direct_failures = 0;
    ESP_LOGI(TAG, "panel ready");
}

// Time sync: the panel replies with its device info (byte 4 = panel type).
// The Atom has no clock, so this sets the panel's clock to 00:00:00.
static void ask_panel_info(void)
{
    static const uint8_t sync[8] = {8, 0, 1, 0x80, 0, 0, 0, 0};
    ipixel_write(sync, sizeof sync);
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err,
                           struct ble_gatt_attr *attr, void *arg)
{
    ESP_LOGI(TAG, "notifications %s", err->status == 0 ? "on" : "failed");
    set_ready();
    if (err->status == 0) ask_panel_info();
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0 && dsc) {
        if (!s_cccd_handle && dsc->handle > s_notify_handle && uuid_is16(&dsc->uuid.u, 0x2902)) {
            s_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (s_cccd_handle) {
        static const uint8_t on[2] = {1, 0};
        ble_gattc_write_flat(conn, s_cccd_handle, on, sizeof on, on_cccd_written, NULL);
    } else {
        ESP_LOGW(TAG, "fa03 has no CCCD; panel size unknown");
        set_ready();
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        if (uuid_is16(&chr->uuid.u, UUID_WRITE)) {
            s_write_handle = chr->val_handle;
            s_write_no_rsp = (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
            ESP_LOGI(TAG, "fa02 at handle %u, props 0x%02x (%s)", chr->val_handle,
                     chr->properties, s_write_no_rsp ? "write without response" : "write");
        } else if (uuid_is16(&chr->uuid.u, UUID_NOTIFY)) {
            ESP_LOGI(TAG, "fa03 at handle %u", chr->val_handle);
            s_notify_handle = chr->val_handle;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (s_write_handle && s_notify_handle) {
            s_cccd_handle = 0;
            ble_gattc_disc_all_dscs(conn, s_notify_handle, 0xffff, on_dsc, NULL);
        } else if (s_write_handle) {
            set_ready();
        } else {
            ESP_LOGE(TAG, "no fa02 characteristic; disconnecting");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }
    ESP_LOGE(TAG, "characteristic discovery failed: %d", err->status);
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    ESP_LOGI(TAG, "MTU %u (status %d)", mtu, err->status);
    ble_gattc_disc_all_chrs(conn, 1, 0xffff, on_chr, NULL);
    return 0;
}

static bool is_panel(const struct ble_gap_disc_desc *d, char *name, size_t name_len)
{
    struct ble_hs_adv_fields f;
    name[0] = 0;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
        return false;
    }
    if (f.name) {
        size_t n = f.name_len < name_len - 1 ? f.name_len : name_len - 1;
        memcpy(name, f.name, n);
        name[n] = 0;
        ESP_LOGD(TAG, "seen %s", name);
    }
    for (int i = 0; i < f.num_uuids16; i++) {
        if (f.uuids16[i].value == 0xFA01) {
            if (!name[0]) strcpy(name, "(fa01 service)");
            return true;
        }
    }
    return strncmp(name, NAME_PREFIX, strlen(NAME_PREFIX)) == 0;
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        char name[32];
        if (!is_panel(&ev->disc, name, sizeof name)) {
            return 0;
        }
        ESP_LOGI(TAG, "found %s (rssi %d), connecting", name, ev->disc.rssi);
        ble_gap_disc_cancel();
        s_known = ev->disc.addr;
        s_have_known = true;
        s_direct_failures = 0;
        if (s_on_found) {
            ipixel_addr_t a = {.type = s_known.type};
            memcpy(a.val, s_known.val, 6);
            s_on_found(&a);
        }
        s_state = IPIXEL_CONNECTING;
        int rc = ble_gap_connect(s_own_addr_type, &ev->disc.addr, 10000, NULL, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "connect failed to start: %d", rc);
            start_scan();
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: %d", ev->connect.status);
            s_direct_failures++;
            reconnect();
            return 0;
        }
        s_conn = ev->connect.conn_handle;
        ESP_LOGI(TAG, "connected");
        ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
        s_ready = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_write_handle = 0;
        s_notify_handle = 0;
        s_direct_failures++;
        vTaskDelay(pdMS_TO_TICKS(1000));
        reconnect();
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            start_scan();
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t buf[32];
        uint16_t n = OS_MBUF_PKTLEN(ev->notify_rx.om);
        if (n > sizeof buf) n = sizeof buf;
        os_mbuf_copydata(ev->notify_rx.om, 0, n, buf);
        bool ack = n == 5 && buf[0] == 0x05;
        if (!ack && n >= 5 && n != 8) {  // 8 bytes = firmware reply
            s_panel_type = buf[4];
            int w = 0, h = 0;
            for (size_t i = 0; i < sizeof PANEL_SIZES / sizeof PANEL_SIZES[0]; i++) {
                if (PANEL_SIZES[i].type == buf[4]) {
                    w = PANEL_SIZES[i].w;
                    h = PANEL_SIZES[i].h;
                }
            }
            s_width = w;
            s_height = h;
            if (w) {
                ESP_LOGI(TAG, "panel type %u: %d x %d", buf[4], w, h);
            } else {
                ESP_LOGW(TAG, "panel type %u: size not supported", buf[4]);
            }
        } else {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, n, ESP_LOG_DEBUG);
        }
        return 0;
    }
    default:
        return 0;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = {
        .passive = 0,  // active scan: the name may only be in the scan response
        .filter_duplicates = 1,
        .itvl = 0x50,
        .window = 0x30,
    };
    s_state = IPIXEL_SCANNING;
    ESP_LOGI(TAG, "scanning for %s*", NAME_PREFIX);
    int rc = ble_gap_disc(s_own_addr_type, 30000, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "scan failed to start: %d", rc);
    }
}

// Connect straight to the saved panel; after 3 failures in a row, scan instead
// (the panel may have been swapped, or another device is holding it).
static void reconnect(void)
{
    if (s_have_known && s_direct_failures < 3) {
        ESP_LOGI(TAG, "connecting to saved panel %02x:%02x:%02x:%02x:%02x:%02x (try %d)",
                 s_known.val[5], s_known.val[4], s_known.val[3], s_known.val[2],
                 s_known.val[1], s_known.val[0], s_direct_failures + 1);
        s_state = IPIXEL_CONNECTING;
        int rc = ble_gap_connect(s_own_addr_type, &s_known, 5000, NULL, gap_event, NULL);
        if (rc == 0) {
            return;
        }
        ESP_LOGE(TAG, "connect failed to start: %d", rc);
    }
    start_scan();
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    reconnect();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset: %d", reason); }

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ipixel_start(const ipixel_addr_t *known, ipixel_found_cb on_found)
{
    s_on_found = on_found;
    if (known) {
        s_known.type = known->type;
        memcpy(s_known.val, known->val, 6);
        s_have_known = true;
    }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}
