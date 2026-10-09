#pragma once

#include <Arduino.h>

struct StorageRecoveryReport {
    bool ok = false;
    uint16_t booksScanned = 0;
    uint16_t filesRestored = 0;
    uint16_t partialsRemoved = 0;
    uint16_t rollbackCopiesRemoved = 0;
    uint16_t errors = 0;
};

struct StorageRecoverySelfTestResult {
    bool recoveredBook = false;
    bool recoveredState = false;
    bool preservedPublishedMetadata = false;
    bool leftoversAbsent = false;
    bool cleanupPassed = false;
};

class StorageRecovery {
public:
    // Reconcile a single writer's files before load/save, including retries
    // after a failed boot-time rename. Never discard an unrestored .old file.
    static bool recoverFile(const char *finalPath);

    // Reconciles every atomic final/.part/.old triple while no writer is
    // active. Safe to call at boot after microSD has mounted.
    static bool run(StorageRecoveryReport &report);

    // Creates a reserved interrupted-write fixture. A real restart followed
    // by run() and verifySelfTest() proves the boot recovery path.
    static bool armSelfTest();
    static bool verifySelfTest(StorageRecoverySelfTestResult &result);
    static bool cleanupSelfTest();
};
