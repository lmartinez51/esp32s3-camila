/* Network

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <string.h>
#include <sys/param.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "network.h"
#include "network_storage.h"
#include "wifi_session_state.h"

#ifdef CONFIG_NETWORK_USE_ETHERNET
#include "esp_eth.h"
#include "ethernet_init.h"
#else
#include <esp_wifi.h>
#endif

#define TAG "NETWORK"
#define WIFI_START_TIMEOUT_MS 5000
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_CONNECT_RETRY_COUNT 3
#define WIFI_MAX_CREDS 4
#define WIFI_MAX_SSID_LEN 32
#define WIFI_MAX_PASSWORD_LEN 64

static bool network_connected = false;
static network_connect_cb connect_cb;

static void network_set_connected(bool connected)
{
    if (network_connected != connected)
    {
        network_connected = connected;
        if (connect_cb)
        {
            connect_cb(connected);
        }
    }
    else
    {
        ESP_LOGI(TAG, "Estado de red ya era: %s", connected ? "Conectado" : "Desconectado");
    }
}

bool network_is_connected(void)
{
    return network_connected;
}

#ifndef CONFIG_NETWORK_USE_ETHERNET

enum
{
    WIFI_EVT_STARTED = BIT0,
    WIFI_EVT_GOT_IP = BIT1,
    WIFI_EVT_DISCONNECTED = BIT2,
};

typedef struct
{
    char ssid[WIFI_MAX_SSID_LEN + 1];
    char password[WIFI_MAX_PASSWORD_LEN + 1];
} wifi_cred_t;

static EventGroupHandle_t s_wifi_events = NULL;
static bool s_wifi_started = false;
static bool s_auto_reconnect_enabled = false;
static bool s_manual_disconnect = false;
static char s_attempt_ssid[WIFI_MAX_SSID_LEN + 1] = "";
static int s_attempt_credential_index = -1;
static int s_attempt_number = 0;
static int64_t s_attempt_start_us = 0;

static const char *wifi_reason_to_string(uint8_t reason)
{
    switch ((wifi_err_reason_t)reason)
    {
    case WIFI_REASON_UNSPECIFIED:
        return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:
        return "AUTH_LEAVE";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        return "DISASSOC_DUE_TO_INACTIVITY";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "ASSOC_TOOMANY";
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA:
        return "CLASS2_FRAME_FROM_NONAUTH_STA";
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA:
        return "CLASS3_FRAME_FROM_NONASSOC_STA";
    case WIFI_REASON_ASSOC_LEAVE:
        return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:
        return "DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
        return "DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_BSS_TRANSITION_DISASSOC:
        return "BSS_TRANSITION_DISASSOC";
    case WIFI_REASON_IE_INVALID:
        return "IE_INVALID";
    case WIFI_REASON_MIC_FAILURE:
        return "MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        return "IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_GROUP_CIPHER_INVALID:
        return "GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        return "PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_AKMP_INVALID:
        return "AKMP_INVALID";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        return "UNSUPP_RSN_IE_VERSION";
    case WIFI_REASON_INVALID_RSN_IE_CAP:
        return "INVALID_RSN_IE_CAP";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "802_1X_AUTH_FAILED";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
        return "CIPHER_SUITE_REJECTED";
    case WIFI_REASON_TIMEOUT:
        return "TIMEOUT";
    case WIFI_REASON_PEER_INITIATED:
        return "PEER_INITIATED";
    case WIFI_REASON_AP_INITIATED:
        return "AP_INITIATED";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET:
        return "AP_TSF_RESET";
    case WIFI_REASON_ROAMING:
        return "ROAMING";
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG:
        return "ASSOC_COMEBACK_TIME_TOO_LONG";
    case WIFI_REASON_SA_QUERY_TIMEOUT:
        return "SA_QUERY_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:
        return "UNKNOWN";
    }
}

static const char *wifi_authmode_to_string(wifi_auth_mode_t authmode)
{
    switch (authmode)
    {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI-PSK";
    case WIFI_AUTH_OWE:
        return "OWE";
    default:
        return "UNKNOWN";
    }
}

static void set_attempt_context(const char *ssid, int credential_index, int attempt_number)
{
    strlcpy(s_attempt_ssid, ssid ? ssid : "", sizeof(s_attempt_ssid));
    s_attempt_credential_index = credential_index;
    s_attempt_number = attempt_number;
    s_attempt_start_us = esp_timer_get_time();
}

static int64_t current_attempt_elapsed_ms(void)
{
    if (s_attempt_start_us == 0)
    {
        return 0;
    }
    return (esp_timer_get_time() - s_attempt_start_us) / 1000;
}

static esp_err_t configure_sta(const wifi_cred_t *cred)
{
    wifi_config_t wifi_config = {0};

    strlcpy((char *)wifi_config.sta.ssid, cred->ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, cred->password, sizeof(wifi_config.sta.password));
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.channel = 0;
    wifi_config.sta.bssid_set = false;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        s_wifi_started = true;
        if (s_wifi_events)
        {
            xEventGroupSetBits(s_wifi_events, WIFI_EVT_STARTED);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP)
    {
        s_wifi_started = false;
        s_auto_reconnect_enabled = false;
        if (s_wifi_events)
        {
            xEventGroupClearBits(s_wifi_events, WIFI_EVT_STARTED | WIFI_EVT_GOT_IP | WIFI_EVT_DISCONNECTED);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        char ssid[WIFI_MAX_SSID_LEN + 1] = {0};

        if (event && event->ssid_len > 0)
        {
            size_t ssid_len = MIN(event->ssid_len, WIFI_MAX_SSID_LEN);
            memcpy(ssid, event->ssid, ssid_len);
        }
        if (ssid[0] == '\0')
        {
            strlcpy(ssid, s_attempt_ssid, sizeof(ssid));
        }

        ESP_LOGW(TAG,
                 "WiFi desconectado: ssid=\"%s\" reason=%u(%s) rssi=%d "
                 "bssid=%02x:%02x:%02x:%02x:%02x:%02x cred=%d intento=%d elapsed=%lldms manual=%d",
                 ssid,
                 event ? event->reason : 0,
                 event ? wifi_reason_to_string(event->reason) : "NO_EVENT_DATA",
                 event ? event->rssi : 0,
                 event ? event->bssid[0] : 0,
                 event ? event->bssid[1] : 0,
                 event ? event->bssid[2] : 0,
                 event ? event->bssid[3] : 0,
                 event ? event->bssid[4] : 0,
                 event ? event->bssid[5] : 0,
                 s_attempt_credential_index,
                 s_attempt_number,
                 current_attempt_elapsed_ms(),
                 s_manual_disconnect);

        bool was_connected = network_connected;
        network_set_connected(false);
        wifi_session_clear_ssid();

        if (s_wifi_events)
        {
            xEventGroupSetBits(s_wifi_events, WIFI_EVT_DISCONNECTED);
        }

        if (was_connected && s_auto_reconnect_enabled && !s_manual_disconnect)
        {
            set_attempt_context(ssid, s_attempt_credential_index, s_attempt_number + 1);
            esp_err_t err = esp_wifi_connect();
            if (err == ESP_OK)
            {
                ESP_LOGW(TAG, "ReconexiÃ³n WiFi automatica iniciada para \"%s\".", ssid);
            }
            else
            {
                ESP_LOGE(TAG, "No se pudo iniciar reconexiÃ³n WiFi: %s", esp_err_to_name(err));
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        wifi_ap_record_t ap_info = {0};
        bool have_ap_info = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
        const char *ssid = have_ap_info ? (const char *)ap_info.ssid : s_attempt_ssid;

        ESP_LOGI(TAG,
                 "WiFi conectado: ssid=\"%s\" ip=" IPSTR
                 " bssid=%02x:%02x:%02x:%02x:%02x:%02x channel=%u auth=%s rssi=%d cred=%d intento=%d elapsed=%lldms",
                 ssid,
                 IP2STR(&event->ip_info.ip),
                 have_ap_info ? ap_info.bssid[0] : 0,
                 have_ap_info ? ap_info.bssid[1] : 0,
                 have_ap_info ? ap_info.bssid[2] : 0,
                 have_ap_info ? ap_info.bssid[3] : 0,
                 have_ap_info ? ap_info.bssid[4] : 0,
                 have_ap_info ? ap_info.bssid[5] : 0,
                 have_ap_info ? ap_info.primary : 0,
                 have_ap_info ? wifi_authmode_to_string(ap_info.authmode) : "UNKNOWN",
                 have_ap_info ? ap_info.rssi : 0,
                 s_attempt_credential_index,
                 s_attempt_number,
                 current_attempt_elapsed_ms());

        wifi_session_set_connected_ssid(ssid);
        network_set_connected(true);
        if (s_wifi_events)
        {
            xEventGroupSetBits(s_wifi_events, WIFI_EVT_GOT_IP);
        }
    }
}

esp_err_t network_wifi_init(network_connect_cb cb)
{
    static bool initialized = false;

    connect_cb = cb;
    if (initialized)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Failed to init netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Failed to create default event loop");
    esp_netif_create_default_wifi_sta();

    s_wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_events != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create WiFi event group");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "Failed to init WiFi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Failed to use RAM WiFi storage");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL), TAG, "Failed to register WIFI event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL), TAG, "Failed to register IP event handler");

    initialized = true;
    return ESP_OK;
}

bool network_wifi_connect_main(const char *default_ssid, const char *default_password)
{
    if (s_wifi_events == NULL)
    {
        ESP_LOGE(TAG, "WiFi no inicializado. Llama network_wifi_init() primero.");
        return false;
    }

    wifi_cred_t creds_to_try[WIFI_MAX_CREDS] = {0};
    int num_creds = 0;

    if (default_ssid && default_password &&
        strlen(default_ssid) > 0 && strlen(default_password) > 0 &&
        num_creds < WIFI_MAX_CREDS)
    {
        strlcpy(creds_to_try[num_creds].ssid, default_ssid, sizeof(creds_to_try[0].ssid));
        strlcpy(creds_to_try[num_creds].password, default_password, sizeof(creds_to_try[0].password));
        num_creds++;
    }

    for (int i = 0; i < 3 && num_creds < WIFI_MAX_CREDS; ++i)
    {
        if (network_get_saved_credentials(i, creds_to_try[num_creds].ssid, creds_to_try[num_creds].password))
        {
            ESP_LOGI(TAG, "Credenciales NVS[%d] encontradas: %s", i, creds_to_try[num_creds].ssid);
            num_creds++;
        }
    }

    if (num_creds == 0)
    {
        ESP_LOGE(TAG, "No hay credenciales WiFi para probar.");
        return false;
    }

    s_auto_reconnect_enabled = false;
    s_manual_disconnect = false;
    xEventGroupClearBits(s_wifi_events, WIFI_EVT_STARTED | WIFI_EVT_GOT_IP | WIFI_EVT_DISCONNECTED);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "No se pudo configurar WiFi STA: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "No se pudo iniciar WiFi: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t start_bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_EVT_STARTED,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_START_TIMEOUT_MS));

    if ((start_bits & WIFI_EVT_STARTED) == 0 && !s_wifi_started)
    {
        ESP_LOGE(TAG, "Timeout esperando WIFI_EVENT_STA_START.");
        esp_wifi_stop();
        return false;
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "No se pudo desactivar WiFi power save: %s", esp_err_to_name(err));
    }

    for (int i = 0; i < num_creds; ++i)
    {
        ESP_LOGI(TAG, "Preparando credencial WiFi[%d]: \"%s\"", i, creds_to_try[i].ssid);
        err = configure_sta(&creds_to_try[i]);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "No se pudo configurar credencial \"%s\": %s",
                     creds_to_try[i].ssid, esp_err_to_name(err));
            continue;
        }

        for (int attempt = 1; attempt <= WIFI_CONNECT_RETRY_COUNT; ++attempt)
        {
            const uint32_t retry_backoff_ms[WIFI_CONNECT_RETRY_COUNT] = {500, 1500, 3000};

            xEventGroupClearBits(s_wifi_events, WIFI_EVT_GOT_IP | WIFI_EVT_DISCONNECTED);
            set_attempt_context(creds_to_try[i].ssid, i, attempt);

            ESP_LOGI(TAG, "Intentando conectar con SSID \"%s\" (cred=%d intento=%d/%d)",
                     creds_to_try[i].ssid, i, attempt, WIFI_CONNECT_RETRY_COUNT);

            err = esp_wifi_connect();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "esp_wifi_connect() fallo para \"%s\": %s",
                         creds_to_try[i].ssid, esp_err_to_name(err));
            }
            else
            {
                EventBits_t bits = xEventGroupWaitBits(
                    s_wifi_events,
                    WIFI_EVT_GOT_IP | WIFI_EVT_DISCONNECTED,
                    pdTRUE,
                    pdFALSE,
                    pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

                if ((bits & WIFI_EVT_GOT_IP) && network_is_connected())
                {
                    ESP_LOGI(TAG, "Conexion WiFi establecida con \"%s\".", creds_to_try[i].ssid);
                    s_auto_reconnect_enabled = true;
                    return true;
                }

                if (bits & WIFI_EVT_DISCONNECTED)
                {
                    ESP_LOGW(TAG, "Intento WiFi fallido para \"%s\" (cred=%d intento=%d/%d).",
                             creds_to_try[i].ssid, i, attempt, WIFI_CONNECT_RETRY_COUNT);
                }
                else
                {
                    ESP_LOGW(TAG, "Timeout de conexion con \"%s\" tras %d ms.",
                             creds_to_try[i].ssid, WIFI_CONNECT_TIMEOUT_MS);
                    s_manual_disconnect = true;
                    esp_wifi_disconnect();
                    xEventGroupWaitBits(s_wifi_events,
                                        WIFI_EVT_DISCONNECTED,
                                        pdTRUE,
                                        pdFALSE,
                                        pdMS_TO_TICKS(1000));
                    s_manual_disconnect = false;
                }
            }

            if (attempt < WIFI_CONNECT_RETRY_COUNT)
            {
                ESP_LOGI(TAG, "Reintentando \"%s\" en %lu ms.",
                         creds_to_try[i].ssid,
                         (unsigned long)retry_backoff_ms[attempt - 1]);
                vTaskDelay(pdMS_TO_TICKS(retry_backoff_ms[attempt - 1]));
            }
        }
    }

    ESP_LOGE(TAG, "No fue posible conectar a ninguna red.");
    s_auto_reconnect_enabled = false;
    s_manual_disconnect = true;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_manual_disconnect = false;
    return false;
}

#else

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id)
    {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        network_set_connected(true);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        network_set_connected(false);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
}

int network_init(const char *ssid, const char *password, network_connect_cb cb)
{
    (void)ssid;
    (void)password;
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(example_eth_init(&eth_handles, &eth_port_cnt));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (eth_port_cnt == 1)
    {
        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
        esp_netif_t *eth_netif = esp_netif_new(&cfg);
        ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));
    }
    else
    {
        esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
        esp_netif_config_t cfg_spi = {
            .base = &esp_netif_config,
            .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
        char if_key_str[10];
        char if_desc_str[10];
        char num_str[3];
        for (int i = 0; i < eth_port_cnt; i++)
        {
            itoa(i, num_str, 10);
            strcat(strcpy(if_key_str, "ETH_"), num_str);
            strcat(strcpy(if_desc_str, "eth"), num_str);
            esp_netif_config.if_key = if_key_str;
            esp_netif_config.if_desc = if_desc_str;
            esp_netif_config.route_prio -= i * 5;
            esp_netif_t *eth_netif = esp_netif_new(&cfg_spi);

            ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[i])));
        }
    }

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
    connect_cb = cb;
    for (int i = 0; i < eth_port_cnt; i++)
    {
        ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
    }
    return 0;
}

int network_connect_wifi(const char *ssid, const char *password)
{
    (void)ssid;
    (void)password;
    ESP_LOGE(TAG, "Using ethernet now not support wifi config");
    return 0;
}

#endif
