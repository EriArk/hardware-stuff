#pragma once

// One storage writer at a time; UI suspends SD actions while this job runs.
namespace BookPreparation {
bool start(const char *bookId);
bool busy();
void cancel();
bool takeResult(bool &ok, bool &cancelled);
const char *error();
}
