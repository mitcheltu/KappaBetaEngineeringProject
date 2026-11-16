#pragma once
#include <cstddef>
#include <cstdint>

#define CONFIG_IDF_TARGET_ESP32S3 1
using QueueHandle_t = void*;
using TickType_t = std::uint32_t;
constexpr int pdTRUE = 1;
constexpr int pdFALSE = 0;
constexpr int LOW = 0, HIGH = 1;
constexpr TickType_t pdMS_TO_TICKS(std::uint32_t ms) { return ms; }
std::uint32_t millis();
void vTaskDelay(TickType_t ticks);
int xQueueSend(QueueHandle_t, const void*, TickType_t);
int xQueueReceive(QueueHandle_t, void*, TickType_t);
int xQueueOverwrite(QueueHandle_t, const void*);
int digitalRead(int);
void rgbLedWrite(int, std::uint8_t, std::uint8_t, std::uint8_t);
