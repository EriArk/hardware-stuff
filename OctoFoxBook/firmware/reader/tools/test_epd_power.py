"""Check the pinned driver patch and execute its actual power sequences on host."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from patch_epd_power import patched_source, patched_memory_source

SOURCE = (Path(__file__).resolve().parents[1] /
          ".pio/libdeps/t5_epaper_s3_fast/EPD Painter/src/epd_painter_powerctl.cpp")


class PanelPowerTests(unittest.TestCase):
    def test_display_memory_patch_is_repeatable_and_rejects_drift(self):
        painter = SOURCE.with_name("EPD_Painter.cpp")
        if not painter.exists():
            self.skipTest("Run pio run to resolve EPD Painter first")
        patched = patched_memory_source(painter.read_text(encoding="utf-8"))
        self.assertEqual(patched_memory_source(patched), patched)
        self.assertEqual(patched_memory_source(patched.replace("\n", "\r\n")), patched)
        with self.assertRaises(RuntimeError):
            patched_memory_source(patched + "\n// unreviewed update\n")

    def source(self):
        if not SOURCE.exists():
            self.skipTest("Run pio run to resolve the pinned EPD Painter dependency first")
        return SOURCE.read_text(encoding="utf-8")

    def test_patch_is_idempotent_and_line_ending_independent(self):
        patched = patched_source(self.source())
        self.assertEqual(patched_source(patched), patched)
        self.assertEqual(patched_source(patched.replace("\n", "\r\n")), patched)

    def test_unreviewed_source_is_rejected(self):
        with self.assertRaises(RuntimeError):
            patched_source(self.source() + "\n// changed dependency\n")

    def test_actual_power_cycle_releases_peripheral_rail(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 g++ required")
        source = patched_source(self.source())
        functions = []
        for name in ("powerOn", "powerOff"):
            match = re.search(r"(?:bool|void) EPD_H716PowerDriver::" + name +
                              r"\(\) \{.*?\n\}", source, re.S)
            self.assertIsNotNone(match)
            functions.append(match.group())
        harness = r'''
#include <cassert>
#include <vector>
struct Event { int pin; bool high; };
std::vector<Event> events;
struct Pin { int index; bool value=false;
  void set(bool v) { value=v; events.push_back({index,v}); }
};
struct EPD_H716PowerDriver {
  Pin _pin_le{0}, _pin_pwr_dis{1}, _pin_pos_pwr{2}, _pin_neg_pwr{3};
  Pin _pin_stv{4}, _pin_scan_dir{5}, _pin_mode{6}, _pin_oe{7};
  bool powerOn(); void powerOff();
};
#define EPD_DELAY_US(x) ((void)0)
'''
        harness += "\n".join(functions)
        harness += r'''
int main() {
  EPD_H716PowerDriver d;
  for (int cycle=0; cycle<4; ++cycle) {
    events.clear(); assert(d.powerOn());
    bool supply=false;
    for (auto e: events) {
      if(e.pin==5) supply=e.high;
      if((e.pin==1 && !e.high) || ((e.pin==2 || e.pin==3) && e.high)) assert(supply);
    }
    assert(d._pin_scan_dir.value && d._pin_pos_pwr.value && d._pin_neg_pwr.value);
    events.clear(); d.powerOff();
    bool pos=true, neg=true, disabled=false;
    for (auto e: events) {
      if(e.pin==2) pos=e.high;
      if(e.pin==3) neg=e.high;
      if(e.pin==1) disabled=e.high;
      if(e.pin==5 && !e.high) assert(!pos && !neg && disabled);
    }
    assert(!d._pin_scan_dir.value && !d._pin_pos_pwr.value && !d._pin_neg_pwr.value);
    assert(d._pin_pwr_dis.value && !d._pin_mode.value && !d._pin_oe.value);
  }
}
'''
        with tempfile.TemporaryDirectory(prefix="abyss-power-") as folder:
            cpp = Path(folder) / "power.cpp"
            exe = Path(folder) / "power.exe"
            cpp.write_text(harness, encoding="utf-8")
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
