#include "bmc_net.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "bmc_net";

static volatile bool s_ready;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_AP_START:
        s_ready = true;
        ESP_LOGI(TAG, "SoftAP \"%s\" up, BMC at 192.168.4.1", CONFIG_BMC_NET_SSID);
        break;
    case WIFI_EVENT_AP_STOP:
        s_ready = false;
        ESP_LOGW(TAG, "SoftAP stopped");
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "client " MACSTR " joined (aid %d)", MAC2STR(e->mac), e->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "client " MACSTR " left (aid %d)", MAC2STR(e->mac), e->aid);
        break;
    }
    default:
        break;
    }
}

esp_err_t bmc_net_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();    /* DHCP server, 192.168.4.1/24 */

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.ap.ssid, CONFIG_BMC_NET_SSID, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(CONFIG_BMC_NET_SSID);
    cfg.ap.channel = CONFIG_BMC_NET_CHANNEL;
    cfg.ap.max_connection = CONFIG_BMC_NET_MAX_STATIONS;
    if (strlen(CONFIG_BMC_NET_PASSWORD) >= 8) {
        strlcpy((char *)cfg.ap.password, CONFIG_BMC_NET_PASSWORD, sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        if (strlen(CONFIG_BMC_NET_PASSWORD) > 0) {
            ESP_LOGW(TAG, "password shorter than 8 characters, starting an OPEN network");
        }
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

bool bmc_net_ready(void)
{
    return s_ready;
}
