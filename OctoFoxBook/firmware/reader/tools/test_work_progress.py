"""Exercise the production worker yielding/callback ownership with RTOS stubs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

FW = Path(__file__).resolve().parent.parent


class WorkProgressTests(unittest.TestCase):
    def test_worker_yields_without_drawing_ui_and_respects_cancel_ownership(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ required")
        with tempfile.TemporaryDirectory(prefix="reader-progress-test-") as directory:
            root = Path(directory)
            (root / "freertos").mkdir()
            (root / "freertos/FreeRTOS.h").write_text('''
#pragma once
#include <cstdint>
using TickType_t = uint32_t;
using TaskHandle_t = void *;
#define pdMS_TO_TICKS(ms) (ms)
''', encoding="utf-8")
            (root / "freertos/task.h").write_text('''
#pragma once
#include "FreeRTOS.h"
TaskHandle_t xTaskGetCurrentTaskHandle();
TickType_t xTaskGetTickCount();
void vTaskDelay(TickType_t);
''', encoding="utf-8")
            (root / "test.cpp").write_text('''
#include "work_progress.h"
#include "freertos/task.h"
#include <cassert>
int ui, worker;
TaskHandle_t current=&ui;
TickType_t now=0;
unsigned sleeps=0, draws=0;
TaskHandle_t xTaskGetCurrentTaskHandle() { return current; }
TickType_t xTaskGetTickCount() { return now; }
void vTaskDelay(TickType_t ticks) { assert(ticks==1); ++sleeps; now+=ticks; }
void draw() { assert(current==&ui); ++draws; }
bool cancel() { return true; }
int main() {
    setWorkProgressCallback(draw);
    reportWorkProgress(); assert(draws==1 && sleeps==0);
    current=&worker; setWorkCancelCallback(cancel);
    assert(workCancelled());
    now=19; reportWorkProgress(); assert(sleeps==0 && draws==1);
    now=20; reportWorkProgress(); assert(sleeps==1 && draws==1);
    reportWorkProgress(); assert(sleeps==1);
    now=40; reportWorkProgress(); assert(sleeps==2);
    current=&ui; assert(!workCancelled());
    reportWorkProgress(); assert(draws==2 && sleeps==2);
    current=&worker;
    now=UINT32_MAX-9; setWorkCancelCallback(cancel);
    now=10; reportWorkProgress(); assert(sleeps==3); // RTOS tick rollover.
    setWorkCancelCallback(nullptr); assert(!workCancelled());
}
''', encoding="utf-8")
            binary = root / "progress.exe"
            result = subprocess.run([compiler, "-std=c++17", "-DARDUINO", "-Wall", "-Wextra", "-Werror",
                                     "-I", str(root), "-I", str(FW / "include"),
                                     str(root / "test.cpp"), str(FW / "src/work_progress.cpp"),
                                     "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_all_long_sync_stages_have_progress_checkpoints(self):
        network = (FW / "src/network_service.cpp").read_text(encoding="utf-8")
        hashing = network.split("bool hashFile(", 1)[1].split("bool acceptedFb2ContentType", 1)[0]
        self.assertIn("reportWorkProgress();", hashing)
        self.assertIn("workCancelled()", hashing)
        for name in ("downloadFb2", "downloadCover"):
            body = network.split("bool NetworkService::" + name + "(", 1)[1].split("\n}\n", 1)[0]
            loop = body.split("while (http.connected()", 1)[1]
            self.assertLess(loop.index("reportWorkProgress();"), loop.index("stream->available();"))
        parser = (FW / "src/fb2_cache.cpp").read_text(encoding="utf-8").split("bool Fb2Cache::build(", 1)[1]
        self.assertIn("reportWorkProgress();", parser)


if __name__ == "__main__":
    unittest.main()
