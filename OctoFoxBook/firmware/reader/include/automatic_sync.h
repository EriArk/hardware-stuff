#pragma once

#include <Arduino.h>
#include "sync_policy.h"

// Owns network I/O in a low-priority task. It never touches the framebuffer,
// active reader state or existing book caches. Main loop consumes completions.
namespace AutomaticSync {
void start();
// start() only creates a sleeping worker. Radio is acquired by request() only.
// Explicit user request resumes an idle pause, never an active storage operation.
bool request(bool exclusiveBusy = false);
void cancel();
enum class Status : uint8_t { Idle, Running, Complete, Failed, Cancelled, More, Cancelling };
Status status();
unsigned downloaded();
bool takeFinished();
void setPaused(bool paused);
bool busy();
bool isPaused();
ReaderSyncPolicy::Error error();
int httpCode();
bool cancelRequested();
// Only the running sync worker may acquire Wi-Fi, including its book/cover I/O.
bool ownsNetworkSession();
bool takeLibraryChanged();
const char *deviceId();
}
