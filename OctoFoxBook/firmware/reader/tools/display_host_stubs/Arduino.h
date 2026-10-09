#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <algorithm>
using std::min;
using std::max;
inline uint32_t millis() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}
inline void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
struct SerialStub {
    void printf(const char *fmt, ...) { va_list args; va_start(args, fmt); vprintf(fmt, args); va_end(args); }
};
inline SerialStub Serial;
using SemaphoreHandle_t = std::mutex *;
struct Task {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable changed;
    unsigned notifications = 0;
    bool stopped = false;
};
using TaskHandle_t = Task *;
inline thread_local Task *currentTask = nullptr;
struct TaskStopped {};
constexpr int pdTRUE = 1, pdPASS = 1;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex; }
inline void xSemaphoreTake(SemaphoreHandle_t s, uint32_t) { s->lock(); }
inline void xSemaphoreGive(SemaphoreHandle_t s) { s->unlock(); }
inline void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }
inline int xTaskCreatePinnedToCore(void (*fn)(void *), const char *, int, void *arg, int, TaskHandle_t *out, int) {
    auto *task = new Task;
    *out = task;
    task->thread = std::thread([=] { currentTask = task; try { fn(arg); } catch (TaskStopped &) {} });
    return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t task) {
    std::lock_guard<std::mutex> lock(task->mutex);
    ++task->notifications;
    task->changed.notify_all();
}
inline unsigned ulTaskNotifyTake(int, uint32_t) {
    std::unique_lock<std::mutex> lock(currentTask->mutex);
    currentTask->changed.wait(lock, [] { return currentTask->notifications || currentTask->stopped; });
    if (currentTask->stopped) throw TaskStopped{};
    const auto n = currentTask->notifications;
    currentTask->notifications = 0;
    return n;
}
inline void vTaskDelete(TaskHandle_t task) {
    { std::lock_guard<std::mutex> lock(task->mutex); task->stopped = true; task->changed.notify_all(); }
    task->thread.join();
    delete task;
}
