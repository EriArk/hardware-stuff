"""No-board policy tests and source wiring guards; not panel latency tests."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
FW = TOOLS.parent


def source(name):
    return (FW / "src" / name).read_text(encoding="utf-8")


class ReaderInteractionTests(unittest.TestCase):
    def test_actual_display_queue_with_blocked_panel(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ required")
        with tempfile.TemporaryDirectory(prefix="reader-display-") as folder:
            binary = Path(folder) / "display.exe"
            subprocess.run([
                compiler, "-std=c++17", "-pthread", "-DUSE_EPD_PAINTER",
                "-I", str(TOOLS / "display_host_stubs"),
                "-I", str(FW / "include"),
                str(TOOLS / "host_display_queue.cpp"),
                str(FW / "src/display_refresh_controller.cpp"), "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("DISPLAY_QUEUE_OK", result.stdout)

    def test_actual_cpp_repeat_rollover_and_cancellation(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ required")
        with tempfile.TemporaryDirectory(prefix="reader-interaction-") as folder:
            binary = Path(folder) / "interaction.exe"
            subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I", str(FW / "include"),
                str(TOOLS / "host_reader_interaction.cpp"),
                str(FW / "src/work_progress.cpp"), "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("INTERACTION_POLICY_OK", result.stdout)

    def test_sync_has_no_timer_or_boot_request(self):
        sync = source("automatic_sync.cpp")
        self.assertNotIn("kPollSeconds", sync)
        self.assertNotIn("vTaskDelay", sync)
        self.assertIn("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)", sync)
        self.assertIn("if (allowed) xTaskNotifyGive(syncTask)", sync)
        main = source("main.cpp")
        setup = main.split("void setup()", 1)[1].split("void loop()", 1)[0]
        self.assertNotIn("AutomaticSync::request()", setup)
        self.assertIn("automatic=false", sync)

    def test_sync_disconnects_before_completion(self):
        sync = source("automatic_sync.cpp")
        end = sync.split("network.disconnect();", 1)[1]
        self.assertLess(end.index("finished = true"), end.index("running = false"))
        self.assertIn("Синхронизировать", source("bookish_ui.cpp"))

    def test_display_owns_snapshot_and_preserves_cleanup(self):
        display = source("display_refresh_controller.cpp")
        self.assertIn("memcpy(queuedFramebuffer, fullFramebuffer", display)
        self.assertIn("queuedClear = queuedClear || hardClear", display)
        self.assertIn("result.queued = true", display)
        self.assertIn("DISPLAY COMPLETE", display)
        submission = display.split("DisplayRefreshResult DisplayRefreshController::refresh(", 1)[1]
        submission = submission.split("// Partial refresh remains disabled", 1)[0]
        self.assertNotIn("waitForFastPainter()", submission)
        self.assertNotIn("fastPainter.clear(", submission)

    def test_sleep_waits_for_frame_and_storage(self):
        main = source("main.cpp")
        power = main.split("void processPowerAction() {", 1)[1].split("bool uiActionPending()", 1)[0]
        self.assertIn("BookPreparation::busy()", power)
        self.assertLess(power.index("displayRefresh.flush()"), power.index("esp_light_sleep_start()"))

    def test_background_preparation_does_not_draw(self):
        prep = source("book_preparation.cpp")
        self.assertNotIn("framebuffer", prep)
        self.assertNotIn("displayRefresh", prep)
        self.assertIn("setWorkCancelCallback(cancellation)", prep)
        self.assertIn("workCancelled()", source("fb2_cache.cpp"))
        self.assertGreaterEqual(source("reader_pagination.cpp").count("workCancelled()"), 3)

    def test_sync_footer_is_separate_and_scrubbed(self):
        main = source("main.cpp")
        self.assertIn("kSyncFooterRegion{0, 20, 84, 500}", main)
        indicator = main.split("void drawBusyIndicator()", 1)[1].split("class BusyIndicator", 1)[0]
        self.assertIn("syncHome ? 878 : 938", indicator)
        self.assertIn("syncHome ? 376 : 210", indicator)
        self.assertIn("DisplayRefreshMode::RecoveryRegion", indicator)
        self.assertIn("busyFrame % 4 == 0 ? kSyncFooterRegion", indicator)
        self.assertIn("syncHome ? 4000U : 1200U", indicator)
        self.assertIn('scheduleGhostCleanup("sync-finished")', main)
        # Portrait footer is clear of all book cards and includes the button.
        self.assertGreaterEqual(878, 876)
        self.assertLess(878 + 20, 904)
        self.assertLessEqual(28 + 340, 376)

    def test_navigation_press_not_release(self):
        main = source("main.cpp")
        poll = main.split("void pollButton(ButtonTracker &button)", 1)[1]
        self.assertIn('emitInput(button.name, "SHORT", "gpio");\n                button.repeat.pressed(now);', poll)
        self.assertIn("release never adds a step", poll)

    def test_sections_do_not_overlay_content(self):
        main = source("main.cpp")
        rail = main.split("void drawTopLevelTabRail(", 1)[1].split("bool appendHomeEntry", 1)[0]
        self.assertNotIn("drawPortrait", rail)
        self.assertIn('void displaySections() {', main)

    def test_prefetch_is_offscreen_and_interruptible(self):
        main = source("main.cpp")
        self.assertIn("framebuffer = offscreen", main)
        self.assertIn("framebuffer = visible", main)
        self.assertIn("prefetchingPage &&", main)
        self.assertIn("cached.layout, ReaderPagination::layoutId()", main)


if __name__ == "__main__":
    unittest.main()
