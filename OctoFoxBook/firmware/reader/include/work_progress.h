#pragma once

// Cooperative progress only: callbacks run on the calling task, never on a
// timer/ISR. Callers install a handler only while no other UI is being drawn.
// Cancellable workers also yield one RTOS tick per 20 ms slice so long SD/hash/
// parsing loops do not starve the idle-task watchdog, even without a UI handler.
using WorkProgressCallback = void (*)();
WorkProgressCallback setWorkProgressCallback(WorkProgressCallback callback);
void reportWorkProgress();
using WorkCancelCallback = bool (*)();
void setWorkCancelCallback(WorkCancelCallback callback);
bool workCancelled();
