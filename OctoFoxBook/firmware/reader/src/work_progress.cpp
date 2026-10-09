#include "work_progress.h"
#include <atomic>
#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace {
std::atomic<WorkProgressCallback> currentCallback{nullptr};
std::atomic<WorkCancelCallback> cancelCallback{nullptr};
#ifdef ARDUINO
std::atomic<TaskHandle_t> callbackOwner{nullptr};
std::atomic<TaskHandle_t> cancelOwner{nullptr};
std::atomic<TickType_t> lastWorkerYield{0};
#endif
}

void setWorkCancelCallback(WorkCancelCallback callback) {
#ifdef ARDUINO
    cancelOwner = xTaskGetCurrentTaskHandle();
    lastWorkerYield = xTaskGetTickCount();
#endif
    cancelCallback = callback;
}

bool workCancelled() {
#ifdef ARDUINO
    if (cancelOwner.load() != xTaskGetCurrentTaskHandle()) return false;
#endif
    const auto callback = cancelCallback.load();
    return callback && callback();
}

WorkProgressCallback setWorkProgressCallback(WorkProgressCallback callback) {
    const auto previous = currentCallback.load();
#ifdef ARDUINO
    callbackOwner = xTaskGetCurrentTaskHandle();
#endif
    currentCallback = callback;
    return previous;
}

void reportWorkProgress() {
#ifdef ARDUINO
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    // SD/SPI and parsing can run without blocking the worker. Give IDLE0 CPU
    // time during long batches; taskYIELD alone cannot schedule lower priority
    // tasks. Keep the watchdog enabled and never draw UI from the worker.
    const TickType_t now = xTaskGetTickCount();
    if (cancelOwner.load() == task && now - lastWorkerYield.load() >= pdMS_TO_TICKS(20)) {
        lastWorkerYield = now;
        vTaskDelay(1);
    }
    if (callbackOwner.load() != task) return;
#endif
    const auto callback = currentCallback.load();
    if (callback != nullptr) {
        callback();
    }
}
