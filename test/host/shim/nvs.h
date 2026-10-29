#pragma once
/* Host-test shim: config_store.h only needs the NVS error codes. */
#include "esp_err.h"
#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
