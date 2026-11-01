#pragma once
#include "FreeRTOS.h"
typedef struct QueueDefinition *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s);
