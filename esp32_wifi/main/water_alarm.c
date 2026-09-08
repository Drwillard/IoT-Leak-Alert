#include "water_alarm.h"
#include "water_logic.h"
#include "button_logic.h"
#include "email_test.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"

#define POWER_PIN GPIO_NUM_25
#define TEST_BUTTON_PIN GPIO_NUM_0 /* Onboard BOOT button; EN remains reset. */
#define SIGNAL_CHANNEL ADC_CHANNEL_6 /* ADC1 GPIO34: usable with Wi-Fi */

typedef struct {
    uint32_t version;
    int dry;
    int wet;
    int cooldown_seconds;
    char label[49];
    char sender[255];
    char recipient[255];
    char api_key[129];
    char secret_key[129];
} alarm_config_t;

static nvs_handle_t store;
static SemaphoreHandle_t guard;
static adc_oneshot_unit_handle_t adc;
static bool started, monitoring, armed;
static alarm_config_t config;
static alarm_config_t mail_config;
static bool mail_saved;
static bool button_ready, button_pending;
static uint32_t button_presses;

static void button_task(void *unused)
{
    button_logic_t button = {0};
    for (;;) {
        if (button_sample(&button, gpio_get_level(TEST_BUTTON_PIN) == 0, esp_timer_get_time() / 1000)) {
            xSemaphoreTake(guard, portMAX_DELAY);
            button_presses++;
            if (mail_saved || armed) button_pending = true;
            xSemaphoreGive(guard);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static water_logic_t state;
static int raw_value = -1;
static uint32_t acknowledged, attempts, accepted;
static int64_t next_try_us, saved_next_epoch, next_check_us;
static bool restored_throttle;
static const char *last_error;

static void wipe(void *p, size_t n) { mbedtls_platform_zeroize(p, n); }

static void sample_task(void *unused)
{
    for (;;) {
        xSemaphoreTake(guard, portMAX_DELAY);
        bool active = monitoring;
        xSemaphoreGive(guard);
        if (active) {
            gpio_set_level(POWER_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(20));
            int sum = 0, reading = 0;
            bool valid = true;
            for (int i = 0; i < 16; i++) {
                if (adc_oneshot_read(adc, SIGNAL_CHANNEL, &reading) != ESP_OK) { valid = false; break; }
                sum += reading;
            }
            gpio_set_level(POWER_PIN, 0);
            xSemaphoreTake(guard, portMAX_DELAY);
            if (monitoring) {
                raw_value = valid ? sum / 16 : -1;
                water_logic_sample(&state, raw_value, armed ? config.dry : 400,
                                   armed ? config.wet : 700, esp_timer_get_time() / 1000);
            }
            xSemaphoreGive(guard);
        }
        vTaskDelay(pdMS_TO_TICKS(480));
    }
}

const char *water_alarm_start(void)
{
    if (!guard) return "Water alarm storage is unavailable.";
    if (!started) {
        adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
        if (adc_oneshot_new_unit(&unit, &adc) != ESP_OK) return "Could not start ADC1.";
        adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
        if (adc_oneshot_config_channel(adc, SIGNAL_CHANNEL, &channel) != ESP_OK) {
            adc_oneshot_del_unit(adc); adc = NULL;
            return "Could not configure GPIO34.";
        }
        gpio_config_t power = {.pin_bit_mask = 1ULL << POWER_PIN, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&power);
        gpio_set_drive_capability(POWER_PIN, GPIO_DRIVE_CAP_3);
        gpio_set_level(POWER_PIN, 0);
        if (xTaskCreate(sample_task, "water_sample", 4096, NULL, 5, NULL) != pdPASS) {
            adc_oneshot_del_unit(adc); adc = NULL;
            return "Could not start sensor task.";
        }
        started = true;
    }
    xSemaphoreTake(guard, portMAX_DELAY);
    monitoring = true;
    xSemaphoreGive(guard);
    return NULL;
}

static bool valid_config(const alarm_config_t *c)
{
    return c->version == 1 && c->dry >= 0 && c->wet > c->dry && c->wet <= 4095 &&
           c->cooldown_seconds >= 60 && c->cooldown_seconds <= 86400 &&
           c->label[48] == 0 && c->sender[254] == 0 && c->recipient[254] == 0 &&
           c->api_key[128] == 0 && c->secret_key[128] == 0 &&
           c->sender[0] && c->recipient[0] && c->api_key[0] && c->secret_key[0];
}

static void apply_mail(alarm_config_t *destination)
{
    destination->cooldown_seconds = mail_config.cooldown_seconds;
    memcpy(destination->sender, mail_config.sender, sizeof(destination->sender));
    memcpy(destination->recipient, mail_config.recipient, sizeof(destination->recipient));
    memcpy(destination->api_key, mail_config.api_key, sizeof(destination->api_key));
    memcpy(destination->secret_key, mail_config.secret_key, sizeof(destination->secret_key));
}

void water_alarm_init(nvs_handle_t storage)
{
    store = storage;
    guard = xSemaphoreCreateMutex();
    if (!guard) return;
    size_t mail_size = sizeof(mail_config);
    mail_saved = nvs_get_blob(store, "mail_cfg", &mail_config, &mail_size) == ESP_OK &&
                 mail_size == sizeof(mail_config) && valid_config(&mail_config);
    if (!mail_saved) wipe(&mail_config, sizeof(mail_config));
    size_t size = sizeof(config);
    if (nvs_get_blob(store, "water_cfg", &config, &size) == ESP_OK &&
        size == sizeof(config) && valid_config(&config)) {
        armed = true;
        if (mail_saved) apply_mail(&config);
        last_error = water_alarm_start();
    } else wipe(&config, sizeof(config));
    if (nvs_get_i64(store, "water_next", &saved_next_epoch) == ESP_OK && saved_next_epoch > 0) {
        restored_throttle = true;
        next_try_us = esp_timer_get_time() + (int64_t)(armed ? config.cooldown_seconds : 1800) * 1000000;
    }
    gpio_config_t button = {.pin_bit_mask = 1ULL << TEST_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    if (gpio_config(&button) == ESP_OK)
        button_ready = xTaskCreate(button_task, "test_button", 2048, NULL, 4, NULL) == pdPASS;
}

static bool field(cJSON *request, const char *name, char *destination, size_t capacity)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(request, name);
    if (!cJSON_IsString(value) || !value->valuestring[0] || strlen(value->valuestring) >= capacity) return false;
    for (const unsigned char *p = (unsigned char *)value->valuestring; *p; p++)
        if (*p < 32 || *p > 126) return false;
    strcpy(destination, value->valuestring);
    return true;
}

const char *water_alarm_configure(cJSON *request)
{
    if (!guard) return "Water alarm storage unavailable.";
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "arm"))) return "Explicit ARM confirmation required.";
    alarm_config_t candidate = {.version = 1};
    cJSON *dry = cJSON_GetObjectItemCaseSensitive(request, "dry_threshold");
    cJSON *wet = cJSON_GetObjectItemCaseSensitive(request, "wet_threshold");
    cJSON *cooldown = cJSON_GetObjectItemCaseSensitive(request, "cooldown_seconds");
    const char *error = "Invalid alarm settings.";
    if (!cJSON_IsNumber(dry) || !cJSON_IsNumber(wet) || !cJSON_IsNumber(cooldown)) goto done;
    candidate.dry = dry->valueint;
    candidate.wet = wet->valueint;
    candidate.cooldown_seconds = cooldown->valueint;
    if (!field(request, "label", candidate.label, sizeof(candidate.label))) goto done;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "use_saved_mail"))) {
        if (!mail_saved) { error = "Run Mailjet setup first."; goto done; }
        apply_mail(&candidate);
    } else if (
        !field(request, "sender", candidate.sender, sizeof(candidate.sender)) ||
        !field(request, "recipient", candidate.recipient, sizeof(candidate.recipient)) ||
        !field(request, "api_key", candidate.api_key, sizeof(candidate.api_key)) ||
        !field(request, "secret_key", candidate.secret_key, sizeof(candidate.secret_key)) ||
        strchr(candidate.api_key, ':')) goto done;
    if (!valid_config(&candidate)) goto done;
    // A separately saved mail profile remains authoritative across calibration.
    if (mail_saved) apply_mail(&candidate);
    error = water_alarm_start();
    if (error) goto done;
    error = "Could not save alarm settings.";
    if (nvs_set_blob(store, "water_cfg", &candidate, sizeof(candidate)) != ESP_OK || nvs_commit(store) != ESP_OK) goto done;
    xSemaphoreTake(guard, portMAX_DELAY);
    wipe(&config, sizeof(config));
    config = candidate;
    armed = true;
    state = (water_logic_t){0};
    acknowledged = 0;
    last_error = NULL;
    // Preserve the cooldown across reconfiguration/disable/enable.
    xSemaphoreGive(guard);
    error = NULL;
done:
    wipe(&candidate, sizeof(candidate));
    return error;
}

cJSON *mailjet_settings_status(void)
{
    cJSON *r = cJSON_CreateObject();
    const alarm_config_t *settings = mail_saved ? &mail_config : &config;
    bool configured = mail_saved || armed;
    cJSON_AddBoolToObject(r, "configured", configured);
    cJSON_AddBoolToObject(r, "saved_profile", mail_saved);
    cJSON_AddBoolToObject(r, "armed", armed);
    cJSON_AddStringToObject(r, "sender", configured ? settings->sender : "");
    cJSON_AddStringToObject(r, "recipient", configured ? settings->recipient : "");
    cJSON_AddNumberToObject(r, "cooldown_seconds", configured ? settings->cooldown_seconds : 1800);
    return r;
}

const char *mailjet_settings_save(cJSON *request)
{
    if (!guard) return "Storage unavailable.";
    alarm_config_t candidate = {.version = 1, .dry = 400, .wet = 700};
    const char *error = "Invalid Mailjet settings.";
    cJSON *interval = cJSON_GetObjectItemCaseSensitive(request, "cooldown_seconds");
    if (!cJSON_IsNumber(interval) || interval->valuedouble != interval->valueint) goto done;
    candidate.cooldown_seconds = interval->valueint;
    if (!field(request, "sender", candidate.sender, sizeof(candidate.sender)) ||
        !field(request, "recipient", candidate.recipient, sizeof(candidate.recipient)) ||
        !field(request, "api_key", candidate.api_key, sizeof(candidate.api_key)) ||
        !field(request, "secret_key", candidate.secret_key, sizeof(candidate.secret_key)) ||
        !valid_config(&candidate) || strchr(candidate.api_key, ':') ||
        !strchr(candidate.sender, '@') || !strchr(candidate.recipient, '@')) goto done;
    error = "Could not save Mailjet settings.";
    if (nvs_set_blob(store, "mail_cfg", &candidate, sizeof(candidate)) != ESP_OK || nvs_commit(store) != ESP_OK) goto done;
    xSemaphoreTake(guard, portMAX_DELAY);
    mail_config = candidate;
    mail_saved = true;
    if (armed) apply_mail(&config);
    xSemaphoreGive(guard);
    error = NULL;
done:
    wipe(&candidate, sizeof(candidate));
    return error;
}

const char *water_alarm_disable(void)
{
    if (!guard) return "Water alarm storage unavailable.";
    esp_err_t err = nvs_erase_key(store, "water_cfg");
    if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || nvs_commit(store) != ESP_OK)
        return "Could not remove saved alarm settings.";
    xSemaphoreTake(guard, portMAX_DELAY);
    armed = false;
    button_pending = false;
    monitoring = false;
    wipe(&config, sizeof(config));
    state = (water_logic_t){0};
    acknowledged = 0;
    raw_value = -1;
    last_error = NULL;
    xSemaphoreGive(guard);
    if (started) gpio_set_level(POWER_PIN, 0);
    return NULL;
}

cJSON *water_alarm_status(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "signal_gpio", 34);
    cJSON_AddNumberToObject(r, "power_gpio", 25);
    if (!guard) { cJSON_AddBoolToObject(r, "available", false); return r; }
    xSemaphoreTake(guard, portMAX_DELAY);
    cJSON_AddBoolToObject(r, "available", true);
    cJSON_AddNumberToObject(r, "test_button_gpio", TEST_BUTTON_PIN);
    cJSON_AddBoolToObject(r, "test_button_ready", button_ready);
    cJSON_AddBoolToObject(r, "test_button_pending", button_pending);
    cJSON_AddNumberToObject(r, "test_button_presses", button_presses);
    cJSON_AddBoolToObject(r, "armed", armed);
    cJSON_AddBoolToObject(r, "monitoring", monitoring);
    cJSON_AddNumberToObject(r, "raw", raw_value);
    cJSON_AddBoolToObject(r, "wet", state.wet);
    cJSON_AddBoolToObject(r, "pending", state.generation != acknowledged);
    cJSON_AddStringToObject(r, "label", armed ? config.label : "");
    cJSON_AddNumberToObject(r, "dry_threshold", armed ? config.dry : 400);
    cJSON_AddNumberToObject(r, "wet_threshold", armed ? config.wet : 700);
    cJSON_AddNumberToObject(r, "cooldown_seconds", armed ? config.cooldown_seconds : 1800);
    int64_t left = next_try_us - esp_timer_get_time();
    cJSON_AddNumberToObject(r, "cooldown_remaining", left > 0 ? (left + 999999) / 1000000 : 0);
    cJSON_AddNumberToObject(r, "attempts_this_boot", attempts);
    cJSON_AddNumberToObject(r, "accepted_this_boot", accepted);
    if (last_error) cJSON_AddStringToObject(r, "last_error", last_error);
    xSemaphoreGive(guard);
    return r;
}

void water_alarm_service(bool connected)
{
    int64_t now = esp_timer_get_time();
    if (!guard || !connected) return;
    xSemaphoreTake(guard, portMAX_DELAY);
    bool requested_test = button_pending && (mail_saved || armed);
    bool need = (armed && (state.wet || state.generation != acknowledged)) ||
                (button_pending && (mail_saved || armed));
    xSemaphoreGive(guard);
    if (!requested_test && now < next_check_us) return;
    next_check_us = now + 1000000;
    if (!need) return;
    if (!requested_test && restored_throttle && time(NULL) < 1735689600) {
        if (!email_sync_clock()) { next_check_us = esp_timer_get_time() + 60000000; return; }
    }
    if (restored_throttle && time(NULL) >= 1735689600) {
        int64_t seconds = saved_next_epoch - (int64_t)time(NULL);
        if (seconds < 0) seconds = 0;
        if (seconds > 86400) seconds = 86400;
        next_try_us = esp_timer_get_time() + seconds * 1000000;
        restored_throttle = false;
    }
    if (!requested_test && esp_timer_get_time() < next_try_us) return;
    if (!email_sync_clock()) {
        last_error = "Could not sync time. Will retry in one minute; no email attempted.";
        if (requested_test) {
            xSemaphoreTake(guard, portMAX_DELAY);
            button_pending = false;
            xSemaphoreGive(guard);
            last_error = "Button test failed before HTTPS: NTP clock sync failed. Check internet access and press again.";
        }
        next_check_us = esp_timer_get_time() + 60000000;
        return;
    }
    xSemaphoreTake(guard, portMAX_DELAY);
    water_logic_t snapshot = state;
    alarm_config_t settings = config;
    int reading = raw_value;
    bool due = armed && water_logic_due(&snapshot, acknowledged, esp_timer_get_time(), next_try_us);
    bool manual = button_pending && (mail_saved || armed);
    if (manual) {
        if (mail_saved) apply_mail(&settings);
        if (!settings.label[0]) strcpy(settings.label, "Water sensor");
        due = true;
    }
    xSemaphoreGive(guard);
    if (!due) { wipe(&settings, sizeof(settings)); return; }
    // Only automatic water alerts reserve or consume the persisted cooldown.
    if (!manual) {
    saved_next_epoch = (int64_t)time(NULL) + settings.cooldown_seconds;
    if (nvs_set_i64(store, "water_next", saved_next_epoch) != ESP_OK || nvs_commit(store) != ESP_OK) {
        last_error = "Could not persist cooldown; email skipped to prevent repeated sends.";
        next_check_us = esp_timer_get_time() + 60000000;
        wipe(&settings, sizeof(settings));
        return;
    }
    next_try_us = esp_timer_get_time() + (int64_t)settings.cooldown_seconds * 1000000;
    }
    attempts++;
    if (manual) {
        xSemaphoreTake(guard, portMAX_DELAY);
        button_pending = false;
        xSemaphoreGive(guard);
    }
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "sender", settings.sender);
    cJSON_AddStringToObject(request, "recipient", settings.recipient);
    cJSON_AddStringToObject(request, "api_key", settings.api_key);
    cJSON_AddStringToObject(request, "secret_key", settings.secret_key);
    cJSON_AddBoolToObject(request, "sandbox", false);
    cJSON_AddBoolToObject(request, "button_test", manual);
    last_error = send_water_email(request, settings.label, reading, snapshot.wet);
    if (!last_error) { accepted++; if (!manual) acknowledged = snapshot.generation; }
    const char *keys[] = {"api_key", "secret_key"};
    for (int i = 0; i < 2; i++) {
        cJSON *key = cJSON_GetObjectItemCaseSensitive(request, keys[i]);
        if (cJSON_IsString(key)) wipe(key->valuestring, strlen(key->valuestring));
    }
    cJSON_Delete(request);
    wipe(&settings, sizeof(settings));
}
