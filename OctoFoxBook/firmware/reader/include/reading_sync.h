#pragma once
#include "sync_policy.h"
class NetworkService;
namespace ReadingSync {
bool run(NetworkService &network, const char *device, bool uploadOnly = false);
const char *error();
ReaderSyncPolicy::Error policyError();
}
