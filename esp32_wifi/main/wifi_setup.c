#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_flash_encrypt.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_secure_boot.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "email_test.h"
#include "water_alarm.h"

#ifndef CONFIG_WIFI_SETUP_DEVELOPMENT
#define CONFIG_WIFI_SETUP_DEVELOPMENT 0
#endif

#if CONFIG_WIFI_SETUP_DEVELOPMENT && (CONFIG_SECURE_BOOT || CONFIG_SECURE_FLASH_ENC_ENABLED || CONFIG_NVS_ENCRYPTION)
#error "Development firmware must not enable permanent security or encrypted NVS."
#endif

#define GOT_IP BIT0
#define MAX_NETWORKS 32
#define LINE_SIZE 2048
#define UNLOCK_US (300LL * 1000000)

typedef struct {
    char ssid[33];
    char password[64];
} credentials_t;

static EventGroupHandle_t events;
static esp_netif_t *netif;
static nvs_handle_t storage;
static bool secure_storage;
static bool storage_ready;
static bool have_saved;
static volatile bool changing;
static credentials_t saved;
static int64_t unlocked_until;

static void wipe(void *p, size_t size) { mbedtls_platform_zeroize(p, size); }

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(events, GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, GOT_IP);
        // Reconnection is scheduled in the main loop, not a tight retry loop.
    }
}

static cJSON *response(int id, const char *error)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", id);
    cJSON_AddBoolToObject(r, "ok", error == NULL);
    if (error) cJSON_AddStringToObject(r, "error", error);
    return r;
}

static void send_response(cJSON *r)
{
    char *text = cJSON_PrintUnformatted(r);
    if (text) {
        uart_write_bytes(UART_NUM_0, text, strlen(text));
        uart_write_bytes(UART_NUM_0, "\n", 1);
        free(text);
    }
    cJSON_Delete(r);
}

static void add_ip(cJSON *r)
{
    esp_netif_ip_info_t info = {0};
    esp_netif_get_ip_info(netif, &info);
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    cJSON_AddStringToObject(r, "ip", ip);
}

static esp_err_t configure(const credentials_t *credentials)
{
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, credentials->ssid, strnlen(credentials->ssid, 32));
    memcpy(cfg.sta.password, credentials->password, strnlen(credentials->password, 63));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false; // Remain compatible with WPA2 home routers.
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    wipe(&cfg, sizeof(cfg));
    return result;
}

static void pause_connection(void)
{
    changing = true;
    esp_wifi_disconnect();
    // Let pending disconnect/IP events drain before starting a new attempt.
    vTaskDelay(pdMS_TO_TICKS(250));
    xEventGroupClearBits(events, GOT_IP);
}

static void restore_connection(void)
{
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    xEventGroupClearBits(events, GOT_IP);
    if (have_saved && configure(&saved) == ESP_OK) esp_wifi_connect();
    changing = false;
}

static bool supported(wifi_auth_mode_t mode)
{
    return mode == WIFI_AUTH_WPA2_PSK || mode == WIFI_AUTH_WPA2_WPA3_PSK ||
           mode == WIFI_AUTH_WPA3_PSK;
}

static cJSON *scan(int id)
{
    pause_connection();
    wifi_scan_config_t config = {.show_hidden = true};
    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
        restore_connection();
        return response(id, "Scan failed; try again.");
    }
    uint16_t count = MAX_NETWORKS;
    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    if (!records) {
        esp_wifi_clear_ap_list();
        restore_connection();
        return response(id, "Not enough memory for scan.");
    }
    err = esp_wifi_scan_get_ap_records(&count, records);
    cJSON *r = response(id, err == ESP_OK ? NULL : "Could not read scan results.");
    if (err == ESP_OK) {
        cJSON *array = cJSON_AddArrayToObject(r, "networks");
        for (unsigned i = 0; i < count; i++) {
            cJSON *ap = cJSON_CreateObject();
            cJSON_AddStringToObject(ap, "ssid", (const char *)records[i].ssid);
            cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
            cJSON_AddBoolToObject(ap, "supported", supported(records[i].authmode));
            cJSON_AddItemToArray(array, ap);
        }
    }
    free(records);
    restore_connection();
    return r;
}

static cJSON *connect_network(int id, cJSON *request)
{
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(request, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(request, "password");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password))
        return response(id, "Network name and password are required.");
    size_t ssid_length = strlen(ssid->valuestring);
    size_t password_length = strlen(password->valuestring);
    if (ssid_length < 1 || ssid_length > 32 || password_length < 8 || password_length > 63)
        return response(id, "Invalid network name or passphrase length.");
    for (size_t i = 0; i < password_length; i++) {
        unsigned char c = password->valuestring[i];
        if (c < 32 || c > 126) return response(id, "Passphrase must use printable ASCII.");
    }
    credentials_t candidate = {0};
    memcpy(candidate.ssid, ssid->valuestring, ssid_length);
    memcpy(candidate.password, password->valuestring, password_length);
    pause_connection();
    esp_err_t err = configure(&candidate);
    if (err == ESP_OK) err = esp_wifi_connect();
    EventBits_t bits = 0;
    if (err == ESP_OK)
        bits = xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(25000));
    if (!(bits & GOT_IP)) {
        wipe(&candidate, sizeof(candidate));
        restore_connection();
        return response(id, "No Wi-Fi connection/IP within 25 seconds. Check password and signal. Previous network retained.");
    }
    // A single blob replacement keeps the old committed credentials on failed attempts.
    // Wi-Fi driver is RAM-only; only this namespace persists credentials.
    err = nvs_set_blob(storage, "credentials", &candidate, sizeof(candidate));
    if (err == ESP_OK) err = nvs_commit(storage);
    if (err != ESP_OK) {
        wipe(&candidate, sizeof(candidate));
        restore_connection();
        return response(id, "Connected, but storage write failed. New network was not confirmed saved.");
    }
    wipe(&saved, sizeof(saved));
    memcpy(&saved, &candidate, sizeof(saved));
    wipe(&candidate, sizeof(candidate));
    have_saved = true;
    changing = false;
    unlocked_until = 0;
    cJSON *r = response(id, NULL);
    add_ip(r);
    return r;
}

static cJSON *handle(cJSON *request)
{
    cJSON *id_field = cJSON_GetObjectItemCaseSensitive(request, "id");
    int id = cJSON_IsNumber(id_field) ? id_field->valueint : 0;
    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(request, "cmd");
    if (!cJSON_IsString(cmd)) return response(id, "Missing command.");
    if (strcmp(cmd->valuestring, "status") == 0) {
        cJSON *r = response(id, NULL);
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char text[18];
        snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddNumberToObject(r, "protocol", 1);
        cJSON_AddStringToObject(r, "mac", text);
        cJSON_AddBoolToObject(r, "secure_storage", secure_storage);
        cJSON_AddBoolToObject(r, "storage_ready", storage_ready);
        cJSON_AddBoolToObject(r, "development", CONFIG_WIFI_SETUP_DEVELOPMENT);
        cJSON_AddBoolToObject(r, "flash_encryption", esp_flash_encryption_enabled());
        cJSON_AddBoolToObject(r, "secure_boot", esp_secure_boot_enabled());
        cJSON_AddBoolToObject(r, "email_test", true);
        cJSON_AddStringToObject(r, "email_provider", "mailjet");
        cJSON_AddBoolToObject(r, "water_alarm", true);
        cJSON_AddBoolToObject(r, "mailjet_setup", true);
        cJSON_AddBoolToObject(r, "connected", (xEventGroupGetBits(events) & GOT_IP) != 0);
        add_ip(r);
        return r;
    }
    if (strcmp(cmd->valuestring, "lock") == 0) {
        unlocked_until = 0;
        return response(id, NULL);
    }
    if (!storage_ready) return response(id, "Credential storage not ready; credentials refused.");
    if (strcmp(cmd->valuestring, "water_status") == 0) {
        cJSON *r = response(id, NULL);
        cJSON_AddItemToObject(r, "water", water_alarm_status());
        return r;
    }
    if (strcmp(cmd->valuestring, "scan") == 0) return scan(id);
    if (strcmp(cmd->valuestring, "unlock") == 0) {
#if CONFIG_WIFI_SETUP_DEVELOPMENT
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "development_ack")))
            return response(id, "Development mode requires acknowledging unencrypted storage.");
#else
        if (gpio_get_level(GPIO_NUM_0) != 0)
            return response(id, "Hold BOOT while pressing Enter, then run setup again.");
#endif
        unlocked_until = esp_timer_get_time() + UNLOCK_US;
        return response(id, NULL);
    }
    if (esp_timer_get_time() >= unlocked_until)
        return response(id, "Setup locked or timed out. Run setup again and hold BOOT to unlock.");
    if (strcmp(cmd->valuestring, "mailjet_status") == 0) {
        cJSON *r = response(id, NULL);
        cJSON_AddItemToObject(r, "mailjet", mailjet_settings_status());
        return r;
    }
    if (strcmp(cmd->valuestring, "mailjet_save") == 0) {
        const char *error = mailjet_settings_save(request);
        unlocked_until = 0;
        return response(id, error);
    }
    if (strcmp(cmd->valuestring, "water_start") == 0) return response(id, water_alarm_start());
    if (strcmp(cmd->valuestring, "water_configure") == 0) {
        const char *error = water_alarm_configure(request);
        unlocked_until = 0;
        return response(id, error);
    }
    if (strcmp(cmd->valuestring, "water_disable") == 0) return response(id, water_alarm_disable());
    if (strcmp(cmd->valuestring, "connect") == 0) return connect_network(id, request);
    if (strcmp(cmd->valuestring, "forget") == 0) {
        esp_err_t err = nvs_erase_key(storage, "credentials");
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return response(id, "Could not forget network.");
        if (nvs_commit(storage) != ESP_OK) return response(id, "Could not commit network removal.");
        have_saved = false;
        wipe(&saved, sizeof(saved));
        esp_wifi_disconnect();
        wifi_config_t empty = {0};
        esp_wifi_set_config(WIFI_IF_STA, &empty);
        xEventGroupClearBits(events, GOT_IP);
        unlocked_until = 0;
        return response(id, NULL);
    }
    if (strcmp(cmd->valuestring, "send_email") == 0) {
        unlocked_until = 0; // Each send requires a fresh authorization.
        if (!(xEventGroupGetBits(events) & GOT_IP)) return response(id, "Connect Wi-Fi first.");
        const char *error = send_test_email(request);
        cJSON *r = response(id, error);
        if (!error) cJSON_AddStringToObject(r, "result",
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "sandbox")) ?
            "validated_by_mailjet" : "accepted_by_mailjet");
        return r;
    }
    return response(id, "Unknown command.");
}

void app_main(void)
{
    events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    // Fail closed: do not generate/store NVS keys in unprotected flash.
    if (esp_flash_encryption_enabled() && esp_secure_boot_enabled()) {
#if CONFIG_NVS_ENCRYPTION
        esp_err_t err = nvs_flash_init();
        if (err == ESP_OK) err = nvs_open("wifi_setup", NVS_READWRITE, &storage);
        secure_storage = err == ESP_OK;
        storage_ready = secure_storage;
        if (secure_storage) {
            size_t size = sizeof(saved);
            err = nvs_get_blob(storage, "credentials", &saved, &size);
            have_saved = err == ESP_OK && size == sizeof(saved) &&
                         saved.ssid[32] == 0 && saved.password[63] == 0 &&
                         strlen(saved.ssid) > 0 && strlen(saved.password) >= 8;
            if (!have_saved) wipe(&saved, sizeof(saved));
        }
#endif
    }

#if CONFIG_WIFI_SETUP_DEVELOPMENT
    // Explicit test build: normal NVS only; no eFuse or security activation APIs.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("wifi_setup", NVS_READWRITE, &storage));
    storage_ready = true;
    size_t stored_size = sizeof(saved);
    esp_err_t stored_err = nvs_get_blob(storage, "credentials", &saved, &stored_size);
    have_saved = stored_err == ESP_OK && stored_size == sizeof(saved) &&
                 saved.ssid[32] == 0 && saved.password[63] == 0 &&
                 strlen(saved.ssid) > 0 && strlen(saved.password) >= 8;
    if (!have_saved) wipe(&saved, sizeof(saved));
#endif

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (have_saved && configure(&saved) == ESP_OK) esp_wifi_connect();

    gpio_config_t boot = {.pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&boot));
    uart_config_t uart = {.baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
                         .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
                         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0));
    if (storage_ready) water_alarm_init(storage);

    char line[LINE_SIZE];
    size_t used = 0;
    bool overflow = false;
    int64_t retry_at = esp_timer_get_time() + 10000000;
    for (;;) {
        char c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) == 1) {
            if (c == '\n') {
                line[used] = 0;
                cJSON *request = overflow ? NULL : cJSON_Parse(line);
                cJSON *reply = request ? handle(request) : response(0, "Invalid or oversized request.");
                if (request) {
                    const char *secret_fields[] = {"password", "api_key", "secret_key"};
                    for (unsigned i = 0; i < 3; i++) {
                        cJSON *pw = cJSON_GetObjectItemCaseSensitive(request, secret_fields[i]);
                        if (cJSON_IsString(pw)) wipe(pw->valuestring, strlen(pw->valuestring));
                    }
                    cJSON_Delete(request);
                }
                wipe(line, sizeof(line));
                used = 0;
                overflow = false;
                send_response(reply);
            } else if (!overflow) {
                if (used < sizeof(line) - 1) line[used++] = c;
                else { overflow = true; wipe(line, sizeof(line)); used = 0; }
            }
        }
        if (have_saved && !changing && !(xEventGroupGetBits(events) & GOT_IP) &&
            esp_timer_get_time() >= retry_at) {
            esp_wifi_connect();
            retry_at = esp_timer_get_time() + 10000000;
        }
        water_alarm_service((xEventGroupGetBits(events) & GOT_IP) != 0);
    }
}
