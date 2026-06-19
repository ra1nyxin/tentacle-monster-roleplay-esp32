#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

void ble_store_config_init(void);

static const char *TAG = "GALAKU_BRIDGE";
static const char *TARGET_NAME = "GK36";

static const ble_uuid16_t GALAKU_SERVICE_UUID = BLE_UUID16_INIT(0x1000);
static const ble_uuid16_t GALAKU_WRITE_UUID = BLE_UUID16_INIT(0x1001);

static uint8_t own_addr_type;
static uint16_t conn_handle;
static uint16_t service_start_handle;
static uint16_t service_end_handle;
static uint16_t write_value_handle;
static bool host_synced;
static bool scanning;
static bool connecting;
static bool connected;
static bool service_ready;
static SemaphoreHandle_t ble_write_sem;
static SemaphoreHandle_t ble_send_mutex;

static uint8_t current_level;
static uint8_t last_sent_level = 255;
static int64_t hold_until_us;
static int64_t last_tick_us;

static const int HOLD_MS = 7000;
static const int TICK_MS = 50;
static const int DECAY_PER_TICK = 1;
static const int DAMAGE_TO_PERCENT = 10;

static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_scan(void);
static void start_service_discovery(void);

static const int8_t KEYTAB[4][12] = {
    {0, 24, -104, -9, -91, 61, 13, 41, 37, 80, 68, 70},
    {0, 69, 110, 106, 111, 120, 32, 83, 45, 49, 46, 55},
    {0, 101, 120, 32, 84, 111, 121, 115, 10, -114, -99, -93},
    {0, -59, -42, -25, -8, 10, 50, 32, 111, 98, 13, 10},
};

static uint8_t clamp_level(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return (uint8_t)value;
}

static uint8_t get_tab_key(uint8_t prev, int index)
{
    return (uint8_t)KEYTAB[prev & 0x03][index];
}

static uint8_t checksum_signed(const uint8_t *buf, int len)
{
    int sum = 0;
    for (int i = 0; i < len; i++) {
        sum += (int8_t)buf[i];
    }
    return (uint8_t)sum;
}

static void encrypt12(const uint8_t plain[12], uint8_t out[12])
{
    uint8_t head = plain[0];
    out[0] = head;
    for (int i = 1; i < 12; i++) {
        uint8_t key = get_tab_key(out[i - 1], i);
        out[i] = (uint8_t)(((key ^ head) ^ plain[i]) + key);
    }
}

static void build_packet(const uint8_t cmd10[10], uint8_t out12[12])
{
    uint8_t plain[12] = {0};
    plain[0] = 0x23;
    memcpy(&plain[1], cmd10, 10);
    plain[11] = checksum_signed(plain, 11);
    encrypt12(plain, out12);
}

static void build_vibrate(uint8_t level, uint8_t out12[12])
{
    level = clamp_level(level);
    uint8_t cmd[10] = {0x5A, 0x00, 0x00, 0x01, 0x31, level, 0x00, 0x00, 0x00, 0x00};
    build_packet(cmd, out12);
}

static uint8_t shock_from_level(uint8_t level)
{
    if (level == 0) {
        return 0;
    }
    return (uint8_t)((level * 6 + 99) / 100);
}

static void build_gk36_electric(uint8_t level, uint8_t out12[12])
{
    level = clamp_level(level);
    uint8_t frequency = level == 0 ? 0 : 4;
    uint8_t shock = shock_from_level(level);
    uint8_t cmd[10] = {0x5A, 0x00, 0x00, 0x01, 0x90, frequency, shock, 0x00, 0x00, 0x00};
    build_packet(cmd, out12);
}

static int write_done_cb(uint16_t handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGW(TAG, "BLE write status=%d", error->status);
    }
    if (ble_write_sem != NULL) {
        xSemaphoreGive(ble_write_sem);
    }
    return 0;
}

static bool wait_ble_write_done(void)
{
    if (ble_write_sem == NULL) {
        return true;
    }
    if (xSemaphoreTake(ble_write_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "BLE write timeout");
        return false;
    }
    return true;
}

static bool ble_ready(void)
{
    return connected && service_ready && write_value_handle != 0;
}

static void send_gk36_level(uint8_t level)
{
    bool locked = false;
    if (!ble_ready()) {
        return;
    }
    if (ble_send_mutex != NULL) {
        if (xSemaphoreTake(ble_send_mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
            ESP_LOGW(TAG, "BLE send busy");
            return;
        }
        locked = true;
    }

    level = clamp_level(level);
    if (level == last_sent_level) {
        goto out;
    }

    uint8_t packet[12];
    int rc;

    while (ble_write_sem != NULL && xSemaphoreTake(ble_write_sem, 0) == pdTRUE) {
    }

    build_gk36_electric(level, packet);
    rc = ble_gattc_write_flat(conn_handle, write_value_handle, packet, sizeof(packet), write_done_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "electric write failed rc=%d", rc);
        goto out;
    }
    if (!wait_ble_write_done()) {
        goto out;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    build_vibrate(level, packet);
    rc = ble_gattc_write_flat(conn_handle, write_value_handle, packet, sizeof(packet), write_done_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "vibrate write failed rc=%d", rc);
        goto out;
    }
    if (!wait_ble_write_done()) {
        goto out;
    }

    last_sent_level = level;

out:
    if (locked) {
        xSemaphoreGive(ble_send_mutex);
    }
}

static int discover_chr_cb(uint16_t handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr != NULL) {
        write_value_handle = chr->val_handle;
        service_ready = true;
        last_sent_level = 255;
        ESP_LOGI(TAG, "GALAKU write characteristic handle=%u", write_value_handle);
        puts("GALAKU connected");
        return 0;
    }

    if (error->status == BLE_HS_EDONE && write_value_handle == 0) {
        ESP_LOGW(TAG, "GALAKU write characteristic not found");
    } else if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "characteristic discovery status=%d", error->status);
    }
    return 0;
}

static int discover_svc_cb(uint16_t handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == 0 && service != NULL) {
        service_start_handle = service->start_handle;
        service_end_handle = service->end_handle;
        ESP_LOGI(TAG, "GALAKU service handles=%u..%u", service_start_handle, service_end_handle);
        int rc = ble_gattc_disc_chrs_by_uuid(conn_handle, service_start_handle, service_end_handle,
                                             &GALAKU_WRITE_UUID.u, discover_chr_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "start characteristic discovery failed rc=%d", rc);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE && service_start_handle == 0) {
        ESP_LOGW(TAG, "GALAKU service not found");
    } else if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "service discovery status=%d", error->status);
    }
    return 0;
}

static int mtu_cb(uint16_t handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    ESP_LOGI(TAG, "MTU status=%d mtu=%u", error->status, mtu);
    start_service_discovery();
    return 0;
}

static int list_svc_cb(uint16_t handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == 0 && service != NULL) {
        char uuid_str[BLE_UUID_STR_LEN];
        printf("SVC %u..%u %s\n", service->start_handle, service->end_handle,
               ble_uuid_to_str(&service->uuid.u, uuid_str));
    } else if (error->status == BLE_HS_EDONE) {
        puts("SVC_DONE");
    } else {
        printf("SVC_ERR status=%d\n", error->status);
    }
    return 0;
}

static bool adv_name_matches(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return false;
    }

    const uint8_t *name = fields.name;
    uint8_t name_len = fields.name_len;
    if (name == NULL || name_len == 0) {
        return false;
    }

    printf("FOUND_ADV %.*s\n", name_len, (const char *)name);
    return name_len == strlen(TARGET_NAME) && memcmp(name, TARGET_NAME, name_len) == 0;
}

static void reset_ble_state(void)
{
    connected = false;
    connecting = false;
    service_ready = false;
    conn_handle = 0;
    service_start_handle = 0;
    service_end_handle = 0;
    write_value_handle = 0;
    last_sent_level = 255;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (!connecting && adv_name_matches(&event->disc)) {
            ble_addr_t addr = event->disc.addr;
            ESP_LOGI(TAG, "target %s found, connecting", TARGET_NAME);
            connecting = true;
            scanning = false;
            ble_gap_disc_cancel();
            int rc = ble_gap_connect(own_addr_type, &addr, 30000, NULL, gap_event_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "connect start failed rc=%d", rc);
                connecting = false;
                start_scan();
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        scanning = false;
        ESP_LOGI(TAG, "scan complete reason=%d", event->disc_complete.reason);
        if (!connected && !connecting) {
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        connecting = false;
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
            connected = true;
            ESP_LOGI(TAG, "BLE connected handle=%u", conn_handle);
            int rc = ble_gattc_exchange_mtu(conn_handle, mtu_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "MTU exchange start failed rc=%d", rc);
                start_service_discovery();
            }
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            reset_ble_state();
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "BLE disconnected reason=%d", event->disconnect.reason);
        reset_ble_state();
        start_scan();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated=%u", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void start_scan(void)
{
    if (!host_synced || scanning || connected || connecting) {
        return;
    }

    struct ble_gap_disc_params params = {0};
    params.filter_duplicates = 0;
    params.passive = 0;
    params.itvl = 0;
    params.window = 0;

    int rc = ble_gap_disc(own_addr_type, 10000, &params, gap_event_cb, NULL);
    if (rc == 0) {
        scanning = true;
        puts("Scanning for GK36...");
    } else {
        ESP_LOGW(TAG, "scan start failed rc=%d", rc);
    }
}

static void start_service_discovery(void)
{
    if (!connected || service_ready) {
        return;
    }

    service_start_handle = 0;
    service_end_handle = 0;
    write_value_handle = 0;

    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &GALAKU_SERVICE_UUID.u, discover_svc_cb, NULL);
    if (rc == 0) {
        puts("Discovering GALAKU service...");
    } else {
        ESP_LOGW(TAG, "start service discovery failed rc=%d", rc);
        printf("DISCOVER failed rc=%d\n", rc);
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "own addr infer failed rc=%d", rc);
        return;
    }
    host_synced = true;
    start_scan();
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void handle_hit(float damage)
{
    if (damage < 0.0f) {
        damage = 0.0f;
    }
    int damage_units = (int)(damage + 0.5f);
    if (damage_units < 1) {
        damage_units = 1;
    }
    int add = damage_units * DAMAGE_TO_PERCENT;
    current_level = clamp_level((int)current_level + add);
    hold_until_us = esp_timer_get_time() + (int64_t)HOLD_MS * 1000;
    send_gk36_level(current_level);
    printf("OK HIT damage=%.2f level=%u\n", damage, current_level);
}

static void handle_line(char *line)
{
    while (isspace((unsigned char)*line)) {
        line++;
    }
    char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    if (*line == '\0') {
        return;
    }

    if (strcmp(line, "PING") == 0) {
        puts("PONG");
    } else if (strcmp(line, "STOP") == 0) {
        current_level = 0;
        hold_until_us = 0;
        send_gk36_level(0);
        puts("OK STOP");
    } else if (strcmp(line, "STATUS") == 0) {
        if (connected && !service_ready) {
            start_service_discovery();
        }
        printf("STATUS ble=%d host_synced=%d scanning=%d connecting=%d connected=%d service_ready=%d target=%s level=%u handle=%u\n",
               ble_ready(), host_synced, scanning, connecting, connected, service_ready,
               TARGET_NAME, current_level, write_value_handle);
    } else if (strcmp(line, "SCAN") == 0) {
        if (!connected && !connecting) {
            scanning = false;
            start_scan();
        }
        printf("OK SCAN host_synced=%d scanning=%d connecting=%d connected=%d\n",
               host_synced, scanning, connecting, connected);
    } else if (strcmp(line, "SERVICES") == 0) {
        if (!connected) {
            puts("ERR not connected");
        } else {
            int rc = ble_gattc_disc_all_svcs(conn_handle, list_svc_cb, NULL);
            if (rc == 0) {
                puts("OK SERVICES");
            } else {
                printf("SERVICES failed rc=%d\n", rc);
            }
        }
    } else if (strncmp(line, "SET ", 4) == 0) {
        current_level = clamp_level(atoi(line + 4));
        hold_until_us = esp_timer_get_time() + (int64_t)HOLD_MS * 1000;
        send_gk36_level(current_level);
        printf("OK SET %u\n", current_level);
    } else if (strncmp(line, "HIT ", 4) == 0) {
        handle_hit(strtof(line + 4, NULL));
    } else {
        printf("ERR unknown command: %s\n", line);
    }
}

static void serial_task(void *param)
{
    char line[96];
    size_t len = 0;

    while (true) {
        uint8_t byte = 0;
        int n = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(20));
        if (n <= 0) {
            continue;
        }
        int c = byte;
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[len] = '\0';
            handle_line(line);
            len = 0;
        } else if (len + 1 < sizeof(line)) {
            line[len++] = (char)c;
        }
    }
}

static void feedback_task(void *param)
{
    while (true) {
        int64_t now = esp_timer_get_time();
        if (now - last_tick_us >= (int64_t)TICK_MS * 1000) {
            last_tick_us = now;
            if (current_level == 0) {
                send_gk36_level(0);
            } else if (now >= hold_until_us) {
                current_level = clamp_level((int)current_level - DECAY_PER_TICK);
                send_gk36_level(current_level);
            } else {
                send_gk36_level(current_level);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ble_write_sem = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(ble_write_sem == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ble_send_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(ble_send_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    usb_serial_jtag_driver_config_t usb_serial_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_cfg));

    puts("GALAKU ESP32S3 bridge boot");
    puts("Serial commands: PING, STATUS, SET <0-100>, HIT <damage>, STOP");

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_store_config_init();

    nimble_port_freertos_init(nimble_host_task);

    xTaskCreate(serial_task, "serial", 4096, NULL, 5, NULL);
    xTaskCreate(feedback_task, "feedback", 4096, NULL, 4, NULL);
}
