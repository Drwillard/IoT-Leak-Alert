#pragma once
#include "cJSON.h"
#include <stdbool.h>
// Returns NULL only after Mailjet reports message success. Never persists API keys.
const char *send_test_email(cJSON *request);
const char *send_water_email(cJSON *request, const char *label, int raw, bool still_wet);
bool email_sync_clock(void);
