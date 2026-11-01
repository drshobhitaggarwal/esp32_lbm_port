#pragma once
#include <stdio.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_LOGE(tag, fmt, ...) printf("E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOG_BUFFER_HEX(tag, buf, len) do { (void)(tag); (void)(buf); (void)(len); } while (0)
