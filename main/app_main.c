// Garage door + car charger as a native HomeKit accessory on a Shelly Pro 1 over Ethernet.
// Behaviour mirrors the ESPHome config it replaces; see board.h for the mapping.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mdns.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <hap.h>
#include <hap_apple_chars.h>
#include <hap_apple_servs.h>

#include "board.h"
#include "bootloader_update.h"
#include "diag.h"
#include "io.h"
#include "net.h"
#include "ota.h"
#include "setup.h"

static const char *TAG = "garage";

#define HOSTNAME "garage"
// A new image is kept only once a HomeKit controller (the home hub, within seconds) has
// connected through it; otherwise it is rolled back after this delay.
#define ROLLBACK_DEADLINE_MS (5 * 60 * 1000)

// HomeKit door states (HAP spec): current 0 open, 1 closed, 2 opening, 3 closing.
enum { DOOR_OPEN = 0, DOOR_CLOSED = 1, DOOR_OPENING = 2, DOOR_CLOSING = 3 };

typedef enum {
    EV_TARGET_OPEN,
    EV_TARGET_CLOSED,
    EV_BUTTON_SHORT,
    EV_TRAVEL_DONE,
    EV_AUTOCLOSE,
    EV_CHARGER_SET,    // arg: 0/1
    EV_CHARGER_TOGGLE,
    EV_NET_UP,
    EV_NET_DOWN,
    EV_HK_CONNECTED,
    EV_RESTART,
    EV_RESET_PAIRINGS,
    EV_TICK,           // temperature check, every 10 s
} ev_type_t;

typedef struct { ev_type_t type; int arg; } ev_t;

static QueueHandle_t q;
static esp_timer_handle_t travel_timer, autoclose_timer, tick_timer, rollback_timer;
static hap_char_t *hc_cur, *hc_tgt, *hc_charger;
static int door = DOOR_CLOSED;
static bool charger;
static bool hap_running;
static int sessions;      // pair-verified HomeKit connections open now
static int sessions_max;  // highest since boot
static unsigned pulses;

static void post(ev_type_t t, int arg)
{
    ev_t e = { t, arg };
    xQueueSend(q, &e, 0);
}

static void timer_cb(void *arg) { post((ev_type_t)(intptr_t)arg, 0); }

static void publish_door(int cur, int tgt)
{
    door = cur;
    if (!hap_running) {
        return;
    }
    hap_val_t v = { .u = cur };
    hap_char_update_val(hc_cur, &v);
    v.u = tgt;
    hap_char_update_val(hc_tgt, &v);
}

static void set_charger(bool on)
{
    charger = on;
    io_set_charger(on);
    nvs_handle_t h;
    if (nvs_open("garage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "charger", on);
        nvs_commit(h);
        nvs_close(h);
    }
    if (hap_running) {
        hap_val_t v = { .b = on };
        hap_char_update_val(hc_charger, &v);
    }
    ESP_LOGI(TAG, "charger %s", on ? "on" : "off");
}

static bool load_charger(void)
{
    uint8_t on = 0;  // ESPHome default: restore, else off
    nvs_handle_t h;
    if (nvs_open("garage", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "charger", &on);
        nvs_close(h);
    }
    return on;
}

// Open: press the key fob, then 15 s travel, then the door closes itself 32 s later.
// Pulses even when the model says open: the shared door sometimes ignores a press, and
// re-sending "open" is the remedy. Each press restarts the model.
static void door_open(void)
{
    esp_timer_stop(travel_timer);
    esp_timer_stop(autoclose_timer);
    pulses++;
    io_door_pulse();
    publish_door(DOOR_OPENING, DOOR_OPEN);
    esp_timer_start_once(travel_timer, DOOR_TRAVEL_MS * 1000ULL);
}

// Close never pulses: the door closes on its own, so only the model moves. A press during
// closing reopens the door; that only ever happens on an explicit "open".
static void door_close(void)
{
    if (door == DOOR_CLOSED || door == DOOR_CLOSING) {
        return;
    }
    esp_timer_stop(travel_timer);
    esp_timer_stop(autoclose_timer);
    publish_door(DOOR_CLOSING, DOOR_CLOSED);
    esp_timer_start_once(travel_timer, DOOR_TRAVEL_MS * 1000ULL);
}

static void travel_done(void)
{
    if (door == DOOR_OPENING) {
        publish_door(DOOR_OPEN, DOOR_OPEN);
        esp_timer_start_once(autoclose_timer, DOOR_AUTOCLOSE_MS * 1000ULL);
    } else if (door == DOOR_CLOSING) {
        publish_door(DOOR_CLOSED, DOOR_CLOSED);
    }
}

static const char *ctrl(void *priv)
{
    const char *id = priv ? hap_req_get_ctrl_id(priv) : NULL;
    return id ? id : "?";
}

static int door_write(hap_write_data_t w[], int count, void *serv_priv, void *write_priv)
{
    for (int i = 0; i < count; i++) {
        if (!strcmp(hap_char_get_type_uuid(w[i].hc), HAP_CHAR_UUID_TARGET_DOOR_STATE)) {
            ESP_LOGI(TAG, "door %s from %.8s", w[i].val.u == DOOR_OPEN ? "open" : "close",
                     ctrl(write_priv));
            post(w[i].val.u == DOOR_OPEN ? EV_TARGET_OPEN : EV_TARGET_CLOSED, 0);
            hap_char_update_val(w[i].hc, &w[i].val);
            *(w[i].status) = HAP_STATUS_SUCCESS;
        } else {
            *(w[i].status) = HAP_STATUS_RES_ABSENT;
        }
    }
    return HAP_SUCCESS;
}

static int charger_write(hap_write_data_t w[], int count, void *serv_priv, void *write_priv)
{
    for (int i = 0; i < count; i++) {
        if (!strcmp(hap_char_get_type_uuid(w[i].hc), HAP_CHAR_UUID_ON)) {
            ESP_LOGI(TAG, "charger %d from %.8s", w[i].val.b, ctrl(write_priv));
            post(EV_CHARGER_SET, w[i].val.b);
            hap_char_update_val(w[i].hc, &w[i].val);
            *(w[i].status) = HAP_STATUS_SUCCESS;
        } else {
            *(w[i].status) = HAP_STATUS_RES_ABSENT;
        }
    }
    return HAP_SUCCESS;
}

static int identify(hap_acc_t *ha)
{
    ESP_LOGI(TAG, "identify");
    return HAP_SUCCESS;
}

static void on_button(btn_event_t ev)
{
    switch (ev) {
    case BTN_SHORT:      post(EV_BUTTON_SHORT, 0); break;
    case BTN_DOUBLE:     post(EV_CHARGER_TOGGLE, 0); break;
    case BTN_RESTART:    post(EV_RESTART, 0); break;
    case BTN_RESET_PAIR: post(EV_RESET_PAIRINGS, 0); break;
    }
}

static void on_net(bool up) { post(up ? EV_NET_UP : EV_NET_DOWN, 0); }

// Connection churn is the first thing to read when Home says "No response".
static void on_hap_event(hap_event_t ev, void *data)
{
    switch (ev) {
    case HAP_EVENT_CTRL_CONNECTED:
        if (++sessions > sessions_max) {
            sessions_max = sessions;
        }
        ESP_LOGI(TAG, "hk connect %.8s (%d open)", (char *)data, sessions);
        post(EV_HK_CONNECTED, 0);
        break;
    case HAP_EVENT_CTRL_DISCONNECTED:
        if (sessions > 0) {
            sessions--;
        }
        ESP_LOGI(TAG, "hk disconnect %.8s (%d open)", (char *)data, sessions);
        break;
    case HAP_EVENT_CTRL_PAIRED:   ESP_LOGW(TAG, "hk paired %.8s", (char *)data); break;
    case HAP_EVENT_CTRL_UNPAIRED: ESP_LOGW(TAG, "hk unpaired %.8s", (char *)data); break;
    default: break;
    }
}

static int status(char *buf, size_t len)
{
    static const char *door_names[] = { "open", "closed", "opening", "closing" };
    float t = io_read_temperature_c();
    return snprintf(buf, len,
                    "version %s\nuptime_s %lld\nreset_reason %d\nheap_free %u\nheap_min %u\n"
                    "hk_sessions %d\nhk_sessions_max %d\nhk_controllers %d\npulses %u\n"
                    "door %s\ncharger %d\ntemp_c %.1f\n",
                    esp_app_get_description()->version, esp_timer_get_time() / 1000000,
                    esp_reset_reason(), (unsigned)esp_get_free_heap_size(),
                    (unsigned)esp_get_minimum_free_heap_size(), sessions, sessions_max,
                    hap_running ? hap_get_paired_controller_count() : -1, pulses,
                    door_names[door], charger, t);
}

static void hap_setup(void)
{
    uint8_t mac[6];
    char serial[18];
    net_get_mac(mac);
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    hap_cfg_t hcfg;
    hap_get_config(&hcfg);
    hcfg.unique_param = UNIQUE_NONE;  // the SDK's UNIQUE_NAME reads the Wi-Fi MAC
    hap_set_config(&hcfg);

    hap_init(HAP_TRANSPORT_ETHERNET);
    hap_register_event_handler(on_hap_event);
#if !CONFIG_GARAGE_QEMU
    hap_set_debug_level(HAP_DEBUG_LEVEL_WARN);
#endif
    hap_acc_cfg_t cfg = {
        .name = "Garage",
        .manufacturer = "Shelly (firmware maison)",
        .model = "Pro 1",
        .serial_num = serial,
        .fw_rev = (char *)esp_app_get_description()->version,
        .hw_rev = NULL,
        .pv = "1.1.0",
        .identify_routine = identify,
        .cid = HAP_CID_GARAGE_DOOR_OPENER,
    };
    hap_acc_t *acc = hap_acc_create(&cfg);

    hap_serv_t *s = hap_serv_garage_door_opener_create(DOOR_CLOSED, DOOR_CLOSED, false);
    hap_serv_add_char(s, hap_char_name_create("Porte Garage"));
    hc_cur = hap_serv_get_char_by_uuid(s, HAP_CHAR_UUID_CURRENT_DOOR_STATE);
    hc_tgt = hap_serv_get_char_by_uuid(s, HAP_CHAR_UUID_TARGET_DOOR_STATE);
    hap_serv_set_write_cb(s, door_write);
    hap_acc_add_serv(acc, s);

    s = hap_serv_switch_create(charger);
    hap_serv_add_char(s, hap_char_name_create("Chargeur Voiture"));
    hc_charger = hap_serv_get_char_by_uuid(s, HAP_CHAR_UUID_ON);
    hap_serv_set_write_cb(s, charger_write);
    hap_acc_add_serv(acc, s);

    hap_add_accessory(acc);
    hap_start();
    mdns_hostname_set(HOSTNAME);  // hap_start sets a placeholder
    hap_running = true;
}

static void confirm_image(void)
{
#ifdef GARAGE_NO_CONFIRM
    return;
#endif
    static bool done;
    if (done) {
        return;
    }
    done = true;
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "image confirmed");
    }
    esp_timer_stop(rollback_timer);
    // Only from an image proven good: a bootloader written by a bad one would outlive it.
    for (int i = 0; i < 3 && !bootloader_update(); i++) {
    }
}

static void rollback_cb(void *arg)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "no HomeKit controller after update: rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

static void make_timer(esp_timer_handle_t *t, esp_timer_cb_t cb, void *arg, const char *name)
{
    esp_timer_create_args_t a = { .callback = cb, .arg = arg, .name = name };
    ESP_ERROR_CHECK(esp_timer_create(&a, t));
}

void app_main(void)
{
    diag_init();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    q = xQueueCreate(16, sizeof(ev_t));

    charger = load_charger();
    io_init(charger, on_button);
    io_set_led(SR_BIT_LED_RED, true);  // red until the network is up

    make_timer(&travel_timer, timer_cb, (void *)EV_TRAVEL_DONE, "travel");
    make_timer(&autoclose_timer, timer_cb, (void *)EV_AUTOCLOSE, "autoclose");
    make_timer(&tick_timer, timer_cb, (void *)EV_TICK, "tick");
    make_timer(&rollback_timer, rollback_cb, NULL, "rollback");
    esp_timer_start_periodic(tick_timer, 10 * 1000 * 1000ULL);
    esp_timer_start_once(rollback_timer, ROLLBACK_DEADLINE_MS * 1000ULL);

    setup_init();
    net_init(HOSTNAME, on_net);
    ESP_LOGI(TAG, "version %s", esp_app_get_description()->version);

    for (;;) {
        ev_t e;
        if (xQueueReceive(q, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (e.type) {
        case EV_TARGET_OPEN:   door_open(); break;
        case EV_TARGET_CLOSED: door_close(); break;
        case EV_BUTTON_SHORT:  door_open(); break;
        case EV_TRAVEL_DONE:   travel_done(); break;
        case EV_AUTOCLOSE:     door_close(); break;
        case EV_CHARGER_SET:   set_charger(e.arg); break;
        case EV_CHARGER_TOGGLE: set_charger(!charger); break;
        case EV_NET_UP:
            io_set_led(SR_BIT_LED_RED, false);
            io_set_led(SR_BIT_LED_BLUE, true);
            if (!hap_running) {
                diag_register(ota_start(), status);
                hap_setup();
            }
            if (hap_get_paired_controller_count() == 0) {
                confirm_image();  // nobody to wait for
            }
            break;
        case EV_HK_CONNECTED:
            confirm_image();
            break;
        case EV_NET_DOWN:
            io_set_led(SR_BIT_LED_BLUE, false);
            io_set_led(SR_BIT_LED_RED, true);
            break;
        case EV_RESTART:       esp_restart(); break;
        case EV_RESET_PAIRINGS:
            ESP_LOGW(TAG, "resetting HomeKit pairings");
            hap_reset_pairings();  // reboots
            break;
        case EV_TICK: {
            float t = io_read_temperature_c();
            if (!isnan(t) && t > CHARGER_CUTOFF_C && charger) {
                ESP_LOGW(TAG, "%.1f °C: charger off", t);
                set_charger(false);
            }
            break;
        }
        }
    }
}
