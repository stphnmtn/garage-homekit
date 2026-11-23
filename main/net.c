#include <stdint.h>
#include "net.h"
#include "board.h"

#include <esp_eth.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>

static const char *TAG = "net";
static net_cb_t net_cb;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "ip " IPSTR, IP2STR(&ev->ip_info.ip));
        net_cb(true);
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "link down");
        net_cb(false);
    }
}

void net_get_mac(uint8_t mac[6]) { esp_read_mac(mac, ESP_MAC_ETH); }

void net_init(const char *hostname, net_cb_t cb)
{
    net_cb = cb;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);
    ESP_ERROR_CHECK(esp_netif_set_hostname(netif, hostname));

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = -1;
#if CONFIG_GARAGE_QEMU
    phy_cfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_cfg);
#else
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = PIN_ETH_MDC;
    emac.smi_gpio.mdio_num = PIN_ETH_MDIO;
    emac.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    emac.clock_config.rmii.clock_gpio = EMAC_CLK_OUT_180_GPIO;  // GPIO17
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_cfg);
    phy_cfg.phy_addr = ETH_PHY_ADDR;
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);
#endif

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&cfg, &eth));
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, on_event, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}
