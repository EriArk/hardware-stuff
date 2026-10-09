#include <cassert>
#include <cstdint>
#include <iostream>
#include "reader_input_policy.h"
#include "work_progress.h"

bool stopRequested = false;
bool shouldStop() { return stopRequested; }
int frames = 0;
void progress() { ++frames; }

int main() {
    NavigationRepeat repeat;
    repeat.pressed(100);
    assert(!repeat.held(100));
    assert(!repeat.held(749));
    assert(repeat.held(750));
    assert(!repeat.held(929));
    assert(repeat.held(930));
    assert(repeat.held(9000));
    assert(!repeat.held(9000)); // never replay a backlog of repeat ticks
    repeat.pressed(UINT32_MAX - 100);
    assert(!repeat.held(548));
    assert(repeat.held(549));
    assert(!workCancelled());
    setWorkCancelCallback(shouldStop);
    assert(!workCancelled());
    stopRequested = true;
    assert(workCancelled());
    setWorkProgressCallback(progress);
    reportWorkProgress();
    assert(frames == 1 && workCancelled());
    setWorkCancelCallback(nullptr);
    assert(!workCancelled());
    setWorkProgressCallback(nullptr);
    reportWorkProgress();
    assert(frames == 1);
    std::cout << "INTERACTION_POLICY_OK\n";
}
