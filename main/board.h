#pragma once

// Shelly Pro 1, mapping taken 1:1 from the ESPHome config `interrupteur-garage.yaml`
// (2026-11-20). Change a value here only after checking it against that file or the board.

// Ethernet: LAN8720, PHY address 0 (ESPHome default), RMII clock output on GPIO17.
#define PIN_ETH_MDC        23
#define PIN_ETH_MDIO       18
#define ETH_PHY_ADDR       0

// 74HC595 shift register (one chip): data, clock, latch.
#define PIN_SR_DATA        13
#define PIN_SR_CLOCK       14
#define PIN_SR_LATCH       4
#define SR_BIT_CHARGER     0   // relay "Chargeur Voiture", active high
#define SR_BIT_LED_BLUE    2   // active low
#define SR_BIT_LED_GREEN   3   // active low
#define SR_BIT_LED_RED     4   // active low

// Door opener pulse output, active low (ESPHome: GPIO2 inverted, 500 ms).
#define PIN_DOOR           2
#define DOOR_PULSE_MS      500

// Front button, active low, input-only pin.
#define PIN_BUTTON         35

// Internal NTC on GPIO36 = ADC1 channel 0, 11/12 dB attenuation.
// Divider: 10 kΩ to 3.3 V on top, NTC to ground below (ESPHome "DOWNSTREAM").
#define NTC_ADC_CHANNEL    ADC_CHANNEL_0
#define NTC_SERIES_OHM     10000.0f
#define NTC_R0_OHM         10000.0f
#define NTC_T0_K           298.15f
#define NTC_B              3350.0f
#define NTC_VREF_MV        3300.0f
#define CHARGER_CUTOFF_C   80.0f

// Door model (ESPHome time_based cover, no position sensor): 15 s travel each way; the
// door closes on its own, modelled 32 s after it is fully open.
#define DOOR_TRAVEL_MS     15000
#define DOOR_AUTOCLOSE_MS  32000
