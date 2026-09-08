#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "nvs.h"

void water_alarm_init(nvs_handle_t storage);
cJSON *water_alarm_status(void);
cJSON *mailjet_settings_status(void);
const char *mailjet_settings_save(cJSON *request);
const char *water_alarm_start(void);
const char *water_alarm_configure(cJSON *request);
const char *water_alarm_disable(void);
void water_alarm_service(bool connected);
