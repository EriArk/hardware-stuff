#include "download_queue.h"

#include <ArduinoJson.h>
#include <SD.h>

#include "book_upload.h"

namespace {

constexpr char kQueueDirectory[] = "/reader";
constexpr char kQueuePath[] = "/reader/download-queue-v1.json";
constexpr char kQueuePartPath[] = "/reader/download-queue-v1.json.part";
constexpr char kQueueOldPath[] = "/reader/download-queue-v1.json.old";

void setError(BookDownloadQueueState &state, const char *error) {
    snprintf(state.error, sizeof(state.error), "%s",
             error == nullptr ? "" : error);
}

bool validJob(const BookDownloadJob &job) {
    return BookUploadReceiver::validBookId(job.bookId) &&
           job.title[0] != '\0' &&
           strncmp(job.acquisitionUrl, "https://", 8) == 0 &&
           strnlen(job.acquisitionUrl, sizeof(job.acquisitionUrl)) <
               sizeof(job.acquisitionUrl) &&
           (job.coverUrl[0] == '\0' ||
            (strncmp(job.coverUrl, "https://", 8) == 0 &&
             strnlen(job.coverUrl, sizeof(job.coverUrl)) <
                 sizeof(job.coverUrl)));
}

int findJob(const BookDownloadQueueState &state, const char *bookId) {
    for (size_t index = 0; index < state.count; ++index) {
        if (strcmp(state.jobs[index].bookId, bookId) == 0) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool publishQueueFile() {
    SD.remove(kQueueOldPath);
    const bool hadPrevious = SD.exists(kQueuePath);
    if (hadPrevious && !SD.rename(kQueuePath, kQueueOldPath)) {
        return false;
    }
    if (!SD.rename(kQueuePartPath, kQueuePath)) {
        if (hadPrevious) {
            SD.rename(kQueueOldPath, kQueuePath);
        }
        return false;
    }
    return !hadPrevious || SD.remove(kQueueOldPath);
}

}  // namespace

bool DownloadQueue::load(BookDownloadQueueState &state) {
    state = BookDownloadQueueState{};
    if (!SD.exists(kQueuePath)) {
        return true;
    }
    File input = SD.open(kQueuePath, FILE_READ);
    if (!input) {
        setError(state, "queue-open-failed");
        return false;
    }
    JsonDocument document;
    const DeserializationError jsonError = deserializeJson(document, input);
    input.close();
    if (jsonError || document["schema"].as<uint8_t>() != 1) {
        setError(state, "queue-json-invalid");
        return false;
    }
    JsonArrayConst jobs = document["jobs"].as<JsonArrayConst>();
    for (JsonObjectConst item : jobs) {
        if (state.count >= kBookDownloadQueueCapacity) {
            setError(state, "queue-over-capacity");
            return false;
        }
        BookDownloadJob &job = state.jobs[state.count];
        snprintf(job.bookId, sizeof(job.bookId), "%s",
                 item["book_id"] | "");
        snprintf(job.title, sizeof(job.title), "%s", item["title"] | "");
        snprintf(job.author, sizeof(job.author), "%s",
                 item["author"] | "");
        snprintf(job.acquisitionUrl, sizeof(job.acquisitionUrl), "%s",
                 item["url"] | "");
        snprintf(job.coverUrl, sizeof(job.coverUrl), "%s",
                 item["cover_url"] | "");
        const int storedAttempts = item["attempts"] | 0;
        job.attempts = static_cast<uint8_t>(
            max(0, min(storedAttempts, 10)));
        snprintf(job.lastError, sizeof(job.lastError), "%s",
                 item["last_error"] | "");
        if (!validJob(job) || findJob(state, job.bookId) >= 0) {
            setError(state, "queue-job-invalid");
            return false;
        }
        ++state.count;
    }
    setError(state, "");
    return true;
}

bool DownloadQueue::save(const BookDownloadQueueState &state) {
    if (state.count > kBookDownloadQueueCapacity ||
        (!SD.exists(kQueueDirectory) && !SD.mkdir(kQueueDirectory))) {
        return false;
    }
    for (size_t index = 0; index < state.count; ++index) {
        if (!validJob(state.jobs[index])) {
            return false;
        }
    }

    JsonDocument document;
    document["schema"] = 1;
    JsonArray jobs = document["jobs"].to<JsonArray>();
    for (size_t index = 0; index < state.count; ++index) {
        const BookDownloadJob &job = state.jobs[index];
        JsonObject item = jobs.add<JsonObject>();
        item["book_id"] = job.bookId;
        item["title"] = job.title;
        item["author"] = job.author;
        item["url"] = job.acquisitionUrl;
        if (job.coverUrl[0] != '\0') {
            item["cover_url"] = job.coverUrl;
        }
        item["attempts"] = job.attempts;
        item["last_error"] = job.lastError;
    }

    SD.remove(kQueuePartPath);
    File output = SD.open(kQueuePartPath, FILE_WRITE);
    if (!output) {
        return false;
    }
    const bool written = serializeJson(document, output) > 0;
    output.flush();
    output.close();
    if (!written || !publishQueueFile()) {
        SD.remove(kQueuePartPath);
        return false;
    }
    return true;
}

bool DownloadQueue::enqueue(const BookDownloadJob &job,
                            BookDownloadQueueState &state) {
    if (!validJob(job)) {
        setError(state, "queue-job-invalid");
        return false;
    }
    const int existing = findJob(state, job.bookId);
    if (existing >= 0) {
        const uint8_t attempts = state.jobs[existing].attempts;
        const char *lastError = state.jobs[existing].lastError;
        char preservedError[64]{};
        snprintf(preservedError, sizeof(preservedError), "%s", lastError);
        state.jobs[existing] = job;
        state.jobs[existing].attempts = attempts;
        snprintf(state.jobs[existing].lastError,
                 sizeof(state.jobs[existing].lastError), "%s",
                 preservedError);
    } else {
        if (state.count >= kBookDownloadQueueCapacity) {
            setError(state, "queue-full");
            return false;
        }
        state.jobs[state.count++] = job;
    }
    if (!save(state)) {
        setError(state, "queue-save-failed");
        return false;
    }
    setError(state, "");
    return true;
}

bool DownloadQueue::markFailure(const char *bookId, const char *error,
                                BookDownloadQueueState &state) {
    const int index = findJob(state, bookId);
    if (index < 0) {
        setError(state, "queue-job-missing");
        return false;
    }
    state.jobs[index].attempts = static_cast<uint8_t>(min(
        static_cast<int>(state.jobs[index].attempts) + 1, 10));
    snprintf(state.jobs[index].lastError,
             sizeof(state.jobs[index].lastError), "%s",
             error == nullptr ? "unknown" : error);
    if (!save(state)) {
        setError(state, "queue-save-failed");
        return false;
    }
    setError(state, "");
    return true;
}

bool DownloadQueue::remove(const char *bookId,
                           BookDownloadQueueState &state) {
    const int index = findJob(state, bookId);
    if (index < 0) {
        return true;
    }
    for (size_t cursor = static_cast<size_t>(index); cursor + 1 < state.count;
         ++cursor) {
        state.jobs[cursor] = state.jobs[cursor + 1];
    }
    if (state.count > 0) {
        state.jobs[state.count - 1] = BookDownloadJob{};
        --state.count;
    }
    if (!save(state)) {
        setError(state, "queue-save-failed");
        return false;
    }
    setError(state, "");
    return true;
}
