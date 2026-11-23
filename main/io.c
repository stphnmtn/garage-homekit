#include <sdkconfig.h>
#include "io.h"
#include "board.h"

#include <math.h>
#include <stdint.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "io";

static uint8_t sr_state;
static portMUX_TYPE sr_lock = portMUX_INITIALIZER_UNLOCKED;
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t adc_cali;
static btn_cb_t btn_cb;

static void sr_write(void)
{
    // MSB first, as ESPHome's sn74hc595 does: bit 7 ends on Q7, bit 0 on Q0.
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(PIN_SR_DATA, (sr_state >> i) & 1);
        gpio_set_level(PIN_SR_CLOCK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_SR_CLOCK, 0);
    }
    gpio_set_level(PIN_SR_LATCH, 1);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_SR_LATCH, 0);
}

static void sr_set_bit(int bit, bool level)
{
    taskENTER_CRITICAL(&sr_lock);
    if (level) {
        sr_state |= (1u << bit);
    } else {
        sr_state &= ~(1u << bit);
    }
    sr_write();
    taskEXIT_CRITICAL(&sr_lock);
}

void io_set_charger(bool on) { sr_set_bit(SR_BIT_CHARGER, on); }

void io_set_led(int sr_bit, bool on) { sr_set_bit(sr_bit, !on); }

void io_door_pulse(void)
{
    ESP_LOGI(TAG, "door pulse");
    io_set_led(SR_BIT_LED_GREEN, true);
    gpio_set_level(PIN_DOOR, 0);
    vTaskDelay(pdMS_TO_TICKS(DOOR_PULSE_MS));
    gpio_set_level(PIN_DOOR, 1);
    io_set_led(SR_BIT_LED_GREEN, false);
}

float io_read_temperature_c(void)
{
    int raw, mv;
    if (!adc || adc_oneshot_read(adc, NTC_ADC_CHANNEL, &raw) != ESP_OK) {
        return NAN;
    }
    if (!adc_cali || adc_cali_raw_to_voltage(adc_cali, raw, &mv) != ESP_OK) {
        mv = raw * 3100 / 4095;  // uncalibrated: 12 dB range tops out near 3.1 V
    }
    float z = mv / NTC_VREF_MV;
    if (z <= 0.0f || z >= 1.0f) {
        return NAN;
    }
    float r = NTC_SERIES_OHM * z / (1.0f - z);
    float inv_t = 1.0f / NTC_T0_K + logf(r / NTC_R0_OHM) / NTC_B;
    return 1.0f / inv_t - 273.15f;
}

// Polls every 10 ms; press lengths and the double click follow the ESPHome config.
static void button_task(void *arg)
{
    bool was_down = false;
    int64_t down_ms = 0, up_ms = 0;
    int pending_short = 0;  // 1 = one quick press waiting for a possible second
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int64_t now = esp_log_timestamp();
        bool down = gpio_get_level(PIN_BUTTON) == 0;
        if (down && !was_down) {
            down_ms = now;
        } else if (!down && was_down) {
            int64_t len = now - down_ms;
            up_ms = now;
            if (len < 350 && pending_short) {
                pending_short = 0;
                btn_cb(BTN_DOUBLE);
            } else if (len < 350) {
                pending_short = 1;
                if (len < 200) {
                    pending_short = 2;  // too short for a click, only valid as half a double
                }
            } else if (len <= 3000) {
                btn_cb(BTN_SHORT);
            } else if (len >= 4000 && len <= 8000) {
                btn_cb(BTN_RESTART);
            } else if (len >= 10000 && len <= 15000) {
                btn_cb(BTN_RESET_PAIR);
            }
        }
        if (pending_short && !down && now - up_ms > 500) {
            if (pending_short == 1) {
                btn_cb(BTN_SHORT);
            }
            pending_short = 0;
        }
        was_down = down;
    }
}

void io_init(bool charger_on, btn_cb_t cb)
{
    btn_cb = cb;

    // Door output idle (high) before it becomes an output, so boot never pulses the opener.
    gpio_set_level(PIN_DOOR, 1);
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_DOOR) | (1ULL << PIN_SR_DATA) |
                        (1ULL << PIN_SR_CLOCK) | (1ULL << PIN_SR_LATCH),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(PIN_DOOR, 1);

    sr_state = (1u << SR_BIT_LED_BLUE) | (1u << SR_BIT_LED_GREEN) | (1u << SR_BIT_LED_RED);
    if (charger_on) {
        sr_state |= (1u << SR_BIT_CHARGER);
    }
    sr_write();

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

#if CONFIG_GARAGE_QEMU
    // QEMU does not emulate the SAR ADC: a conversion never completes and stalls the CPU.
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
    ESP_LOGW(TAG, "QEMU build: no ADC, no temperature cutoff");
    return;
#endif
    // A sensor fault must not stop the door: failures only disable or degrade the reading.
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_new_unit(&unit, &adc) != ESP_OK ||
        adc_oneshot_config_channel(adc, NTC_ADC_CHANNEL, &ch) != ESP_OK) {
        ESP_LOGE(TAG, "ADC init failed: no temperature cutoff");
        adc = NULL;
    }
    adc_cali_line_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cal, &adc_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable: using raw scaling");
        adc_cali = NULL;
    }

    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "io ready, charger %s", charger_on ? "on" : "off");
}
