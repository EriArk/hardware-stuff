#include <cassert>
#include <iostream>
#include "SD.h"
#include "storage_recovery.h"
#include "work_progress.h"

FakeSD SD;
int pulses = 0;
void pulse() { ++pulses; }

int main() {
    int scenarios = 0;
    for (const std::string path : {"/reader/settings-v1.json",
                                   "/reader/favorites-v1.json"}) {
        const std::string old = path + ".old", part = path + ".part";
        StorageRecoveryReport report;
        // Power loss at each publication boundary. A partial never wins.
        for (int phase = 0; phase < 4; ++phase) {
            SD.files.clear();
            if (phase == 0) SD.files[path] = "previous";
            if (phase == 1 || phase == 2) SD.files[old] = "previous";
            if (phase == 2 || phase == 3) SD.files[path] = "published";
            SD.files[part] = "incomplete";
            assert(StorageRecovery::run(report));
            assert(SD.files.at(path) == (phase < 2 ? "previous" : "published"));
            assert(!SD.exists(old.c_str()) && !SD.exists(part.c_str()));
            assert(report.errors == 0);
            assert(StorageRecovery::run(report)); // idempotent boot
            assert(report.filesRestored == 0 && report.partialsRemoved == 0);
            ++scenarios;
        }
        SD.files.clear();
        SD.files[part] = "incomplete-first-save";
        assert(StorageRecovery::run(report));
        assert(!SD.exists(path.c_str()) && !SD.exists(part.c_str()));
        ++scenarios;

        SD.files[old] = "previous";
        SD.files[part] = "incomplete";
        SD.failingRename = old;
        assert(!StorageRecovery::run(report));
        assert(report.errors == 1 && SD.files.at(old) == "previous");
        assert(!StorageRecovery::recoverFile(path.c_str()));
        assert(SD.files.at(old) == "previous");
        SD.failingRename.clear();
        assert(StorageRecovery::recoverFile(path.c_str()));
        assert(SD.files.at(path) == "previous");
        ++scenarios;
    }
    reportWorkProgress();
    assert(pulses == 0);
    assert(!StorageRecovery::recoverFile(nullptr));
    assert(!StorageRecovery::recoverFile(""));
    assert(!StorageRecovery::recoverFile(std::string(160, 'x').c_str()));
    auto previous = setWorkProgressCallback(pulse);
    reportWorkProgress();
    assert(pulses == 1 && previous == nullptr);
    setWorkProgressCallback(previous);
    reportWorkProgress();
    assert(pulses == 1);
    std::cout << "HOST_REGRESSIONS_OK recovery_scenarios=" << scenarios
              << " cooperative_progress=true\n";
}
