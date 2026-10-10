#pragma once
#include <stdint.h>

namespace SyncProgress {
enum class Stage : uint8_t {
    Connecting, CollectionsUpload, ReadingUpload, CheckingBooks, Download,
    Preparing, Cover, Verifying, Removing, ReadingDownload, CollectionsDownload
};
struct Snapshot {
    Stage stage = Stage::Connecting;
    uint32_t done = 0, total = 0, downloaded = 0;
    uint32_t startedAt = 0, stageStartedAt = 0;
};
// Unknown work must never look like a measured percentage. This percentage
// describes the current step/file, not the entire synchronization session.
inline int percent(const Snapshot &s) {
    if (!s.total) return -1;
    return s.done >= s.total ? 100 : static_cast<int>(uint64_t(s.done) * 100 / s.total);
}
inline bool changed(const Snapshot &a, const Snapshot &b) {
    return a.stage != b.stage || a.done != b.done || a.total != b.total ||
           a.downloaded != b.downloaded;
}
}
