"""Run production request/pause code and sync policy without USB or Wi-Fi."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
FW = TOOLS.parent


def source(name):
    return (FW / "src" / name).read_text(encoding="utf-8")


class SyncRegressionTests(unittest.TestCase):
    def test_actual_connect_with_slow_home_fallback_failure_and_cancel(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ required")
        network = source("network_service.cpp")
        connect = "bool NetworkService::connect(" + network.split("bool NetworkService::connect(", 1)[1].split("void NetworkService::disconnect()", 1)[0]
        header = (FW / "include" / "network_service.h").read_text(encoding="utf-8")
        status = "struct NetworkStatus {" + header.split("struct NetworkStatus {", 1)[1].split("struct OpdsProbeResult", 1)[0]
        harness = r'''
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <vector>
#include <string>
#include "sync_policy.h"
uint32_t now = 0, cancelAt = UINT32_MAX;
uint32_t millis() { return now; }
void delay(uint32_t ms) { now += ms; }
void reportWorkProgress() {}
void setError(char *out, size_t size, const char *error) { snprintf(out, size, "%s", error); }
namespace AutomaticSync {
bool ownsSession = true;
bool cancelRequested() { return now >= cancelAt; }
bool ownsNetworkSession() { return ownsSession; }
}
constexpr int WIFI_STA = 1, WL_CONNECTED = 3;
struct WifiCredential { char ssid[33] = "phone"; char password[65] = "dummy"; };
struct Configuration { char wifiSsid[33] = "home"; char wifiPassword[65] = "dummy"; };
bool hasFallback = true, clockWorks = true;
struct ProvisioningStore {
    static bool loadFallbackWifi(WifiCredential &, char *, size_t) { return hasFallback; }
};
struct FakeWifi {
    uint32_t homeDelay = UINT32_MAX, phoneDelay = UINT32_MAX, began = 0, wait = UINT32_MAX;
    bool attempting = false, on = false;
    std::vector<std::string> attempts;
    int status() { return attempting && wait != UINT32_MAX && now - began >= wait ? WL_CONNECTED : 0; }
    void begin(const char *ssid, const char *) {
        on = attempting = true; began = now; attempts.emplace_back(ssid);
        wait = attempts.back() == "home" ? homeDelay : phoneDelay;
    }
    void persistent(bool) {}
    void mode(int mode) { on = mode != 0; }
    void setSleep(bool) {}
    void setAutoReconnect(bool) {}
    void setHostname(const char *) {}
    void disconnect(bool, bool) { attempting = false; }
    int RSSI() { return -50; }
} WiFi;
''' + status + r'''
class NetworkService {
public:
    Configuration configuration_;
    NetworkStatus lastStatus_;
    bool connect(NetworkStatus &, uint32_t, uint8_t = 0);
    bool loadConfiguration(char *, size_t) { return true; }
    bool synchronizeTime(char *error, size_t capacity) {
        setError(error, capacity, clockWorks ? "" : "time-sync-timeout"); return clockWorks;
    }
    void disconnect() { WiFi.disconnect(true, false); WiFi.mode(0); }
};
''' + connect + r'''
void reset() { now = 0; cancelAt = UINT32_MAX; clockWorks = true; WiFi = FakeWifi{}; }
int main() {
    NetworkService network;
    NetworkStatus status;
    AutomaticSync::ownsSession = false;
    assert(!network.connect(status, ReaderSyncPolicy::kWifiAttemptMs));
    assert(strcmp(status.error, "sync-only") == 0 && !WiFi.on && WiFi.attempts.empty());
    // A non-owner must not change the radio underneath an active sync owner.
    WiFi.on = true;
    assert(!network.connect(status, ReaderSyncPolicy::kWifiAttemptMs));
    assert(WiFi.on && WiFi.attempts.empty());
    AutomaticSync::ownsSession = true;
    reset();
    WiFi.homeDelay = 8000;
    assert(network.connect(status, ReaderSyncPolicy::kWifiAttemptMs));
    assert(status.credentialSlot == 1 && WiFi.attempts.size() == 1 && now == 8000);
    reset(); WiFi.phoneDelay = 6000;
    assert(network.connect(status, ReaderSyncPolicy::kWifiAttemptMs));
    assert(status.credentialSlot == 2 && WiFi.attempts.size() == 2 && now >= 26000);
    reset();
    assert(!network.connect(status, ReaderSyncPolicy::kWifiAttemptMs));
    assert(WiFi.attempts.size() == 2 && now < 41000 && !WiFi.on);
    assert(strcmp(status.error, "wifi-connect-timeout") == 0);
    reset(); cancelAt = 1500;
    assert(!network.connect(status, ReaderSyncPolicy::kWifiAttemptMs));
    assert(now <= 1600 && WiFi.attempts.size() == 1 && !WiFi.on);
    reset(); WiFi.phoneDelay = 1000;
    assert(network.connect(status, ReaderSyncPolicy::kWifiAttemptMs, 2));
    assert(WiFi.attempts.size() == 1 && WiFi.attempts.front() == "phone");
    clockWorks = false; // Already associated but no trustworthy clock.
    assert(!network.connect(status, ReaderSyncPolicy::kWifiAttemptMs));
    assert(!WiFi.on && strcmp(status.error, "time-sync-timeout") == 0);
    puts("SYNC_WIFI_POLICY_OK");
}
'''
        with tempfile.TemporaryDirectory(prefix="reader-wifi-test-") as directory:
            cpp = Path(directory) / "wifi.cpp"
            binary = Path(directory) / "wifi.exe"
            cpp.write_text(harness, encoding="utf-8")
            built = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra",
                                    "-I", str(FW / "include"), str(cpp), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            tested = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(tested.returncode, 0, tested.stderr)
            self.assertIn("SYNC_WIFI_POLICY_OK", tested.stdout)

    def test_actual_request_resumes_idle_but_preserves_active_owners(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ required")
        sync = source("automatic_sync.cpp")
        request = sync.split("bool request(", 1)[1].split("void cancel()", 1)[0]
        pause = sync.split("void setPaused(", 1)[1].split("bool busy()", 1)[0]
        harness = r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "sync_policy.h"
#include "sync_progress.h"
SyncProgress::Snapshot syncProgress;
int progressGate=0;
uint32_t millis(){return 1234;}
enum class Status { Idle, Running, Failed, Cancelling };
std::atomic<bool> running{false}, paused{false}, cancelled{false}, finished{false};
std::atomic<unsigned> deliveredCount{0};
std::atomic<Status> syncStatus{Status::Idle};
std::atomic<ReaderSyncPolicy::Error> lastError{ReaderSyncPolicy::Error::None};
std::atomic<int> lastHttpCode{0};
void *syncTask = reinterpret_cast<void *>(1);
int syncGate = 0, notifications = 0;
namespace WifiSetup { bool opened = false; bool active() { return opened; } }
void start() {}
void portENTER_CRITICAL(int *) {}
void portEXIT_CRITICAL(int *) {}
void xTaskNotifyGive(void *) { ++notifications; }
''' + "bool request(" + request + "void setPaused(" + pause + r'''
int main() {
    using namespace ReaderSyncPolicy;
    setPaused(true); // Previous USB diagnostic, no task owns SD.
    assert(request(false));
    assert(!paused && running && notifications == 1 && lastError == Error::None);
    setPaused(true); // Cancel is still draining network/SD.
    assert(!request(false));
    assert(paused && running && notifications == 1);
    running = false;
    WifiSetup::opened = true;
    assert(!request(false) && lastError == Error::Busy && notifications == 1);
    WifiSetup::opened = false;
    for (int owner = 0; owner < 4; ++owner) { // Provision, upload, preparation, sleep.
        assert(!request(true));
        assert(paused && !running && notifications == 1 && lastError == Error::Busy);
    }
    cancelled = true; finished = true; deliveredCount = 4; lastHttpCode = 401;
    assert(request(false));
    assert(!paused && !cancelled && !finished && deliveredCount == 0 && lastHttpCode == 0);
    assert(notifications == 2);
    running = false; syncTask = nullptr;
    assert(!request(false));
    assert(lastError == Error::Memory && notifications == 2);
    assert(isReadOnlyStatus("PROVISION STATUS"));
    assert(isReadOnlyStatus("NETWORK STATUS"));
    assert(isReadOnlyStatus("WIFI FALLBACK STATUS"));
    assert(isReadOnlyStatus("STORAGE RECOVERY STATUS"));
    assert(!isReadOnlyStatus("PROVISION BEGIN"));
    assert(!isReadOnlyStatus("NETWORK CONNECT"));
    assert(!isReadOnlyStatus("UPLOAD BEGIN"));
    assert(httpError(401) == Error::Authentication);
    assert(httpError(403) == Error::Authentication);
    assert(httpError(-1) == Error::Transport);
    assert(httpError(503) == Error::Http);
    assert(connectionError("time-sync-timeout") == Error::Clock);
    assert(connectionError("wifi-connect-timeout") == Error::Wifi);
    assert(connectionError("not-configured") == Error::Configuration);
    assert(downloadError("unexpected-http-status", 401) == Error::Authentication);
    assert(downloadError("incomplete-response", 200) == Error::Transport);
    assert(downloadError("transfer-timeout", 200) == Error::Transport);
    assert(downloadError("sd-verification-failed", 200) == Error::Storage);
    assert(downloadError("invalid-fb2-body", 200) == Error::Protocol);
    assert(downloadError("transfer-allocation-failed", 200) == Error::Memory);
    assert(kWifiAttemptMs >= 20000);
    for (unsigned i = 0; i <= static_cast<unsigned>(Error::Preparation); ++i) {
        assert(strlen(errorLabel(static_cast<Error>(i))) < 96);
    }
    puts("SYNC_REQUEST_POLICY_OK");
}
'''
        with tempfile.TemporaryDirectory(prefix="reader-sync-test-") as directory:
            cpp = Path(directory) / "sync.cpp"
            binary = Path(directory) / "sync.exe"
            cpp.write_text(harness, encoding="utf-8")
            built = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    "-I", str(FW / "include"), str(cpp), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            tested = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(tested.returncode, 0, tested.stderr)
            self.assertIn("SYNC_REQUEST_POLICY_OK", tested.stdout)

    def test_home_and_usb_share_exclusive_ownership_guards(self):
        main = source("main.cpp")
        calls = main.split("AutomaticSync::request(")[1:]
        self.assertEqual(len(calls), 2)  # Library and USB.
        for call in calls:
            condition = call.split(";", 1)[0]
            for guard in ("provisioningActive", "bookUpload.active()", "BookPreparation::busy()", "pendingPowerSleep"):
                self.assertIn(guard, condition)
        self.assertIn("!ReaderSyncPolicy::isReadOnlyStatus(line)", main)
        self.assertIn("ReaderSyncPolicy::errorLabel(AutomaticSync::error())", source("bookish_adapter.inc"))

    def test_network_does_not_share_http_timeout_or_fallback_budget(self):
        network = source("network_service.cpp")
        connect = network.split("bool NetworkService::connect(", 1)[1].split("void NetworkService::disconnect", 1)[0]
        self.assertNotIn("5000U", connect)
        self.assertNotIn("timeoutMs -", connect)
        self.assertIn("tryCredential(fallback.ssid, fallback.password, timeoutMs)", connect)
        request = network.split("bool NetworkService::syncRequest(", 1)[1].split("bool NetworkService::verifiedLocalDigest", 1)[0]
        self.assertIn("connect(connection, ReaderSyncPolicy::kWifiAttemptMs)", request)
        self.assertIn("http.setTimeout(timeoutMs)", request)
        self.assertIn("client.setCACert(kTrustedRoots)", request)
        self.assertNotIn("setInsecure", request)
        self.assertIn("ReaderSyncPolicy::httpError(result.httpCode)", request)

    def test_queue_failure_continues_only_after_durable_acknowledgement(self):
        sync = source("automatic_sync.cpp")
        worker = sync.split("void worker(", 1)[1].split("namespace AutomaticSync", 1)[0]
        self.assertIn("unsigned long long after = 0;", worker)
        self.assertIn("&v=2&after=%llu", worker)
        self.assertIn("jobNumber <= after", worker)
        self.assertIn("after = jobNumber;", worker)
        self.assertIn("if (!ack) break;", worker)
        self.assertIn("if (!acknowledged) break;", worker)
        self.assertEqual(worker.count("if (!ok) { hadFailures = true; continue; }"), 2)
        self.assertIn("complete && !hadFailures", worker)
        network = source("network_service.cpp")
        self.assertIn("kSdWriteBlockBytes = 512;", network)
        self.assertIn("::fsync(fd)", network)
        self.assertEqual(network.count("output.finish()"), 2)

    def test_build_does_not_embed_a_private_profile(self):
        config = (FW / "platformio.ini").read_text(encoding="utf-8")
        self.assertNotIn("ABYSS_WIFI", config)
        self.assertNotIn("ABYSS_OPDS", config)
        self.assertIn(".env", (FW / ".gitignore").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
