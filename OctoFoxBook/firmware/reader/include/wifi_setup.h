#pragma once
#include "provisioning_store.h"
namespace WifiSetup {
enum class State { Idle, Scanning, Networks, Connecting, Success, Failed };
struct Network { char ssid[33]{}; int rssi = 0; bool open = false, supported = true; };
constexpr unsigned kCapacity = 24;
bool enter();
void leave();
bool active();
void scan();
void cancel();
bool connect(const WifiCredential &credential, bool open);
bool poll();
State state();
const char *error();
unsigned count();
const Network &network(unsigned index);
}
