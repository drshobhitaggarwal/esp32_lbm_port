#pragma once
#include "FreeRTOS.h"
typedef struct tskTaskControlBlock *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *woken);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
