#include "email_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"

#define RESPONSE_MAX 8192

typedef struct {
    char data[RESPONSE_MAX + 1];
    size_t used;
    bool overflow;
    bool connected, headers_sent, finished;
} response_t;

// Requests run serially in the firmware main task; this survives client cleanup.
static char detailed_error[512];

static esp_err_t receive_response(esp_http_client_event_t *event)
{
    response_t *r = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) r->connected = true;
    if (event->event_id == HTTP_EVENT_HEADERS_SENT) r->headers_sent = true;
    if (event->event_id == HTTP_EVENT_ON_FINISH) r->finished = true;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t count = (size_t)event->data_len;
        if (count > RESPONSE_MAX - r->used) {
            r->overflow = true;
            return ESP_FAIL;
        }
        memcpy(r->data + r->used, event->data, count);
        r->used += count;
        r->data[r->used] = 0;
    }
    return ESP_OK;
}

static bool mailbox(const char *s)
{
    size_t n = strlen(s);
    if (n < 3 || n > 254 || !strchr(s, '@') || s[0] == '@' || s[n - 1] == '@') return false;
    if (strchr(strchr(s, '@') + 1, '@')) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || strchr("@._+-", c))) return false;
    }
    return true;
}

static bool valid_key(const char *s, bool username)
{
    size_t n = strlen(s);
    if (!n || n > 128) return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 33 || (unsigned char)s[i] > 126 || (username && s[i] == ':')) return false;
    return true;
}

bool email_sync_clock(void)
{
    if (time(NULL) >= 1735689600) return true;
    static bool started;
    if (!started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        started = true;
    }
    for (int i = 0; i < 150 && time(NULL) < 1735689600; i++) vTaskDelay(pdMS_TO_TICKS(100));
    return time(NULL) >= 1735689600;
}

static const char *send_email(cJSON *request, const char *label, int raw, bool still_wet)
{
    cJSON *sender = cJSON_GetObjectItemCaseSensitive(request, "sender");
    cJSON *recipient = cJSON_GetObjectItemCaseSensitive(request, "recipient");
    cJSON *api_key = cJSON_GetObjectItemCaseSensitive(request, "api_key");
    cJSON *secret_key = cJSON_GetObjectItemCaseSensitive(request, "secret_key");
    bool sandbox = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "sandbox"));
    bool button_test = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "button_test"));
    if (!cJSON_IsString(sender) || !cJSON_IsString(recipient) ||
        !mailbox(sender->valuestring) || !mailbox(recipient->valuestring))
        return "Use plain sender and recipient email addresses without display names.";
    if (!cJSON_IsString(api_key) || !cJSON_IsString(secret_key) ||
        !valid_key(api_key->valuestring, true) || !valid_key(secret_key->valuestring, false))
        return "A Mailjet API key and secret key are required (1-128 printable characters).";
    if (!email_sync_clock()) return "Could not set the clock using NTP. Check internet access; HTTPS was not attempted.";

    const char *error = "Not enough memory to prepare Mailjet request.";
    cJSON *payload = cJSON_CreateObject();
    char *body = NULL;
    response_t *response = calloc(1, sizeof(*response));
    esp_http_client_handle_t client = NULL;
    char credentials[258] = {0};
    unsigned char authorization[360] = {0};
    cJSON *reply = NULL;
    if (!payload || !response) goto done;
    cJSON_AddBoolToObject(payload, "SandboxMode", sandbox);
    cJSON *messages = cJSON_AddArrayToObject(payload, "Messages");
    cJSON *message = cJSON_CreateObject();
    if (!messages || !message) { cJSON_Delete(message); goto done; }
    cJSON_AddItemToArray(messages, message);
    cJSON *from = cJSON_AddObjectToObject(message, "From");
    cJSON *to = cJSON_AddArrayToObject(message, "To");
    cJSON *address = cJSON_CreateObject();
    if (!from || !to || !address) { cJSON_Delete(address); goto done; }
    cJSON_AddStringToObject(from, "Email", sender->valuestring);
    cJSON_AddItemToArray(to, address);
    cJSON_AddStringToObject(address, "Email", recipient->valuestring);
    cJSON_AddStringToObject(message, "Subject", button_test ? "TEST - ESP32 water alert" :
                           label ? "Water detected - ESP32 alert" : "ESP32 Wi-Fi email test");
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char text[300];
    snprintf(text, sizeof(text),
             "This test was sent directly by your ESP32 over Wi-Fi using Mailjet HTTPS.\n"
             "Board MAC: %02x:%02x:%02x:%02x:%02x:%02x\n"
             "The PC supplied test settings over USB; the ESP32 made the API request.",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (label) snprintf(text, sizeof(text),
        "Water detected at: %s\nCurrent condition: %s\nSensor raw reading: %d\n"
        "Board: %02x:%02x:%02x:%02x:%02x:%02x\nPlease inspect the area. "
        "This is an experimental sensor notification.", label,
        still_wet ? "still wet" : "dry now; water was detected earlier", raw,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (button_test) snprintf(text, sizeof(text),
        "Manual button test at: %s\nThis email was requested by pressing the test button.\n"
        "Current sensor state: %s\nSensor raw reading: %d\n"
        "Board: %02x:%02x:%02x:%02x:%02x:%02x\nThis is a test notification.",
        label ? label : "Water sensor", raw < 0 ? "not monitoring" : still_wet ? "wet" : "dry",
        raw, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(message, "TextPart", text);
    body = cJSON_PrintUnformatted(payload);
    if (!body) goto done;

    snprintf(credentials, sizeof(credentials), "%s:%s", api_key->valuestring, secret_key->valuestring);
    memcpy(authorization, "Basic ", 6);
    size_t encoded_size;
    if (mbedtls_base64_encode(authorization + 6, sizeof(authorization) - 7, &encoded_size,
                             (unsigned char *)credentials, strlen(credentials)) != 0) goto done;
    authorization[6 + encoded_size] = 0;
    esp_http_client_config_t config = {
        .url = "https://api.mailjet.com/v3.1/send",
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .disable_auto_redirect = true,
        .event_handler = receive_response,
        .user_data = response,
    };
    client = esp_http_client_init(&config);
    if (!client) goto done;
    error = "Could not prepare Mailjet HTTPS headers.";
    if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
        esp_http_client_set_header(client, "Authorization", (char *)authorization) != ESP_OK ||
        esp_http_client_set_post_field(client, body, strlen(body)) != ESP_OK) goto done;
    // One request, no redirects or automatic application retries. Keys stay in RAM.
    esp_err_t result_code = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (result_code != ESP_OK || response->overflow) {
        int socket_error = esp_http_client_get_errno(client);
        int tls_code = 0, cert_flags = 0;
        esp_err_t tls_error = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &cert_flags);
        snprintf(detailed_error, sizeof(detailed_error),
            "Mailjet HTTPS failed: %s (0x%x); stage=%s; socket_errno=%d; "
            "TLS=%s (0x%x); mbedTLS=%d; cert_flags=0x%x; HTTP=%d; response_bytes=%u; overflow=%d. %s",
            esp_err_to_name(result_code), (unsigned)result_code,
            response->finished ? "response complete" : response->headers_sent ? "request/response" :
            response->connected ? "sending request" : "DNS/TCP/TLS connection",
            socket_error, esp_err_to_name(tls_error), (unsigned)tls_error, tls_code,
            (unsigned)cert_flags, status, (unsigned)response->used, response->overflow,
            response->headers_sent ? "Delivery unconfirmed; check Mailjet activity before another press." :
            "Request headers were not sent; no Mailjet acceptance confirmed.");
        error = detailed_error;
        goto done;
    }
    if (status == 401) { error = "Mailjet authentication failed. Check the API key and secret key."; goto done; }
    if (status == 403) { error = "Mailjet denied access. Check key permissions and sender verification."; goto done; }
    if (status == 429) { error = "Mailjet rate limit reached. Wait before trying again."; goto done; }
    if (status < 200 || status >= 300) {
        snprintf(detailed_error, sizeof(detailed_error),
            "Mailjet returned HTTP %d (%u response bytes). Check sender verification, account limits and Mailjet activity.",
            status, (unsigned)response->used);
        error = detailed_error;
        goto done;
    }
    reply = cJSON_ParseWithLength(response->data, response->used + 1);
    if (!reply) {
        snprintf(detailed_error, sizeof(detailed_error), "Mailjet HTTP %d response was not valid JSON (%u bytes); delivery unconfirmed.",
                 status, (unsigned)response->used);
        error = detailed_error;
        goto done;
    }
    cJSON *results = cJSON_GetObjectItemCaseSensitive(reply, "Messages");
    cJSON *result = cJSON_GetArrayItem(results, 0);
    cJSON *message_status = cJSON_GetObjectItemCaseSensitive(result, "Status");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) != 1 ||
        !cJSON_IsString(message_status) || strcmp(message_status->valuestring, "success") != 0) {
        error = "Mailjet did not confirm message success. Check that the sender is verified and review Mailjet activity.";
        goto done;
    }
    error = NULL;
done:
    if (client) {
        esp_http_client_delete_header(client, "Authorization");
        esp_http_client_cleanup(client);
    }
    mbedtls_platform_zeroize(credentials, sizeof(credentials));
    mbedtls_platform_zeroize(authorization, sizeof(authorization));
    if (response) { mbedtls_platform_zeroize(response, sizeof(*response)); free(response); }
    cJSON_Delete(reply);
    free(body);
    cJSON_Delete(payload);
    return error;
}

const char *send_test_email(cJSON *request) { return send_email(request, NULL, 0, false); }
const char *send_water_email(cJSON *request, const char *label, int raw, bool still_wet)
{
    return send_email(request, label, raw, still_wet);
}
