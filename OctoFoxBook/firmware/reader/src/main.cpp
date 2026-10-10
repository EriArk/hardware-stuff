#include "i18n.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <atomic>
#include <Preferences.h>
#include "battery_policy.h"
#include "list_paging.h"
#include "text_keyboard.h"
#include "wifi_credentials.h"
#include "wifi_setup.h"

#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include "automatic_sync.h"
#include "book_preparation.h"
#include <esp_system.h>

#include "book_upload.h"
#include "cover_cache.h"
#include "display_refresh_controller.h"
#include "download_queue.h"
#include "epd_driver.h"
#include "fb2_cache.h"
#include "favorites_store.h"
#include "collection_store.h"
#include "local_library.h"
#include "home_layout.h"
#include "network_service.h"
#include "tls_memory.h"
#include "opds_parser.h"
#include "provisioning_store.h"
#include "reader_pagination.h"
#include "reader_input_policy.h"
#include "reader_settings.h"
#include "reader_text.h"
#include "recent_books.h"
#include "roboto12.h"
#include "roboto18.h"
#include "roboto32.h"
#include "storage_recovery.h"
#include "ui_condensed9.h"
#include "ui_condensed13.h"
#include "ui_condensed13_bold.h"
#include "ui_condensed14_bold.h"
#include "ui_condensed16_bold.h"
#include "ui_condensed22_medium.h"
#include "utilities.h"
#include "work_progress.h"
#include "bookish_ui.h"
#include "section_back_focus.h"
#include "bookish_logo.h"
#include "bookish_fonts/BookishLora25.h"
#include "bookish_fonts/BookishLora31.h"
#include "bookish_fonts/BookishLora39.h"
#include "bookish_fonts/BookishArimo10.h"
#include "bookish_fonts/BookishArimo18.h"
#include "bookish_fonts/BookishArimo22.h"


namespace {

constexpr char kFirmwareName[] = "abyss-reader";
constexpr char kFirmwareVersion[] = "0.9.0-rc.1";
constexpr size_t kFramebufferBytes = EPD_WIDTH * EPD_HEIGHT / 2;
constexpr int32_t kPortraitWidth = EPD_HEIGHT;
constexpr int32_t kPortraitHeight = EPD_WIDTH;
constexpr size_t kPsramTestBytes = 1024 * 1024;
constexpr uint32_t kDebounceMs = 20;
constexpr uint32_t kLongPressMs = 800;
std::atomic<bool> sleepSwitchLatching{false};
constexpr uint32_t kPanelCleanHoldMs = 6000;
constexpr uint32_t kDoublePressMs = 350;
constexpr uint32_t kNavigationCoalesceMs = 0;
constexpr uint32_t kIdlePreparationDelayMs = 5000;
constexpr uint32_t kPreparationRetryDelayMs = 5U * 60U * 1000U;
constexpr uint32_t kSleepContextMagic = 0x41535953U;
constexpr uint16_t kSleepContextVersion = 1;
// UP/DOWN/OK are momentary. Sleep type is a persistent runtime preference (momentary by default).
// Side pad -> switch -> GND.
// GPIO45 is an ESP32-S3 strapping pin, so CENTER must not be held at reset.
constexpr uint8_t kUpPin = 39;      // side pad CS
constexpr uint8_t kDownPin = 48;    // side pad MSIO
constexpr uint8_t kCenterPin = 45;  // side pad SCLK (silkscreen may say SCL)
constexpr uint8_t kPowerPin = 10;   // side pad MOSI, RTC deep-sleep wake
constexpr uint8_t kCleanPin = 21;   // on-board SENSOP_VN, not a solder target
constexpr Rect_t kInputCardArea{496, 338, 426, 126};
constexpr size_t kLocalLibraryCapacity = 256;
constexpr size_t kLibraryRowsPerScreen = BookishUI::kListRows;
constexpr size_t kCatalogRowsPerScreen = 8;
constexpr size_t kCatalogHistoryCapacity = 8;
constexpr size_t kSearchRowsPerScreen = BookishUI::kListRows;
constexpr size_t kBulkDownloadLimit = 8;

enum class DashboardRefreshRequest : uint8_t {
    None = 0,
    InputRegion = 1,
    QualityFull = 2,
};

enum class UiScreen : uint8_t {
    None = 0,
    Home = 1,
    LocalLibrary = 2,
    TopLevel = 3,
    Reader = 4,
    Preparation = 5,
    Catalog = 6,
    Search = 7,
    Download = 8,
    BookCard = 9,
    Annotation = 10,
    Favorites = 11,
    ReaderMenu = 12,
    Contents = 13,
    Bookmarks = 14,
    ReadingSettings = 15,
    OrderedSelector = 16,
    BulkDownloadConfirm = 17,
    Sections = 18,
    DeviceSettings = 19,
    Wifi = 20,
    CollectionName = 21,
};

enum class TopLevelTab : uint8_t {
    Home = 0,
    OnDevice = 1,
    Catalog = 2,
    Search = 3,
    Favorites = 4,
};

constexpr TopLevelTab kVisibleTabs[] = {TopLevelTab::Home, TopLevelTab::OnDevice,
                                      TopLevelTab::Search, TopLevelTab::Favorites};
constexpr size_t kVisibleTabCount = sizeof(kVisibleTabs) / sizeof(kVisibleTabs[0]);

struct Diagnostics {
    bool psramDetected = false;
    bool psramPassed = false;
    size_t psramBytes = 0;
    size_t freePsramBytes = 0;
    size_t flashBytes = 0;
    bool sdMounted = false;
    bool sdWritePassed = false;
    uint64_t sdBytes = 0;
    bool rtcDetected = false;
    uint16_t batteryRaw = 0;
    uint16_t batteryMillivolts = 0;
    uint8_t batteryPercent = 0;
    esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;
};

struct SleepContext {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint8_t screen = 0;
    uint8_t activeTab = 0;
    uint8_t readerReturnTab = 0;
    uint8_t reserved[3]{};
    char bookId[33]{};
    uint32_t pageCount = 0;
    uint32_t chapterCount = 0;
    uint32_t checksum = 0;
};

struct ButtonTracker {
    ButtonTracker(uint8_t pinValue, const char *nameValue, bool allowDoubleValue)
        : pin(pinValue), name(nameValue), allowDouble(allowDoubleValue) {}

    uint8_t pin;
    const char *name;
    bool allowDouble;
    bool rawLevel = HIGH;
    bool stableLevel = HIGH;
    uint32_t rawChangedAt = 0;
    uint32_t pressedAt = 0;
    bool clickPending = false;
    uint32_t clickReleasedAt = 0;
    NavigationRepeat repeat;
    OkGesture okGesture;
};

struct ReaderSession {
    bool active = false;
    char bookId[33]{};
    uint32_t currentPage = 0;
    uint32_t pageCount = 0;
    uint32_t chapterCount = 0;
};

enum class LocalLibraryPhase : uint8_t {
    Sections = 0,
    Groups = 1,
    Books = 2,
};

enum class LocalLibrarySection : uint8_t {
    All = 0,
    Recent = 1,
    Reading = 2,
    Unread = 3,
    Finished = 4,
    Authors = 5,
    Series = 6,
    Genres = 7,
    Search = 8,
};

struct LocalLibrarySession {
    LocalBookEntry *entries = nullptr;
    LocalLibraryInfo info{};
    uint16_t visible[kLocalLibraryCapacity]{};
    size_t visibleCount = 0;
    char (*groups)[128] = nullptr;
    size_t groupCount = 0;
    char groupLabel[128]{};
    char searchPrefix[8]{};
    size_t selected = 0;
    size_t firstVisible = 0;
    bool loaded = false;
    LocalLibraryPhase phase = LocalLibraryPhase::Sections;
    LocalLibrarySection section = LocalLibrarySection::All;
    TopLevelTab owner = TopLevelTab::OnDevice;
};

struct HomeSession {
    LocalBookEntry entries[HomeLayout::kReadingCapacity]{};
    LocalBookEntry added[HomeLayout::kAddedCapacity]{};
    size_t count = 0;
    size_t addedCount = 0;
    size_t selected = 0;
    uint32_t localBookCount = 0;
    bool loaded = false;
};

struct CatalogSession {
    OpdsFeed *feed = nullptr;
    size_t selected = 0;
    size_t firstVisible = 0;
    bool loaded = false;
    char currentUrl[kOpdsUrlCapacity]{};
    char history[kCatalogHistoryCapacity][kOpdsUrlCapacity]{};
    size_t historyDepth = 0;
    TopLevelTab owner = TopLevelTab::Catalog;
    enum class Relation : uint8_t {
        None = 0,
        Author = 1,
        Series = 2,
    } relation = Relation::None;
    char relationLabel[192]{};
};

enum class SearchPhase : uint8_t {
    Scope = 0,
    Range = 1,
    Letter = 2,
};

enum class SearchScope : uint8_t {
    All = 0,
    Books = 1,
    Authors = 2,
    Series = 3,
    Local = 4,
};

struct SearchSession {
    SearchPhase phase = SearchPhase::Scope;
    SearchScope scope = SearchScope::All;
    size_t range = 0;
    size_t selected = 0;
    size_t firstVisible = 0;
    char prefix[8]{};
};

enum class FavoritesPhase : uint8_t {
    Folders = 0,
    Books = 1,
};

struct FavoritesSession {
    FavoritesPhase phase = FavoritesPhase::Folders;
    size_t folder = 0;
    size_t selected = 0;
    size_t firstVisible = 0;
    bool loaded = false;
};

enum class BookCardFocus : uint8_t {
    Primary = 0,
    FullAnnotation = 1,
    Author = 2,
    Series = 3,
    Favorite = 4,
    Collections = 5,
    Back = 6,
};

struct BookCardSession {
    bool active = false;
    bool local = false;
    bool localCopyPresent = false;
    bool downloading = false;
    bool preparing = false;
    bool downloadFailed = false;
    UiScreen returnScreen = UiScreen::Home;
    TopLevelTab returnTab = TopLevelTab::Home;
    BookCardFocus focus = BookCardFocus::Primary;
    LocalBookEntry localEntry{};
    OpdsEntry remoteEntry{};
    char bookId[33]{};
    uint8_t annotationPage = 0;
    bool favoritePickerOpen = false;
    bool favoriteStored = false;
    size_t collectionSelection = 0;
    uint32_t displayedAt = 0;
    bool coverPreparationAttempted = false;
};

enum class ReaderMenuFocus : uint8_t {
    Contents = 0,
    Bookmarks = 1,
    BookInfo = 2,
    ReadingSettings = 3,
    Favorite = 4,
    MarkRead = 5,
    Close = 6,
};

struct ReaderMenuSession {
    ReaderMenuFocus focus = ReaderMenuFocus::Contents;
};

struct ContentsSession {
    uint32_t selected = 0;
    uint32_t firstVisible = 0;
};

struct ReaderBookmarksSession {
    ReaderUserState state{};
    size_t selected = 0;
    size_t firstVisible = 0;
};

enum class ReadingSettingsFocus : uint8_t {
    TextSize = 0,
    LineSpacing = 1,
};

enum class OrderedSelectorKind : uint8_t {
    TextSize = 0,
    LineSpacing = 1,
};

struct ReadingSettingsSession {
    ReaderSettings settings{};
    ReadingSettingsFocus focus = ReadingSettingsFocus::TextSize;
};

struct OrderedSelectorSession {
    OrderedSelectorKind kind = OrderedSelectorKind::TextSize;
    uint8_t originalValue = 1;
    uint8_t value = 1;
};

struct BulkDownloadSession {
    bool active = false;
    CatalogSession::Relation relation = CatalogSession::Relation::None;
    char label[192]{};
    BookDownloadJob jobs[kBulkDownloadLimit]{};
    size_t count = 0;
    size_t next = 0;
    size_t completed = 0;
    size_t skipped = 0;
    uint8_t selected = 0;
};

enum class CatalogRowKind : uint8_t {
    Entry = 0,
    PreviousPage = 1,
    NextPage = 2,
    BulkDownload = 3,
};

struct CatalogRow {
    CatalogRowKind kind = CatalogRowKind::Entry;
    const OpdsEntry *entry = nullptr;
    const char *href = nullptr;
    const char *title = nullptr;
};

struct BookPreparationResult {
    Fb2CacheInfo cache{};
    ReaderPaginationInfo pagination{};
    bool ok = false;
    bool cacheReused = false;
    bool paginationReused = false;
    uint32_t cacheDurationMs = 0;
    uint32_t paginationDurationMs = 0;
    char error[64]{};
};

// Fb2CacheInfo carries the full annotation, so BookPreparationResult is more
// than two kilobytes.  Keeping one serially reused workspace out of the 8 KiB
// Arduino loop stack leaves enough headroom for SD/VFS and XML parsing calls.
BookPreparationResult bookPreparationWorkspace{};

enum class LongOperationKind : uint8_t {
    StartupRecovery = 0,
    Network = 1,
    BookDownload = 2,
    BookPreparation = 3,
    CoverIndexRebuild = 4,
    Ota = 5,
};

struct LongOperationModel {
    LongOperationKind kind = LongOperationKind::BookPreparation;
    const char *subject = "";
    const char *detail = "";
    const char *identifier = "";
    const char *error = "";
    bool failed = false;
    bool cacheReady = false;
};

struct MalformedBookTestResult {
    bool ok = false;
    bool parserRejected = false;
    bool originalPreserved = false;
    bool partialsAbsent = false;
    bool cleanupPassed = false;
    char parserError[64]{};
};

struct PortraitTextLines {
    static constexpr size_t kMaxLines = 3;
    static constexpr size_t kLineCapacity = 192;
    char line[kMaxLines][kLineCapacity]{};
    size_t count = 0;
    bool overflow = false;
};

Diagnostics diagnostics;
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR SleepContext sleepContext{};
DisplayRefreshController displayRefresh;
BookUploadReceiver bookUpload;
ReaderSession readerSession;
LocalLibrarySession localLibrarySession;
HomeSession homeSession;
CatalogSession catalogSession;
SearchSession searchSession;
FavoritesSession favoritesSession;
BookCardSession bookCardSession;
ReaderMenuSession readerMenuSession;
ContentsSession contentsSession;
ReaderBookmarksSession readerBookmarksSession;
ReadingSettingsSession readingSettingsSession;
OrderedSelectorSession orderedSelectorSession;
ReaderSettings activeReaderSettings;
BulkDownloadSession *bulkDownloadSession = nullptr;
FavoriteCollection *favoriteCollection = nullptr;
BookDownloadQueueState downloadQueue;
BookDownloadJob activeDownloadJob;
StorageRecoveryReport storageRecoveryReport;
bool storageRecoveryRan = false;
bool downloadQueueLoaded = false;
bool downloadFailureVisible = false;
UiScreen uiScreen = UiScreen::None;
TopLevelTab activeTopLevelTab = TopLevelTab::Home;
TopLevelTab readerReturnTab = TopLevelTab::OnDevice;
DisplayRefreshResult lastDisplayRefresh{};
bool hasDisplayRefresh = false;
constexpr uint8_t kUiNavigationClicksPerClean = 12;
uint8_t readerTurnsSinceClean = 0;
uint8_t uiNavigationClicksSinceClean = 0;
bool fullStressCompleted = false;
ButtonTracker upButton{kUpPin, "UP", false};
ButtonTracker downButton{kDownPin, "DOWN", false};
ButtonTracker centerButton{kCenterPin, "CENTER", false};
ButtonTracker powerButton{kPowerPin, "POWER", false};
ButtonTracker cleanButton{kCleanPin, "CLEAN", false};
struct QueuedInput { const char *button; const char *gesture; uint32_t capturedAt; };
QueueHandle_t physicalInputs = nullptr;
TaskHandle_t uiTaskHandle = nullptr;
TaskHandle_t inputTaskHandle = nullptr;
bool pendingPowerSleep = false;
bool pendingTabsOpen = false;
BatteryPolicy batteryPolicy;
bool batteryWarningPending=false, batteryWarningVisible=false;
bool batteryProtectionRequested=false, batteryResumeLoaded=false;
uint8_t *batteryUnderlay=nullptr;
uint8_t *framebuffer = nullptr;
bool displayInitialized = false;
char preparingBookId[33]{};
bool preparingSettings = false;
bool restoringSettings = false;
bool completingSettings = false;
ReaderSettings beforePreparationSettings{};
size_t sectionSelection = 0;
char operationNotice[96]{};
void displaySections();
bool renderPending = false;
DashboardRefreshRequest pendingRefresh = DashboardRefreshRequest::None;
uint32_t inputCount = 0;
char lastInput[32] = "NONE";
char serialLine[512] = {};
size_t serialLength = 0;
ProvisioningConfig stagedProvisioning{};
uint8_t stagedProvisioningMask = 0;
bool provisioningActive = false;
NetworkService networkService;
int8_t pendingPageDelta = 0;
int8_t pendingChapterDelta = 0;
int8_t pendingLibraryDelta = 0;
int8_t pendingHomeDelta = 0;
int8_t pendingTopLevelDelta = 0;
int8_t pendingCatalogDelta = 0;
int8_t pendingSearchDelta = 0;
int8_t pendingBookCardDelta = 0;
int8_t pendingAnnotationDelta = 0;
int8_t pendingFavoritesDelta = 0;
int8_t pendingReaderMenuDelta = 0;
int8_t pendingContentsDelta = 0;
int8_t pendingReaderBookmarksDelta = 0;
int8_t pendingReadingSettingsDelta = 0;
int8_t pendingOrderedSelectorDelta = 0;
int8_t pendingBulkDownloadDelta = 0;
uint32_t pendingPageNavigationAt = 0;
uint32_t pendingChapterNavigationAt = 0;
uint32_t pendingLibraryNavigationAt = 0;
uint32_t pendingHomeNavigationAt = 0;
uint32_t pendingTopLevelNavigationAt = 0;
uint32_t pendingCatalogNavigationAt = 0;
uint32_t pendingSearchNavigationAt = 0;
uint32_t pendingBookCardNavigationAt = 0;
uint32_t pendingAnnotationNavigationAt = 0;
uint32_t pendingFavoritesNavigationAt = 0;
uint32_t pendingReaderMenuNavigationAt = 0;
uint32_t pendingContentsNavigationAt = 0;
uint32_t pendingReaderBookmarksNavigationAt = 0;
uint32_t pendingReadingSettingsNavigationAt = 0;
uint32_t pendingOrderedSelectorNavigationAt = 0;
uint32_t pendingBulkDownloadNavigationAt = 0;
uint32_t libraryInteractionAt = 0;
uint32_t preparationRetryAt = 0;
char preparationRetryBookId[33]{};
bool pendingSelectedBookOpen = false;
bool pendingLocalLibraryBack = false;
bool pendingHomeBookOpen = false;
bool pendingTopLevelOpen = false;
TopLevelTab pendingTopLevelTarget = TopLevelTab::Home;
bool pendingCatalogOpen = false;
bool pendingCatalogBack = false;
bool pendingCatalogRefresh = false;
bool pendingSearchSelect = false;
bool pendingSearchBack = false;
bool pendingDownloadRetry = false;
bool pendingDownloadBack = false;
bool pendingBookCardSelect = false;
bool pendingBookCardBack = false;
bool pendingAnnotationBack = false;
bool pendingFavoritesSelect = false;
bool pendingFavoritesBack = false;
bool pendingReaderMenuOpen = false;
bool pendingReaderMenuSelect = false;
bool pendingReaderMenuBack = false;
bool pendingContentsSelect = false;
bool pendingContentsBack = false;
bool pendingReaderBookmarksSelect = false;
bool pendingReaderBookmarksBack = false;
bool pendingReadingSettingsSelect = false;
bool pendingReadingSettingsBack = false;
bool pendingOrderedSelectorSelect = false;
bool pendingOrderedSelectorBack = false;
bool pendingBulkDownloadSelect = false;
bool pendingBulkDownloadBack = false;
char pendingUsbReaderOpenId[33]{};
bool pendingUsbReaderPage = false;
uint32_t pendingUsbReaderPageNumber = 0;
bool pendingPanelClean = false;
bool suppressPowerUntilRelease = false;
uint32_t lastActivityAt = 0;
uint32_t idleBeforeLastCommand = 0;
uint32_t nextBatterySampleAt = 0;
uint32_t batteryRawTotal = 0;
uint32_t batteryMvTotal = 0;
uint8_t batterySampleCount = 0;

void printDisplayRefresh(const char *recordType,
                         const DisplayRefreshResult &result);
bool displayCatalogUrl(const char *url, bool pushHistory, bool clearHistory,
                       const char *reason);
bool displayFavorites(bool reload, const char *reason);
bool displayReaderMenu(const char *reason);
bool displayContents(bool resetSelection, const char *reason);
bool displayReaderBookmarks(bool resetSelection, const char *reason);
bool displayReadingSettings(bool reload, const char *reason);
bool displayOrderedSelector(const char *reason);
bool displayBulkDownloadConfirm(const char *reason);
bool displayBookCard(const char *reason);
bool displayLocalLibrary(bool rescan, const char *reason);
void loadBookCardFavorite();
bool openReaderBook(const char *bookId, const char *source);
bool acquisitionBookId(const char *url, char *bookId, size_t capacity);
bool localBookPresent(const char *bookId);
void closeBulkDownloadSession();
bool continueBulkDownload();
bool uiActionPending();
void processPanelCleanAction();
void processPowerAction();
void updateBatteryPolicy();
void dismissBatteryWarning();
void enterDeepSleep(const char *reason, uint32_t timerWakeSeconds = 0);

void scheduleGhostCleanup(const char *reason) {
    displayRefresh.requestHardClearBeforeNextRefresh();
    readerTurnsSinceClean = 0;
    uiNavigationClicksSinceClean = 0;
    Serial.printf("DISPLAY CLEAN SCHEDULED reason=%s\n", reason);
}

bool syncReturnCleanupPending = false;

void scheduleScreenTransitionCleanup(UiScreen target, const char *reason) {
    // Defer until the destination is rendered: its SD scan may draw a busy frame.
    if (syncReturnCleanupPending) {
        scheduleGhostCleanup("sync-finished");
        syncReturnCleanupPending = false;
    } else if (uiScreen != target) {
        scheduleGhostCleanup(reason);
    }
}

void noteReaderPageTurn() {
    if (++readerTurnsSinceClean >= activeReaderSettings.readingClearEvery) {
        scheduleGhostCleanup("reader-page-interval");
    }
}

bool isUiNavigationScreen(UiScreen screen) {
    switch (screen) {
        case UiScreen::Home:
        case UiScreen::LocalLibrary:
        case UiScreen::Catalog:
        case UiScreen::Search:
        case UiScreen::Favorites:
        case UiScreen::BookCard:
        case UiScreen::Annotation:
        case UiScreen::ReaderMenu:
        case UiScreen::Contents:
        case UiScreen::Bookmarks:
        case UiScreen::ReadingSettings:
        case UiScreen::OrderedSelector:
        case UiScreen::BulkDownloadConfirm:
            return true;
        default:
            return false;
    }
}

void noteUiNavigationClick() {
    if (++uiNavigationClicksSinceClean >= kUiNavigationClicksPerClean) {
        scheduleGhostCleanup("ui-click-12");
    }
}

uint8_t batteryPercentFromMillivolts(uint16_t millivolts) {
    struct CurvePoint {
        uint16_t millivolts;
        uint8_t percent;
    };
    static constexpr CurvePoint curve[] = {
        {3300, 0},  {3500, 4},  {3600, 10}, {3700, 25},
        {3800, 45}, {3900, 65}, {4000, 80}, {4100, 90},
        {4200, 100},
    };
    if (millivolts <= curve[0].millivolts) {
        return curve[0].percent;
    }
    for (size_t index = 1; index < sizeof(curve) / sizeof(curve[0]);
         ++index) {
        if (millivolts <= curve[index].millivolts) {
            const uint32_t span =
                curve[index].millivolts - curve[index - 1].millivolts;
            const uint32_t offset = millivolts - curve[index - 1].millivolts;
            const uint32_t percentSpan =
                curve[index].percent - curve[index - 1].percent;
            return static_cast<uint8_t>(
                curve[index - 1].percent +
                (offset * percentSpan + span / 2U) / span);
        }
    }
    return 100;
}

void drawText(const GFXfont *font, const char *text, int32_t x, int32_t y,
              uint8_t foreground = 0, uint8_t background = 15,
              bool drawBackground = false) {
    FontProperties properties{};
    properties.fg_color = foreground & 0x0F;
    properties.bg_color = background & 0x0F;
    properties.fallback_glyph = '?';
    properties.flags = drawBackground ? DRAW_BACKGROUND : 0;
    write_mode(font, text, &x, &y, framebuffer, BLACK_ON_WHITE, &properties);
}

const char *passLabel(bool passed) {
    return passed ? "PASS" : "FAIL";
}

void drawCornerMarks(int32_t x, int32_t y, int32_t width, int32_t height) {
    constexpr int32_t length = 16;
    epd_draw_hline(x, y, length, 0, framebuffer);
    epd_draw_vline(x, y, length, 0, framebuffer);
    epd_draw_hline(x + width - length, y, length, 0, framebuffer);
    epd_draw_vline(x + width - 1, y, length, 0, framebuffer);
    epd_draw_hline(x, y + height - 1, length, 0, framebuffer);
    epd_draw_vline(x, y + height - length, length, 0, framebuffer);
    epd_draw_hline(x + width - length, y + height - 1, length, 0, framebuffer);
    epd_draw_vline(x + width - 1, y + height - length, length, 0, framebuffer);
}

void drawStatusCard(int32_t x, int32_t y, int32_t width, int32_t height,
                    const char *index, const char *title, const char *value,
                    bool passed) {
    epd_fill_rect(x, y, width, height, 0xEE, framebuffer);
    drawCornerMarks(x, y, width, height);
    epd_fill_rect(x + 18, y + 18, 50, 34, 0, framebuffer);
    drawText(&Roboto12, index, x + 29, y + 44, 15, 0);
    drawText(&Roboto12, title, x + 82, y + 43);
    drawText(&Roboto18, value, x + 20, y + 97);

    const int32_t badgeWidth = 80;
    const int32_t badgeX = x + width - badgeWidth - 18;
    epd_draw_rect(badgeX, y + height - 43, badgeWidth, 26, 0, framebuffer);
    drawText(&Roboto12, passLabel(passed), badgeX + 14, y + height - 23);
}

uint8_t readNativeShade(const uint8_t *buffer, int32_t x, int32_t y) {
    const uint8_t packed =
        buffer[static_cast<size_t>(y) * (EPD_WIDTH / 2) + x / 2];
    return (x & 1) == 0 ? packed & 0x0F : packed >> 4;
}

void setPortraitPixel(int32_t logicalX, int32_t logicalY, uint8_t shade) {
    if (logicalX < 0 || logicalX >= kPortraitWidth || logicalY < 0 ||
        logicalY >= kPortraitHeight) {
        return;
    }

    // The product coordinate system is always 540x960 portrait. On the actual
    // assembled reader, its photographed top edge maps to the native panel's
    // right edge, so logical portrait coordinates rotate counter-clockwise.
    const int32_t physicalX = EPD_WIDTH - 1 - logicalY;
    const int32_t physicalY = logicalX;
    uint8_t *packed = framebuffer +
                      static_cast<size_t>(physicalY) * (EPD_WIDTH / 2) +
                      physicalX / 2;
    shade &= 0x0F;
    if ((physicalX & 1) == 0) {
        *packed = static_cast<uint8_t>((*packed & 0xF0) | shade);
    } else {
        *packed = static_cast<uint8_t>((*packed & 0x0F) | (shade << 4));
    }
}

void fillPortraitRect(int32_t x, int32_t y, int32_t width, int32_t height,
                      uint8_t shade) {
    for (int32_t yy = y; yy < y + height; ++yy) {
        for (int32_t xx = x; xx < x + width; ++xx) {
            setPortraitPixel(xx, yy, shade);
        }
    }
}

uint32_t busyNextFrameAt = 0;
uint8_t busyFrame = 0;
bool busyPainted = false;
// Portrait x=20..520, y=876..960: status, button and loading indicator.
constexpr Rect_t kSyncFooterRegion{0, 20, 84, 500};

void drawBusyIndicator() {
#ifndef USE_EPD_PAINTER
    if (busyPainted) {
        return;
    }
#endif
    if (static_cast<int32_t>(millis() - busyNextFrameAt) < 0) {
        return;
    }
    if (displayRefresh.busy()) return; // Never queue animation over unfinished text.
    const bool syncHome = AutomaticSync::busy() && uiScreen == UiScreen::Home;
    const int32_t left = syncHome ? 376 : 210, top = syncHome ? 878 : 938;
    constexpr int32_t width = 120, height = 20;
    static uint8_t saved[width * height];
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            saved[y * width + x] = readNativeShade(
                framebuffer, EPD_WIDTH - 1 - (top + y), left + x);
        }
    }
    fillPortraitRect(left, top, width, height, 15);
    for (int32_t crystal = 0; crystal < 5; ++crystal) {
        const int32_t radius = crystal == busyFrame % 5 ? 6 : 3;
        for (int32_t y = -radius; y <= radius; ++y) {
            const int32_t halfWidth = radius - abs(y);
            fillPortraitRect(left + 12 + crystal * 24 - halfWidth,
                             top + 10 + y, halfWidth * 2 + 1, 1, 0);
        }
    }
    const Rect_t indicatorRegion{EPD_WIDTH - top - height, left, height, width};
    const auto result = displayRefresh.refresh(framebuffer,
        syncHome ? DisplayRefreshMode::RecoveryRegion : DisplayRefreshMode::QualityFull,
        syncHome && busyFrame % 4 == 0 ? kSyncFooterRegion : indicatorRegion);
    // The display mailbox owns a snapshot; restore the real screen beneath
    // the indicator without touching the in-flight panel buffer.
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            setPortraitPixel(left + x, top + y, saved[y * width + x]);
        }
    }
    busyPainted = true;
    ++busyFrame;
    // No tight animation loop or fake percentage; legacy full-clear panels
    // get a static indicator instead of repeated flashing.
#ifdef USE_EPD_PAINTER
    busyNextFrameAt = millis() + (result.ok ? (syncHome ? 4000U : 1200U) : 60000U);
#else
    (void)result;
#endif
}

class BusyIndicator {
public:
    explicit BusyIndicator(bool immediate = false) {
        if (!displayInitialized || framebuffer == nullptr ||
            uiScreen == UiScreen::None) {
            return;
        }
        previous_ = setWorkProgressCallback(drawBusyIndicator);
        owner_ = previous_ == nullptr;
        if (owner_) {
            busyFrame = 0;
            busyPainted = false;
            busyNextFrameAt = millis() + (immediate ? 0U : 300U);
        }
        installed_ = true;
        if (owner_ && immediate) {
            reportWorkProgress();
        }
    }
    ~BusyIndicator() {
        if (!installed_) {
            return;
        }
        setWorkProgressCallback(previous_);
        if (owner_ && busyPainted) {
            // Also erase the indicator on an error/early return that leaves
            // the old screen open. Never leave loading crystals stuck there.
            displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
        }
    }
    BusyIndicator(const BusyIndicator &) = delete;
    BusyIndicator &operator=(const BusyIndicator &) = delete;
private:
    WorkProgressCallback previous_ = nullptr;
    bool installed_ = false;
    bool owner_ = false;
};

void drawPortraitRect(int32_t x, int32_t y, int32_t width, int32_t height,
                      uint8_t shade, int32_t thickness = 1) {
    fillPortraitRect(x, y, width, thickness, shade);
    fillPortraitRect(x, y + height - thickness, width, thickness, shade);
    fillPortraitRect(x, y, thickness, height, shade);
    fillPortraitRect(x + width - thickness, y, thickness, height, shade);
}

void fillPortraitRoundedRect(int32_t x, int32_t y, int32_t width,
                             int32_t height, int32_t radius,
                             uint8_t shade) {
    if (width <= 0 || height <= 0) {
        return;
    }
    radius = max(0, min(radius, min(width, height) / 2));
    if (radius == 0) {
        fillPortraitRect(x, y, width, height, shade);
        return;
    }
    const int32_t radiusSquared = radius * radius;
    for (int32_t row = 0; row < height; ++row) {
        const int32_t edgeDistance = min(row, height - 1 - row);
        int32_t inset = 0;
        if (edgeDistance < radius) {
            const int32_t dy = radius - 1 - edgeDistance;
            inset = radius - static_cast<int32_t>(
                                 sqrtf(static_cast<float>(
                                     max(0, radiusSquared - dy * dy))));
        }
        fillPortraitRect(x + inset, y + row, width - inset * 2, 1, shade);
    }
}

void drawPortraitRoundedRect(int32_t x, int32_t y, int32_t width,
                             int32_t height, int32_t radius, uint8_t shade,
                             uint8_t background = 15,
                             int32_t thickness = 1) {
    if (width <= 0 || height <= 0 || thickness <= 0) {
        return;
    }
    fillPortraitRoundedRect(x, y, width, height, radius, shade);
    if (width > thickness * 2 && height > thickness * 2) {
        fillPortraitRoundedRect(x + thickness, y + thickness,
                                width - thickness * 2,
                                height - thickness * 2,
                                max(0, radius - thickness), background);
    }
}

void drawPortraitLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      uint8_t shade, int32_t thickness = 1) {
    const int32_t dx = abs(x1 - x0);
    const int32_t sx = x0 < x1 ? 1 : -1;
    const int32_t dy = -abs(y1 - y0);
    const int32_t sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;
    while (true) {
        fillPortraitRect(x0 - thickness / 2, y0 - thickness / 2, thickness,
                         thickness, shade);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int32_t doubled = error * 2;
        if (doubled >= dy) {
            error += dy;
            x0 += sx;
        }
        if (doubled <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void fillPortraitCircle(int32_t centerX, int32_t centerY, int32_t radius,
                        uint8_t shade) {
    for (int32_t y = -radius; y <= radius; ++y) {
        const int32_t halfWidth = static_cast<int32_t>(sqrtf(static_cast<float>(
            max(0, radius * radius - y * y))));
        fillPortraitRect(centerX - halfWidth, centerY + y,
                         halfWidth * 2 + 1, 1, shade);
    }
}

void drawPortraitCircle(int32_t centerX, int32_t centerY, int32_t radius,
                        uint8_t shade, uint8_t background,
                        int32_t thickness = 1) {
    fillPortraitCircle(centerX, centerY, radius, shade);
    if (radius > thickness) {
        fillPortraitCircle(centerX, centerY, radius - thickness, background);
    }
}

void drawPortraitCover(const CoverBitmap &cover, int32_t centerX,
                       int32_t centerY) {
    if (cover.pixels == nullptr || cover.width == 0 || cover.height == 0) {
        return;
    }
    const int32_t left = centerX - static_cast<int32_t>(cover.width) / 2;
    const int32_t top = centerY - static_cast<int32_t>(cover.height) / 2;
    for (uint16_t y = 0; y < cover.height; ++y) {
        for (uint16_t x = 0; x < cover.width; ++x) {
            const uint32_t ordinal =
                static_cast<uint32_t>(y) * cover.width + x;
            const uint8_t packed = cover.pixels[ordinal / 2U];
            const uint8_t shade = (ordinal & 1U) == 0
                                      ? static_cast<uint8_t>(packed & 0x0FU)
                                      : static_cast<uint8_t>(packed >> 4U);
            setPortraitPixel(left + x, top + y, shade);
        }
    }
}

uint8_t coverShadeAt(const CoverBitmap &cover, uint16_t x, uint16_t y) {
    const uint32_t ordinal = static_cast<uint32_t>(y) * cover.width + x;
    const uint8_t packed = cover.pixels[ordinal / 2U];
    return (ordinal & 1U) == 0 ? static_cast<uint8_t>(packed & 0x0FU)
                              : static_cast<uint8_t>(packed >> 4U);
}

void drawPortraitCoverFitted(const CoverBitmap &cover, int32_t centerX,
                             int32_t centerY, uint16_t maximumWidth,
                             uint16_t maximumHeight) {
    if (cover.pixels == nullptr || cover.width == 0 || cover.height == 0 ||
        maximumWidth == 0 || maximumHeight == 0) {
        return;
    }
    uint16_t width = maximumWidth;
    uint16_t height = static_cast<uint16_t>(
        max(static_cast<uint32_t>(1),
            static_cast<uint32_t>(cover.height) * width / cover.width));
    if (height > maximumHeight) {
        height = maximumHeight;
        width = static_cast<uint16_t>(
            max(static_cast<uint32_t>(1),
                static_cast<uint32_t>(cover.width) * height /
                    cover.height));
    }
    const int32_t left = centerX - static_cast<int32_t>(width) / 2;
    const int32_t top = centerY - static_cast<int32_t>(height) / 2;
    for (uint16_t y = 0; y < height; ++y) {
        const uint16_t sourceY = static_cast<uint16_t>(
            min(static_cast<uint32_t>(cover.height - 1U),
                static_cast<uint32_t>(y) * cover.height / height));
        for (uint16_t x = 0; x < width; ++x) {
            const uint16_t sourceX = static_cast<uint16_t>(
                min(static_cast<uint32_t>(cover.width - 1U),
                    static_cast<uint32_t>(x) * cover.width / width));
            setPortraitPixel(left + x, top + y,
                             coverShadeAt(cover, sourceX, sourceY));
        }
    }
}

void drawPortraitText(uint8_t *scratch, const GFXfont *font, const char *text,
                      int32_t targetX, int32_t targetBaselineY,
                      uint8_t foreground = 0, uint8_t background = 15,
                      bool preserveGray = false) {
    constexpr int32_t kScratchOriginX = 64;
    const int32_t scratchOriginY = font->ascender + 20;
    const uint8_t packedBackground =
        static_cast<uint8_t>((background << 4) | background);
    const int32_t bandTop = max(0, scratchOriginY - font->ascender - 2);
    const int32_t bandBottom = min(EPD_HEIGHT, scratchOriginY - font->descender + 2);
    memset(scratch + bandTop * EPD_WIDTH / 2, packedBackground,
           (bandBottom - bandTop) * EPD_WIDTH / 2);

    FontProperties properties{};
    properties.fg_color = !preserveGray && background == 15 ? 0 : foreground & 0x0F;
    properties.bg_color = background & 0x0F;
    properties.fallback_glyph = '?';
    properties.flags = 0;

    int32_t drawCursorX = kScratchOriginX;
    int32_t drawCursorY = scratchOriginY;
    write_mode(font, text, &drawCursorX, &drawCursorY, scratch, BLACK_ON_WHITE,
               &properties);

    // The upstream get_text_bounds() convention does not match draw_char()'s
    // baseline convention for these compressed Roboto fonts. Scan the exact
    // font ascent/descent band instead; horizontal clipping happens naturally
    // in setPortraitPixel().
    const int32_t left = kScratchOriginX - 32;
    const int32_t top = max(0, scratchOriginY - font->ascender - 2);
    const int32_t right = min(EPD_WIDTH, drawCursorX + 32);
    const int32_t bottom =
        min(EPD_HEIGHT, scratchOriginY - font->descender + 2);
    for (int32_t sourceY = top; sourceY < bottom; ++sourceY) {
        for (int32_t sourceX = left; sourceX < right; ++sourceX) {
            const uint8_t shade = readNativeShade(scratch, sourceX, sourceY);
            if (shade == (background & 0x0F)) {
                continue;
            }
            setPortraitPixel(targetX + sourceX - kScratchOriginX,
                             targetBaselineY + sourceY - scratchOriginY,
                             !preserveGray && background == 15 ? (shade < 12 ? 0 : 15) : shade);
        }
    }
}

int32_t measurePortraitText(const GFXfont *font, const char *text) {
    if (font == nullptr || text == nullptr) {
        return 0;
    }
    const size_t length = strlen(text);
    size_t cursor = 0;
    int32_t width = 0;
    while (cursor < length) {
        const uint32_t codePoint = decodeReaderUtf8(text, length, cursor);
        GFXglyph *glyph = nullptr;
        get_glyph(font, codePoint, &glyph);
        if (glyph == nullptr) {
            get_glyph(font, '?', &glyph);
        }
        if (glyph != nullptr) {
            width += glyph->advance_x;
        }
    }
    return width;
}

void fitPortraitText(const GFXfont *font, const char *text, int32_t maxWidth,
                     char *target, size_t capacity) {
    if (target == nullptr || capacity == 0) {
        return;
    }
    target[0] = '\0';
    if (text == nullptr || *text == '\0' || maxWidth <= 0) {
        return;
    }
    if (measurePortraitText(font, text) <= maxWidth) {
        snprintf(target, capacity, "%s", text);
        return;
    }

    constexpr char suffix[] = "...";
    const int32_t suffixWidth = measurePortraitText(font, suffix);
    const size_t length = strlen(text);
    size_t cursor = 0;
    size_t acceptedBytes = 0;
    int32_t width = 0;
    while (cursor < length) {
        const size_t codePointStart = cursor;
        const uint32_t codePoint = decodeReaderUtf8(text, length, cursor);
        GFXglyph *glyph = nullptr;
        get_glyph(font, codePoint, &glyph);
        if (glyph == nullptr) {
            get_glyph(font, '?', &glyph);
        }
        const int32_t advance = glyph == nullptr ? 0 : glyph->advance_x;
        if (width + advance + suffixWidth > maxWidth ||
            cursor + sizeof(suffix) > capacity) {
            cursor = codePointStart;
            break;
        }
        width += advance;
        acceptedBytes = cursor;
    }
    acceptedBytes = min(acceptedBytes, capacity - sizeof(suffix));
    memcpy(target, text, acceptedBytes);
    memcpy(target + acceptedBytes, suffix, sizeof(suffix));
}

int32_t portraitGlyphAdvance(const GFXfont *font, uint32_t codePoint) {
    GFXglyph *glyph = nullptr;
    get_glyph(font, codePoint, &glyph);
    if (glyph == nullptr) {
        get_glyph(font, '?', &glyph);
    }
    return glyph == nullptr ? 0 : glyph->advance_x;
}

void wrapPortraitText(const GFXfont *font, const char *text, int32_t maxWidth,
                      size_t maxLines, PortraitTextLines &result) {
    result = PortraitTextLines{};
    if (font == nullptr || text == nullptr || *text == '\0' || maxWidth <= 0 ||
        maxLines == 0) {
        return;
    }

    maxLines = min(maxLines, PortraitTextLines::kMaxLines);
    const size_t length = strlen(text);
    size_t source = 0;
    while (source < length && result.count < maxLines) {
        while (source < length &&
               (text[source] == ' ' || text[source] == '\t' ||
                text[source] == '\r' || text[source] == '\n')) {
            ++source;
        }
        if (source >= length) {
            break;
        }

        const size_t lineStart = source;
        size_t cursor = source;
        size_t lineEnd = source;
        size_t lastBreakStart = SIZE_MAX;
        size_t lastBreakAfter = SIZE_MAX;
        int32_t width = 0;
        while (cursor < length) {
            const size_t codePointStart = cursor;
            const uint32_t codePoint = decodeReaderUtf8(text, length, cursor);
            const int32_t advance = portraitGlyphAdvance(font, codePoint);
            if (isReaderLayoutSpace(codePoint) &&
                codePointStart > lineStart) {
                lastBreakStart = codePointStart;
                lastBreakAfter = cursor;
            }
            if (width + advance > maxWidth) {
                if (lastBreakStart != SIZE_MAX) {
                    lineEnd = lastBreakStart;
                    source = lastBreakAfter;
                } else if (codePointStart > lineStart) {
                    lineEnd = codePointStart;
                    source = codePointStart;
                } else {
                    lineEnd = cursor;
                    source = cursor;
                }
                break;
            }
            width += advance;
            lineEnd = cursor;
            source = cursor;
        }

        while (lineEnd > lineStart &&
               (text[lineEnd - 1] == ' ' || text[lineEnd - 1] == '\t' ||
                text[lineEnd - 1] == '\r' || text[lineEnd - 1] == '\n')) {
            --lineEnd;
        }
        const size_t byteCount = min(
            lineEnd - lineStart, PortraitTextLines::kLineCapacity - 1);
        memcpy(result.line[result.count], text + lineStart, byteCount);
        result.line[result.count][byteCount] = '\0';
        ++result.count;
    }
    while (source < length &&
           (text[source] == ' ' || text[source] == '\t' ||
            text[source] == '\r' || text[source] == '\n')) {
        ++source;
    }
    result.overflow = source < length;
}

void drawPortraitTextLines(uint8_t *scratch, const GFXfont *font,
                           const PortraitTextLines &lines, int32_t x,
                           int32_t firstBaselineY, int32_t lineHeight,
                           uint8_t foreground = 0,
                           uint8_t background = 15) {
    lineHeight = max(lineHeight, static_cast<int32_t>(font->advance_y) + 2);
    for (size_t index = 0; index < lines.count; ++index) {
        drawPortraitText(scratch, font, lines.line[index], x,
                         firstBaselineY +
                             static_cast<int32_t>(index) * lineHeight,
                         foreground, background);
    }
}

size_t drawWrappedParagraph(uint8_t *scratch, const GFXfont *font,
                            const char *text, int32_t x,
                            int32_t firstBaseline, int32_t maxWidth,
                            int32_t lineAdvance, size_t skipLines,
                            size_t visibleLines, uint8_t foreground = 0,
                            uint8_t background = 15) {
    lineAdvance = max(lineAdvance, static_cast<int32_t>(font->advance_y) + 2);
    const char *source =
        text == nullptr || text[0] == '\0' ? I18n::tr("АННОТАЦИЯ НЕ УКАЗАНА") : text;
    char line[256]{};
    char word[160]{};
    size_t lineCount = 0;
    size_t visible = 0;
    size_t wordLength = 0;

    const auto emitLine = [&]() {
        if (line[0] == '\0') {
            return;
        }
        if (lineCount >= skipLines && visible < visibleLines &&
            scratch != nullptr) {
            drawPortraitText(scratch, font, line, x,
                             firstBaseline +
                                 static_cast<int32_t>(visible) * lineAdvance,
                             foreground, background);
            ++visible;
        }
        ++lineCount;
        line[0] = '\0';
    };
    const auto appendWord = [&]() {
        if (wordLength == 0) {
            return;
        }
        word[wordLength] = '\0';
        char candidate[sizeof(line)]{};
        snprintf(candidate, sizeof(candidate), "%s%s%s", line,
                 line[0] == '\0' ? "" : " ", word);
        if (line[0] != '\0' &&
            measurePortraitText(font, candidate) > maxWidth) {
            emitLine();
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", candidate);
        }
        wordLength = 0;
    };

    for (size_t index = 0;; ++index) {
        const char value = source[index];
        if (value == '\0' || static_cast<unsigned char>(value) <= 0x20) {
            appendWord();
            if (value == '\n' || value == '\r') {
                emitLine();
            }
            if (value == '\0') {
                break;
            }
        } else if (wordLength + 1 < sizeof(word)) {
            word[wordLength++] = value;
        }
    }
    emitLine();
    return lineCount;
}

void drawPortraitBatteryIcon(int32_t x, int32_t y, uint8_t percent,
                             uint8_t foreground = 15,
                             uint8_t background = 0) {
    constexpr int32_t width = 30;
    constexpr int32_t height = 16;
    drawPortraitRoundedRect(x, y, width, height, 3, foreground, background, 1);
    fillPortraitRoundedRect(x + width, y + 5, 4, 6, 1, foreground);
    const uint8_t segments = percent == 0
                                 ? 0
                                 : static_cast<uint8_t>(
                                       min(4U, (static_cast<uint32_t>(percent) +
                                                24U) /
                                                   25U));
    for (uint8_t segment = 0; segment < segments; ++segment) {
        fillPortraitRoundedRect(x + 3 + segment * 6, y + 3, 5, 10, 1,
                                foreground);
    }
}

void drawPersistentBattery() {
    drawPortraitBatteryIcon(478, 24, diagnostics.batteryPercent, 0, 15);
}

void drawMainTabHeader(uint8_t *scratch, const char *title) {
    drawPortraitText(scratch, &UiCondensed16Bold, title, 28, 48, 0, 15);
    const int32_t titleWidth = measurePortraitText(&UiCondensed16Bold, title);
    const int32_t underlineWidth = min(286, max(92, titleWidth + 28));
    fillPortraitRect(28, 63, underlineWidth, 2, 0);
    fillPortraitRect(28, 68, max(54, underlineWidth - 38), 1, 9);
    drawPersistentBattery();
}

void drawScreenChrome(uint8_t *scratch, const char *eyebrow,
                      const char *title) {
    (void)scratch;
    (void)eyebrow;
    (void)title;
    drawPersistentBattery();
    if (scratch && operationNotice[0])
        drawPortraitText(scratch, &UiCondensed9, operationNotice, 28, 916);
}

void drawSoftRow(int32_t x, int32_t y, int32_t width, int32_t height,
                 bool selected) {
    fillPortraitRect(x, y, width, height, 15);
    if (selected) {
        drawPortraitRoundedRect(x, y, width, height, 10, 0, 15, 2);
        const int32_t cy = y + height / 2;
        drawPortraitLine(x + 5, cy, x + 9, cy - 4, 0);
        drawPortraitLine(x + 9, cy - 4, x + 13, cy, 0);
        drawPortraitLine(x + 13, cy, x + 9, cy + 4, 0);
        drawPortraitLine(x + 9, cy + 4, x + 5, cy, 0);
    }
}

void drawSoftOrdinalBadge(uint8_t *scratch, const char *ordinal, int32_t x,
                          int32_t y, bool selected) {
    drawPortraitRoundedRect(x, y, 38, 30, 7, selected ? 0 : 8, 15,
                            selected ? 2 : 1);
    drawPortraitText(scratch, &UiCondensed9, ordinal, x + 8, y + 21, 0, 15);
}

uint32_t localBookProgressPercent(const LocalBookEntry &entry) {
    if (!entry.hasProgress || entry.currentPage == 0 || entry.pageCount == 0) {
        return 0;
    }
    return min(100UL, static_cast<unsigned long>(
                          (static_cast<uint64_t>(entry.currentPage) * 100U) /
                          entry.pageCount));
}

void formatLocalBookStatus(const LocalBookEntry &entry, char *target,
                           size_t capacity) {
    if (entry.finished) {
        snprintf(target, capacity, I18n::tr("ПРОЧИТАНА"));
    } else if (entry.hasProgress) {
        snprintf(target, capacity, "%lu%%  %lu/%lu",
                 static_cast<unsigned long>(localBookProgressPercent(entry)),
                 static_cast<unsigned long>(entry.currentPage),
                 static_cast<unsigned long>(entry.pageCount));
    } else if (!entry.metadataReady) {
        snprintf(target, capacity, I18n::tr("НОВАЯ"));
    } else if (!entry.paginationReady) {
        snprintf(target, capacity, I18n::tr("ПОДГОТОВИТЬ"));
    } else {
        snprintf(target, capacity, I18n::tr("НЕ НАЧАТА"));
    }
}

const char *topLevelTabName(TopLevelTab tab) {
    switch (tab) {
        case TopLevelTab::Home:
            return I18n::tr("ГЛАВНАЯ");
        case TopLevelTab::OnDevice:
            return I18n::tr("НА УСТРОЙСТВЕ");
        case TopLevelTab::Catalog:
            return I18n::tr("КАТАЛОГ");
        case TopLevelTab::Search:
            return I18n::tr("ПОИСК");
        case TopLevelTab::Favorites:
            return I18n::tr("ИЗБРАННОЕ");
    }
    return I18n::tr("РАЗДЕЛ");
}

const char *topLevelTabCode(TopLevelTab tab) {
    switch (tab) {
        case TopLevelTab::Home:
            return "HOME";
        case TopLevelTab::OnDevice:
            return "ON_DEVICE";
        case TopLevelTab::Catalog:
            return "CATALOG";
        case TopLevelTab::Search:
            return "SEARCH";
        case TopLevelTab::Favorites:
            return "FAVORITES";
    }
    return "UNKNOWN";
}

void drawTopLevelTabRail(uint8_t *scratch, TopLevelTab active) {
    // No touch targets overlaying book titles. Sections use a button menu.
    (void)scratch;
    (void)active;
}

bool appendHomeEntry(const LocalBookEntry *allEntries, size_t allCount,
                     const char *bookId) {
    if (bookId == nullptr || homeSession.count >= HomeLayout::kReadingCapacity) {
        return false;
    }
    for (size_t index = 0; index < homeSession.count; ++index) {
        if (strcmp(homeSession.entries[index].id, bookId) == 0) {
            return false;
        }
    }
    for (size_t index = 0; index < allCount; ++index) {
        if (strcmp(allEntries[index].id, bookId) == 0 &&
            allEntries[index].hasProgress) {
            homeSession.entries[homeSession.count++] = allEntries[index];
            return true;
        }
    }
    return false;
}

bool loadHomeSession() {
    char previousBookId[33]{};
    const auto previousAction = HomeLayout::action(homeSession.selected, homeSession.count, homeSession.addedCount);
    if (homeSession.loaded && (previousAction == HomeLayout::Action::Continue || previousAction == HomeLayout::Action::ReadingCard)) {
        snprintf(previousBookId, sizeof(previousBookId), "%s",
                 homeSession.entries[homeSession.selected - HomeLayout::kFirstBook].id);
    } else if (homeSession.loaded && previousAction == HomeLayout::Action::AddedCard) {
        snprintf(previousBookId, sizeof(previousBookId), "%s",
                 homeSession.added[homeSession.selected - HomeLayout::kFirstBook - homeSession.count].id);
    }

    auto *allEntries = static_cast<LocalBookEntry *>(heap_caps_calloc(
        kLocalLibraryCapacity, sizeof(LocalBookEntry),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (allEntries == nullptr) {
        return false;
    }
    LocalLibraryInfo info{};
    if (!LocalLibrary::scan(allEntries, kLocalLibraryCapacity, info)) {
        heap_caps_free(allEntries);
        Serial.printf("ERROR HOME_SCAN reason=%s\n", info.error);
        return false;
    }

    // HomeSession owns multiple full LocalBookEntry values (including their
    // annotations). Aggregate assignment materializes an ~9 KiB temporary on
    // the small Arduino loop stack, so reset this POD storage in place.
    memset(&homeSession, 0, sizeof(homeSession));
    homeSession.localBookCount = info.loadedCount;
    RecentBookList recent{};
    if (!RecentBooks::load(recent)) {
        Serial.printf("WARNING HOME_RECENT reason=%s fallback=progress\n",
                      recent.error);
    } else {
        for (size_t index = 0; index < recent.count; ++index) {
            appendHomeEntry(allEntries, info.loadedCount, recent.ids[index]);
        }
    }
    for (size_t index = 0;
         index < info.loadedCount && homeSession.count < HomeLayout::kReadingCapacity;
         ++index) {
        if (allEntries[index].hasProgress) {
            appendHomeEntry(allEntries, info.loadedCount,
                            allEntries[index].id);
        }
    }
    uint16_t newest[HomeLayout::kAddedCapacity]{};
    homeSession.addedCount = HomeLayout::newest(allEntries, info.loadedCount, newest, HomeLayout::kAddedCapacity);
    for (size_t index = 0; index < homeSession.addedCount; ++index)
        homeSession.added[index] = allEntries[newest[index]];
    heap_caps_free(allEntries);

    homeSession.selected = HomeLayout::initialSelection(homeSession.count, homeSession.addedCount);
    if (previousBookId[0] != '\0') {
        bool restored = false;
        for (size_t index = 0; index < homeSession.count; ++index) {
            if (strcmp(homeSession.entries[index].id, previousBookId) == 0) {
                homeSession.selected = HomeLayout::kFirstBook + index;
                restored = true;
                break;
            }
        }
        if (previousAction == HomeLayout::Action::AddedCard || !restored) {
            for (size_t index = 0; index < homeSession.addedCount; ++index)
                if (strcmp(homeSession.added[index].id, previousBookId) == 0) {
                    homeSession.selected = HomeLayout::kFirstBook + homeSession.count + index;
                    break;
                }
        }
    }
    homeSession.loaded = true;
    return true;
}

#include "bookish_adapter.inc"
bool displayHome(bool rescan, const char *reason);
#include "wifi_settings_ui.inc"
#include "collection_name_ui.inc"

bool renderHomeFrame() { return renderBookishHome(); }

bool displayHome(bool rescan, const char *reason) {
    if (wifiScreen()) closeWifiSettings();
    const uint32_t startedAt = millis();
    if (rescan || !homeSession.loaded) invalidateHomeCovers();
    if ((rescan || !homeSession.loaded) && !loadHomeSession()) {
        Serial.println("ERROR HOME_OPEN reason=scan-failed");
        return false;
    }
    if (!renderHomeFrame()) {
        Serial.println("ERROR HOME_OPEN reason=frame-build-failed");
        return false;
    }
    Serial.printf(
        "HOME READY books=%lu recent=%lu selected=%lu added=%lu layout_ms=%lu "
        "reason=%s\n",
        static_cast<unsigned long>(homeSession.localBookCount),
        static_cast<unsigned long>(homeSession.count),
        static_cast<unsigned long>(homeSession.selected + 1),
        static_cast<unsigned long>(homeSession.addedCount),
        static_cast<unsigned long>(millis() - startedAt), reason);
    Serial.flush();
    scheduleScreenTransitionCleanup(UiScreen::Home, "screen-home");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer,
            AutomaticSync::busy() ? DisplayRefreshMode::RecoveryRegion : DisplayRefreshMode::QualityFull,
            kSyncFooterRegion);
    hasDisplayRefresh = true;
    printDisplayRefresh("HOME", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.println("ERROR HOME_OPEN reason=display-failed");
        return false;
    }
    uiScreen = UiScreen::Home;
    activeTopLevelTab = TopLevelTab::Home;
    Serial.printf("HOME OPEN COMPLETE recent=%lu selected=%lu added=%lu\n",
                  static_cast<unsigned long>(homeSession.count),
                  static_cast<unsigned long>(homeSession.selected + 1),
                  static_cast<unsigned long>(homeSession.addedCount));
    return true;
}

bool renderTopLevelPlaceholderFrame(TopLevelTab tab) {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr || framebuffer == nullptr) {
        heap_caps_free(scratch);
        return false;
    }
    memset(framebuffer, 0xFF, kFramebufferBytes);
    drawMainTabHeader(scratch, topLevelTabName(tab));
    drawPortraitText(scratch, &UiCondensed9, I18n::tr("ЕДИНАЯ ВЕРТИКАЛЬНАЯ НАВИГАЦИЯ"),
                     28, 92, 4);
    fillPortraitRect(26, 105, 478, 2, 0);
    drawPortraitRoundedRect(26, 132, 478, 404, 16, 12, 15, 1);
    drawPortraitText(scratch, &UiCondensed9, I18n::tr("РАЗДЕЛ ПОДКЛЮЧЕН"), 52, 182, 5);
    drawPortraitText(scratch, &UiCondensed22Medium, topLevelTabName(tab), 52, 250);
    fillPortraitRect(52, 278, 390, 2, 10);
    drawPortraitText(scratch, &UiCondensed13,
                     tab == TopLevelTab::Favorites
                         ? I18n::tr("ЛОКАЛЬНЫЕ ДАННЫЕ ЧИТАЛКИ")
                         : I18n::tr("ДАННЫЕ ПОЯВЯТСЯ ПОСЛЕ СВЯЗИ С OPDS"),
                     52, 338, 3);
    drawTopLevelTabRail(scratch, tab);
    heap_caps_free(scratch);
    return true;
}

bool displayTopLevelPlaceholder(TopLevelTab tab, const char *reason) {
    if (!renderTopLevelPlaceholderFrame(tab)) {
        Serial.printf("ERROR TAB_OPEN tab=%s reason=frame\n",
                      topLevelTabCode(tab));
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::TopLevel, "screen-top-level");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("TAB", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.printf("ERROR TAB_OPEN tab=%s reason=display\n",
                      topLevelTabCode(tab));
        return false;
    }
    uiScreen = UiScreen::TopLevel;
    activeTopLevelTab = tab;
    Serial.printf("TAB OPEN COMPLETE tab=%s reason=%s\n",
                  topLevelTabCode(tab), reason);
    return true;
}

struct SearchRangeDefinition {
    const char *label;
    const char *letters[12];
    size_t count;
};

constexpr const char *kSearchScopeLabels[] = {
    "ВСЕ РЕЗУЛЬТАТЫ", "КНИГИ", "АВТОРЫ", "СЕРИИ",
    "НА УСТРОЙСТВЕ"};
constexpr SearchRangeDefinition kSearchRanges[] = {
    {"А-Д", {"А", "Б", "В", "Г", "Д"}, 5},
    {"Е-К", {"Е", "Ё", "Ж", "З", "И", "Й", "К"}, 7},
    {"Л-П", {"Л", "М", "Н", "О", "П"}, 5},
    {"Р-У", {"Р", "С", "Т", "У"}, 4},
    {"Ф-Я",
     {"Ф", "Х", "Ц", "Ч", "Ш", "Щ", "Ъ", "Ы", "Ь", "Э", "Ю", "Я"},
     12},
    {"A–F", {"A", "B", "C", "D", "E", "F"}, 6},
    {"G–L", {"G", "H", "I", "J", "K", "L"}, 6},
    {"M–R", {"M", "N", "O", "P", "Q", "R"}, 6},
    {"S–Z", {"S", "T", "U", "V", "W", "X", "Y", "Z"}, 8},
    {"0-9", {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"},
     10},
};

const char *searchScopeLabel(SearchScope scope) {
    const size_t index = static_cast<size_t>(scope);
    return index < sizeof(kSearchScopeLabels) / sizeof(kSearchScopeLabels[0])
               ? I18n::tr(kSearchScopeLabels[index])
               : I18n::tr("ВСЕ РЕЗУЛЬТАТЫ");
}

size_t searchRowCount() {
    switch (searchSession.phase) {
        case SearchPhase::Scope:
            return sizeof(kSearchScopeLabels) / sizeof(kSearchScopeLabels[0]);
        case SearchPhase::Range:
            return sizeof(kSearchRanges) / sizeof(kSearchRanges[0]);
        case SearchPhase::Letter:
            return searchSession.range <
                           sizeof(kSearchRanges) / sizeof(kSearchRanges[0])
                       ? kSearchRanges[searchSession.range].count
                       : 0;
    }
    return 0;
}

const char *searchRowLabel(size_t index) {
    if (searchSession.phase == SearchPhase::Scope) {
        return index < sizeof(kSearchScopeLabels) /
                           sizeof(kSearchScopeLabels[0])
                   ? I18n::tr(kSearchScopeLabels[index])
                   : "";
    }
    if (searchSession.phase == SearchPhase::Range) {
        return index < sizeof(kSearchRanges) / sizeof(kSearchRanges[0])
                   ? kSearchRanges[index].label
                   : "";
    }
    if (searchSession.range >=
        sizeof(kSearchRanges) / sizeof(kSearchRanges[0])) {
        return "";
    }
    const SearchRangeDefinition &range = kSearchRanges[searchSession.range];
    return index < range.count ? range.letters[index] : "";
}

void normalizeSearchSelection() {
    const size_t count = searchRowCount();
    if (count == 0) {
        searchSession.selected = 0;
        searchSession.firstVisible = 0;
        return;
    }
    searchSession.selected = min(searchSession.selected, count - 1);
    searchSession.firstVisible =
        listPageStart(searchSession.selected, kSearchRowsPerScreen);
}

bool renderSearchFrame(int tabFocus = -1) {
    BookishUI::List v{};
    v.activeTab = 2; v.tabFocus = tabFocus;
    v.title = I18n::tr("Найдём книгу");
    v.subtitle = searchSession.phase == SearchPhase::Letter
        ? I18n::tr("Первая буква названия, автора или серии.") : I18n::tr("Поиск среди книг на устройстве.");
    v.back = searchSession.phase == SearchPhase::Letter ? I18n::tr("< К алфавиту") : "";
    v.total = searchRowCount(); v.selected = searchSession.selected;
    for (size_t i = searchSession.firstVisible; i < v.total && v.rowCount < BookishUI::kListRows; ++i) {
        auto &r = v.rows[v.rowCount++];
        r.title = searchRowLabel(i);
        r.subtitle = searchSession.phase == SearchPhase::Letter ? I18n::tr("Книги на эту букву") : I18n::tr("Выбрать первую букву");
        r.selected = i == searchSession.selected;
    }
    return renderBookishList(v);
}

bool displaySearch(bool reset, const char *reason) {
    static size_t renderedFirst = SIZE_MAX;

    static SearchPhase renderedPhase = SearchPhase::Scope;
    if (reset) {
        searchSession = SearchSession{};
    }
    searchSession.scope = SearchScope::Local;
    if (searchSession.phase == SearchPhase::Scope) searchSession.phase = SearchPhase::Range;
    normalizeSearchSelection();
    if (renderedFirst != searchSession.firstVisible) scheduleGhostCleanup("list-page");
    renderedFirst = searchSession.firstVisible;
    if (!renderSearchFrame()) {
        Serial.println("ERROR SEARCH_OPEN reason=frame-build-failed");
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::Search, "screen-search");
    if (uiScreen == UiScreen::Search && renderedPhase != searchSession.phase) {
        scheduleGhostCleanup("search-step");
    }
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("SEARCH", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.println("ERROR SEARCH_OPEN reason=display-failed");
        return false;
    }
    uiScreen = UiScreen::Search;
    renderedPhase = searchSession.phase;
    activeTopLevelTab = TopLevelTab::Search;
    Serial.printf("SEARCH OPEN COMPLETE phase=%u selected=%lu reason=%s\n",
                  static_cast<unsigned>(searchSession.phase),
                  static_cast<unsigned long>(searchRowCount() == 0
                                                 ? 0
                                                 : searchSession.selected + 1),
                  reason);
    return true;
}

bool ensureCatalogFeed() {
    if (catalogSession.feed != nullptr) {
        return true;
    }
    catalogSession.feed = static_cast<OpdsFeed *>(heap_caps_calloc(
        1, sizeof(OpdsFeed), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return catalogSession.feed != nullptr;
}

size_t catalogRowCount() {
    if (!catalogSession.loaded || catalogSession.feed == nullptr) {
        return 0;
    }
    return catalogSession.feed->entryCount +
           (catalogSession.relation == CatalogSession::Relation::None ? 0U
                                                                      : 1U) +
           (catalogSession.feed->previousHref[0] == '\0' ? 0U : 1U) +
           (catalogSession.feed->nextHref[0] == '\0' ? 0U : 1U);
}

size_t catalogMissingRelationBooks() {
    if (catalogSession.relation == CatalogSession::Relation::None ||
        catalogSession.feed == nullptr) {
        return 0;
    }
    size_t count = 0;
    for (size_t index = 0;
         index < catalogSession.feed->entryCount &&
         count < kBulkDownloadLimit;
         ++index) {
        const OpdsEntry &entry = catalogSession.feed->entries[index];
        if (entry.kind != OpdsEntryKind::Book ||
            entry.acquisitionHref[0] == '\0') {
            continue;
        }
        char bookId[33]{};
        if (acquisitionBookId(entry.acquisitionHref, bookId,
                              sizeof(bookId)) &&
            !localBookPresent(bookId)) {
            ++count;
        }
    }
    return count;
}

CatalogRow catalogRowAt(size_t index) {
    CatalogRow row{};
    if (!catalogSession.loaded || catalogSession.feed == nullptr) {
        return row;
    }
    size_t cursor = 0;
    if (catalogSession.relation != CatalogSession::Relation::None) {
        if (index == cursor) {
            row.kind = CatalogRowKind::BulkDownload;
            row.title = catalogSession.relation ==
                                CatalogSession::Relation::Series
                            ? I18n::tr("ДОКАЧАТЬ СЕРИЮ")
                            : I18n::tr("СКАЧАТЬ КНИГИ АВТОРА");
            return row;
        }
        ++cursor;
    }
    if (catalogSession.feed->previousHref[0] != '\0') {
        if (index == cursor) {
            row.kind = CatalogRowKind::PreviousPage;
            row.href = catalogSession.feed->previousHref;
            row.title = I18n::tr("ПРЕДЫДУЩАЯ СТРАНИЦА");
            return row;
        }
        ++cursor;
    }
    if (index < cursor + catalogSession.feed->entryCount) {
        row.kind = CatalogRowKind::Entry;
        row.entry = &catalogSession.feed->entries[index - cursor];
        row.href = row.entry->kind == OpdsEntryKind::Navigation
                       ? row.entry->href
                       : row.entry->acquisitionHref;
        row.title = row.entry->title;
        return row;
    }
    cursor += catalogSession.feed->entryCount;
    if (catalogSession.feed->nextHref[0] != '\0' && index == cursor) {
        row.kind = CatalogRowKind::NextPage;
        row.href = catalogSession.feed->nextHref;
        row.title = I18n::tr("СЛЕДУЮЩАЯ СТРАНИЦА");
    }
    return row;
}

void normalizeCatalogSelection() {
    const size_t count = catalogRowCount();
    if (count == 0) {
        catalogSession.selected = 0;
        catalogSession.firstVisible = 0;
        return;
    }
    catalogSession.selected = min(catalogSession.selected, count - 1);
    catalogSession.firstVisible =
        listPageStart(catalogSession.selected, kCatalogRowsPerScreen);
}

bool renderCatalogFrame() {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr || framebuffer == nullptr ||
        catalogSession.feed == nullptr) {
        heap_caps_free(scratch);
        return false;
    }

    memset(framebuffer, 0xFF, kFramebufferBytes);
    drawMainTabHeader(scratch, topLevelTabName(catalogSession.owner));

    char feedTitle[160]{};
    fitPortraitText(&UiCondensed9, catalogSession.feed->title, 340, feedTitle,
                    sizeof(feedTitle));
    drawPortraitText(scratch, &UiCondensed9, feedTitle, 28, 92, 4);
    drawPortraitText(scratch, &UiCondensed9, "TLS // RADIO OFF", 378, 92, 7);
    fillPortraitRect(26, 105, 478, 2, 0);

    const size_t count = catalogRowCount();
    if (count == 0) {
        drawPortraitRect(26, 132, 478, 220, 8, 2);
        drawPortraitText(scratch, &UiCondensed13, I18n::tr("В КАТАЛОГЕ НЕТ ЗАПИСЕЙ"), 72,
                         222);
        drawPortraitText(scratch, &UiCondensed9,
                         catalogSession.feed->error[0] == '\0'
                             ? I18n::tr("ОБНОВИТЕ РАЗДЕЛ ИЛИ ВЕРНИТЕСЬ НАЗАД")
                             : I18n::tr("СЕТЬ НЕДОСТУПНА — ПОВТОРИТЕ ПОЗЖЕ"),
                         54, 268, 5);
    } else {
        constexpr int32_t rowX = 26;
        constexpr int32_t rowWidth = 478;
        constexpr int32_t rowHeight = 78;
        constexpr int32_t rowGap = 6;
        constexpr int32_t firstRowY = 116;
        const size_t end = min(count, catalogSession.firstVisible +
                                         kCatalogRowsPerScreen);
        for (size_t index = catalogSession.firstVisible; index < end; ++index) {
            const size_t visibleIndex = index - catalogSession.firstVisible;
            const int32_t y = firstRowY +
                              static_cast<int32_t>(visibleIndex) *
                                  (rowHeight + rowGap);
            const bool selected = index == catalogSession.selected;
        const uint8_t foreground = 0;
        const uint8_t background = 15;
            drawSoftRow(rowX, y, rowWidth, rowHeight, selected);

            char ordinal[8]{};
            snprintf(ordinal, sizeof(ordinal), "%02lu",
                     static_cast<unsigned long>(index + 1));
            drawSoftOrdinalBadge(scratch, ordinal, rowX + 20, y + 14,
                                 selected);

            const CatalogRow row = catalogRowAt(index);
            PortraitTextLines titleLines{};
            wrapPortraitText(&UiCondensed14Bold,
                             row.title == nullptr ? I18n::tr("ЗАПИСЬ") : row.title,
                             394, 2, titleLines);
            const int32_t titleBaseline =
                titleLines.count <= 1 ? y + 34 : y + 24;
            drawPortraitTextLines(scratch, &UiCondensed14Bold, titleLines,
                                  rowX + 72, titleBaseline, 18, foreground,
                                  background);

            const char *detail = I18n::tr("СТРАНИЦА КАТАЛОГА");
            const char *status = row.kind == CatalogRowKind::PreviousPage
                                     ? I18n::tr("НАЗАД")
                                 : row.kind == CatalogRowKind::BulkDownload
                                     ? I18n::tr("ПАКЕТ")
                                     : I18n::tr("ДАЛЕЕ");
            char bulkDetail[64]{};
            if (row.kind == CatalogRowKind::BulkDownload) {
                const size_t missing = catalogMissingRelationBooks();
                if (missing == 0) {
                    snprintf(bulkDetail, sizeof(bulkDetail),
                             I18n::tr("ВСЕ КНИГИ УЖЕ НА КАРТЕ"));
                } else {
                    snprintf(bulkDetail, sizeof(bulkDetail),
                             I18n::tr("ДО %lu НЕДОСТАЮЩИХ КНИГ"),
                             static_cast<unsigned long>(missing));
                }
                detail = bulkDetail;
                status = missing == 0 ? I18n::tr("ГОТОВО") : I18n::tr("ВЫБРАТЬ");
            }
            if (row.kind == CatalogRowKind::Entry && row.entry != nullptr) {
                if (row.entry->kind == OpdsEntryKind::Book) {
                    detail = row.entry->author[0] == '\0' ? I18n::tr("КНИГА")
                                                          : row.entry->author;
                    status = I18n::tr("КНИГА");
                } else {
                    detail = row.entry->summary[0] == '\0'
                                 ? I18n::tr("РАЗДЕЛ КАТАЛОГА")
                                 : row.entry->summary;
                    status = I18n::tr("РАЗДЕЛ");
                }
            }
            char fittedDetail[128]{};
            fitPortraitText(&UiCondensed9, detail, 260, fittedDetail,
                            sizeof(fittedDetail));
            drawPortraitText(scratch, &UiCondensed9, fittedDetail, rowX + 72,
                             y + 65, 3, background);
            const int32_t statusWidth = measurePortraitText(&UiCondensed9, status);
            drawPortraitText(scratch, &UiCondensed9, status,
                             rowX + rowWidth - 18 - statusWidth, y + 65,
                             foreground, background);
        }
    }

    if (count > 0) {
        char position[24]{};
        snprintf(position, sizeof(position), "%lu / %lu",
                 static_cast<unsigned long>(catalogSession.selected + 1),
                 static_cast<unsigned long>(count));
        const int32_t positionWidth = measurePortraitText(&UiCondensed9, position);
        drawPortraitText(scratch, &UiCondensed9, position, 508 - positionWidth,
                         924, 0);
    }
    drawTopLevelTabRail(scratch, catalogSession.owner);
    heap_caps_free(scratch);
    return true;
}

bool refreshCatalogFrame(const char *reason) {
    static size_t renderedFirst = SIZE_MAX;
    normalizeCatalogSelection();
    if (renderedFirst != catalogSession.firstVisible) scheduleGhostCleanup("list-page");
    renderedFirst = catalogSession.firstVisible;
    if (!renderCatalogFrame()) {
        Serial.println("ERROR CATALOG_OPEN reason=frame-build-failed");
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::Catalog, "screen-catalog");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("CATALOG", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.println("ERROR CATALOG_OPEN reason=display-failed");
        return false;
    }
    uiScreen = UiScreen::Catalog;
    activeTopLevelTab = catalogSession.owner;
    Serial.printf(
        "CATALOG OPEN COMPLETE owner=%s rows=%lu selected=%lu reason=%s\n",
                  topLevelTabCode(catalogSession.owner),
                  static_cast<unsigned long>(catalogRowCount()),
                  static_cast<unsigned long>(catalogRowCount() == 0
                                                 ? 0
                                                 : catalogSession.selected + 1),
                  reason);
    return true;
}

bool displayCatalogUrl(const char *url, bool pushHistory, bool clearHistory,
                       const char *reason) {
    // Compatibility target for old saved screens; no catalogue HTTP requests.
    (void)url; (void)pushHistory; (void)clearHistory;
    localLibrarySession.owner = TopLevelTab::OnDevice;
    localLibrarySession.phase = LocalLibraryPhase::Sections;
    return displayLocalLibrary(true, reason);
}

bool displayCatalogRoot(const char *reason) {
    catalogSession.owner = TopLevelTab::Catalog;
    catalogSession.relation = CatalogSession::Relation::None;
    catalogSession.relationLabel[0] = '\0';
    return displayCatalogUrl(nullptr, false, true, reason);
}

bool encodeUrlQuery(const char *source, char *target, size_t capacity) {
    if (source == nullptr || target == nullptr || capacity == 0) {
        return false;
    }
    size_t used = 0;
    target[0] = '\0';
    constexpr char kHex[] = "0123456789ABCDEF";
    for (size_t index = 0; source[index] != '\0'; ++index) {
        const uint8_t value = static_cast<uint8_t>(source[index]);
        const bool unreserved =
            (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_' ||
            value == '.' || value == '~';
        const size_t needed = unreserved ? 1U : 3U;
        if (used + needed + 1 > capacity) {
            target[0] = '\0';
            return false;
        }
        if (unreserved) {
            target[used++] = static_cast<char>(value);
        } else {
            target[used++] = '%';
            target[used++] = kHex[value >> 4U];
            target[used++] = kHex[value & 0x0FU];
        }
    }
    target[used] = '\0';
    return used > 0;
}

bool executeSearch() {
    searchSession.scope = SearchScope::Local;
    if (searchSession.scope == SearchScope::Local) {
        localLibrarySession.phase = LocalLibraryPhase::Books;
        localLibrarySession.section = LocalLibrarySection::Search;
        localLibrarySession.owner = TopLevelTab::Search;
        localLibrarySession.selected = 0;
        localLibrarySession.firstVisible = 0;
        snprintf(localLibrarySession.searchPrefix,
                 sizeof(localLibrarySession.searchPrefix), "%s",
                 searchSession.prefix);
        localLibrarySession.loaded = false;
        return displayLocalLibrary(true, "offline-search");
    }
    char root[kOpdsUrlCapacity]{};
    char error[64]{};
    if (!networkService.copyOpdsRootUrl(root, sizeof(root), error,
                                        sizeof(error))) {
        Serial.printf("ERROR SEARCH_QUERY reason=%s\n", error);
        return false;
    }
    const size_t rootLength = strlen(root);
    if (rootLength > 0 && root[rootLength - 1] == '/') {
        root[rootLength - 1] = '\0';
    }
    char encoded[32]{};
    if (!encodeUrlQuery(searchSession.prefix, encoded, sizeof(encoded))) {
        Serial.println("ERROR SEARCH_QUERY reason=invalid-prefix");
        return false;
    }
    const char *path = "/catalog";
    if (searchSession.scope == SearchScope::Authors) {
        path = "/authors";
    } else if (searchSession.scope == SearchScope::Series) {
        path = "/series";
    }
    char target[kOpdsUrlCapacity]{};
    const int written = snprintf(target, sizeof(target),
                                 "%s%s?q=%s&page=1&size=20", root, path,
                                 encoded);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(target)) {
        Serial.println("ERROR SEARCH_QUERY reason=url-too-long");
        return false;
    }
    catalogSession.owner = TopLevelTab::Search;
    catalogSession.relation = CatalogSession::Relation::None;
    catalogSession.relationLabel[0] = '\0';
    catalogSession.loaded = false;
    catalogSession.selected = 0;
    catalogSession.firstVisible = 0;
    catalogSession.currentUrl[0] = '\0';
    memset(catalogSession.history, 0, sizeof(catalogSession.history));
    catalogSession.historyDepth = 0;
    Serial.printf("SEARCH QUERY scope=%s prefix=%s\n",
                  searchScopeLabel(searchSession.scope),
                  searchSession.prefix);
    return displayCatalogUrl(target, false, true, "search");
}

constexpr const char *kLocalSectionLabels[] = {
    "Все книги", "Недавно добавлены", "Читаю", "Непрочитанные", "Прочитанные",
    "Авторы", "Серии", "Жанры", "Синхронизировать", "Настройки"};

const char *localSectionLabel(LocalLibrarySection section) {
    const size_t index = static_cast<size_t>(section);
    return index < sizeof(kLocalSectionLabels) /
                       sizeof(kLocalSectionLabels[0])
               ? I18n::tr(kLocalSectionLabels[index])
               : I18n::tr("РЕЗУЛЬТАТЫ");
}

bool localSectionIsGrouping(LocalLibrarySection section) {
    return section == LocalLibrarySection::Authors ||
           section == LocalLibrarySection::Series ||
           section == LocalLibrarySection::Genres;
}

const char *localGroupValue(const LocalBookEntry &entry,
                            LocalLibrarySection section) {
    if (section == LocalLibrarySection::Authors) {
        return entry.author[0] == '\0' ? I18n::tr("БЕЗ АВТОРА") : entry.author;
    }
    if (section == LocalLibrarySection::Series) {
        return entry.series[0] == '\0' ? I18n::tr("БЕЗ СЕРИИ") : entry.series;
    }
    return entry.genre[0] == '\0' ? I18n::tr("БЕЗ ЖАНРА") : entry.genre;
}

uint32_t foldedFirstCodePoint(const char *text) {
    if (text == nullptr) {
        return 0;
    }
    while (*text != '\0' && static_cast<uint8_t>(*text) <= 0x20U) {
        ++text;
    }
    const uint8_t lead = static_cast<uint8_t>(text[0]);
    uint32_t codePoint = lead;
    if ((lead & 0xE0U) == 0xC0U && text[1] != '\0') {
        codePoint = ((lead & 0x1FU) << 6U) |
                    (static_cast<uint8_t>(text[1]) & 0x3FU);
    } else if ((lead & 0xF0U) == 0xE0U && text[1] != '\0' &&
               text[2] != '\0') {
        codePoint = ((lead & 0x0FU) << 12U) |
                    ((static_cast<uint8_t>(text[1]) & 0x3FU) << 6U) |
                    (static_cast<uint8_t>(text[2]) & 0x3FU);
    }
    if (codePoint >= 0x0410U && codePoint <= 0x042FU) {
        codePoint += 0x20U;
    } else if (codePoint == 0x0401U) {
        codePoint = 0x0451U;
    } else if (codePoint >= 'A' && codePoint <= 'Z') {
        codePoint += 'a' - 'A';
    }
    return codePoint;
}

bool localEntryMatchesSearch(const LocalBookEntry &entry) {
    const uint32_t wanted =
        foldedFirstCodePoint(localLibrarySession.searchPrefix);
    return wanted != 0 &&
           (foldedFirstCodePoint(entry.title) == wanted ||
            foldedFirstCodePoint(entry.author) == wanted ||
            foldedFirstCodePoint(entry.series) == wanted);
}

bool localEntryMatchesSection(const LocalBookEntry &entry) {
    switch (localLibrarySession.section) {
        case LocalLibrarySection::All:
        case LocalLibrarySection::Recent:
            return true;
        case LocalLibrarySection::Reading:
            return entry.hasProgress && !entry.finished;
        case LocalLibrarySection::Unread:
            return !entry.hasProgress && !entry.finished;
        case LocalLibrarySection::Finished:
            return entry.finished;
        case LocalLibrarySection::Authors:
        case LocalLibrarySection::Series:
        case LocalLibrarySection::Genres:
            return strcmp(localGroupValue(entry, localLibrarySession.section),
                          localLibrarySession.groupLabel) == 0;
        case LocalLibrarySection::Search:
            return localEntryMatchesSearch(entry);
    }
    return false;
}

size_t localSectionBookCount(LocalLibrarySection section) {
    if (localSectionIsGrouping(section)) {
        size_t distinct = 0;
        for (size_t index = 0; index < localLibrarySession.info.loadedCount;
             ++index) {
            const char *value =
                localGroupValue(localLibrarySession.entries[index], section);
            bool seen = false;
            for (size_t previous = 0; previous < index; ++previous) {
                if (strcmp(localGroupValue(localLibrarySession.entries[previous],
                                           section),
                           value) == 0) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                ++distinct;
            }
        }
        return distinct;
    }
    size_t count = 0;
    for (size_t index = 0; index < localLibrarySession.info.loadedCount;
         ++index) {
        const LocalBookEntry &entry = localLibrarySession.entries[index];
        if (section == LocalLibrarySection::All || section == LocalLibrarySection::Recent ||
            (section == LocalLibrarySection::Reading && entry.hasProgress &&
             !entry.finished) ||
            (section == LocalLibrarySection::Unread && !entry.hasProgress &&
             !entry.finished) ||
            (section == LocalLibrarySection::Finished && entry.finished)) {
            ++count;
        }
    }
    return count;
}

void sortLocalGroups() {
    for (size_t index = 1; index < localLibrarySession.groupCount; ++index) {
        char value[128]{};
        snprintf(value, sizeof(value), "%s",
                 localLibrarySession.groups[index]);
        size_t target = index;
        while (target > 0 &&
               strcmp(localLibrarySession.groups[target - 1], value) > 0) {
            snprintf(localLibrarySession.groups[target],
                     sizeof(localLibrarySession.groups[target]), "%s",
                     localLibrarySession.groups[target - 1]);
            --target;
        }
        snprintf(localLibrarySession.groups[target],
                 sizeof(localLibrarySession.groups[target]), "%s", value);
    }
}

void rebuildLocalLibraryView(const char *preferredBookId = nullptr) {
    char previousGroup[128]{};
    if (localLibrarySession.phase == LocalLibraryPhase::Groups &&
        localLibrarySession.selected < localLibrarySession.groupCount) {
        snprintf(previousGroup, sizeof(previousGroup), "%s",
                 localLibrarySession.groups[localLibrarySession.selected]);
    }
    memset(localLibrarySession.visible, 0,
           sizeof(localLibrarySession.visible));
    memset(localLibrarySession.groups, 0,
           kLocalLibraryCapacity * sizeof(localLibrarySession.groups[0]));
    localLibrarySession.visibleCount = 0;
    localLibrarySession.groupCount = 0;

    if (localLibrarySession.phase == LocalLibraryPhase::Groups) {
        for (size_t index = 0; index < localLibrarySession.info.loadedCount;
             ++index) {
            const char *value = localGroupValue(
                localLibrarySession.entries[index], localLibrarySession.section);
            bool duplicate = false;
            for (size_t group = 0; group < localLibrarySession.groupCount;
                 ++group) {
                if (strcmp(localLibrarySession.groups[group], value) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate &&
                localLibrarySession.groupCount < kLocalLibraryCapacity) {
                snprintf(localLibrarySession.groups[
                             localLibrarySession.groupCount++],
                         sizeof(localLibrarySession.groups[0]), "%s", value);
            }
        }
        sortLocalGroups();
        localLibrarySession.selected = 0;
        if (previousGroup[0] != '\0') {
            for (size_t index = 0; index < localLibrarySession.groupCount;
                 ++index) {
                if (strcmp(localLibrarySession.groups[index], previousGroup) ==
                    0) {
                    localLibrarySession.selected = index;
                    break;
                }
            }
        }
    } else if (localLibrarySession.phase == LocalLibraryPhase::Books) {
        for (size_t index = 0; index < localLibrarySession.info.loadedCount &&
                               localLibrarySession.visibleCount <
                                   kLocalLibraryCapacity;
             ++index) {
            if (localEntryMatchesSection(localLibrarySession.entries[index])) {
                localLibrarySession.visible[localLibrarySession.visibleCount++] =
                    static_cast<uint16_t>(index);
            }
        }
        if (localLibrarySession.section == LocalLibrarySection::Recent) {
            for (size_t i = 1; i < localLibrarySession.visibleCount; ++i) {
                const uint16_t value = localLibrarySession.visible[i];
                size_t j = i;
                while (j > 0 && HomeLayout::newer(localLibrarySession.entries[value],
                                localLibrarySession.entries[localLibrarySession.visible[j - 1]])) {
                    localLibrarySession.visible[j] = localLibrarySession.visible[j - 1];
                    --j;
                }
                localLibrarySession.visible[j] = value;
            }
        }
        localLibrarySession.selected = 0;
        if (preferredBookId != nullptr && preferredBookId[0] != '\0') {
            for (size_t index = 0; index < localLibrarySession.visibleCount;
                 ++index) {
                if (strcmp(localLibrarySession.entries[
                               localLibrarySession.visible[index]].id,
                           preferredBookId) == 0) {
                    localLibrarySession.selected = index;
                    break;
                }
            }
        }
    } else {
        localLibrarySession.selected = min(
            localLibrarySession.selected,
            sizeof(kLocalSectionLabels) / sizeof(kLocalSectionLabels[0]) - 1);
    }
    localLibrarySession.firstVisible = 0;
}

size_t localLibraryRowCount() {
    if (localLibrarySession.phase == LocalLibraryPhase::Sections) {
        return sizeof(kLocalSectionLabels) / sizeof(kLocalSectionLabels[0]);
    }
    return localLibrarySession.phase == LocalLibraryPhase::Groups
               ? localLibrarySession.groupCount
               : localLibrarySession.visibleCount;
}

const LocalBookEntry *localVisibleEntry(size_t index) {
    if (localLibrarySession.phase != LocalLibraryPhase::Books ||
        index >= localLibrarySession.visibleCount) {
        return nullptr;
    }
    return &localLibrarySession.entries[localLibrarySession.visible[index]];
}

size_t localGroupBookCount(const char *label) {
    size_t count = 0;
    for (size_t index = 0; index < localLibrarySession.info.loadedCount;
         ++index) {
        if (strcmp(localGroupValue(localLibrarySession.entries[index],
                                   localLibrarySession.section),
                   label) == 0) {
            ++count;
        }
    }
    return count;
}

void drawLocalSectionIcon(LocalLibrarySection section, int32_t centerX,
                          int32_t centerY, uint8_t foreground,
                          uint8_t background) {
    switch (section) {
        case LocalLibrarySection::Recent:
            drawPortraitRoundedRect(centerX - 13, centerY - 15, 26, 30, 5,
                                    foreground, background, 2);
            drawPortraitLine(centerX, centerY - 8, centerX, centerY + 1, foreground, 2);
            drawPortraitLine(centerX, centerY + 1, centerX + 7, centerY + 5, foreground, 2);
            break;
        case LocalLibrarySection::All:
            drawPortraitRoundedRect(centerX - 13, centerY - 14, 26, 29, 5,
                                    foreground, background, 2);
            drawPortraitLine(centerX, centerY - 11, centerX, centerY + 12,
                             foreground, 1);
            drawPortraitLine(centerX - 10, centerY - 8, centerX - 3,
                             centerY - 6, foreground, 1);
            drawPortraitLine(centerX + 3, centerY - 6, centerX + 10,
                             centerY - 8, foreground, 1);
            break;
        case LocalLibrarySection::Reading:
            drawPortraitRoundedRect(centerX - 13, centerY - 14, 26, 29, 5,
                                    foreground, background, 2);
            fillPortraitRect(centerX - 10, centerY + 9, 7, 3,
                             foreground);
            break;
        case LocalLibrarySection::Unread:
            drawPortraitRoundedRect(centerX - 12, centerY - 14, 24, 28, 4,
                                    foreground, background, 2);
            fillPortraitCircle(centerX + 9, centerY - 10, 4, foreground);
            break;
        case LocalLibrarySection::Finished:
            drawPortraitCircle(centerX, centerY, 14, foreground, background,
                               2);
            drawPortraitLine(centerX - 8, centerY, centerX - 2,
                             centerY + 7, foreground, 3);
            drawPortraitLine(centerX - 2, centerY + 7, centerX + 10,
                             centerY - 8, foreground, 3);
            break;
        case LocalLibrarySection::Authors:
            fillPortraitCircle(centerX, centerY - 8, 7, foreground);
            for (int32_t row = 0; row < 13; ++row) {
                const int32_t halfWidth = min(14, row + 4);
                fillPortraitRect(centerX - halfWidth, centerY + 2 + row,
                                 halfWidth * 2 + 1, 1, foreground);
            }
            break;
        case LocalLibrarySection::Series:
            for (int32_t row = -1; row <= 1; ++row) {
                const int32_t y = centerY + row * 10;
                fillPortraitRoundedRect(centerX - 14 + (row + 1) * 3,
                                        y - 3, 25 - (row + 1) * 3, 6, 2,
                                        foreground);
            }
            break;
        case LocalLibrarySection::Genres:
            drawPortraitLine(centerX - 14, centerY - 7, centerX - 4,
                             centerY - 14, foreground, 2);
            drawPortraitLine(centerX - 4, centerY - 14, centerX + 14,
                             centerY + 4, foreground, 2);
            drawPortraitLine(centerX + 14, centerY + 4, centerX + 5,
                             centerY + 13, foreground, 2);
            drawPortraitLine(centerX + 5, centerY + 13, centerX - 14,
                             centerY - 7, foreground, 2);
            fillPortraitCircle(centerX - 5, centerY - 7, 2, foreground);
            break;
        case LocalLibrarySection::Search:
            drawPortraitCircle(centerX - 2, centerY - 3, 9, foreground,
                               background, 2);
            drawPortraitLine(centerX + 5, centerY + 4, centerX + 14,
                             centerY + 13, foreground, 3);
            break;
    }
}

bool renderLocalLibraryFrame(int tabFocus = -1) {
    BookishUI::List v{};
    v.activeTab = localLibrarySession.owner == TopLevelTab::Search ? 2 : 1;
    v.tabFocus = tabFocus;
    const bool root = localLibrarySession.phase == LocalLibraryPhase::Sections;
    const bool groups = localLibrarySession.phase == LocalLibraryPhase::Groups;
    const bool search = localLibrarySession.section == LocalLibrarySection::Search;
    v.title = root ? I18n::tr("Ваша библиотека") : search ? I18n::tr("Результаты поиска") : localSectionLabel(localLibrarySession.section);
    char summary[160]{};
    if (root) snprintf(summary, sizeof(summary), I18n::tr("На устройстве: %lu · выберите подборку"),
        static_cast<unsigned long>(localLibrarySession.info.loadedCount));
    else if (search) snprintf(summary, sizeof(summary), I18n::tr("На «%s» · найдено: %lu"),
        localLibrarySession.searchPrefix, static_cast<unsigned long>(localLibrarySession.visibleCount));
    else if (!groups && localSectionIsGrouping(localLibrarySession.section))
        snprintf(summary, sizeof(summary), "%s", localLibrarySession.groupLabel);
    else snprintf(summary, sizeof(summary), "%s: %lu", groups ? I18n::tr("Подборок") : I18n::tr("Книг"),
        static_cast<unsigned long>(localLibraryRowCount()));
    v.subtitle = summary;
    v.back = root ? "" : search ? I18n::tr("< Выбрать другую букву") :
        !groups && localSectionIsGrouping(localLibrarySession.section) ? I18n::tr("< К списку подборок") : I18n::tr("< К разделам библиотеки");
    v.emptyTitle = search ? I18n::tr("Ничего не найдено") : I18n::tr("Подборка пока пуста");
    v.emptyHint = search ? I18n::tr("Попробуйте другую первую букву.") : I18n::tr("Здесь появятся подходящие книги.");
    v.emptyHint2 = I18n::tr("UP — к возврату · OK — назад");
    v.total = localLibraryRowCount(); v.selected = localLibrarySession.selected;
    char details[BookishUI::kListRows][80]{};
    const char *hints[] = {I18n::tr("Все загруженные книги"), I18n::tr("Последние пополнения"), I18n::tr("Истории, которые вы начали"),
        I18n::tr("Откройте что-нибудь новое"), I18n::tr("Прочитанные истории"), I18n::tr("Книги любимых писателей"),
        I18n::tr("Истории с продолжением"), I18n::tr("Подберите книгу по настроению"), I18n::tr("Обмен книгами и коллекциями с сервером"), I18n::tr("Wi-Fi и подключение")};
    for (size_t i = localLibrarySession.firstVisible; i < v.total && v.rowCount < BookishUI::kListRows; ++i) {
        const size_t row = v.rowCount++;
        auto &r = v.rows[row]; r.selected = i == localLibrarySession.selected;
        if (root || groups) {
            r.title = root ? localSectionLabel(static_cast<LocalLibrarySection>(i)) : localLibrarySession.groups[i];
            r.subtitle = root ? hints[i] : I18n::tr("Открыть подборку");
            if (root && i >= 8) { r.detail = i == 8 ? (AutomaticSync::busy() ? I18n::tr("OK — отменить") : AutomaticSync::status()==AutomaticSync::Status::Failed ? ReaderSyncPolicy::errorLabel(AutomaticSync::error()) : AutomaticSync::status()==AutomaticSync::Status::Complete ? I18n::tr("Библиотека обновлена") : I18n::tr("OK — начать")) : I18n::tr("OK — открыть"); continue; }
            snprintf(details[row], sizeof(details[row]), I18n::tr("Книг: %lu"), static_cast<unsigned long>(root
                ? localSectionBookCount(static_cast<LocalLibrarySection>(i)) : localGroupBookCount(r.title)));
        } else {
            const auto *entry = localVisibleEntry(i);
            if (!entry) { --v.rowCount; continue; }
            r.title = entry->title; r.subtitle = entry->author; r.coverId = entry->id; r.book = true;
            r.progress = static_cast<uint8_t>(localBookProgressPercent(*entry));
            formatLocalBookStatus(*entry, details[row], sizeof(details[row]));
        }
        r.detail = details[row];
    }
    return renderBookishList(v);
}

const char *longOperationEyebrow(LongOperationKind kind) {
    switch (kind) {
        case LongOperationKind::StartupRecovery:
            return "ABYSS // SAFE START";
        case LongOperationKind::Network:
            return "ABYSS // CONNECTION";
        case LongOperationKind::BookDownload:
            return "ABYSS // SECURE ACQUISITION";
        case LongOperationKind::BookPreparation:
            return "ABYSS // OFFLINE PREPARATION";
        case LongOperationKind::CoverIndexRebuild:
            return "ABYSS // LOCAL INDEX";
        case LongOperationKind::Ota:
            return "ABYSS // SYSTEM UPDATE";
    }
    return "ABYSS // WORKING";
}

const char *longOperationTitle(const LongOperationModel &model) {
    if (model.failed) {
        return I18n::tr("НУЖНО ВНИМАНИЕ");
    }
    switch (model.kind) {
        case LongOperationKind::StartupRecovery:
            return I18n::tr("ВОССТАНАВЛИВАЮ");
        case LongOperationKind::Network:
            return I18n::tr("ПОДКЛЮЧАЮСЬ");
        case LongOperationKind::BookDownload:
            return I18n::tr("ЗАГРУЖАЮ КНИГУ");
        case LongOperationKind::BookPreparation:
            return I18n::tr("ГОТОВЛЮ КНИГУ");
        case LongOperationKind::CoverIndexRebuild:
            return I18n::tr("ОБНОВЛЯЮ БИБЛИОТЕКУ");
        case LongOperationKind::Ota:
            return I18n::tr("ОБНОВЛЯЮ СИСТЕМУ");
    }
    return I18n::tr("ПОДОЖДИТЕ");
}

const char *longOperationPhase(LongOperationKind kind, size_t index,
                               bool cacheReady) {
    const char *startup[] = {
        I18n::tr("ПРОВЕРКА ХРАНИЛИЩА"), I18n::tr("ВОССТАНОВЛЕНИЕ СОСТОЯНИЯ"),
        I18n::tr("ОТКРЫТИЕ БИБЛИОТЕКИ")};
    const char *network[] = {
        I18n::tr("ЗАЩИЩЁННОЕ СОЕДИНЕНИЕ"), I18n::tr("ПРОВЕРКА КАТАЛОГА"),
        I18n::tr("ОТКЛЮЧЕНИЕ РАДИО")};
    const char *download[] = {
        I18n::tr("ЗАЩИЩЁННОЕ СОЕДИНЕНИЕ"), I18n::tr("ПРОВЕРКА FB2 И SHA-256"),
        I18n::tr("АТОМАРНАЯ ЗАПИСЬ")};
    const char *preparation[] = {
        I18n::tr("РАЗБОР И НОРМАЛИЗАЦИЯ FB2"), I18n::tr("РАЗБИЕНИЕ НА СТРАНИЦЫ"),
        I18n::tr("ОГЛАВЛЕНИЕ И ПОЗИЦИИ")};
    const char *cachedPreparation[] = {
        I18n::tr("ПРОВЕРКА ТЕКСТОВОГО КЕША"), I18n::tr("РАЗБИЕНИЕ НА СТРАНИЦЫ"),
        I18n::tr("ОГЛАВЛЕНИЕ И ПОЗИЦИИ")};
    const char *indexPhases[] = {
        I18n::tr("ПРОВЕРКА ОБЛОЖЕК"), I18n::tr("ОБНОВЛЕНИЕ МЕТАДАННЫХ"),
        I18n::tr("ПУБЛИКАЦИЯ ИНДЕКСА")};
    const char *ota[] = {
        I18n::tr("ПРОВЕРКА ПОДПИСИ"), I18n::tr("ЗАПИСЬ В РЕЗЕРВНЫЙ СЛОТ"),
        I18n::tr("ПОДГОТОВКА БЕЗОПАСНОГО ЗАПУСКА")};

    const char *const *phases = preparation;
    switch (kind) {
        case LongOperationKind::StartupRecovery:
            phases = startup;
            break;
        case LongOperationKind::Network:
            phases = network;
            break;
        case LongOperationKind::BookDownload:
            phases = download;
            break;
        case LongOperationKind::BookPreparation:
            phases = cacheReady ? cachedPreparation : preparation;
            break;
        case LongOperationKind::CoverIndexRebuild:
            phases = indexPhases;
            break;
        case LongOperationKind::Ota:
            phases = ota;
            break;
    }
    return phases[index < 3 ? index : 2];
}

const char *longOperationNote(LongOperationKind kind) {
    switch (kind) {
        case LongOperationKind::StartupRecovery:
            return I18n::tr("ВАШИ КНИГИ И ПОЗИЦИИ СОХРАНЕНЫ");
        case LongOperationKind::Network:
            return I18n::tr("WI-FI ОТКЛЮЧИТСЯ ПОСЛЕ ОПЕРАЦИИ");
        case LongOperationKind::BookDownload:
            return I18n::tr("ФАЙЛ ПОЯВИТСЯ ТОЛЬКО ПОСЛЕ ПРОВЕРКИ");
        case LongOperationKind::BookPreparation:
            return I18n::tr("ПОВТОРНОЕ ОТКРЫТИЕ БУДЕТ БЫСТРЫМ");
        case LongOperationKind::CoverIndexRebuild:
            return I18n::tr("ИСХОДНЫЕ КНИГИ НЕ ИЗМЕНЯЮТСЯ");
        case LongOperationKind::Ota:
            return I18n::tr("ПРЕДЫДУЩАЯ ВЕРСИЯ ОСТАНЕТСЯ РЕЗЕРВНОЙ");
    }
    return I18n::tr("ОПЕРАЦИЯ ВЫПОЛНЯЕТСЯ БЕЗ ОЦЕНКИ ВРЕМЕНИ");
}

bool renderLongOperationFrame(const LongOperationModel &model) {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr || framebuffer == nullptr) {
        heap_caps_free(scratch);
        return false;
    }
    memset(framebuffer, 0xFF, kFramebufferBytes);
    drawScreenChrome(scratch, longOperationEyebrow(model.kind),
                     longOperationTitle(model));

    drawPortraitText(scratch, &UiCondensed9,
                     model.failed ? I18n::tr("ОПЕРАЦИЯ БЕЗОПАСНО ПРИОСТАНОВЛЕНА")
                                  : I18n::tr("СТАТИЧНЫЙ ЭКРАН · БЕЗ ЛОЖНОГО ПРОГРЕССА"),
                     30, 154, 5);
    fillPortraitRect(26, 166, 488, 2, 0);

    drawPortraitRoundedRect(26, 192, 488, 176, 18, 12, 15, 1);
    PortraitTextLines subjectLines{};
    wrapPortraitText(&UiCondensed14Bold,
                     model.subject != nullptr && model.subject[0] != '\0'
                         ? model.subject
                         : I18n::tr("ПОДГОТОВКА"),
                     428, 3, subjectLines);
    drawPortraitTextLines(scratch, &UiCondensed14Bold, subjectLines, 50,
                          subjectLines.count <= 1 ? 246 : 226, 20, 0, 15);
    if (model.detail != nullptr && model.detail[0] != '\0') {
        char fittedDetail[160]{};
        fitPortraitText(&UiCondensed9, model.detail, 426, fittedDetail,
                        sizeof(fittedDetail));
        drawPortraitText(scratch, &UiCondensed9, fittedDetail, 50, 310, 5);
    }
    if (model.identifier != nullptr && model.identifier[0] != '\0') {
        char fittedIdentifier[96]{};
        fitPortraitText(&UiCondensed9, model.identifier, 426,
                        fittedIdentifier, sizeof(fittedIdentifier));
        drawPortraitText(scratch, &UiCondensed9, fittedIdentifier, 50, 340,
                         8);
    }

    for (size_t index = 0; index < 3; ++index) {
        const int32_t y = 412 + static_cast<int32_t>(index) * 74;
        drawPortraitRoundedRect(34, y, 472, 52, 14, 13, 15, 1);
        fillPortraitRect(48, y + 10, 32, 32, model.failed ? 7 : 0);
        drawPortraitText(scratch, &UiCondensed9,
                         model.failed ? "!" : (index == 0 ? "1" :
                                                index == 1 ? "2" : "3"),
                         58, y + 32, 15, model.failed ? 7 : 0);
        drawPortraitText(scratch, &UiCondensed9,
                         model.failed ? I18n::tr("ОЖИДАЕТ БЕЗОПАСНОГО ПОВТОРА")
                                      : longOperationPhase(
                                            model.kind, index,
                                            model.cacheReady),
                         98, y + 32, model.failed ? 4 : 0);
    }

    if (model.failed) {
        drawPortraitRoundedRect(26, 664, 488, 126, 18, 12, 15, 1);
        char fittedError[96]{};
        fitPortraitText(&UiCondensed9,
                        model.error == nullptr || model.error[0] == '\0'
                            ? I18n::tr("НЕИЗВЕСТНАЯ ОШИБКА")
                            : model.error,
                        424, fittedError, sizeof(fittedError));
        drawPortraitText(scratch, &UiCondensed9, I18n::tr("ПРИЧИНА"), 50, 704, 6);
        drawPortraitText(scratch, &UiCondensed9, fittedError, 50, 750, 0);
        fillPortraitRect(26, 826, 488, 72, 0);
        drawPortraitText(scratch, &UiCondensed13, I18n::tr("ПОВТОРИТЬ"), 198, 872, 15,
                         0);
    } else {
        fillPortraitRect(26, 684, 488, 144, 0);
        PortraitTextLines noteLines{};
        wrapPortraitText(&UiCondensed13, longOperationNote(model.kind), 424,
                         2, noteLines);
        drawPortraitTextLines(scratch, &UiCondensed13, noteLines, 58, 740, 22,
                              15, 0);
        drawPortraitText(scratch, &UiCondensed9,
                         I18n::tr("МОЖНО ОСТАВИТЬ УСТРОЙСТВО РАБОТАТЬ"), 58, 794, 11,
                         0);
    }

    drawPortraitText(scratch, &UiCondensed9,
                     model.failed ? I18n::tr("СОСТОЯНИЕ СОХРАНЕНО")
                                  : I18n::tr("НЕ ВЫКЛЮЧАЙТЕ ПИТАНИЕ"),
                     30, 916, 5);
    drawPortraitText(scratch, &UiCondensed9, "ATOMIC // SAFE", 388, 916, 8);

    heap_caps_free(scratch);
    return true;
}

bool renderBookDownloadFrame(const BookDownloadJob &job, bool failed,
                             const char *error) {
    LongOperationModel model{};
    model.kind = LongOperationKind::BookDownload;
    model.subject = job.title;
    model.detail = job.author[0] == '\0' ? I18n::tr("АВТОР НЕ УКАЗАН") : job.author;
    model.identifier = job.bookId;
    model.error = error;
    model.failed = failed;
    return renderLongOperationFrame(model);
}

bool displayBookDownload(const BookDownloadJob &job, bool failed,
                         const char *error, const char *reason) {
    if (!displayInitialized || framebuffer == nullptr ||
        !renderBookDownloadFrame(job, failed, error)) {
        Serial.printf("ERROR DOWNLOAD_SCREEN id=%s reason=frame\n",
                      job.bookId);
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::Download, "screen-download");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("DOWNLOAD", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.printf("ERROR DOWNLOAD_SCREEN id=%s reason=display\n",
                      job.bookId);
        return false;
    }
    uiScreen = UiScreen::Download;
    downloadFailureVisible = failed;
    Serial.printf("DOWNLOAD SCREEN id=%s state=%s reason=%s\n", job.bookId,
                  failed ? "retry" : "active", reason);
    return true;
}

bool renderBookPreparationFrame(const char *bookId, const char *title,
                                bool cacheReady) {
    LongOperationModel model{};
    model.kind = LongOperationKind::BookPreparation;
    model.subject = title != nullptr && title[0] != '\0' ? title : bookId;
    model.detail = I18n::tr("ПЕРВЫЙ ЗАПУСК КНИГИ");
    model.identifier = bookId;
    model.cacheReady = cacheReady;
    return renderLongOperationFrame(model);
}

bool displayBookPreparation(const char *bookId, const char *title,
                            bool cacheReady, const char *reason) {
    if (!displayInitialized || framebuffer == nullptr ||
        !renderBookPreparationFrame(bookId, title, cacheReady)) {
        Serial.printf("ERROR BOOK_PREPARATION_SCREEN id=%s reason=frame\n",
                      bookId);
        return false;
    }

    scheduleScreenTransitionCleanup(UiScreen::Preparation,
                                    "screen-preparation");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("PREPARATION", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.printf("ERROR BOOK_PREPARATION_SCREEN id=%s reason=display\n",
                      bookId);
        return false;
    }
    uiScreen = UiScreen::Preparation;
    Serial.printf("BOOK PREPARATION VISIBLE id=%s reason=%s\n", bookId,
                  reason);
    return true;
}

bool prepareBookArtifacts(const char *bookId, const char *source,
                          BookPreparationResult &result,
                          const Fb2CacheInfo *knownCache = nullptr,
                          const ReaderPaginationInfo *knownPagination =
                              nullptr) {
    // Aggregate assignment materializes another full result on the caller's
    // stack.  This POD is deliberately reset in place instead.
    memset(&result, 0, sizeof(result));
    if (!BookUploadReceiver::validBookId(bookId)) {
        snprintf(result.error, sizeof(result.error), "invalid-book-id");
        return false;
    }

    BusyIndicator busy;
    uint32_t startedAt = millis();
    result.cacheReused = knownCache != nullptr && knownCache->ok;
    if (result.cacheReused) {
        result.cache = *knownCache;
    } else {
        result.cacheReused = Fb2Cache::load(bookId, result.cache);
    }
    if (!result.cacheReused && !Fb2Cache::build(bookId, result.cache)) {
        snprintf(result.error, sizeof(result.error), "%s", result.cache.error);
        return false;
    }
    result.cacheDurationMs = millis() - startedAt;
    Serial.printf(
        "BOOK PREPARATION CACHE id=%s reused=%s duration_ms=%lu "
        "records=%lu source=%s\n",
        bookId, result.cacheReused ? "true" : "false",
        static_cast<unsigned long>(result.cacheDurationMs),
        static_cast<unsigned long>(result.cache.records), source);

    startedAt = millis();
    result.paginationReused = result.cacheReused &&
                              knownPagination != nullptr &&
                              knownPagination->ok;
    if (result.paginationReused) {
        result.pagination = *knownPagination;
    } else {
        result.paginationReused =
            result.cacheReused &&
            ReaderPagination::load(bookId, result.pagination);
    }
    if (!result.paginationReused &&
        !ReaderPagination::build(bookId, result.pagination)) {
        snprintf(result.error, sizeof(result.error), "%s",
                 result.pagination.error);
        return false;
    }
    result.paginationDurationMs = millis() - startedAt;
    result.ok = true;
    Serial.printf(
        "BOOK PREPARATION PAGINATION id=%s reused=%s duration_ms=%lu "
        "pages=%lu chapters=%lu source=%s\n",
        bookId, result.paginationReused ? "true" : "false",
        static_cast<unsigned long>(result.paginationDurationMs),
        static_cast<unsigned long>(result.pagination.pageCount),
        static_cast<unsigned long>(result.pagination.chapterCount), source);
    return true;
}

bool displayLocalLibrary(bool rescan, const char *reason) {
    static size_t renderedFirst = SIZE_MAX;

    static LocalLibraryPhase renderedPhase = LocalLibraryPhase::Sections;
    static LocalLibrarySection renderedSection = LocalLibrarySection::All;
    static char renderedGroup[128]{};
    char previousBookId[33]{};
    const LocalBookEntry *previousEntry =
        localVisibleEntry(localLibrarySession.selected);
    if (localLibrarySession.loaded && previousEntry != nullptr) {
        snprintf(previousBookId, sizeof(previousBookId), "%s",
                 previousEntry->id);
    }

    const uint32_t startedAt = millis();
    if (rescan || !localLibrarySession.loaded) {
        BusyIndicator busy;
        LocalLibraryInfo info{};
        if (!LocalLibrary::scan(localLibrarySession.entries,
                                kLocalLibraryCapacity, info)) {
            Serial.printf("ERROR LIBRARY_OPEN reason=%s\n", info.error);
            return false;
        }
        localLibrarySession.info = info;
        localLibrarySession.loaded = true;
        rebuildLocalLibraryView(previousBookId);
    }

    const size_t rowCount = localLibraryRowCount();
    if (rowCount == 0) {
        localLibrarySession.selected = 0;
        localLibrarySession.firstVisible = 0;
    } else {
        localLibrarySession.selected =
            min(localLibrarySession.selected,
                static_cast<size_t>(rowCount - 1));
        localLibrarySession.firstVisible =
            listPageStart(localLibrarySession.selected, kLibraryRowsPerScreen);
    }

    if (renderedFirst != localLibrarySession.firstVisible) scheduleGhostCleanup("list-page");
    renderedFirst = localLibrarySession.firstVisible;
    if (!renderLocalLibraryFrame()) {
        Serial.println("ERROR LIBRARY_OPEN reason=frame-build-failed");
        return false;
    }
    Serial.printf(
        "LIBRARY READY books=%lu rows=%lu phase=%u section=%u prepared=%lu "
        "selected=%lu omitted=%lu layout_ms=%lu reason=%s first=%lu\n",
        static_cast<unsigned long>(localLibrarySession.info.loadedCount),
        static_cast<unsigned long>(rowCount),
        static_cast<unsigned>(localLibrarySession.phase),
        static_cast<unsigned>(localLibrarySession.section),
        static_cast<unsigned long>(localLibrarySession.info.preparedCount),
        static_cast<unsigned long>(
            rowCount == 0 ? 0 : localLibrarySession.selected + 1),
        static_cast<unsigned long>(localLibrarySession.info.omittedCount),
        static_cast<unsigned long>(millis() - startedAt), reason, static_cast<unsigned long>(localLibrarySession.firstVisible));
    Serial.flush();

    scheduleScreenTransitionCleanup(UiScreen::LocalLibrary,
                                    "screen-local-library");
    if (uiScreen == UiScreen::LocalLibrary &&
        (renderedPhase != localLibrarySession.phase ||
         renderedSection != localLibrarySession.section ||
         strcmp(renderedGroup, localLibrarySession.groupLabel) != 0)) {
        scheduleGhostCleanup("local-library-content");
    }
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, localLibrarySession.phase == LocalLibraryPhase::Books
            ? DisplayRefreshMode::QualityFull : DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("LIBRARY", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.println("ERROR LIBRARY_OPEN reason=display-failed");
        return false;
    }
    uiScreen = UiScreen::LocalLibrary;
    renderedPhase = localLibrarySession.phase;
    renderedSection = localLibrarySession.section;
    snprintf(renderedGroup, sizeof(renderedGroup), "%s",
             localLibrarySession.groupLabel);
    activeTopLevelTab = localLibrarySession.owner;
    libraryInteractionAt = millis();
    Serial.printf("LIBRARY OPEN COMPLETE books=%lu rows=%lu selected=%lu\n",
                  static_cast<unsigned long>(
                      localLibrarySession.info.loadedCount),
                  static_cast<unsigned long>(rowCount),
                  static_cast<unsigned long>(
                      rowCount == 0 ? 0 : localLibrarySession.selected + 1));
    return true;
}

const char *bookCardTitle() {
    return bookCardSession.local ? bookCardSession.localEntry.title
                                 : bookCardSession.remoteEntry.title;
}

const char *bookCardAuthor() {
    return bookCardSession.local ? bookCardSession.localEntry.author
                                 : bookCardSession.remoteEntry.author;
}

const char *bookCardAnnotation() {
    return bookCardSession.local ? bookCardSession.localEntry.annotation
                                 : bookCardSession.remoteEntry.summary;
}

const char *bookCardSeries() {
    return bookCardSession.local ? bookCardSession.localEntry.series
                                 : bookCardSession.remoteEntry.series;
}

const char *bookCardSeriesNumber() {
    return bookCardSession.local ? bookCardSession.localEntry.seriesNumber
                                 : bookCardSession.remoteEntry.seriesNumber;
}

bool bookCardActionEnabled(BookCardFocus focus) {
    if (focus == BookCardFocus::Author) {
        return bookCardAuthor()[0] != '\0';
    }
    if (focus == BookCardFocus::Series) {
        return bookCardSeries()[0] != '\0';
    }
    if (focus == BookCardFocus::Favorite) {
        return BookUploadReceiver::validBookId(bookCardSession.bookId);
    }
    return true;
}

void drawBookCardAction(uint8_t *scratch, BookCardFocus focus,
                        const char *label, int32_t y, bool prominent = false) {
    const bool selected = bookCardSession.focus == focus;
    const bool enabled = bookCardActionEnabled(focus);
    const uint8_t background = 15;
    const uint8_t foreground = enabled ? 0 : 8;
    if (selected) {
        drawPortraitRoundedRect(34, y, 472, 50, 15, 0, 15, 2);
        drawPortraitRoundedRect(39, y + 5, 462, 40, 11, 9, 15, 1);
    } else if (prominent) {
        drawPortraitRoundedRect(34, y, 472, 50, 15, 5, 15, 2);
    } else {
        drawPortraitRoundedRect(34, y, 472, 50, 15, 12, 15, 1);
    }
    const int32_t width = measurePortraitText(
        prominent ? &UiCondensed14Bold : &UiCondensed13, label);
    drawPortraitText(scratch,
                     prominent ? &UiCondensed14Bold : &UiCondensed13,
                     label, max(48, (kPortraitWidth - width) / 2), y + 34,
                     foreground, background);
}

bool renderBookCardFrame() {
    if(!bookCardSession.active)return false;
    if(bookCardSession.favoritePickerOpen) {
        BookishUI::List v{}; v.activeTab=3;v.title=I18n::tr("В коллекции");v.subtitle=I18n::tr("OK — добавить или убрать книгу.");
        v.total=Collections::count()+2;v.selected=bookCardSession.collectionSelection;
        const size_t first=(v.selected/BookishUI::kListRows)*BookishUI::kListRows;
        static size_t previous=SIZE_MAX;if(first!=previous)scheduleGhostCleanup("collection-picker-page");previous=first;
        for(size_t i=first;i<v.total && v.rowCount<BookishUI::kListRows;++i) {
            auto &r=v.rows[v.rowCount++];r.selected=i==v.selected;
            if(i==0){r.title=I18n::tr("< Готово");r.subtitle=I18n::tr("Вернуться к книге");}
            else if(i==Collections::count()+1){r.title=I18n::tr("+ Новая коллекция");r.subtitle=I18n::tr("Создать свою подборку");}
            else {r.title=Collections::name(i-1);r.subtitle=Collections::contains(i-1,bookCardSession.bookId)?I18n::tr("Книга добавлена"):I18n::tr("Книга не добавлена");}
        }
        return renderBookishList(v);
    }
    auto *scratch=static_cast<uint8_t*>(heap_caps_malloc(kFramebufferBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!scratch || !framebuffer){heap_caps_free(scratch);return false;}
    memset(framebuffer,0xff,kFramebufferBytes);ReaderBookishCanvas c(scratch);BookishUI::Card v{};
    for(unsigned i=0;i<kVisibleTabCount;++i)if(kVisibleTabs[i]==activeTopLevelTab)v.activeTab=i;
    char status[64]{};formatLocalBookStatus(bookCardSession.localEntry,status,sizeof(status));
    v.book={bookCardSession.bookId,bookCardTitle(),bookCardAuthor(),bookCardSession.local?status:I18n::tr("Нет на устройстве"),
        static_cast<uint8_t>(bookCardSession.local?localBookProgressPercent(bookCardSession.localEntry):0),false};
    v.primary=bookCardSession.preparing?I18n::tr("Подготовка…"):bookCardSession.localCopyPresent?I18n::tr("Читать"):I18n::tr("Выбрать на сайте");
    v.series=*bookCardSeries()?bookCardSeries():I18n::tr("Серия не указана");
    v.focus=static_cast<unsigned>(bookCardSession.focus);v.favorite=Collections::contains(0,bookCardSession.bookId);
    BookishUI::card(c,v);heap_caps_free(scratch);return true;
}

bool displayBookCard(const char *reason) {
    if (!renderBookCardFrame()) {
        Serial.println("ERROR BOOK_CARD reason=frame-build-failed");
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::BookCard, "screen-book-card");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("BOOK_CARD", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.println("ERROR BOOK_CARD reason=display-failed");
        return false;
    }
    uiScreen = UiScreen::BookCard;
    bookCardSession.displayedAt = millis();
    Serial.printf(
        "BOOK CARD OPEN source=%s id=%s focus=%u annotation=%lu "
        "local_copy=%s downloading=%s preparing=%s reason=%s\n",
        bookCardSession.local ? "local" : "opds",
        bookCardSession.bookId[0] == '\0' ? "remote" : bookCardSession.bookId,
        static_cast<unsigned>(bookCardSession.focus),
        static_cast<unsigned long>(strlen(bookCardAnnotation())),
        bookCardSession.localCopyPresent || bookCardSession.local ? "true" : "false",
        bookCardSession.downloading ? "true" : "false",
        bookCardSession.preparing ? "true" : "false", reason);
    return true;
}

bool renderAnnotationFrame() {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr || framebuffer == nullptr ||
        !bookCardSession.active) {
        heap_caps_free(scratch);
        return false;
    }
    memset(framebuffer, 0xFF, kFramebufferBytes);
    ReaderBookishCanvas c(scratch); BookishUI::header(c,static_cast<unsigned>(bookCardSession.returnTab)==4?3:1);
    c.text(BookishUI::Font::Hero,I18n::tr("Аннотация"),30,212,480);
    constexpr size_t kLinesPerPage = 17;
    const size_t totalLines = drawWrappedParagraph(
        nullptr, &BookishArimo22, bookCardAnnotation(), 42, 287, 456, 34, 0,
        0);
    const size_t totalPages = max(static_cast<size_t>(1),
                                  (totalLines + kLinesPerPage - 1) /
                                      kLinesPerPage);
    bookCardSession.annotationPage = min(
        bookCardSession.annotationPage,
        static_cast<uint8_t>(min(static_cast<size_t>(255), totalPages - 1)));
    drawPortraitRoundedRect(24, 251, 492, 640, 14, 12, 15, 1);
    c.text(BookishUI::Font::Control,I18n::tr("OK — к книге"),30,937,350);
    drawWrappedParagraph(
        scratch, &BookishArimo22, bookCardAnnotation(), 42, 287, 456, 34,
        static_cast<size_t>(bookCardSession.annotationPage) * kLinesPerPage,
        kLinesPerPage);
    char page[32]{};
    snprintf(page, sizeof(page), "%u / %lu",
             static_cast<unsigned>(bookCardSession.annotationPage + 1),
             static_cast<unsigned long>(totalPages));
    const int32_t width = measurePortraitText(&UiCondensed9, page);
    drawPortraitText(scratch, &UiCondensed9, page, 508 - width, 922, 5);
    heap_caps_free(scratch);
    return true;
}

bool displayAnnotation(const char *reason) {
    if (!renderAnnotationFrame()) {
        Serial.println("ERROR ANNOTATION reason=frame-build-failed");
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::Annotation, "screen-annotation");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("ANNOTATION", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.println("ERROR ANNOTATION reason=display-failed");
        return false;
    }
    uiScreen = UiScreen::Annotation;
    Serial.printf("ANNOTATION OPEN page=%u reason=%s\n",
                  static_cast<unsigned>(bookCardSession.annotationPage + 1),
                  reason);
    return true;
}

LocalBookEntry *collectionBooks=nullptr;
LocalLibraryInfo collectionBookInfo{};
bool loadFavoritesSession() {
    favoritesSession.loaded=Collections::load();
    if(!collectionBooks)collectionBooks=static_cast<LocalBookEntry*>(heap_caps_calloc(kLocalLibraryCapacity,sizeof(LocalBookEntry),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!collectionBooks || !LocalLibrary::scan(collectionBooks,kLocalLibraryCapacity,collectionBookInfo))return false;
    if (favoritesSession.folder>=Collections::count()) favoritesSession.folder=0;
    return favoritesSession.loaded;
}
size_t favoriteFolderCount(size_t folder) { return Collections::bookCount(folder); }
const FavoriteEntry *favoriteFolderEntry(size_t folder,size_t ordinal) {
    static FavoriteEntry entry; entry=FavoriteEntry{};
    const char *id=Collections::book(folder,ordinal); if(!*id)return nullptr;
    snprintf(entry.bookId,sizeof(entry.bookId),"%s",id);
    snprintf(entry.title,sizeof(entry.title),"%s",id);
    for(size_t i=0;collectionBooks && i<collectionBookInfo.loadedCount;++i)if(!strcmp(collectionBooks[i].id,id)) {
        snprintf(entry.title,sizeof(entry.title),"%s",collectionBooks[i].title);
        snprintf(entry.author,sizeof(entry.author),"%s",collectionBooks[i].author);entry.local=true;break;
    }
    return &entry;
}

void normalizeFavoritesSelection() {
    const size_t count =
        favoritesSession.phase == FavoritesPhase::Folders
            ? Collections::count()+1
            : favoriteFolderCount(favoritesSession.folder);
    if (count == 0) {
        favoritesSession.selected = 0;
        favoritesSession.firstVisible = 0;
        return;
    }
    favoritesSession.selected = min(favoritesSession.selected, count - 1);
    constexpr size_t kRows = BookishUI::kListRows;
    favoritesSession.firstVisible =
        listPageStart(favoritesSession.selected, kRows);
}

void drawFavoriteFolderArtwork(uint8_t *scratch, FavoriteFolder folder,
                               int32_t x, int32_t y, bool selected) {
    const uint8_t ink = 0;
    const uint8_t paper = 15;
    for (int32_t layer = 2; layer >= 0; --layer) {
        const int32_t offset = layer * 8;
        drawPortraitRoundedRect(x + offset, y - offset, 82, 112, 7, ink,
                                paper, 1);
    }
    if (folder == FavoriteFolder::Favorite) {
        fillPortraitCircle(x + 34, y + 51, 10, ink);
        fillPortraitCircle(x + 48, y + 51, 10, ink);
        for (int32_t row = 0; row < 22; ++row) {
            const int32_t halfWidth = max(1, 18 - row);
            fillPortraitRect(x + 41 - halfWidth, y + 51 + row,
                             halfWidth * 2 + 1, 1, ink);
        }
    } else {
        const char *mark = folder == FavoriteFolder::WantToRead ? "+" : "·";
        const int32_t width = measurePortraitText(&UiCondensed22Medium, mark);
        drawPortraitText(scratch, &UiCondensed22Medium, mark,
                         x + 41 - width / 2, y + 66, ink, paper);
    }
    fillPortraitRect(x + 18, y + 84, 46, 2, ink);
}

bool renderFavoritesFrame(int tabFocus = -1) {
    if(!Collections::load())return false;
    BookishUI::List v{}; v.activeTab=3;v.tabFocus=tabFocus;
    const bool folders=favoritesSession.phase==FavoritesPhase::Folders;
    v.title=folders?I18n::tr("Ваши коллекции"):Collections::name(favoritesSession.folder);
    v.subtitle=folders?I18n::tr("Избранное и ваши подборки."):I18n::tr("Собранные вами истории.");
    v.back=folders?"":I18n::tr("< Ко всем коллекциям");
    v.emptyTitle=I18n::tr("Здесь пока нет книг");v.emptyHint=I18n::tr("Добавляйте книги из их карточек.");v.emptyHint2=I18n::tr("Или синхронизируйте с библиотекой.");
    v.total=folders?Collections::count()+1:favoriteFolderCount(favoritesSession.folder);
    v.selected=favoritesSession.selected;
    char details[BookishUI::kListRows][64]{}; FavoriteEntry entries[BookishUI::kListRows]{};
    for(size_t i=favoritesSession.firstVisible;i<v.total && v.rowCount<BookishUI::kListRows;++i) {
        size_t row=v.rowCount++;auto &r=v.rows[row];r.selected=i==favoritesSession.selected;
        if(folders) {
            if(i==Collections::count()) {r.title=I18n::tr("+ Новая коллекция");r.subtitle=I18n::tr("Дайте имя своей подборке");continue;}
            r.title=Collections::name(i);r.subtitle=i==0?I18n::tr("Самые любимые книги"):I18n::tr("Ваша подборка");
            snprintf(details[row],sizeof(details[row]),I18n::tr("Книг: %lu"),static_cast<unsigned long>(Collections::bookCount(i)));r.detail=details[row];
        } else {
            auto *entry=favoriteFolderEntry(favoritesSession.folder,i);if(!entry){--v.rowCount;continue;}
            entries[row]=*entry;r.title=entries[row].title;r.subtitle=entries[row].author;r.coverId=entries[row].bookId;r.book=true;
            r.detail=entry->local?I18n::tr("На устройстве"):I18n::tr("Нет на устройстве");
        }
    }
    return renderBookishList(v);
}

bool displayFavorites(bool reload, const char *reason) {
    static size_t renderedFirst = SIZE_MAX;

    static FavoritesPhase renderedPhase = FavoritesPhase::Folders;
    static size_t renderedFolder = SIZE_MAX;
    if ((reload || !favoritesSession.loaded) && !loadFavoritesSession()) {
        return false;
    }
    normalizeFavoritesSelection();
    if (renderedFirst != favoritesSession.firstVisible) scheduleGhostCleanup("list-page");
    renderedFirst = favoritesSession.firstVisible;
    if (!renderFavoritesFrame()) {
        Serial.println("ERROR FAVORITES_OPEN reason=frame-build-failed");
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::Favorites,
                                    "screen-favorites");
    if (uiScreen == UiScreen::Favorites &&
        (renderedPhase != favoritesSession.phase ||
         renderedFolder != favoritesSession.folder)) {
        scheduleGhostCleanup("favorites-folder");
    }
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, favoritesSession.phase == FavoritesPhase::Books
            ? DisplayRefreshMode::QualityFull : DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("FAVORITES", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.println("ERROR FAVORITES_OPEN reason=display-failed");
        return false;
    }
    uiScreen = UiScreen::Favorites;
    renderedPhase = favoritesSession.phase;
    renderedFolder = favoritesSession.folder;
    activeTopLevelTab = TopLevelTab::Favorites;
    Serial.printf("FAVORITES OPEN phase=%u count=%lu selected=%lu reason=%s\n",
                  static_cast<unsigned>(favoritesSession.phase),
                  static_cast<unsigned long>(
                      favoritesSession.phase == FavoritesPhase::Folders
                          ? Collections::count()+1
                          : favoriteFolderCount(favoritesSession.folder)),
                  static_cast<unsigned long>(favoritesSession.selected + 1),
                  reason);
    return true;
}

struct LibraryTabNavigation {
    LocalLibraryPhase phase = LocalLibraryPhase::Sections;
    LocalLibrarySection section = LocalLibrarySection::All;
    size_t selected = 0, firstVisible = 0;
    char group[128]{}, prefix[8]{};
};
LibraryTabNavigation savedLibraryTab;

bool displayTopLevelTab(TopLevelTab tab, bool rescan, const char *reason) {
    sectionBackFocused = false;
    if (localLibrarySession.owner == TopLevelTab::OnDevice && localLibrarySession.loaded) {
        savedLibraryTab.phase = localLibrarySession.phase;
        savedLibraryTab.section = localLibrarySession.section;
        savedLibraryTab.selected = localLibrarySession.selected;
        savedLibraryTab.firstVisible = localLibrarySession.firstVisible;
        snprintf(savedLibraryTab.group, sizeof(savedLibraryTab.group), "%s", localLibrarySession.groupLabel);
        snprintf(savedLibraryTab.prefix, sizeof(savedLibraryTab.prefix), "%s", localLibrarySession.searchPrefix);
    }
    switch (tab) {
        case TopLevelTab::Home:
            return displayHome(rescan, reason);
        case TopLevelTab::OnDevice:
            if (localLibrarySession.loaded) {
                if (localLibrarySession.owner != TopLevelTab::OnDevice) {
                    localLibrarySession.owner = TopLevelTab::OnDevice;
                    localLibrarySession.phase = savedLibraryTab.phase;
                    localLibrarySession.section = savedLibraryTab.section;
                    snprintf(localLibrarySession.groupLabel, sizeof(localLibrarySession.groupLabel), "%s", savedLibraryTab.group);
                    snprintf(localLibrarySession.searchPrefix, sizeof(localLibrarySession.searchPrefix), "%s", savedLibraryTab.prefix);
                    rebuildLocalLibraryView();
                    localLibrarySession.selected = savedLibraryTab.selected;
                    localLibrarySession.firstVisible = savedLibraryTab.firstVisible;
                }
                return displayLocalLibrary(rescan, reason);
            }
            localLibrarySession.phase = LocalLibraryPhase::Sections;
            localLibrarySession.section = LocalLibrarySection::All;
            localLibrarySession.owner = TopLevelTab::OnDevice;
            localLibrarySession.selected = 0;
            localLibrarySession.firstVisible = 0;
            localLibrarySession.groupLabel[0] = '\0';
            localLibrarySession.searchPrefix[0] = '\0';
            if (localLibrarySession.loaded && !rescan) {
                rebuildLocalLibraryView();
            }
            return displayLocalLibrary(rescan, reason);
        case TopLevelTab::Catalog:
            return displayTopLevelTab(TopLevelTab::OnDevice, rescan, reason);
        case TopLevelTab::Search:
            return displaySearch(rescan, reason);
        case TopLevelTab::Favorites:
            if (favoritesSession.loaded) return displayFavorites(rescan, reason);
            favoritesSession.phase = FavoritesPhase::Folders;
            favoritesSession.selected = 0;
            favoritesSession.firstVisible = 0;
            return displayFavorites(rescan, reason);
    }
    return false;
}

struct CachedReaderFrame {
    uint8_t *pixels = nullptr;
    char bookId[33]{};
    char layout[64]{};
    uint32_t page = 0, pageCount = 0;
    uint16_t lines = 0;
    ReaderPageIndexEntry logical{};
};
CachedReaderFrame pageFrames[3];
size_t pageFrameVictim = 0;
bool prefetchingPage = false;

CachedReaderFrame *findPageFrame(const char *bookId, uint32_t page) {
    for (auto &cached : pageFrames)
        if (cached.pixels && cached.page == page &&
            cached.pageCount == readerSession.pageCount &&
            strcmp(cached.bookId, bookId) == 0 &&
            strcmp(cached.layout, ReaderPagination::layoutId()) == 0) return &cached;
    return nullptr;
}

bool renderReaderPage(const char *bookId, uint32_t page, uint16_t &lineCount,
                      ReaderPageIndexEntry &logical) {
    if (auto *cached = findPageFrame(bookId, page)) {
        memcpy(framebuffer, cached->pixels, kFramebufferBytes);
        lineCount = cached->lines;
        logical = cached->logical;
        drawPortraitBatteryIcon(478, 10, diagnostics.batteryPercent, 0, 15);
        return true;
    }
    if (!ReaderPagination::pageEntry(bookId, page, logical)) {
        return false;
    }
    char pagesPath[96]{};
    if (!ReaderPagination::pagesPath(bookId, pagesPath, sizeof(pagesPath))) {
        return false;
    }
    File input = SD.open(pagesPath, FILE_READ);
    if (!input || !input.seek(logical.fileOffset)) {
        input.close();
        return false;
    }

    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr) {
        input.close();
        return false;
    }
    memset(framebuffer, 0xFF, kFramebufferBytes);

    const ReaderLayoutConfig &layout = readerLayout();
    // Stay above the first text line for every supported reading font.
    drawPortraitBatteryIcon(478, 10, diagnostics.batteryPercent, 0, 15);
    int32_t baselineY = layout.firstBaseline;
    uint16_t headingLines = 0;
    bool pageMarkerRead = false;
    bool rulePending = false;
    bool passed = true;
    lineCount = 0;
    while (input.available()) {
        if (prefetchingPage && ((physicalInputs && uxQueueMessagesWaiting(physicalInputs)) || Serial.available())) {
            passed = false;
            break;
        }
        String record = input.readStringUntil('\n');
        if (record.endsWith("\r")) {
            record.remove(record.length() - 1);
        }
        if (record.startsWith("PAGE\t")) {
            if (pageMarkerRead) {
                break;
            }
            pageMarkerRead = true;
            continue;
        }
        if (!pageMarkerRead || record.length() < 2 || record[1] != '\t') {
            continue;
        }
        const char type = record[0];
        if (type == 'H') {
            const String text = record.substring(2);
            const int32_t width = measureReaderUtf8(text);
            const int32_t x =
                max(layout.leftMargin, (kPortraitWidth - width) / 2);
            const int32_t headingY =
                layout.headingBaseline + headingLines * layout.lineAdvance;
            drawPortraitText(scratch, readerBodyFont(), text.c_str(), x,
                             headingY);
            ++headingLines;
            baselineY = headingY + layout.headingToBody;
            rulePending = true;
        } else if (type == 'L' && record.length() >= 4 &&
                   record[3] == '\t') {
            if (rulePending) {
                fillPortraitRect(220, baselineY - 36, 100, 1, 8);
                rulePending = false;
            }
            const bool indent = record[2] == '1';
            const String text = record.substring(4);
            drawPortraitText(scratch, readerBodyFont(), text.c_str(),
                             layout.leftMargin +
                                 (indent ? layout.firstLineIndent : 0),
                             baselineY);
            baselineY += layout.lineAdvance;
            ++lineCount;
        } else if (type == 'G') {
            const long gap = record.substring(2).toInt();
            if (gap > 0) {
                baselineY += static_cast<int32_t>(gap);
            }
        }
    }
    input.close();
    if (!passed) { heap_caps_free(scratch); return false; }
    if (rulePending) {
        fillPortraitRect(220, baselineY - 36, 100, 1, 8);
    }

    char pageLabel[32]{};
    snprintf(pageLabel, sizeof(pageLabel), I18n::tr("СТР. %lu"),
             static_cast<unsigned long>(page));
    fillPortraitRect(34, 902, 472, 1, 10);
    drawPortraitText(scratch, &UiCondensed9, pageLabel, 34, 936, 5);
    ReaderChapterEntry chapter{};
    if (ReaderPagination::currentChapter(bookId, page, chapter)) {
        char chapterLabel[128]{};
        fitPortraitText(&UiCondensed9, chapter.title, 292, chapterLabel,
                        sizeof(chapterLabel));
        const int32_t chapterWidth =
            measurePortraitText(&UiCondensed9, chapterLabel);
        drawPortraitText(scratch, &UiCondensed9, chapterLabel,
                         (kPortraitWidth - chapterWidth) / 2, 936, 5);
    }
    char percentLabel[16]{};
    const uint32_t percent =
        readerSession.pageCount == 0
            ? 0
            : min(100UL, static_cast<unsigned long>(
                             static_cast<uint64_t>(page) * 100U /
                             readerSession.pageCount));
    snprintf(percentLabel, sizeof(percentLabel), "%lu%%",
             static_cast<unsigned long>(percent));
    const int32_t percentWidth =
        measurePortraitText(&UiCondensed9, percentLabel);
    drawPortraitText(scratch, &UiCondensed9, percentLabel,
                     506 - percentWidth, 936, 5);
    heap_caps_free(scratch);
    passed = passed && pageMarkerRead && lineCount > 0;
    if (passed) {
        auto &cached = pageFrames[pageFrameVictim++ % 3];
        if (!cached.pixels) cached.pixels = static_cast<uint8_t *>(heap_caps_malloc(
            kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (cached.pixels) {
            memcpy(cached.pixels, framebuffer, kFramebufferBytes);
            snprintf(cached.bookId, sizeof(cached.bookId), "%s", bookId);
            snprintf(cached.layout, sizeof(cached.layout), "%s", ReaderPagination::layoutId());
            cached.page = page;
            cached.pageCount = readerSession.pageCount;
            cached.lines = lineCount;
            cached.logical = logical;
        }
    }
    return passed;
}

void prefetchReaderPage() {
    if (uiScreen != UiScreen::Reader || !readerSession.active ||
        displayRefresh.busy() || uiActionPending() ||
        BookPreparation::busy() || AutomaticSync::busy() ||
        millis() - lastActivityAt < 350 || Serial.available() ||
        (physicalInputs && uxQueueMessagesWaiting(physicalInputs))) return;
    uint32_t page = readerSession.currentPage + 1;
    if (page > readerSession.pageCount || findPageFrame(readerSession.bookId, page)) {
        if (readerSession.currentPage <= 1) return;
        page = readerSession.currentPage - 1;
        if (findPageFrame(readerSession.bookId, page)) return;
    }
    auto *offscreen = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!offscreen) return;
    uint8_t *visible = framebuffer;
    framebuffer = offscreen;
    prefetchingPage = true;
    uint16_t lines = 0;
    ReaderPageIndexEntry logical{};
    renderReaderPage(readerSession.bookId, page, lines, logical);
    prefetchingPage = false;
    framebuffer = visible;
    heap_caps_free(offscreen);
}

bool buildDisplayCalibrationFrameA() {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr) {
        return false;
    }

    memset(framebuffer, 0xFF, kFramebufferBytes);
    drawPortraitRect(14, 14, 512, 932, 0, 2);

    fillPortraitRect(14, 14, 512, 112, 0);
    drawPortraitText(scratch, &Roboto18, "TOP // DISPLAY LAB", 32, 58, 15, 0);
    drawPortraitText(scratch, &Roboto12, "PORTRAIT 540 x 960 / FRAME A", 32,
                     96, 15, 0);

    drawPortraitText(scratch, &Roboto32, "BLACK / WHITE", 30, 205);
    drawPortraitText(scratch, &Roboto18, "TEXT 0123456789", 32, 258);
    fillPortraitRect(30, 282, 480, 2, 0);
    fillPortraitRect(30, 290, 480, 4, 0);

    fillPortraitRect(30, 325, 480, 215, 0);
    drawPortraitText(scratch, &Roboto32, "SOLID BLACK", 64, 425, 15, 0);
    drawPortraitText(scratch, &Roboto18, "WHITE LETTERS", 118, 486, 15, 0);

    constexpr uint8_t shades[] = {0, 3, 6, 9, 12, 15};
    constexpr const char *labels[] = {"00", "03", "06", "09", "12", "15"};
    for (size_t index = 0; index < 6; ++index) {
        const int32_t x = 30 + static_cast<int32_t>(index) * 82;
        fillPortraitRect(x, 580, 68, 125, shades[index]);
        drawPortraitRect(x, 580, 68, 125, 0);
        drawPortraitText(scratch, &Roboto12, labels[index], x + 20, 746);
    }

    drawPortraitText(scratch, &Roboto18, "LINE WIDTH", 30, 807);
    fillPortraitRect(210, 782, 300, 1, 0);
    fillPortraitRect(210, 799, 300, 2, 0);
    fillPortraitRect(210, 818, 300, 4, 0);

    drawPortraitRect(30, 860, 480, 60, 0, 2);
    drawPortraitText(scratch, &Roboto12, "PHOTO: BLACK / EDGES / OLD TEXT",
                     62, 900);

    heap_caps_free(scratch);
    return true;
}

bool buildDisplayCalibrationFrameB() {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr) {
        return false;
    }

    memset(framebuffer, 0xFF, kFramebufferBytes);
    drawPortraitRect(14, 14, 512, 932, 0, 2);

    // Frame B deliberately reverses Frame A's large dark zones. Any failure
    // to erase A will be obvious in the white header and inspection area.
    drawPortraitRect(14, 14, 512, 112, 0, 2);
    drawPortraitText(scratch, &Roboto18, "TOP // FRAME B", 32, 58);
    drawPortraitText(scratch, &Roboto12, "STANDARD CLEAR + FULL DRAW", 32,
                     96);

    drawPortraitText(scratch, &Roboto32, "FRAME B", 30, 205);
    drawPortraitText(scratch, &Roboto18, "A MUST BE GONE", 32, 258);
    fillPortraitRect(30, 282, 480, 2, 0);
    fillPortraitRect(30, 290, 480, 4, 0);

    drawPortraitRect(30, 325, 480, 235, 0, 4);
    drawPortraitText(scratch, &Roboto18, "THIS WHITE AREA", 84, 410);
    drawPortraitText(scratch, &Roboto18, "WAS BLACK IN A", 86, 475);

    drawPortraitText(scratch, &Roboto12, "OLD SWATCHES MUST DISAPPEAR", 83,
                     625);
    fillPortraitRect(30, 650, 480, 2, 0);

    fillPortraitRect(30, 700, 480, 155, 0);
    drawPortraitText(scratch, &Roboto32, "NEW BLACK B", 68, 788, 15, 0);
    drawPortraitText(scratch, &Roboto12, "ONLY THIS ZONE IS SOLID", 132, 828,
                     15, 0);

    drawPortraitRect(30, 885, 480, 40, 0, 2);
    drawPortraitText(scratch, &Roboto12, "PHOTO: CHECK FOR REMAINS OF A", 72,
                     913);

    heap_caps_free(scratch);
    return true;
}

void requestDashboardRefresh(DashboardRefreshRequest request) {
    if (static_cast<uint8_t>(request) > static_cast<uint8_t>(pendingRefresh)) {
        pendingRefresh = request;
    }
    renderPending = pendingRefresh != DashboardRefreshRequest::None;
}

void printDisplayRefresh(const char *recordType,
                         const DisplayRefreshResult &result) {
    Serial.printf(
        "DISPLAY %s requested=%s applied=%s ok=%s duration_ms=%lu "
        "area=%ld,%ld,%ld,%ld sequence=%lu regional_debt=%lu queued=%s\n",
        recordType, DisplayRefreshController::modeName(result.requested),
        DisplayRefreshController::modeName(result.applied),
        result.ok ? "true" : "false",
        static_cast<unsigned long>(result.durationMs),
        static_cast<long>(result.area.x), static_cast<long>(result.area.y),
        static_cast<long>(result.area.width),
        static_cast<long>(result.area.height),
        static_cast<unsigned long>(result.sequence),
        static_cast<unsigned long>(
            displayRefresh.regionalRefreshesSinceFull()), result.queued ? "true" : "false");
}

void printFramebufferSnapshot() {
    if (framebuffer == nullptr) {
        Serial.println("DISPLAY SNAPSHOT available=false");
        return;
    }
    uint32_t hash = 2166136261U;
    uint32_t blackPixels = 0;
    uint32_t grayPixels = 0;
    uint32_t whitePixels = 0;
    for (size_t index = 0; index < kFramebufferBytes; ++index) {
        const uint8_t packed = framebuffer[index];
        hash ^= packed;
        hash *= 16777619U;
        const uint8_t shades[2] = {
            static_cast<uint8_t>(packed & 0x0FU),
            static_cast<uint8_t>(packed >> 4U),
        };
        for (const uint8_t shade : shades) {
            if (shade <= 1U) {
                ++blackPixels;
            } else if (shade >= 14U) {
                ++whitePixels;
            } else {
                ++grayPixels;
            }
        }
    }
    Serial.printf(
        "DISPLAY SNAPSHOT available=true width=540 height=960 bytes=%lu "
        "fnv1a=%08lx black=%lu gray=%lu white=%lu screen=%u tab=%s\n",
        static_cast<unsigned long>(kFramebufferBytes),
        static_cast<unsigned long>(hash),
        static_cast<unsigned long>(blackPixels),
        static_cast<unsigned long>(grayPixels),
        static_cast<unsigned long>(whitePixels),
        static_cast<unsigned>(uiScreen), topLevelTabCode(activeTopLevelTab));
}

void renderDashboard(DashboardRefreshRequest request) {
    if (framebuffer == nullptr) {
        return;
    }

    memset(framebuffer, 0xFF, kFramebufferBytes);
    epd_fill_rect(0, 0, EPD_WIDTH, 76, 0, framebuffer);
    epd_fill_rect(0, 76, EPD_WIDTH, 5, 0x77, framebuffer);

    drawText(&Roboto18, "ABYSSTAIL // E-READER", 34, 50, 15, 0);
    drawText(&Roboto12, "V2.4  |  BRING-UP 01", 714, 48, 15, 0);
    drawText(&Roboto32, "SYSTEM DIAGNOSTICS", 34, 143);
    drawText(&Roboto12, "REAL HARDWARE / SAFE WRITE TEST / USB INPUT ONLINE", 38, 170, 5);

    char psramValue[48];
    snprintf(psramValue, sizeof(psramValue), "%.1f MB  /  1 MB TEST",
             diagnostics.psramBytes / 1048576.0);
    drawStatusCard(38, 192, 426, 126, "01", "PSRAM", psramValue,
                   diagnostics.psramDetected && diagnostics.psramPassed);

    char sdValue[48];
    if (diagnostics.sdMounted) {
        snprintf(sdValue, sizeof(sdValue), "%.1f GB  /  ATOMIC I-O",
                 diagnostics.sdBytes / 1073741824.0);
    } else {
        snprintf(sdValue, sizeof(sdValue), "CARD NOT MOUNTED");
    }
    drawStatusCard(496, 192, 426, 126, "02", "MICRO SD", sdValue,
                   diagnostics.sdMounted && diagnostics.sdWritePassed);

    char batteryValue[48];
    snprintf(batteryValue, sizeof(batteryValue), "%u mV  /  RAW %u",
             diagnostics.batteryMillivolts, diagnostics.batteryRaw);
    const bool batteryPlausible = diagnostics.batteryMillivolts >= 3000 &&
                                  diagnostics.batteryMillivolts <= 4500;
    drawStatusCard(38, 338, 426, 126, "03", "BATTERY", batteryValue,
                   batteryPlausible);

    char inputValue[48];
    snprintf(inputValue, sizeof(inputValue), "%s  /  #%lu", lastInput,
             static_cast<unsigned long>(inputCount));
    drawStatusCard(496, 338, 426, 126, "04", "INPUT CHANNEL", inputValue, true);

    char footer[128];
    snprintf(footer, sizeof(footer),
             "USB CDC 115200  //  RTC %s  //  FLASH %.1f MB  //  CENTER GPIO21",
             diagnostics.rtcDetected ? "ONLINE" : "OFFLINE",
             diagnostics.flashBytes / 1048576.0);
    epd_draw_hline(38, 492, 884, 0x55, framebuffer);
    drawText(&Roboto12, footer, 40, 521, 3);

    DisplayRefreshMode mode = DisplayRefreshMode::QualityFull;
    Rect_t area = epd_full_screen();
    if (request == DashboardRefreshRequest::InputRegion) {
        mode = DisplayRefreshMode::QualityRegion;
        area = kInputCardArea;
    }

    lastDisplayRefresh = displayRefresh.refresh(framebuffer, mode, area);
    hasDisplayRefresh = true;
    printDisplayRefresh("REFRESH", lastDisplayRefresh);

    pendingRefresh = DashboardRefreshRequest::None;
    renderPending = false;
}

bool displayReaderPage(uint32_t page, const char *reason) {
    if (!readerSession.active || page == 0 ||
        page > readerSession.pageCount || framebuffer == nullptr ||
        !displayInitialized) {
        Serial.printf("ERROR READER_PAGE reason=invalid-session page=%lu\n",
                      static_cast<unsigned long>(page));
        return false;
    }

    uint16_t lineCount = 0;
    ReaderPageIndexEntry logical{};
    const uint32_t layoutStartedAt = millis();
    if (!renderReaderPage(readerSession.bookId, page, lineCount, logical)) {
        Serial.printf("ERROR READER_PAGE reason=render-cache page=%lu\n",
                      static_cast<unsigned long>(page));
        return false;
    }
    Serial.printf(
        "READER PAGE READY id=%s page=%lu/%lu lines=%u logical=%lu:%lu "
        "layout_ms=%lu reason=%s\n",
        readerSession.bookId, static_cast<unsigned long>(page),
        static_cast<unsigned long>(readerSession.pageCount),
        static_cast<unsigned>(lineCount),
        static_cast<unsigned long>(logical.sourceRecord),
        static_cast<unsigned long>(logical.sourceByte),
        static_cast<unsigned long>(millis() - layoutStartedAt), reason);
    Serial.flush();

    scheduleScreenTransitionCleanup(UiScreen::Reader, "screen-reader");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::ReaderText);
    hasDisplayRefresh = true;
    printDisplayRefresh("READER", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.printf("ERROR READER_PAGE reason=display page=%lu\n",
                      static_cast<unsigned long>(page));
        return false;
    }

    readerSession.currentPage = page;
    ReaderProgress progress{};
    const bool stateSaved = ReaderPagination::saveProgress(
        readerSession.bookId, page, readerSession.pageCount, logical, progress);
    Serial.printf(
        "READER PAGE COMPLETE id=%s page=%lu/%lu state_saved=%s "
        "logical=%lu:%lu\n",
        readerSession.bookId, static_cast<unsigned long>(page),
        static_cast<unsigned long>(readerSession.pageCount),
        stateSaved ? "true" : "false",
        static_cast<unsigned long>(logical.sourceRecord),
        static_cast<unsigned long>(logical.sourceByte));
    if (!stateSaved) {
        Serial.printf("ERROR READER_STATE reason=%s page=%lu\n",
                      progress.error, static_cast<unsigned long>(page));
    }
    uiScreen = UiScreen::Reader;
    // The page is already visible and the in-memory position is valid. A
    // failed checkpoint must not turn a successful page turn into a failed
    // reader session; report it separately and keep reading available.
    return true;
}

const char *readerMenuLabel(ReaderMenuFocus focus) {
    switch (focus) {
        case ReaderMenuFocus::Contents:
            return I18n::tr("ОГЛАВЛЕНИЕ");
        case ReaderMenuFocus::Bookmarks:
            return I18n::tr("ЗАКЛАДКИ");
        case ReaderMenuFocus::BookInfo:
            return I18n::tr("О КНИГЕ");
        case ReaderMenuFocus::ReadingSettings:
            return I18n::tr("НАСТРОЙКИ ЧТЕНИЯ");
        case ReaderMenuFocus::Favorite:
            return I18n::tr("В ИЗБРАННОЕ");
        case ReaderMenuFocus::MarkRead:
            return I18n::tr("ОТМЕТИТЬ ПРОЧИТАННОЙ");
        case ReaderMenuFocus::Close:
            return I18n::tr("ЗАКРЫТЬ КНИГУ");
    }
    return I18n::tr("ДЕЙСТВИЕ");
}

void drawReaderMenuIcon(ReaderMenuFocus focus, int32_t centerX,
                        int32_t centerY, uint8_t shade,
                        uint8_t background) {
    switch (focus) {
        case ReaderMenuFocus::Contents:
            for (int32_t row = -9; row <= 9; row += 9) {
                fillPortraitCircle(centerX - 10, centerY + row, 2, shade);
                drawPortraitLine(centerX - 4, centerY + row,
                                 centerX + 12, centerY + row, shade, 2);
            }
            break;
        case ReaderMenuFocus::Bookmarks:
            drawPortraitRoundedRect(centerX - 9, centerY - 13, 18, 27, 3,
                                    shade, background, 2);
            drawPortraitLine(centerX - 8, centerY + 12, centerX,
                             centerY + 5, shade, 2);
            drawPortraitLine(centerX, centerY + 5, centerX + 8,
                             centerY + 12, shade, 2);
            break;
        case ReaderMenuFocus::BookInfo:
            drawPortraitCircle(centerX, centerY, 14, shade, background, 2);
            fillPortraitCircle(centerX, centerY - 7, 2, shade);
            drawPortraitLine(centerX, centerY - 1, centerX, centerY + 9,
                             shade, 3);
            break;
        case ReaderMenuFocus::ReadingSettings:
            drawPortraitLine(centerX - 13, centerY - 8, centerX + 13,
                             centerY - 8, shade, 2);
            drawPortraitLine(centerX - 13, centerY, centerX + 13,
                             centerY, shade, 2);
            drawPortraitLine(centerX - 13, centerY + 8, centerX + 13,
                             centerY + 8, shade, 2);
            fillPortraitCircle(centerX - 5, centerY - 8, 4, shade);
            fillPortraitCircle(centerX + 7, centerY, 4, shade);
            fillPortraitCircle(centerX - 1, centerY + 8, 4, shade);
            break;
        case ReaderMenuFocus::Favorite:
            fillPortraitCircle(centerX - 6, centerY - 5, 7, shade);
            fillPortraitCircle(centerX + 6, centerY - 5, 7, shade);
            for (int32_t row = 0; row < 13; ++row) {
                const int32_t halfWidth = max(0, 12 - row);
                fillPortraitRect(centerX - halfWidth, centerY - 1 + row,
                                 halfWidth * 2 + 1, 1, shade);
            }
            break;
        case ReaderMenuFocus::MarkRead:
            drawPortraitCircle(centerX, centerY, 14, shade, background, 2);
            drawPortraitLine(centerX - 8, centerY, centerX - 2,
                             centerY + 7, shade, 3);
            drawPortraitLine(centerX - 2, centerY + 7, centerX + 9,
                             centerY - 7, shade, 3);
            break;
        case ReaderMenuFocus::Close:
            drawPortraitLine(centerX - 10, centerY - 10, centerX + 10,
                             centerY + 10, shade, 3);
            drawPortraitLine(centerX + 10, centerY - 10, centerX - 10,
                             centerY + 10, shade, 3);
            break;
    }
}

bool renderReaderMenuFrame() {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr || framebuffer == nullptr) {
        heap_caps_free(scratch);
        return false;
    }
    Fb2CacheInfo metadata{};
    Fb2Cache::load(readerSession.bookId, metadata);
    char eyebrow[192]{};
    fitPortraitText(&UiCondensed9,
                    metadata.title[0] == '\0' ? "ABYSS // READER"
                                               : metadata.title,
                    390, eyebrow, sizeof(eyebrow));
    memset(framebuffer, 0xFF, kFramebufferBytes);
    drawScreenChrome(scratch, eyebrow, I18n::tr("МЕНЮ ЧТЕНИЯ"));
    char position[48]{};
    snprintf(position, sizeof(position), "%lu / %lu",
             static_cast<unsigned long>(readerSession.currentPage),
             static_cast<unsigned long>(readerSession.pageCount));
    const int32_t positionWidth =
        measurePortraitText(&UiCondensed9, position);
    drawPortraitText(scratch, &UiCondensed9, position, 468 - positionWidth,
                     38, 4, 15);
    drawPortraitBatteryIcon(480, 27, diagnostics.batteryPercent, 0, 15);

    ReaderUserState userState{};
    ReaderPagination::loadUserState(readerSession.bookId,
                                    readerSession.pageCount, userState);

    for (uint8_t index = 0; index < 7; ++index) {
        const ReaderMenuFocus focus = static_cast<ReaderMenuFocus>(index);
        const bool selected = readerMenuSession.focus == focus;
        const int32_t y = 142 + static_cast<int32_t>(index) * 104;
        drawSoftRow(26, y, 488, 88, selected);
        const uint8_t foreground = 0;
        const uint8_t background = 15;
        drawReaderMenuIcon(focus, 68, y + 44, foreground, background);
        const char *label =
            focus == ReaderMenuFocus::MarkRead && userState.finished
                ? I18n::tr("ОТМЕТИТЬ НЕПРОЧИТАННОЙ")
                : readerMenuLabel(focus);
        drawPortraitText(scratch, &UiCondensed14Bold, label, 108, y + 53,
                         foreground, background);
    }
    heap_caps_free(scratch);
    return true;
}

bool displayReaderMenu(const char *reason) {
    if (!readerSession.active || !renderReaderMenuFrame()) {
        Serial.println("ERROR READER_MENU reason=frame-build-failed");
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::ReaderMenu,
                                    "screen-reader-menu");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("READER_MENU", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.println("ERROR READER_MENU reason=display-failed");
        return false;
    }
    uiScreen = UiScreen::ReaderMenu;
    Serial.printf("READER MENU READY focus=%u reason=%s\n",
                  static_cast<unsigned>(readerMenuSession.focus), reason);
    return true;
}

bool renderContentsFrame() {
    constexpr uint32_t kRowsPerScreen = 8;
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr || framebuffer == nullptr) {
        heap_caps_free(scratch);
        return false;
    }
    Fb2CacheInfo metadata{};
    Fb2Cache::load(readerSession.bookId, metadata);
    char eyebrow[192]{};
    fitPortraitText(&UiCondensed9,
                    metadata.title[0] == '\0' ? "ABYSS // READER"
                                               : metadata.title,
                    430, eyebrow, sizeof(eyebrow));
    memset(framebuffer, 0xFF, kFramebufferBytes);
    drawScreenChrome(scratch, eyebrow, I18n::tr("ОГЛАВЛЕНИЕ"));
    char summary[48]{};
    snprintf(summary, sizeof(summary), I18n::tr("%lu ГЛАВ"),
             static_cast<unsigned long>(readerSession.chapterCount));
    drawPortraitText(scratch, &UiCondensed9, summary, 30, 145, 4);
    fillPortraitRect(28, 158, 484, 2, 0);

    ReaderChapterEntry current{};
    const bool hasCurrent = ReaderPagination::currentChapter(
        readerSession.bookId, readerSession.currentPage, current);
    for (uint32_t row = 0; row < kRowsPerScreen; ++row) {
        const uint32_t index = contentsSession.firstVisible + row;
        if (index >= readerSession.chapterCount) {
            break;
        }
        ReaderChapterEntry chapter{};
        if (!ReaderPagination::chapterEntry(readerSession.bookId, index + 1,
                                            chapter)) {
            continue;
        }
        const bool selected = index == contentsSession.selected;
        const uint8_t foreground = 0;
        const uint8_t background = 15;
        const int32_t y = 176 + static_cast<int32_t>(row) * 88;
        drawSoftRow(26, y, 488, 76, selected);
        if (hasCurrent && current.ordinal == chapter.ordinal) {
            fillPortraitCircle(52, y + 38, 4, foreground);
        }
        char ordinal[8]{};
        snprintf(ordinal, sizeof(ordinal), "%02lu",
                 static_cast<unsigned long>(chapter.ordinal));
        drawPortraitText(scratch, &UiCondensed9, ordinal, 66, y + 31,
                         selected ? 11 : 5, background);
        PortraitTextLines title{};
        wrapPortraitText(&UiCondensed13, chapter.title, 326, 2, title);
        drawPortraitTextLines(scratch, &UiCondensed13, title, 106,
                              title.count <= 1 ? y + 45 : y + 31, 22,
                              foreground, background);
        char page[20]{};
        snprintf(page, sizeof(page), "%lu",
                 static_cast<unsigned long>(chapter.page));
        const int32_t pageWidth = measurePortraitText(&UiCondensed9, page);
        drawPortraitText(scratch, &UiCondensed9, page, 490 - pageWidth,
                         y + 45, 3, background);
    }
    char position[32]{};
    const uint32_t pageCount = max(
        static_cast<uint32_t>(1),
        (readerSession.chapterCount + kRowsPerScreen - 1) / kRowsPerScreen);
    snprintf(position, sizeof(position), "%lu / %lu",
             static_cast<unsigned long>(
                 contentsSession.firstVisible / kRowsPerScreen + 1),
             static_cast<unsigned long>(pageCount));
    const int32_t width = measurePortraitText(&UiCondensed9, position);
    drawPortraitText(scratch, &UiCondensed9, position, 508 - width, 922, 5);
    heap_caps_free(scratch);
    return true;
}

bool displayContents(bool resetSelection, const char *reason) {
    static size_t renderedFirst = SIZE_MAX;
    constexpr uint32_t kRowsPerScreen = 8;
    if (!readerSession.active || readerSession.chapterCount == 0) {
        Serial.println("ERROR CONTENTS reason=no-chapters");
        return false;
    }
    if (resetSelection) {
        ReaderChapterEntry current{};
        contentsSession.selected =
            ReaderPagination::currentChapter(readerSession.bookId,
                                             readerSession.currentPage,
                                             current)
                ? current.ordinal - 1
                : 0;
        contentsSession.firstVisible =
            listPageStart(contentsSession.selected, kRowsPerScreen);
    }
    if (renderedFirst != contentsSession.firstVisible) scheduleGhostCleanup("list-page");
    renderedFirst = contentsSession.firstVisible;
    if (!renderContentsFrame()) {
        Serial.println("ERROR CONTENTS reason=frame-build-failed");
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::Contents, "screen-contents");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("CONTENTS", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.println("ERROR CONTENTS reason=display-failed");
        return false;
    }
    uiScreen = UiScreen::Contents;
    Serial.printf("CONTENTS READY selected=%lu/%lu reason=%s\n",
                  static_cast<unsigned long>(contentsSession.selected + 1),
                  static_cast<unsigned long>(readerSession.chapterCount),
                  reason);
    return true;
}

bool currentPageBookmarked(const ReaderUserState &state) {
    for (size_t index = 0; index < state.bookmarkCount; ++index) {
        if (state.bookmarks[index].page == readerSession.currentPage) {
            return true;
        }
    }
    return false;
}

bool renderReaderBookmarksFrame() {
    constexpr size_t kRowsPerScreen = 8;
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr || framebuffer == nullptr) {
        heap_caps_free(scratch);
        return false;
    }
    Fb2CacheInfo metadata{};
    Fb2Cache::load(readerSession.bookId, metadata);
    char eyebrow[192]{};
    fitPortraitText(&UiCondensed9,
                    metadata.title[0] == '\0' ? "ABYSS // READER"
                                               : metadata.title,
                    430, eyebrow, sizeof(eyebrow));
    memset(framebuffer, 0xFF, kFramebufferBytes);
    drawScreenChrome(scratch, eyebrow, I18n::tr("ЗАКЛАДКИ"));
    char summary[48]{};
    snprintf(summary, sizeof(summary), I18n::tr("%lu СОХРАНЕНО"),
             static_cast<unsigned long>(
                 readerBookmarksSession.state.bookmarkCount));
    drawPortraitText(scratch, &UiCondensed9, summary, 30, 145, 4);
    fillPortraitRect(28, 158, 484, 2, 0);

    const size_t itemCount =
        readerBookmarksSession.state.bookmarkCount + 1;
    for (size_t row = 0; row < kRowsPerScreen; ++row) {
        const size_t index = readerBookmarksSession.firstVisible + row;
        if (index >= itemCount) {
            break;
        }
        const bool selected = index == readerBookmarksSession.selected;
        const uint8_t foreground = 0;
        const uint8_t background = 15;
        const int32_t y = 176 + static_cast<int32_t>(row) * 88;
        drawSoftRow(26, y, 488, 76, selected);
        if (index == 0) {
            drawReaderMenuIcon(ReaderMenuFocus::Bookmarks, 58, y + 38,
                               foreground, background);
            const char *action =
                currentPageBookmarked(readerBookmarksSession.state)
                    ? I18n::tr("УДАЛИТЬ ЗАКЛАДКУ ЗДЕСЬ")
                    : I18n::tr("ДОБАВИТЬ ТЕКУЩУЮ СТРАНИЦУ");
            drawPortraitText(scratch, &UiCondensed13, action, 92, y + 34,
                             foreground, background);
            char page[32]{};
            snprintf(page, sizeof(page), I18n::tr("СТР. %lu"),
                     static_cast<unsigned long>(readerSession.currentPage));
            drawPortraitText(scratch, &UiCondensed9, page, 92, y + 59,
                             selected ? 11 : 5, background);
            continue;
        }
        const ReaderBookmark &bookmark =
            readerBookmarksSession.state.bookmarks[index - 1];
        char ordinal[8]{};
        snprintf(ordinal, sizeof(ordinal), "%02lu",
                 static_cast<unsigned long>(index));
        drawPortraitText(scratch, &UiCondensed9, ordinal, 48, y + 31,
                         selected ? 11 : 5, background);
        PortraitTextLines title{};
        wrapPortraitText(&UiCondensed13, bookmark.title, 326, 2, title);
        drawPortraitTextLines(scratch, &UiCondensed13, title, 88,
                              title.count <= 1 ? y + 43 : y + 29, 22,
                              foreground, background);
        char progress[36]{};
        const uint32_t percent = min(
            100UL, static_cast<unsigned long>(
                       static_cast<uint64_t>(bookmark.page) * 100U /
                       readerSession.pageCount));
        snprintf(progress, sizeof(progress), "%lu%%  %lu",
                 static_cast<unsigned long>(percent),
                 static_cast<unsigned long>(bookmark.page));
        const int32_t width = measurePortraitText(&UiCondensed9, progress);
        drawPortraitText(scratch, &UiCondensed9, progress, 490 - width,
                         y + 61, 3, background);
    }
    heap_caps_free(scratch);
    return true;
}

bool displayReaderBookmarks(bool resetSelection, const char *reason) {
    static size_t renderedFirst = SIZE_MAX;
    constexpr size_t kRowsPerScreen = 8;
    ReaderUserState state{};
    if (!readerSession.active || !ReaderPagination::loadUserState(
                                     readerSession.bookId,
                                     readerSession.pageCount, state)) {
        Serial.printf("ERROR READER_BOOKMARKS reason=%s\n", state.error);
        return false;
    }
    readerBookmarksSession.state = state;
    if (resetSelection) {
        readerBookmarksSession.selected = 0;
        readerBookmarksSession.firstVisible = 0;
    } else {
        const size_t itemCount = state.bookmarkCount + 1;
        readerBookmarksSession.selected =
            min(readerBookmarksSession.selected, itemCount - 1);
        readerBookmarksSession.firstVisible =
            listPageStart(readerBookmarksSession.selected, kRowsPerScreen);
    }
    if (renderedFirst != readerBookmarksSession.firstVisible) scheduleGhostCleanup("list-page");
    renderedFirst = readerBookmarksSession.firstVisible;
    if (!renderReaderBookmarksFrame()) {
        Serial.println("ERROR READER_BOOKMARKS reason=frame-build-failed");
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::Bookmarks,
                                    "screen-bookmarks");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("READER_BOOKMARKS", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        Serial.println("ERROR READER_BOOKMARKS reason=display-failed");
        return false;
    }
    uiScreen = UiScreen::Bookmarks;
    Serial.printf("READER BOOKMARKS READY count=%lu selected=%lu reason=%s\n",
                  static_cast<unsigned long>(state.bookmarkCount),
                  static_cast<unsigned long>(
                      readerBookmarksSession.selected),
                  reason);
    return true;
}

void processReaderBookmarksActions() {
    constexpr size_t kRowsPerScreen = 8;
    if (pendingReaderBookmarksBack) {
        pendingReaderBookmarksBack = false;
        pendingReaderBookmarksSelect = false;
        pendingReaderBookmarksDelta = 0;
        if (uiScreen == UiScreen::Bookmarks) {
            displayReaderMenu("bookmarks-back");
        }
        return;
    }
    if (pendingReaderBookmarksDelta != 0 &&
        millis() - pendingReaderBookmarksNavigationAt <
            kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingReaderBookmarksDelta;
    pendingReaderBookmarksDelta = 0;
    if (delta != 0) {
        if (uiScreen != UiScreen::Bookmarks) {
            return;
        }
        const size_t itemCount =
            readerBookmarksSession.state.bookmarkCount + 1;
        const int32_t target = max(
            0, min(static_cast<int32_t>(itemCount) - 1,
                   static_cast<int32_t>(readerBookmarksSession.selected) +
                       delta));
        if (target !=
            static_cast<int32_t>(readerBookmarksSession.selected)) {
            readerBookmarksSession.selected = static_cast<size_t>(target);
            readerBookmarksSession.firstVisible =
                listPageStart(readerBookmarksSession.selected, kRowsPerScreen);
            displayReaderBookmarks(false, "selection");
        }
        return;
    }
    if (!pendingReaderBookmarksSelect) {
        return;
    }
    pendingReaderBookmarksSelect = false;
    if (uiScreen != UiScreen::Bookmarks) {
        return;
    }
    if (readerBookmarksSession.selected > 0) {
        const ReaderBookmark bookmark = readerBookmarksSession.state.bookmarks[
            readerBookmarksSession.selected - 1];
        displayReaderPage(bookmark.page, "bookmark-jump");
        return;
    }
    ReaderPageIndexEntry logical{};
    if (!ReaderPagination::pageEntry(readerSession.bookId,
                                     readerSession.currentPage, logical)) {
        Serial.println("ERROR READER_BOOKMARK reason=page-index");
        return;
    }
    ReaderChapterEntry chapter{};
    char label[160]{};
    if (ReaderPagination::currentChapter(readerSession.bookId,
                                         readerSession.currentPage,
                                         chapter)) {
        snprintf(label, sizeof(label), "%s", chapter.title);
    } else {
        snprintf(label, sizeof(label), I18n::tr("СТРАНИЦА %lu"),
                 static_cast<unsigned long>(readerSession.currentPage));
    }
    bool added = false;
    ReaderUserState state{};
    if (!ReaderPagination::toggleBookmark(
            readerSession.bookId, readerSession.currentPage,
            readerSession.pageCount, logical, label, added, state)) {
        Serial.printf("ERROR READER_BOOKMARK reason=%s\n", state.error);
        return;
    }
    Serial.printf("READER BOOKMARK SAVED page=%lu added=%s count=%lu\n",
                  static_cast<unsigned long>(readerSession.currentPage),
                  added ? "true" : "false",
                  static_cast<unsigned long>(state.bookmarkCount));
    displayReaderBookmarks(false, added ? "bookmark-added"
                                        : "bookmark-removed");
}

const char *readerTextSizeLabel(ReaderTextSize value) {
    switch (value) {
        case ReaderTextSize::Small:
            return I18n::tr("КОМПАКТНЫЙ");
        case ReaderTextSize::Large:
            return I18n::tr("КРУПНЫЙ");
        case ReaderTextSize::Medium:
            return I18n::tr("ОБЫЧНЫЙ");
    }
    return I18n::tr("ОБЫЧНЫЙ");
}

const char *readerLineSpacingLabel(ReaderLineSpacing value) {
    switch (value) {
        case ReaderLineSpacing::Compact:
            return I18n::tr("ПЛОТНЫЙ");
        case ReaderLineSpacing::Airy:
            return I18n::tr("ВОЗДУШНЫЙ");
        case ReaderLineSpacing::Normal:
            return I18n::tr("ОБЫЧНЫЙ");
    }
    return I18n::tr("ОБЫЧНЫЙ");
}

const char *orderedSelectorLabel(OrderedSelectorKind kind, uint8_t value) {
    return kind == OrderedSelectorKind::TextSize
               ? readerTextSizeLabel(static_cast<ReaderTextSize>(value))
               : readerLineSpacingLabel(
                     static_cast<ReaderLineSpacing>(value));
}

bool renderReadingSettingsFrame() {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr || framebuffer == nullptr) {
        heap_caps_free(scratch);
        return false;
    }
    memset(framebuffer, 0xFF, kFramebufferBytes);
    drawScreenChrome(scratch, "ABYSS // READER", I18n::tr("ЧТЕНИЕ"));
    drawPortraitText(scratch, &UiCondensed9, I18n::tr("ВИД СТРАНИЦЫ"), 30, 148, 5);
    fillPortraitRect(28, 160, 484, 2, 0);

    const char *labels[2] = {I18n::tr("РАЗМЕР ТЕКСТА"), I18n::tr("МЕЖСТРОЧНЫЙ ИНТЕРВАЛ")};
    const char *values[2] = {
        readerTextSizeLabel(readingSettingsSession.settings.textSize),
        readerLineSpacingLabel(readingSettingsSession.settings.lineSpacing),
    };
    for (uint8_t index = 0; index < 2; ++index) {
        const bool selected =
            index == static_cast<uint8_t>(readingSettingsSession.focus);
        const uint8_t foreground = 0;
        const uint8_t background = 15;
        const int32_t y = 212 + static_cast<int32_t>(index) * 142;
        drawSoftRow(26, y, 488, 116, selected);
        drawReaderMenuIcon(ReaderMenuFocus::ReadingSettings, 66, y + 58,
                           foreground, background);
        drawPortraitText(scratch, &UiCondensed9, labels[index], 104, y + 40,
                         selected ? 11 : 5, background);
        drawPortraitText(scratch, &UiCondensed16Bold, values[index], 104,
                         y + 82, foreground, background);
        drawPortraitText(scratch, &UiCondensed13, "›", 474, y + 70,
                         selected ? 11 : 5, background);
    }
    drawPortraitText(scratch, &UiCondensed9,
                     I18n::tr("ИЗМЕНЕНИЯ ПЕРЕСЧИТАЮТ СТРАНИЦЫ, ПОЗИЦИЯ СОХРАНИТСЯ"),
                     30, 884, 6);
    heap_caps_free(scratch);
    return true;
}

bool displayReadingSettings(bool reload, const char *reason) {
    if (reload) {
        ReaderSettings loaded{};
        if (ReaderSettingsStore::load(loaded)) {
            activeReaderSettings = loaded;
        }
        sleepSwitchLatching.store(activeReaderSettings.sleepLatching);
        readingSettingsSession.settings = activeReaderSettings;
    }
    if (!renderReadingSettingsFrame()) {
        Serial.println("ERROR READING_SETTINGS reason=frame-build-failed");
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::ReadingSettings,
                                    "screen-reading-settings");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("READING_SETTINGS", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        return false;
    }
    uiScreen = UiScreen::ReadingSettings;
    Serial.printf("READING SETTINGS READY size=%u spacing=%u reason=%s\n",
                  static_cast<unsigned>(
                      readingSettingsSession.settings.textSize),
                  static_cast<unsigned>(
                      readingSettingsSession.settings.lineSpacing),
                  reason);
    return true;
}

bool renderOrderedSelectorFrame() {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr || framebuffer == nullptr) {
        heap_caps_free(scratch);
        return false;
    }
    memset(framebuffer, 0xFF, kFramebufferBytes);
    const char *title = orderedSelectorSession.kind ==
                                OrderedSelectorKind::TextSize
                            ? I18n::tr("РАЗМЕР ТЕКСТА")
                            : I18n::tr("ИНТЕРВАЛ");
    drawScreenChrome(scratch, "ABYSS // READER", title);
    drawPortraitText(scratch, &UiCondensed9, I18n::tr("ВЫБЕРИТЕ ЗНАЧЕНИЕ"), 30, 148,
                     5);
    fillPortraitRect(28, 160, 484, 2, 0);

    for (int8_t offset = -1; offset <= 1; ++offset) {
        const int16_t value =
            static_cast<int16_t>(orderedSelectorSession.value) + offset;
        if (value < 0 || value > 2) {
            continue;
        }
        const bool selected = offset == 0;
        const int32_t y = 258 + static_cast<int32_t>(offset + 1) * 154;
        drawSoftRow(selected ? 38 : 58, y,
                    selected ? 464 : 424, selected ? 126 : 106, selected);
        const char *label = orderedSelectorLabel(
            orderedSelectorSession.kind, static_cast<uint8_t>(value));
        const GFXfont *font = selected ? &UiCondensed22Medium
                                      : &UiCondensed16Bold;
        const int32_t width = measurePortraitText(font, label);
        drawPortraitText(scratch, font, label, (540 - width) / 2,
                         y + (selected ? 78 : 66), selected ? 0 : 6, 15);
    }
    fillPortraitRect(214, 827, 112, 3, 0);
    heap_caps_free(scratch);
    return true;
}

bool displayOrderedSelector(const char *reason) {
    if (!renderOrderedSelectorFrame()) {
        Serial.println("ERROR ORDERED_SELECTOR reason=frame-build-failed");
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::OrderedSelector,
                                    "screen-selector");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("ORDERED_SELECTOR", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        return false;
    }
    uiScreen = UiScreen::OrderedSelector;
    Serial.printf("ORDERED SELECTOR READY kind=%u value=%u reason=%s\n",
                  static_cast<unsigned>(orderedSelectorSession.kind),
                  static_cast<unsigned>(orderedSelectorSession.value),
                  reason);
    return true;
}

bool restorePortableReaderUserState(const ReaderUserState &,
                                    uint32_t pageCount) {
    // State loading remaps all bookmarks together, retaining IDs and exact
    // text anchors even when multiple bookmarks share a page after reflow.
    ReaderUserState state{};
    return ReaderPagination::loadUserState(readerSession.bookId, pageCount, state);

}

bool applyOrderedReaderSetting() {
    ReaderSettings candidate = activeReaderSettings;
    if (orderedSelectorSession.kind == OrderedSelectorKind::TextSize) {
        candidate.textSize =
            static_cast<ReaderTextSize>(orderedSelectorSession.value);
    } else {
        candidate.lineSpacing =
            static_cast<ReaderLineSpacing>(orderedSelectorSession.value);
    }
    if (candidate.textSize == activeReaderSettings.textSize &&
        candidate.lineSpacing == activeReaderSettings.lineSpacing) {
        readingSettingsSession.settings = activeReaderSettings;
        return displayReadingSettings(false, "selector-unchanged");
    }

    if (!completingSettings) {
        if (BookPreparation::busy() || AutomaticSync::busy()) return false;
        beforePreparationSettings = activeReaderSettings;
        ReaderSettingsStore::apply(candidate);
        AutomaticSync::setPaused(true);
        if (!BookPreparation::start(readerSession.bookId)) {
            ReaderSettingsStore::apply(activeReaderSettings);
            AutomaticSync::setPaused(false);
            return false;
        }
        preparingSettings = true;
        busyFrame = 0;
        busyNextFrameAt = 0;
        return true;
    }

    ReaderProgress portableProgress{};
    ReaderUserState portableState{};
    const bool portableFound = ReaderPagination::loadPortableState(
        readerSession.bookId, portableProgress, portableState);
    ReaderPageIndexEntry fallbackLogical{};
    ReaderPagination::pageEntry(readerSession.bookId,
                                readerSession.currentPage,
                                fallbackLogical);
    const ReaderSettings previous = activeReaderSettings;
    if (!ReaderSettingsStore::save(candidate)) {
        Serial.println("ERROR READING_SETTINGS reason=save-failed");
        return displayReadingSettings(false, "save-failed");
    }
    ReaderSettingsStore::apply(candidate);
    activeReaderSettings = candidate;

    Fb2CacheInfo cache{};
    Fb2Cache::load(readerSession.bookId, cache);
    BookPreparationResult &preparation = bookPreparationWorkspace;
    if (!prepareBookArtifacts(readerSession.bookId, "reading-settings",
                              preparation, cache.ok ? &cache : nullptr,
                              nullptr)) {
        ReaderSettingsStore::save(previous);
        ReaderSettingsStore::apply(previous);
        activeReaderSettings = previous;
        readingSettingsSession.settings = previous;
        Serial.printf("ERROR READING_SETTINGS reason=%s\n",
                      preparation.error);
        return displayReadingSettings(false, "pagination-failed");
    }

    const ReaderPageIndexEntry targetLogical =
        portableFound ? portableProgress.logical : fallbackLogical;
    uint32_t targetPage = 1;
    ReaderPagination::pageForLogical(readerSession.bookId, targetLogical,
                                     targetPage);
    ReaderPageIndexEntry newLogical{};
    ReaderPagination::pageEntry(readerSession.bookId, targetPage,
                                newLogical);
    readerSession.pageCount = preparation.pagination.pageCount;
    readerSession.chapterCount = preparation.pagination.chapterCount;
    readerSession.currentPage = targetPage;
    ReaderProgress saved{};
    if (!ReaderPagination::saveProgress(
            readerSession.bookId, targetPage, readerSession.pageCount,
            newLogical, saved)) {
        Serial.printf("ERROR READING_SETTINGS state=%s\n", saved.error);
    } else if (portableFound &&
               !restorePortableReaderUserState(portableState,
                                               readerSession.pageCount)) {
        Serial.println("ERROR READING_SETTINGS bookmarks=migration-failed");
    }
    readingSettingsSession.settings = activeReaderSettings;
    homeSession.loaded = false;
    localLibrarySession.loaded = false;
    favoritesSession.loaded = false;
    Serial.printf(
        "READING SETTINGS APPLIED size=%u spacing=%u page=%lu/%lu\n",
        static_cast<unsigned>(candidate.textSize),
        static_cast<unsigned>(candidate.lineSpacing),
        static_cast<unsigned long>(targetPage),
        static_cast<unsigned long>(readerSession.pageCount));
    return displayReadingSettings(false, "selector-applied");
}

void processReadingSettingsActions() {
    if (pendingReadingSettingsBack) {
        pendingReadingSettingsBack = false;
        pendingReadingSettingsSelect = false;
        pendingReadingSettingsDelta = 0;
        if (uiScreen == UiScreen::ReadingSettings) {
            displayReaderMenu("reading-settings-back");
        }
        return;
    }
    if (pendingReadingSettingsDelta != 0 &&
        millis() - pendingReadingSettingsNavigationAt <
            kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingReadingSettingsDelta;
    pendingReadingSettingsDelta = 0;
    if (delta != 0) {
        if (uiScreen != UiScreen::ReadingSettings) {
            return;
        }
        const int32_t target = max(
            0, min(1, static_cast<int32_t>(readingSettingsSession.focus) +
                          delta));
        if (target != static_cast<int32_t>(readingSettingsSession.focus)) {
            readingSettingsSession.focus =
                static_cast<ReadingSettingsFocus>(target);
            displayReadingSettings(false, "selection");
        }
        return;
    }
    if (!pendingReadingSettingsSelect) {
        return;
    }
    pendingReadingSettingsSelect = false;
    if (uiScreen != UiScreen::ReadingSettings) {
        return;
    }
    orderedSelectorSession.kind =
        readingSettingsSession.focus == ReadingSettingsFocus::TextSize
            ? OrderedSelectorKind::TextSize
            : OrderedSelectorKind::LineSpacing;
    orderedSelectorSession.value =
        orderedSelectorSession.kind == OrderedSelectorKind::TextSize
            ? static_cast<uint8_t>(readingSettingsSession.settings.textSize)
            : static_cast<uint8_t>(
                  readingSettingsSession.settings.lineSpacing);
    orderedSelectorSession.originalValue = orderedSelectorSession.value;
    displayOrderedSelector("reading-settings");
}

void processOrderedSelectorActions() {
    if (pendingOrderedSelectorBack) {
        pendingOrderedSelectorBack = false;
        pendingOrderedSelectorSelect = false;
        pendingOrderedSelectorDelta = 0;
        if (uiScreen == UiScreen::OrderedSelector) {
            displayReadingSettings(false, "selector-cancel");
        }
        return;
    }
    if (pendingOrderedSelectorDelta != 0 &&
        millis() - pendingOrderedSelectorNavigationAt <
            kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingOrderedSelectorDelta;
    pendingOrderedSelectorDelta = 0;
    if (delta != 0) {
        if (uiScreen != UiScreen::OrderedSelector) {
            return;
        }
        const int32_t target = max(
            0, min(2, static_cast<int32_t>(orderedSelectorSession.value) +
                          delta));
        if (target != orderedSelectorSession.value) {
            orderedSelectorSession.value = static_cast<uint8_t>(target);
            displayOrderedSelector("selection");
        }
        return;
    }
    if (!pendingOrderedSelectorSelect) {
        return;
    }
    pendingOrderedSelectorSelect = false;
    if (uiScreen == UiScreen::OrderedSelector) {
        applyOrderedReaderSetting();
    }
}

bool openCurrentReaderBookCard(bool favoritePicker) {
    Fb2CacheInfo cache{};
    if (!readerSession.active || !Fb2Cache::load(readerSession.bookId, cache)) {
        Serial.println("ERROR READER_BOOK_CARD reason=metadata-unavailable");
        return false;
    }
    LocalBookEntry entry{};
    snprintf(entry.id, sizeof(entry.id), "%s", readerSession.bookId);
    snprintf(entry.title, sizeof(entry.title), "%s", cache.title);
    snprintf(entry.author, sizeof(entry.author), "%s", cache.author);
    snprintf(entry.annotation, sizeof(entry.annotation), "%s",
             cache.annotation);
    snprintf(entry.series, sizeof(entry.series), "%s", cache.series);
    snprintf(entry.seriesNumber, sizeof(entry.seriesNumber), "%s",
             cache.seriesNumber);
    entry.sourceBytes = cache.sourceBytes;
    entry.currentPage = readerSession.currentPage;
    entry.pageCount = readerSession.pageCount;
    entry.metadataReady = true;
    entry.detailsReady = cache.detailsReady;
    entry.paginationReady = true;
    entry.hasProgress = true;

    bookCardSession = BookCardSession{};
    bookCardSession.active = true;
    bookCardSession.local = true;
    bookCardSession.localCopyPresent = true;
    bookCardSession.returnScreen = UiScreen::ReaderMenu;
    bookCardSession.returnTab = readerReturnTab;
    bookCardSession.localEntry = entry;
    snprintf(bookCardSession.bookId, sizeof(bookCardSession.bookId), "%s",
             entry.id);
    loadBookCardFavorite();
    if (favoritePicker) {
        bookCardSession.focus = BookCardFocus::Favorite;
        bookCardSession.favoritePickerOpen = true;
    }
    return displayBookCard(favoritePicker ? "reader-favorite"
                                          : "reader-book-info");
}

void processReaderMenuActions() {
    if (pendingReaderMenuOpen) {
        pendingReaderMenuOpen = false;
        pendingReaderMenuDelta = 0;
        pendingReaderMenuSelect = false;
        pendingReaderMenuBack = false;
        if (readerSession.active) {
            scheduleGhostCleanup("reader-menu-open");
            displayReaderMenu("reader-center");
        }
        return;
    }
    if (pendingReaderMenuBack) {
        pendingReaderMenuBack = false;
        pendingReaderMenuSelect = false;
        pendingReaderMenuDelta = 0;
        if (uiScreen == UiScreen::ReaderMenu && readerSession.active) {
            displayReaderPage(readerSession.currentPage, "menu-back");
        }
        return;
    }
    if (pendingReaderMenuDelta != 0 &&
        millis() - pendingReaderMenuNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingReaderMenuDelta;
    pendingReaderMenuDelta = 0;
    if (delta != 0) {
        if (uiScreen != UiScreen::ReaderMenu) {
            return;
        }
        const int32_t target = max(
            0, min(6, static_cast<int32_t>(readerMenuSession.focus) + delta));
        if (target != static_cast<int32_t>(readerMenuSession.focus)) {
            readerMenuSession.focus = static_cast<ReaderMenuFocus>(target);
            displayReaderMenu("selection");
        }
        return;
    }
    if (!pendingReaderMenuSelect) {
        return;
    }
    pendingReaderMenuSelect = false;
    if (uiScreen != UiScreen::ReaderMenu || !readerSession.active) {
        return;
    }
    switch (readerMenuSession.focus) {
        case ReaderMenuFocus::Contents:
            displayContents(true, "reader-menu");
            return;
        case ReaderMenuFocus::BookInfo:
            openCurrentReaderBookCard(false);
            return;
        case ReaderMenuFocus::Favorite:
            openCurrentReaderBookCard(true);
            return;
        case ReaderMenuFocus::Close:
            readerSession.active = false;
            scheduleGhostCleanup("reader-close");
            displayTopLevelTab(readerReturnTab, true, "reader-close");
            return;
        case ReaderMenuFocus::Bookmarks:
            displayReaderBookmarks(true, "reader-menu");
            return;
        case ReaderMenuFocus::ReadingSettings:
            displayReadingSettings(true, "reader-menu");
            return;
        case ReaderMenuFocus::MarkRead:
            {
                ReaderUserState state{};
                ReaderPagination::loadUserState(readerSession.bookId,
                                                readerSession.pageCount,
                                                state);
                if (!ReaderPagination::setFinished(
                        readerSession.bookId, readerSession.pageCount,
                        !state.finished, state)) {
                    Serial.printf("ERROR READER MARK_READ reason=%s\n",
                                  state.error);
                    return;
                }
                homeSession.loaded = false;
                favoritesSession.loaded = false;
                displayReaderMenu("reading-state-saved");
            }
            return;
    }
}

void processContentsActions() {
    constexpr uint32_t kRowsPerScreen = 8;
    if (pendingContentsBack) {
        pendingContentsBack = false;
        pendingContentsSelect = false;
        pendingContentsDelta = 0;
        if (uiScreen == UiScreen::Contents) {
            displayReaderMenu("contents-back");
        }
        return;
    }
    if (pendingContentsDelta != 0 &&
        millis() - pendingContentsNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingContentsDelta;
    pendingContentsDelta = 0;
    if (delta != 0) {
        if (uiScreen != UiScreen::Contents ||
            readerSession.chapterCount == 0) {
            return;
        }
        const int32_t target = max(
            0, min(static_cast<int32_t>(readerSession.chapterCount) - 1,
                   static_cast<int32_t>(contentsSession.selected) + delta));
        if (target != static_cast<int32_t>(contentsSession.selected)) {
            contentsSession.selected = static_cast<uint32_t>(target);
            contentsSession.firstVisible =
                listPageStart(contentsSession.selected, kRowsPerScreen);
            displayContents(false, "selection");
        }
        return;
    }
    if (!pendingContentsSelect) {
        return;
    }
    pendingContentsSelect = false;
    if (uiScreen != UiScreen::Contents) {
        return;
    }
    ReaderChapterEntry chapter{};
    if (ReaderPagination::chapterEntry(readerSession.bookId,
                                       contentsSession.selected + 1,
                                       chapter)) {
        displayReaderPage(chapter.page, "contents-jump");
    }
}

void processReaderNavigation() {
    if (pendingUsbReaderOpenId[0] != '\0') {
        char bookId[sizeof(pendingUsbReaderOpenId)]{};
        snprintf(bookId, sizeof(bookId), "%s", pendingUsbReaderOpenId);
        pendingUsbReaderOpenId[0] = '\0';
        openReaderBook(bookId, "usb");
        return;
    }
    if (pendingUsbReaderPage) {
        const uint32_t page = pendingUsbReaderPageNumber;
        pendingUsbReaderPage = false;
        pendingUsbReaderPageNumber = 0;
        displayReaderPage(page, "usb-direct");
        return;
    }
    if (pendingPageDelta != 0 &&
        millis() - pendingPageNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingPageDelta;
    pendingPageDelta = 0;
    if (delta == 0) {
        return;
    }
    if (!readerSession.active) {
        Serial.println("ERROR READER_NAVIGATION reason=no-active-book");
        return;
    }
    int64_t requested =
        static_cast<int64_t>(readerSession.currentPage) + delta;
    if (requested < 1) {
        requested = 1;
    } else if (requested > static_cast<int64_t>(readerSession.pageCount)) {
        requested = readerSession.pageCount;
    }
    const uint32_t target = static_cast<uint32_t>(requested);
    if (target == readerSession.currentPage) {
        Serial.printf("READER PAGE BOUNDARY page=%lu/%lu\n",
                      static_cast<unsigned long>(readerSession.currentPage),
                      static_cast<unsigned long>(readerSession.pageCount));
        return;
    }
    displayReaderPage(target, "input");
}

void processReaderChapterNavigation() {
    if (pendingChapterDelta != 0 &&
        millis() - pendingChapterNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t direction = pendingChapterDelta;
    pendingChapterDelta = 0;
    if (direction == 0) {
        return;
    }
    if (!readerSession.active) {
        Serial.println("ERROR READER_CHAPTER reason=no-active-book");
        return;
    }

    ReaderChapterEntry chapter{};
    if (!ReaderPagination::adjacentChapter(readerSession.bookId,
                                           readerSession.currentPage,
                                           direction, chapter)) {
        Serial.printf("READER CHAPTER BOUNDARY page=%lu/%lu direction=%s\n",
                      static_cast<unsigned long>(readerSession.currentPage),
                      static_cast<unsigned long>(readerSession.pageCount),
                      direction > 0 ? "next" : "previous");
        return;
    }
    Serial.printf(
        "READER CHAPTER TARGET chapter=%lu/%lu page=%lu title=%s\n",
        static_cast<unsigned long>(chapter.ordinal),
        static_cast<unsigned long>(readerSession.chapterCount),
        static_cast<unsigned long>(chapter.page), chapter.title);
    displayReaderPage(chapter.page,
                      direction > 0 ? "chapter-next" : "chapter-previous");
}

bool openReaderBook(const char *bookId, const char *source) {
    operationNotice[0] = '\0';
    if (!BookUploadReceiver::validBookId(bookId)) {
        Serial.println("ERROR READER_OPEN reason=invalid-book-id");
        return false;
    }
    Serial.printf("OK READER OPEN id=%s cache=auto source=%s\n", bookId,
                  source);
    Serial.flush();

    ReaderProgress portableProgress{};
    ReaderUserState portableState{};
    const bool portableFound = ReaderPagination::loadPortableState(
        bookId, portableProgress, portableState);

    if (BookPreparation::busy() || AutomaticSync::busy()) return false;
    const UiScreen previousScreen = uiScreen;
    if (previousScreen != UiScreen::Reader &&
        previousScreen != UiScreen::Preparation) {
        readerReturnTab = activeTopLevelTab;
    }
    Fb2CacheInfo probeCache{};
    const bool cacheReady = Fb2Cache::load(bookId, probeCache);
    ReaderPaginationInfo probePagination{};
    const bool paginationReady =
        cacheReady && ReaderPagination::load(bookId, probePagination);
    if (!cacheReady || !paginationReady) {
        AutomaticSync::setPaused(true);
        if (!BookPreparation::start(bookId)) {
            AutomaticSync::setPaused(false);
            bookCardSession.preparing = false;
            return false;
        }
        snprintf(preparingBookId, sizeof(preparingBookId), "%s", bookId);
        busyFrame = 0;
        busyNextFrameAt = 0;
        Serial.printf("BOOK PREPARATION queued=true id=%s\n", bookId);
        if (uiScreen == UiScreen::Home) displayHome(false, "prepare-start");
        return true;
    }

    BookPreparationResult &preparation = bookPreparationWorkspace;
    if (!prepareBookArtifacts(bookId, source, preparation,
                              cacheReady ? &probeCache : nullptr,
                              paginationReady ? &probePagination : nullptr)) {
        Serial.printf("ERROR READER_OPEN reason=%s id=%s\n",
                      preparation.error, bookId);
        if (previousScreen == UiScreen::Home ||
            previousScreen == UiScreen::LocalLibrary ||
            previousScreen == UiScreen::TopLevel) {
            displayTopLevelTab(readerReturnTab, true, "prepare-failed");
        } else if (previousScreen == UiScreen::BookCard) {
            bookCardSession.preparing = false;
            displayBookCard("prepare-failed");
        }
        return false;
    }
    bookCardSession.preparing = false;
    const Fb2CacheInfo &cacheInfo = preparation.cache;
    const ReaderPaginationInfo &paginationInfo = preparation.pagination;
    Serial.printf(
        "READER CACHE id=%s ok=true reused=%s duration_ms=%lu "
        "source_bytes=%lu records=%lu title=%s author=%s\n",
        bookId, preparation.cacheReused ? "true" : "false",
        static_cast<unsigned long>(preparation.cacheDurationMs),
        static_cast<unsigned long>(cacheInfo.sourceBytes),
        static_cast<unsigned long>(cacheInfo.records), cacheInfo.title,
        cacheInfo.author);
    Serial.printf(
        "READER PAGINATION id=%s ok=true reused=%s duration_ms=%lu "
        "pages=%lu chapters=%lu layout_bytes=%lu source_records=%lu "
        "layout=%s\n",
        bookId, preparation.paginationReused ? "true" : "false",
        static_cast<unsigned long>(preparation.paginationDurationMs),
        static_cast<unsigned long>(paginationInfo.pageCount),
        static_cast<unsigned long>(paginationInfo.chapterCount),
        static_cast<unsigned long>(paginationInfo.pagesBytes),
        static_cast<unsigned long>(paginationInfo.sourceRecords),
        ReaderPagination::layoutId());

    ReaderProgress savedProgress{};
    const bool resumed = ReaderPagination::loadProgress(
        bookId, paginationInfo.pageCount, savedProgress);
    uint32_t resumePage = resumed ? savedProgress.currentPage : 1;
    const bool migratePortable = !resumed && portableFound;
    if (migratePortable) {
        ReaderPagination::pageForLogical(bookId, portableProgress.logical,
                                         resumePage);
    }
    const ReaderSession previousSession = readerSession;
    readerSession = ReaderSession{};
    readerSession.active = true;
    snprintf(readerSession.bookId, sizeof(readerSession.bookId), "%s", bookId);
    readerSession.pageCount = paginationInfo.pageCount;
    readerSession.chapterCount = paginationInfo.chapterCount;
    readerSession.currentPage = resumePage;
    scheduleGhostCleanup("reader-open");
    if (!displayReaderPage(readerSession.currentPage,
                           resumed ? "resume"
                                   : (migratePortable ? "resume-migrated"
                                                      : "open"))) {
        readerSession = previousSession;
        Serial.printf("ERROR READER_OPEN reason=page-display-failed id=%s\n",
                      bookId);
        return false;
    }
    if (migratePortable &&
        !restorePortableReaderUserState(portableState,
                                        readerSession.pageCount)) {
        Serial.println("WARNING READER_STATE_MIGRATION bookmarks=false");
    }
    uiScreen = UiScreen::Reader;
    RecentBookList recent{};
    const bool recentSaved = RecentBooks::touch(bookId, recent);
    homeSession.loaded = false;
    Serial.printf(
        "READER OPEN COMPLETE id=%s ok=true page=%lu/%lu resumed=%s "
        "title=%s author=%s source=%s recent_saved=%s\n",
        bookId, static_cast<unsigned long>(readerSession.currentPage),
        static_cast<unsigned long>(readerSession.pageCount),
        (resumed || migratePortable) ? "true" : "false", cacheInfo.title,
        cacheInfo.author,
        source, recentSaved ? "true" : "false");
    if (!recentSaved) {
        Serial.printf("WARNING RECENT_SAVE id=%s reason=%s\n", bookId,
                      recent.error);
    }
    return true;
}

bool ensureDownloadQueueLoaded() {
    if (downloadQueueLoaded) {
        return true;
    }
    if (!DownloadQueue::load(downloadQueue)) {
        Serial.printf("ERROR DOWNLOAD_QUEUE reason=%s\n",
                      downloadQueue.error[0] == '\0' ? "load-failed"
                                                       : downloadQueue.error);
        return false;
    }
    downloadQueueLoaded = true;
    Serial.printf("DOWNLOAD QUEUE loaded=true jobs=%lu\n",
                  static_cast<unsigned long>(downloadQueue.count));
    return true;
}

bool acquisitionBookId(const char *url, char *bookId, size_t capacity) {
    if (url == nullptr || bookId == nullptr || capacity < 8) {
        return false;
    }
    const char *segment = strstr(url, "/opds/");
    if (segment == nullptr) {
        return false;
    }
    segment += strlen("/opds/");
    const char *cursor = segment;
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
    }
    const size_t digits = static_cast<size_t>(cursor - segment);
    if (digits == 0 || digits > 20 || strncmp(cursor, "/download", 9) != 0 ||
        (cursor[9] != '\0' && cursor[9] != '?')) {
        return false;
    }
    const int written = snprintf(bookId, capacity, "opds-%.*s",
                                 static_cast<int>(digits), segment);
    return written > 0 && static_cast<size_t>(written) < capacity &&
           BookUploadReceiver::validBookId(bookId);
}

bool localBookPresent(const char *bookId) {
    char path[96]{};
    if (!BookUploadReceiver::bookPath(bookId, path, sizeof(path))) {
        return false;
    }
    File input = SD.open(path, FILE_READ);
    if (!input) {
        return false;
    }
    const size_t bytes = input.size();
    input.close();
    return bytes >= BookUploadReceiver::kMinimumBookBytes &&
           bytes <= BookUploadReceiver::kMaximumBookBytes;
}

bool downloadJobToStorage(const BookDownloadJob &job, const char *source,
                          bool showOperationScreen) {
    if (localBookPresent(job.bookId)) {
        Serial.printf("DOWNLOAD LOCAL HIT id=%s action=skip\n",
                      job.bookId);
        if (ensureDownloadQueueLoaded()) {
            DownloadQueue::remove(job.bookId, downloadQueue);
        }
        return true;
    }
    if (!ensureDownloadQueueLoaded()) {
        if (showOperationScreen) {
            displayBookDownload(job, true, "queue-load-failed", source);
        }
        return false;
    }
    if (!DownloadQueue::enqueue(job, downloadQueue)) {
        if (showOperationScreen) {
            displayBookDownload(job, true, downloadQueue.error, source);
        }
        return false;
    }

    if (showOperationScreen) {
        displayBookDownload(job, false, "", source);
    }
    Serial.printf("DOWNLOAD START id=%s attempt=%u credentials=stored\n",
                  job.bookId,
                  static_cast<unsigned>(job.attempts + 1));
    Serial.flush();
    BookDownloadResult result{};
    bool downloaded = false;
    {
        BusyIndicator busy(true);
        downloaded = networkService.downloadFb2(job.acquisitionUrl, job.bookId,
                                                 result);
    }
    if (!downloaded) {
        const char *reason = result.error[0] == '\0' ? "download-failed"
                                                      : result.error;
        DownloadQueue::markFailure(job.bookId, reason, downloadQueue);
        Serial.printf(
            "DOWNLOAD FAILED id=%s http=%d bytes=%lu duration_ms=%lu "
            "radio=off reason=%s queued=true\n",
            job.bookId, result.httpCode,
            static_cast<unsigned long>(result.responseBytes),
            static_cast<unsigned long>(result.durationMs), reason);
        if (showOperationScreen) {
            displayBookDownload(job, true, reason, "network-failed");
        }
        return false;
    }

    Serial.printf(
        "DOWNLOAD COMPLETE id=%s bytes=%lu sha256=%s duration_ms=%lu "
        "tls=true radio=off atomic=true\n",
        job.bookId, static_cast<unsigned long>(result.responseBytes),
        result.sha256, static_cast<unsigned long>(result.durationMs));
    if (job.coverUrl[0] != '\0' && !CoverCache::sourcePresent(job.bookId)) {
        BookDownloadResult coverDownload{};
        if (networkService.downloadCover(job.coverUrl, job.bookId,
                                         coverDownload)) {
            CoverCacheInfo coverCache{};
            const bool coverPrepared =
                CoverCache::build(job.bookId, coverCache);
            Serial.printf(
                "COVER DOWNLOAD COMPLETE id=%s bytes=%lu duration_ms=%lu "
                "prepared=%s size=%ux%u reason=%s\n",
                job.bookId,
                static_cast<unsigned long>(coverDownload.responseBytes),
                static_cast<unsigned long>(coverDownload.durationMs),
                coverPrepared ? "true" : "false", coverCache.width,
                coverCache.height,
                coverPrepared ? "none" : coverCache.error);
        } else {
            Serial.printf(
                "WARNING COVER_DOWNLOAD id=%s http=%d duration_ms=%lu "
                "reason=%s\n",
                job.bookId, coverDownload.httpCode,
                static_cast<unsigned long>(coverDownload.durationMs),
                coverDownload.error);
        }
    }
    if (!DownloadQueue::remove(job.bookId, downloadQueue)) {
        Serial.printf("WARNING DOWNLOAD_QUEUE id=%s reason=remove-failed\n",
                      job.bookId);
    }
    localLibrarySession.loaded = false;
    homeSession.loaded = false;
    preparationRetryBookId[0] = '\0';
    preparationRetryAt = 0;
    return true;
}

bool executeDownloadJob(const BookDownloadJob &job, const char *source) {
    readerReturnTab = catalogSession.owner;
    const bool localHit = localBookPresent(job.bookId);
    if (!downloadJobToStorage(job, source, !localHit)) {
        return false;
    }
    return displayLocalLibrary(true, "download-complete");
}

bool bookDownloadJobFromEntry(const OpdsEntry &entry, BookDownloadJob &job) {
    job = BookDownloadJob{};
    if (entry.kind != OpdsEntryKind::Book ||
        entry.acquisitionHref[0] == '\0' ||
        !acquisitionBookId(entry.acquisitionHref, job.bookId,
                           sizeof(job.bookId))) {
        return false;
    }
    snprintf(job.title, sizeof(job.title), "%s",
             entry.title[0] == '\0' ? job.bookId : entry.title);
    snprintf(job.author, sizeof(job.author), "%s", entry.author);
    snprintf(job.acquisitionUrl, sizeof(job.acquisitionUrl), "%s",
             entry.acquisitionHref);
    snprintf(job.coverUrl, sizeof(job.coverUrl), "%s",
             entry.coverHref[0] != '\0' ? entry.coverHref
                                         : entry.thumbnailHref);
    return true;
}

bool startCatalogBookDownload(const OpdsEntry &entry, const char *source) {
    activeDownloadJob = BookDownloadJob{};
    if (!bookDownloadJobFromEntry(entry, activeDownloadJob)) {
        Serial.println("ERROR DOWNLOAD_SELECT reason=invalid-acquisition-link");
        return false;
    }
    Serial.printf("CATALOG BOOK SELECTED id=%s action=download\n",
                  activeDownloadJob.bookId);
    bookCardSession.downloading = true;
    bookCardSession.downloadFailed = false;
    displayBookCard("download-start");
    const bool downloaded =
        downloadJobToStorage(activeDownloadJob, source, false);
    bookCardSession.downloading = false;
    bookCardSession.downloadFailed = !downloaded;
    if (downloaded) {
        bookCardSession.localCopyPresent = true;
    }
    displayBookCard(downloaded ? "download-complete" : "download-failed");
    return downloaded;
}

void loadBookCardFavorite() {
    bookCardSession.favoriteStored=Collections::contains(0,bookCardSession.bookId);
    bookCardSession.collectionSelection=0;
}

bool saveBookCardFavorite() {
    const bool saved=Collections::toggle(0,bookCardSession.bookId);
    loadBookCardFavorite(); favoritesSession.loaded=false;
    Serial.printf("COLLECTION FAVORITE saved=%s selected=%s\n",saved?"true":"false",bookCardSession.favoriteStored?"true":"false");
    return saved;
}

bool openLocalBookCard(const LocalBookEntry &entry, UiScreen returnScreen,
                       const char *source) {
    bookCardSession = BookCardSession{};
    bookCardSession.active = true;
    bookCardSession.local = true;
    bookCardSession.localCopyPresent = true;
    bookCardSession.returnScreen = returnScreen;
    bookCardSession.returnTab = activeTopLevelTab;
    bookCardSession.localEntry = entry;
    snprintf(bookCardSession.bookId, sizeof(bookCardSession.bookId), "%s",
             entry.id);
    loadBookCardFavorite();
    Serial.printf("BOOK CARD SELECT source=%s id=%s origin=%s\n", source,
                  entry.id,
                  returnScreen == UiScreen::Home ? "home" : "library");
    return displayBookCard(source);
}

bool openRemoteBookCard(const OpdsEntry &entry, const char *source) {
    bookCardSession = BookCardSession{};
    bookCardSession.active = true;
    bookCardSession.local = false;
    bookCardSession.returnScreen = UiScreen::Catalog;
    bookCardSession.returnTab = catalogSession.owner;
    bookCardSession.remoteEntry = entry;
    if (entry.acquisitionHref[0] != '\0') {
        acquisitionBookId(entry.acquisitionHref, bookCardSession.bookId,
                          sizeof(bookCardSession.bookId));
    }
    bookCardSession.localCopyPresent =
        bookCardSession.bookId[0] != '\0' &&
        localBookPresent(bookCardSession.bookId);
    loadBookCardFavorite();
    Serial.printf(
        "BOOK CARD SELECT source=%s id=%s origin=opds local=%s\n", source,
        bookCardSession.bookId[0] == '\0' ? "remote" : bookCardSession.bookId,
        bookCardSession.localCopyPresent ? "true" : "false");
    return displayBookCard(source);
}

bool restoreBookCardOrigin(const char *reason) {
    const UiScreen returnScreen = bookCardSession.returnScreen;
    const TopLevelTab returnTab = bookCardSession.returnTab;
    bookCardSession.active = false;
    if (returnScreen == UiScreen::Catalog) {
        catalogSession.owner = returnTab;
        return refreshCatalogFrame(reason);
    }
    if (returnScreen == UiScreen::LocalLibrary) {
        return displayLocalLibrary(false, reason);
    }
    if (returnScreen == UiScreen::Favorites) {
        return displayFavorites(true, reason);
    }
    if (returnScreen == UiScreen::ReaderMenu && readerSession.active) {
        return displayReaderMenu(reason);
    }
    return displayHome(false, reason);
}

bool openFavoriteBookCard(const FavoriteEntry &favorite) {
    if (localBookPresent(favorite.bookId)) {
        auto *entries = static_cast<LocalBookEntry *>(heap_caps_calloc(
            kLocalLibraryCapacity, sizeof(LocalBookEntry),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (entries == nullptr) {
            Serial.println("ERROR FAVORITES_SELECT reason=allocation-failed");
            return false;
        }
        LocalLibraryInfo info{};
        const bool scanned =
            LocalLibrary::scan(entries, kLocalLibraryCapacity, info);
        if (scanned) {
            for (size_t index = 0; index < info.loadedCount; ++index) {
                if (strcmp(entries[index].id, favorite.bookId) == 0) {
                    const LocalBookEntry selected = entries[index];
                    heap_caps_free(entries);
                    return openLocalBookCard(selected, UiScreen::Favorites,
                                             "favorites");
                }
            }
        }
        heap_caps_free(entries);
    }
    if (favorite.acquisitionHref[0] == '\0') {
        Serial.println("ERROR FAVORITES_SELECT reason=book-unavailable");
        return false;
    }
    bookCardSession = BookCardSession{};
    bookCardSession.active = true;
    bookCardSession.local = false;
    bookCardSession.returnScreen = UiScreen::Favorites;
    bookCardSession.returnTab = TopLevelTab::Favorites;
    snprintf(bookCardSession.bookId, sizeof(bookCardSession.bookId), "%s",
             favorite.bookId);
    snprintf(bookCardSession.remoteEntry.title,
             sizeof(bookCardSession.remoteEntry.title), "%s",
             favorite.title);
    snprintf(bookCardSession.remoteEntry.author,
             sizeof(bookCardSession.remoteEntry.author), "%s",
             favorite.author);
    snprintf(bookCardSession.remoteEntry.acquisitionHref,
             sizeof(bookCardSession.remoteEntry.acquisitionHref), "%s",
             favorite.acquisitionHref);
    snprintf(bookCardSession.remoteEntry.coverHref,
             sizeof(bookCardSession.remoteEntry.coverHref), "%s",
             favorite.coverHref);
    bookCardSession.remoteEntry.kind = OpdsEntryKind::Book;
    bookCardSession.localCopyPresent = false;
    loadBookCardFavorite();
    return displayBookCard("favorites");
}

void processFavoritesActions() {
    if (pendingFavoritesBack) {
        pendingFavoritesBack = false;
        pendingFavoritesSelect = false;
        pendingFavoritesDelta = 0;
        if (uiScreen != UiScreen::Favorites) {
            Serial.println("ERROR FAVORITES_BACK reason=not-favorites");
            return;
        }
        if (favoritesSession.phase == FavoritesPhase::Books) {
            favoritesSession.phase = FavoritesPhase::Folders;
            favoritesSession.selected =
                static_cast<size_t>(favoritesSession.folder);
            favoritesSession.firstVisible = 0;
            displayFavorites(false, "folder-back");
        } else {
            pendingTopLevelTarget = TopLevelTab::Home;
            pendingTopLevelOpen = true;
        }
        return;
    }

    if (pendingFavoritesDelta != 0 &&
        millis() - pendingFavoritesNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingFavoritesDelta;
    pendingFavoritesDelta = 0;
    if (delta != 0) {
        if (uiScreen != UiScreen::Favorites) {
            Serial.println("ERROR FAVORITES_NAVIGATION reason=not-favorites");
            return;
        }
        const size_t count =
            favoritesSession.phase == FavoritesPhase::Folders
                ? Collections::count()+1
                : favoriteFolderCount(favoritesSession.folder);
        if (count == 0) {
            Serial.println("FAVORITES BOUNDARY reason=empty");
            return;
        }
        int32_t target = static_cast<int32_t>(favoritesSession.selected) +
                         delta;
        target = max(0, min(target, static_cast<int32_t>(count - 1)));
        if (target == static_cast<int32_t>(favoritesSession.selected)) {
            Serial.printf("FAVORITES BOUNDARY selected=%lu/%lu\n",
                          static_cast<unsigned long>(
                              favoritesSession.selected + 1),
                          static_cast<unsigned long>(count));
            return;
        }
        favoritesSession.selected = static_cast<size_t>(target);
        displayFavorites(false, "selection");
        return;
    }

    if (!pendingFavoritesSelect) {
        return;
    }
    pendingFavoritesSelect = false;
    if (uiScreen != UiScreen::Favorites) {
        Serial.println("ERROR FAVORITES_SELECT reason=not-favorites");
        return;
    }
    if (favoritesSession.phase == FavoritesPhase::Folders) {
        if(favoritesSession.selected==Collections::count()){openCollectionName(false);return;}
        favoritesSession.folder = favoritesSession.selected;
        favoritesSession.phase = FavoritesPhase::Books;
        favoritesSession.selected = 0;
        favoritesSession.firstVisible = 0;
        displayFavorites(false, "folder-open");
        return;
    }
    const FavoriteEntry *entry = favoriteFolderEntry(
        favoritesSession.folder, favoritesSession.selected);
    if (entry == nullptr) {
        Serial.println("ERROR FAVORITES_SELECT reason=no-book");
        return;
    }
    const FavoriteEntry copy = *entry;
    openFavoriteBookCard(copy);
}

bool bookCardRelationUrl(bool series, char *target, size_t capacity) {
    if (target == nullptr || capacity == 0) {
        return false;
    }
    target[0] = '\0';
    const char *explicitHref = series ? bookCardSession.remoteEntry.seriesHref
                                      : bookCardSession.remoteEntry.authorHref;
    if (!bookCardSession.local && explicitHref[0] != '\0') {
        snprintf(target, capacity, "%s", explicitHref);
        return true;
    }
    const char *value = series ? bookCardSeries() : bookCardAuthor();
    char encoded[512]{};
    if (value[0] == '\0' ||
        !encodeUrlQuery(value, encoded, sizeof(encoded))) {
        return false;
    }
    char root[kOpdsUrlCapacity]{};
    char error[64]{};
    if (!networkService.copyOpdsRootUrl(root, sizeof(root), error,
                                        sizeof(error))) {
        Serial.printf("ERROR BOOK_CARD_RELATION reason=%s\n", error);
        return false;
    }
    const size_t rootLength = strlen(root);
    if (rootLength > 0 && root[rootLength - 1] == '/') {
        root[rootLength - 1] = '\0';
    }
    const int written = snprintf(
        target, capacity, "%s/catalog?%s=%s&page=1&size=20", root,
        series ? "series" : "author", encoded);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

void processBookCardActions() {
    if (pendingBookCardBack) {
        pendingBookCardBack = false;
        pendingBookCardSelect = false;
        pendingBookCardDelta = 0;
        if (uiScreen != UiScreen::BookCard) {
            Serial.println("ERROR BOOK_CARD_BACK reason=not-card");
            return;
        }
        if (bookCardSession.favoritePickerOpen) {
            bookCardSession.favoritePickerOpen = false;
            displayBookCard("favorite-picker-close");
            return;
        }
        restoreBookCardOrigin("book-card-back");
        return;
    }

    if (pendingBookCardDelta != 0 &&
        millis() - pendingBookCardNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingBookCardDelta;
    pendingBookCardDelta = 0;
    if (delta != 0) {
        if (uiScreen != UiScreen::BookCard || !bookCardSession.active) {
            Serial.println("ERROR BOOK_CARD_NAVIGATION reason=not-card");
            return;
        }
        if (bookCardSession.favoritePickerOpen) {
            const int target=max(0,min(static_cast<int>(Collections::count()+1),static_cast<int>(bookCardSession.collectionSelection)+delta));
            bookCardSession.collectionSelection=target; displayBookCard("collection-selection");return;
        }
        int32_t target = static_cast<int32_t>(bookCardSession.focus);
        const int32_t direction = delta < 0 ? -1 : 1;
        const int32_t steps = abs(static_cast<int32_t>(delta));
        for (int32_t step = 0; step < steps; ++step) {
            do {
                target = max(0, min(6, target + direction));
            } while (!bookCardActionEnabled(
                         static_cast<BookCardFocus>(target)) &&
                     target > 0 && target < 6);
        }
        if (target == static_cast<int32_t>(bookCardSession.focus)) {
            Serial.printf("BOOK CARD BOUNDARY focus=%u\n",
                          static_cast<unsigned>(bookCardSession.focus));
            return;
        }
        bookCardSession.focus = static_cast<BookCardFocus>(target);
        displayBookCard("selection");
        return;
    }

    if (!pendingBookCardSelect) {
        return;
    }
    pendingBookCardSelect = false;
    if (uiScreen != UiScreen::BookCard || !bookCardSession.active) {
        Serial.println("ERROR BOOK_CARD_SELECT reason=not-card");
        return;
    }
    if (bookCardSession.favoritePickerOpen) {
        const size_t chosen=bookCardSession.collectionSelection;
        if(chosen==0){bookCardSession.favoritePickerOpen=false;scheduleGhostCleanup("collection-close");}
        else if(chosen==Collections::count()+1){openCollectionName(true);return;}
        else if(!Collections::toggle(chosen-1,bookCardSession.bookId)) Serial.printf("COLLECTION SAVE error=%s\n",Collections::error());
        favoritesSession.loaded=false;displayBookCard("collection-toggle");return;
    }
    switch (bookCardSession.focus) {
        case BookCardFocus::Primary:
            if (bookCardSession.local || bookCardSession.localCopyPresent) {
                Fb2CacheInfo cache{};
                ReaderPaginationInfo pagination{};
                const bool cacheReady =
                    Fb2Cache::load(bookCardSession.bookId, cache);
                const bool paginationReady =
                    cacheReady && ReaderPagination::load(
                                      bookCardSession.bookId, pagination);
                if (!cacheReady || !paginationReady) {
                    bookCardSession.preparing = true;
                    displayBookCard("prepare-start");
                }
                openReaderBook(bookCardSession.bookId, "book-card");
            } else {
                // Legacy remote favorites remain readable as cards; only the
                // website queues new downloads in the offline-first firmware.
                Serial.println("BOOK DOWNLOAD use=companion");
            }
            return;
        case BookCardFocus::FullAnnotation:
            bookCardSession.annotationPage = 0;
            displayAnnotation("book-card");
            return;
        case BookCardFocus::Author:
        case BookCardFocus::Series: {
            const bool series =
                bookCardSession.focus == BookCardFocus::Series;
            localLibrarySession.owner = TopLevelTab::OnDevice;
            localLibrarySession.phase = LocalLibraryPhase::Books;
            localLibrarySession.section = series ? LocalLibrarySection::Series : LocalLibrarySection::Authors;
            snprintf(localLibrarySession.groupLabel, sizeof(localLibrarySession.groupLabel),
                     "%s", series ? bookCardSeries() : bookCardAuthor());
            localLibrarySession.selected = 0;
            localLibrarySession.firstVisible = 0;
            displayLocalLibrary(true, series ? "local-book-series" : "local-book-author");
            return;
        }
        case BookCardFocus::Back:
            restoreBookCardOrigin("card-back");return;
        case BookCardFocus::Favorite:
            saveBookCardFavorite();displayBookCard("favorite-toggle");return;
        case BookCardFocus::Collections:
            bookCardSession.collectionSelection=0;
            scheduleGhostCleanup("collection-picker");
            bookCardSession.favoritePickerOpen = true;
            displayBookCard("favorite-picker");
            return;
    }
}

void processAnnotationActions() {
    if (pendingAnnotationBack) {
        pendingAnnotationBack = false;
        pendingAnnotationDelta = 0;
        if (uiScreen != UiScreen::Annotation) {
            Serial.println("ERROR ANNOTATION_BACK reason=not-annotation");
            return;
        }
        displayBookCard("annotation-back");
        return;
    }
    if (pendingAnnotationDelta != 0 &&
        millis() - pendingAnnotationNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingAnnotationDelta;
    pendingAnnotationDelta = 0;
    if (delta == 0) {
        return;
    }
    if (uiScreen != UiScreen::Annotation || !bookCardSession.active) {
        Serial.println("ERROR ANNOTATION_NAVIGATION reason=not-annotation");
        return;
    }
    constexpr size_t kLinesPerPage = 17;
    const size_t totalLines = drawWrappedParagraph(
        nullptr, &BookishArimo22, bookCardAnnotation(), 42, 287, 456, 34, 0,
        0);
    const size_t totalPages = max(
        static_cast<size_t>(1),
        (totalLines + kLinesPerPage - 1) / kLinesPerPage);
    int32_t target = static_cast<int32_t>(bookCardSession.annotationPage) +
                     delta;
    target = max(0, min(target, static_cast<int32_t>(totalPages - 1)));
    if (target == bookCardSession.annotationPage) {
        Serial.printf("ANNOTATION BOUNDARY page=%u/%lu\n",
                      static_cast<unsigned>(bookCardSession.annotationPage + 1),
                      static_cast<unsigned long>(totalPages));
        return;
    }
    bookCardSession.annotationPage = static_cast<uint8_t>(target);
    scheduleGhostCleanup("annotation-page");
    displayAnnotation(delta > 0 ? "page-next" : "page-previous");
}

void processDownloadActions() {
    if (pendingDownloadBack) {
        pendingDownloadBack = false;
        pendingDownloadRetry = false;
        if (uiScreen != UiScreen::Download) {
            Serial.println("ERROR DOWNLOAD_BACK reason=not-download");
            return;
        }
        closeBulkDownloadSession();
        refreshCatalogFrame("download-back");
        return;
    }
    if (!pendingDownloadRetry) {
        return;
    }
    pendingDownloadRetry = false;
    if (bulkDownloadSession != nullptr && bulkDownloadSession->active) {
        continueBulkDownload();
        return;
    }
    if (uiScreen != UiScreen::Download || !downloadFailureVisible ||
        !ensureDownloadQueueLoaded() || downloadQueue.count == 0) {
        Serial.println("ERROR DOWNLOAD_RETRY reason=no-queued-job");
        return;
    }
    activeDownloadJob = downloadQueue.jobs[0];
    executeDownloadJob(activeDownloadJob, "retry");
}

bool renderCurrentTabFocus(int focus) {
    static UiScreen source = UiScreen::Home;
    if (uiScreen != UiScreen::Sections) source = uiScreen;
    switch (activeTopLevelTab) {
        case TopLevelTab::OnDevice:
            if (localLibrarySession.owner != TopLevelTab::OnDevice) {
                localLibrarySession.owner = TopLevelTab::OnDevice;
                localLibrarySession.phase = savedLibraryTab.phase;
                localLibrarySession.section = savedLibraryTab.section;
                snprintf(localLibrarySession.groupLabel, sizeof(localLibrarySession.groupLabel), "%s", savedLibraryTab.group);
                snprintf(localLibrarySession.searchPrefix, sizeof(localLibrarySession.searchPrefix), "%s", savedLibraryTab.prefix);
                rebuildLocalLibraryView();
                localLibrarySession.selected = savedLibraryTab.selected;
                localLibrarySession.firstVisible = savedLibraryTab.firstVisible;
            }
            if (!localLibrarySession.loaded) {
                if (!LocalLibrary::scan(localLibrarySession.entries, kLocalLibraryCapacity, localLibrarySession.info)) return false;
                localLibrarySession.loaded = true;
                rebuildLocalLibraryView();
            }
            return renderLocalLibraryFrame(focus);
        case TopLevelTab::Search:
            if (localLibrarySession.owner == TopLevelTab::Search &&
                (source == UiScreen::LocalLibrary || source == UiScreen::BookCard))
                return renderLocalLibraryFrame(focus);
            return renderSearchFrame(focus);
        case TopLevelTab::Favorites: return renderFavoritesFrame(focus);
        default:
            if (!homeSession.loaded && !loadHomeSession()) return false;
            return renderBookishHome(focus);
    }
}

void displaySections() {
    const bool entering = uiScreen != UiScreen::Sections;
    if (uiScreen != UiScreen::Sections) {
        for (size_t i = 0; i < kVisibleTabCount; ++i)
            if (kVisibleTabs[i] == activeTopLevelTab) sectionSelection = i;
    }
    sectionBackFocused = false;
    if (!renderCurrentTabFocus(static_cast<int>(sectionSelection))) return;
    scheduleScreenTransitionCleanup(UiScreen::Sections, "tabs-focus");
    uiScreen = UiScreen::Sections;
    lastDisplayRefresh = displayRefresh.refresh(framebuffer,
        entering ? DisplayRefreshMode::QualityFull : DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = lastDisplayRefresh.ok;
    Serial.printf("SECTIONS OPEN COMPLETE selected=%u\n", static_cast<unsigned>(sectionSelection + 1));
}

// A highlighted tab always owns navigation, never the Home content list.
bool handleTabNavigation(const char *button, const char *gesture) {
    const bool ok = strcmp(button, "CENTER") == 0;
    const bool held = strcmp(gesture, "LONG") == 0;
    const bool click = strcmp(gesture, "SHORT") == 0;
    if (uiScreen == UiScreen::Sections) {
        if (ok && !strcmp(gesture, "DOUBLE")) { displayTopLevelTab(activeTopLevelTab, false, "tabs-back"); return true; }
        if (ok && click) displayTopLevelTab(kVisibleTabs[sectionSelection], false, "sections-select");
        else if (click && (!strcmp(button, "UP") || !strcmp(button, "DOWN"))) {
            sectionSelection = (sectionSelection + (!strcmp(button, "UP") ? kVisibleTabCount - 1 : 1)) % kVisibleTabCount;
            noteUiNavigationClick();
            displaySections();
        }
        return true; // Holding OK at the highest level stays there.
    }
    if (!ok || !held) return false;
    displaySections();
    return true;
}

bool handleSectionBackFocus(const char *button, const char *gesture) {
    if (strcmp(gesture, "SHORT")) return false;
    size_t selected = 0, count = 0;
    if (uiScreen == UiScreen::LocalLibrary && localLibrarySession.phase != LocalLibraryPhase::Sections) {
        selected = localLibrarySession.selected; count = localLibraryRowCount();
    } else if (uiScreen == UiScreen::Search && searchSession.phase == SearchPhase::Letter) {
        selected = searchSession.selected; count = searchRowCount();
    } else if (uiScreen == UiScreen::Favorites && favoritesSession.phase == FavoritesPhase::Books) {
        selected = favoritesSession.selected; count = favoriteFolderCount(favoritesSession.folder);
    } else return false;
    const int direction = !strcmp(button, "UP") ? -1 : !strcmp(button, "DOWN") ? 1 : 0;
    const auto action = SectionBackFocus::input(sectionBackFocused, selected, count, direction, !strcmp(button, "CENTER"));
    if (action == SectionBackFocus::Action::None) return false;
    if (action == SectionBackFocus::Action::Back) {
        if (uiScreen == UiScreen::LocalLibrary) pendingLocalLibraryBack = true;
        if (uiScreen == UiScreen::Search) pendingSearchBack = true;
        if (uiScreen == UiScreen::Favorites) pendingFavoritesBack = true;
    } else {
        noteUiNavigationClick();
        if (uiScreen == UiScreen::LocalLibrary) displayLocalLibrary(false, "back-focus");
        if (uiScreen == UiScreen::Search) displaySearch(false, "back-focus");
        if (uiScreen == UiScreen::Favorites) displayFavorites(false, "back-focus");
    }
    return true;
}

void processTopLevelActions() {
    if (AutomaticSync::busy() || BookPreparation::busy()) return;
    if (pendingTopLevelOpen) {
        const TopLevelTab target = pendingTopLevelTarget;
        pendingTopLevelOpen = false;
        pendingPageDelta = 0;
        pendingChapterDelta = 0;
        pendingTopLevelDelta = 0;
        pendingHomeDelta = 0;
        pendingHomeBookOpen = false;
        pendingLibraryDelta = 0;
        pendingSelectedBookOpen = false;
        pendingLocalLibraryBack = false;
        pendingCatalogDelta = 0;
        pendingCatalogOpen = false;
        pendingCatalogBack = false;
        pendingSearchDelta = 0;
        pendingSearchSelect = false;
        pendingSearchBack = false;
        pendingBookCardDelta = 0;
        pendingBookCardSelect = false;
        pendingBookCardBack = false;
        pendingAnnotationDelta = 0;
        pendingAnnotationBack = false;
        pendingFavoritesDelta = 0;
        pendingFavoritesSelect = false;
        pendingFavoritesBack = false;
        pendingReaderMenuDelta = 0;
        pendingReaderMenuOpen = false;
        pendingReaderMenuSelect = false;
        pendingReaderMenuBack = false;
        pendingContentsDelta = 0;
        pendingContentsSelect = false;
        pendingContentsBack = false;
        pendingReaderBookmarksDelta = 0;
        pendingReaderBookmarksSelect = false;
        pendingReaderBookmarksBack = false;
        pendingReadingSettingsDelta = 0;
        pendingReadingSettingsSelect = false;
        pendingReadingSettingsBack = false;
        pendingOrderedSelectorDelta = 0;
        pendingOrderedSelectorSelect = false;
        pendingOrderedSelectorBack = false;
        pendingBulkDownloadDelta = 0;
        pendingBulkDownloadSelect = false;
        pendingBulkDownloadBack = false;
        closeBulkDownloadSession();
        bookCardSession.active = false;
        scheduleGhostCleanup("top-level-open");
        if (pendingTabsOpen) {
            pendingTabsOpen = false;
            activeTopLevelTab = target;
            if (target == TopLevelTab::Home && !homeSession.loaded) loadHomeSession();
            if (target == TopLevelTab::Favorites && !favoritesSession.loaded) loadFavoritesSession();
            displaySections();
        } else displayTopLevelTab(target, true, "input");
        return;
    }

    if (pendingTopLevelDelta != 0 &&
        millis() - pendingTopLevelNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t tabDelta = pendingTopLevelDelta;
    pendingTopLevelDelta = 0;
    if (tabDelta != 0) {
        if (uiScreen != UiScreen::Home &&
            uiScreen != UiScreen::LocalLibrary &&
            uiScreen != UiScreen::TopLevel &&
            uiScreen != UiScreen::Catalog &&
            uiScreen != UiScreen::Search &&
            uiScreen != UiScreen::Favorites) {
            Serial.println("ERROR TAB_NAVIGATION reason=not-top-level");
            return;
        }
        int32_t current = 0;
        for (size_t index = 0; index < kVisibleTabCount; ++index)
            if (kVisibleTabs[index] == activeTopLevelTab) current = index;
        int32_t target = current + tabDelta;
        while (target < 0) {
            target += kVisibleTabCount;
        }
        target %= kVisibleTabCount;
        scheduleGhostCleanup("top-level-switch");
        displayTopLevelTab(kVisibleTabs[target], true,
                           tabDelta > 0 ? "tab-next" : "tab-previous");
        return;
    }

    if (pendingHomeDelta != 0 &&
        millis() - pendingHomeNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t homeDelta = pendingHomeDelta;
    pendingHomeDelta = 0;
    if (homeDelta != 0) {
        if (uiScreen != UiScreen::Home) {
            Serial.println("ERROR HOME_NAVIGATION reason=not-home");
            return;
        }
        int32_t target = static_cast<int32_t>(homeSession.selected) + homeDelta;
        if (target < 0) { displaySections(); return; }
        if (!HomeLayout::actionCount(homeSession.count, homeSession.addedCount)) { displaySections(); return; }
        target = max(0, target);
        target = min(target, static_cast<int32_t>(HomeLayout::actionCount(homeSession.count, homeSession.addedCount) - 1));
        if (static_cast<size_t>(target) == homeSession.selected) {
            Serial.printf("HOME BOUNDARY selected=%lu/%lu\n",
                          static_cast<unsigned long>(homeSession.selected + 1),
                          static_cast<unsigned long>(HomeLayout::actionCount(homeSession.count, homeSession.addedCount)));
            return;
        }
        homeSession.selected = static_cast<size_t>(target);
        displayHome(false, "selection");
        return;
    }

    if (pendingHomeBookOpen) {
        pendingHomeBookOpen = false;
        const auto action = HomeLayout::action(homeSession.selected, homeSession.count, homeSession.addedCount);
        if (uiScreen != UiScreen::Home || action == HomeLayout::Action::Invalid) {
            Serial.println("ERROR HOME_OPEN_BOOK reason=no-selection");
            return;
        }
        const LocalBookEntry &selected = action == HomeLayout::Action::AddedCard
            ? homeSession.added[homeSession.selected - HomeLayout::kFirstBook - homeSession.count]
            : homeSession.entries[homeSession.selected - HomeLayout::kFirstBook];
        if (action == HomeLayout::Action::Continue) {
            openReaderBook(selected.id, "home-continue");
        } else {
            openLocalBookCard(selected, UiScreen::Home,
                action == HomeLayout::Action::AddedCard ? "home-added" : "home-recent");
        }
    }
}

void closeBulkDownloadSession() {
    if (bulkDownloadSession != nullptr) {
        heap_caps_free(bulkDownloadSession);
        bulkDownloadSession = nullptr;
    }
}

bool prepareBulkDownloadSession() {
    closeBulkDownloadSession();
    if (catalogSession.feed == nullptr ||
        catalogSession.relation == CatalogSession::Relation::None) {
        return false;
    }
    bulkDownloadSession = static_cast<BulkDownloadSession *>(heap_caps_calloc(
        1, sizeof(BulkDownloadSession),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (bulkDownloadSession == nullptr) {
        Serial.println("ERROR BULK_DOWNLOAD reason=allocation-failed");
        return false;
    }
    bulkDownloadSession->active = true;
    bulkDownloadSession->relation = catalogSession.relation;
    snprintf(bulkDownloadSession->label,
             sizeof(bulkDownloadSession->label), "%s",
             catalogSession.relationLabel[0] == '\0'
                 ? catalogSession.feed->title
                 : catalogSession.relationLabel);
    for (size_t index = 0;
         index < catalogSession.feed->entryCount &&
         bulkDownloadSession->count < kBulkDownloadLimit;
         ++index) {
        BookDownloadJob job{};
        if (!bookDownloadJobFromEntry(catalogSession.feed->entries[index],
                                      job)) {
            continue;
        }
        if (localBookPresent(job.bookId)) {
            ++bulkDownloadSession->skipped;
            continue;
        }
        bulkDownloadSession->jobs[bulkDownloadSession->count++] = job;
    }
    if (bulkDownloadSession->count == 0) {
        Serial.println("BULK DOWNLOAD ready=false reason=nothing-missing");
        closeBulkDownloadSession();
        return false;
    }
    Serial.printf(
        "BULK DOWNLOAD ready=true relation=%s books=%lu skipped=%lu "
        "limit=%lu\n",
        catalogSession.relation == CatalogSession::Relation::Series
            ? "series"
            : "author",
        static_cast<unsigned long>(bulkDownloadSession->count),
        static_cast<unsigned long>(bulkDownloadSession->skipped),
        static_cast<unsigned long>(kBulkDownloadLimit));
    return true;
}

bool renderBulkDownloadConfirmFrame() {
    if (bulkDownloadSession == nullptr || !bulkDownloadSession->active) {
        return false;
    }
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr || framebuffer == nullptr) {
        heap_caps_free(scratch);
        return false;
    }
    memset(framebuffer, 0xFF, kFramebufferBytes);
    const bool series = bulkDownloadSession->relation ==
                        CatalogSession::Relation::Series;
    drawScreenChrome(scratch, "ABYSS // BOUNDED ACQUISITION",
                     series ? I18n::tr("ДОКАЧАТЬ СЕРИЮ") : I18n::tr("КНИГИ АВТОРА"));

    drawPortraitText(scratch, &UiCondensed9,
                     I18n::tr("ТОЛЬКО НЕДОСТАЮЩИЕ · ТЕКУЩАЯ СТРАНИЦА"), 30, 154, 5);
    fillPortraitRect(26, 166, 488, 2, 0);
    drawPortraitRoundedRect(26, 194, 488, 244, 18, 12, 15, 1);

    PortraitTextLines labelLines{};
    wrapPortraitText(&UiCondensed14Bold, bulkDownloadSession->label, 426, 3,
                     labelLines);
    drawPortraitTextLines(scratch, &UiCondensed14Bold, labelLines, 50, 250,
                          22, 0, 15);
    char summary[96]{};
    snprintf(summary, sizeof(summary),
             I18n::tr("К ЗАГРУЗКЕ // %02lu    БЕЗОПАСНЫЙ ЛИМИТ // %02lu"),
             static_cast<unsigned long>(bulkDownloadSession->count),
             static_cast<unsigned long>(kBulkDownloadLimit));
    drawPortraitText(scratch, &UiCondensed9, summary, 50, 350, 4);
    drawPortraitText(scratch, &UiCondensed9,
                     I18n::tr("КАЖДАЯ КНИГА ПУБЛИКУЕТСЯ ОТДЕЛЬНО И АТОМАРНО"), 50,
                     394, 6);

    const char *labels[2] = {I18n::tr("СКАЧАТЬ"), I18n::tr("ОТМЕНА")};
    for (size_t index = 0; index < 2; ++index) {
        const int32_t y = 500 + static_cast<int32_t>(index) * 118;
        const bool selected = bulkDownloadSession->selected == index;
        drawSoftRow(42, y, 456, 88, selected);
        char label[64]{};
        if (index == 0) {
            snprintf(label, sizeof(label), "%s  //  %02lu",
                     labels[index],
                     static_cast<unsigned long>(bulkDownloadSession->count));
        } else {
            snprintf(label, sizeof(label), "%s", labels[index]);
        }
        drawPortraitText(scratch, &UiCondensed14Bold, label, 76, y + 54,
                         0, 15);
    }
    fillPortraitRect(26, 786, 488, 2, 0);
    drawPortraitText(scratch, &UiCondensed9,
                     I18n::tr("ОПЕРАЦИЮ МОЖНО БЕЗОПАСНО ПОВТОРИТЬ"), 30, 838, 5);
    drawPortraitText(scratch, &UiCondensed9,
                     I18n::tr("УЖЕ ЗАГРУЖЕННЫЕ КНИГИ НЕ ИЗМЕНЯЮТСЯ"), 30, 878, 7);

    heap_caps_free(scratch);
    return true;
}

bool displayBulkDownloadConfirm(const char *reason) {
    if (!renderBulkDownloadConfirmFrame()) {
        Serial.println("ERROR BULK_CONFIRM reason=frame");
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::BulkDownloadConfirm,
                                    "screen-bulk-confirm");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("BULK_CONFIRM", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        return false;
    }
    uiScreen = UiScreen::BulkDownloadConfirm;
    Serial.printf("BULK CONFIRM visible=true selected=%u reason=%s\n",
                  static_cast<unsigned>(bulkDownloadSession->selected),
                  reason);
    return true;
}

bool displayBulkDownloadProgress() {
    if (bulkDownloadSession == nullptr || !bulkDownloadSession->active) {
        return false;
    }
    char detail[96]{};
    snprintf(detail, sizeof(detail), I18n::tr("%lu КНИГ · ПОСЛЕДОВАТЕЛЬНАЯ ЗАГРУЗКА"),
             static_cast<unsigned long>(bulkDownloadSession->count));
    LongOperationModel model{};
    model.kind = LongOperationKind::BookDownload;
    model.subject = bulkDownloadSession->label;
    model.detail = detail;
    model.identifier = I18n::tr("ТЕКУЩАЯ СТРАНИЦА КАТАЛОГА");
    if (!renderLongOperationFrame(model)) {
        return false;
    }
    scheduleScreenTransitionCleanup(UiScreen::Download,
                                    "screen-bulk-download");
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("BULK_DOWNLOAD", lastDisplayRefresh);
    if (!lastDisplayRefresh.ok) {
        return false;
    }
    uiScreen = UiScreen::Download;
    downloadFailureVisible = false;
    return true;
}

bool continueBulkDownload() {
    if (bulkDownloadSession == nullptr || !bulkDownloadSession->active) {
        return false;
    }
    while (bulkDownloadSession->next < bulkDownloadSession->count) {
        activeDownloadJob =
            bulkDownloadSession->jobs[bulkDownloadSession->next];
        if (localBookPresent(activeDownloadJob.bookId)) {
            ++bulkDownloadSession->skipped;
            ++bulkDownloadSession->next;
            continue;
        }
        if (!downloadJobToStorage(activeDownloadJob, "bulk", false)) {
            Serial.printf(
                "BULK DOWNLOAD paused=true next=%lu completed=%lu "
                "reason=queued-job-failure\n",
                static_cast<unsigned long>(bulkDownloadSession->next + 1),
                static_cast<unsigned long>(bulkDownloadSession->completed));
            return false;
        }
        ++bulkDownloadSession->completed;
        ++bulkDownloadSession->next;
    }
    Serial.printf(
        "BULK DOWNLOAD complete=true downloaded=%lu skipped=%lu limit=%lu\n",
        static_cast<unsigned long>(bulkDownloadSession->completed),
        static_cast<unsigned long>(bulkDownloadSession->skipped),
        static_cast<unsigned long>(kBulkDownloadLimit));
    closeBulkDownloadSession();
    localLibrarySession.loaded = false;
    homeSession.loaded = false;
    return refreshCatalogFrame("bulk-complete");
}

void processBulkDownloadActions() {
    if (pendingBulkDownloadBack) {
        pendingBulkDownloadBack = false;
        pendingBulkDownloadSelect = false;
        pendingBulkDownloadDelta = 0;
        if (uiScreen != UiScreen::BulkDownloadConfirm) {
            return;
        }
        closeBulkDownloadSession();
        refreshCatalogFrame("bulk-cancel");
        return;
    }
    if (pendingBulkDownloadDelta != 0 &&
        millis() - pendingBulkDownloadNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingBulkDownloadDelta;
    pendingBulkDownloadDelta = 0;
    if (delta != 0) {
        if (uiScreen != UiScreen::BulkDownloadConfirm ||
            bulkDownloadSession == nullptr) {
            return;
        }
        const uint8_t target = static_cast<uint8_t>(max(
            0, min(1, static_cast<int>(bulkDownloadSession->selected) +
                          (delta < 0 ? -1 : 1))));
        if (target != bulkDownloadSession->selected) {
            bulkDownloadSession->selected = target;
            displayBulkDownloadConfirm("selection");
        }
        return;
    }
    if (!pendingBulkDownloadSelect) {
        return;
    }
    pendingBulkDownloadSelect = false;
    if (uiScreen != UiScreen::BulkDownloadConfirm ||
        bulkDownloadSession == nullptr) {
        return;
    }
    if (bulkDownloadSession->selected == 1) {
        closeBulkDownloadSession();
        refreshCatalogFrame("bulk-cancel");
        return;
    }
    if (displayBulkDownloadProgress()) {
        continueBulkDownload();
    }
}

void processCatalogActions() {
    if (pendingCatalogRefresh) {
        pendingCatalogRefresh = false;
        displayCatalogUrl(catalogSession.loaded ? catalogSession.currentUrl
                                                : nullptr,
                          false, !catalogSession.loaded, "usb-refresh");
        return;
    }
    if (pendingCatalogBack) {
        pendingCatalogBack = false;
        pendingCatalogOpen = false;
        pendingCatalogDelta = 0;
        if (uiScreen != UiScreen::Catalog) {
            Serial.println("ERROR CATALOG_BACK reason=not-catalog");
            return;
        }
        if (catalogSession.historyDepth == 0) {
            if (catalogSession.relation != CatalogSession::Relation::None) {
                catalogSession.relation = CatalogSession::Relation::None;
                catalogSession.relationLabel[0] = '\0';
                displayTopLevelTab(catalogSession.owner, true,
                                   "relation-back");
                return;
            }
            if (catalogSession.owner == TopLevelTab::Search) {
                catalogSession.relation = CatalogSession::Relation::None;
                catalogSession.relationLabel[0] = '\0';
                displaySearch(false, "results-back");
                return;
            }
            pendingTopLevelTarget = TopLevelTab::Home;
            pendingTopLevelOpen = true;
            return;
        }
        const size_t previousDepth = catalogSession.historyDepth;
        char target[kOpdsUrlCapacity]{};
        snprintf(target, sizeof(target), "%s",
                 catalogSession.history[previousDepth - 1]);
        const CatalogSession::Relation previousRelation =
            catalogSession.relation;
        char previousRelationLabel[sizeof(catalogSession.relationLabel)]{};
        snprintf(previousRelationLabel, sizeof(previousRelationLabel), "%s",
                 catalogSession.relationLabel);
        catalogSession.relation = CatalogSession::Relation::None;
        catalogSession.relationLabel[0] = '\0';
        if (displayCatalogUrl(target, false, false, "back")) {
            catalogSession.historyDepth = previousDepth - 1;
            memset(catalogSession.history[catalogSession.historyDepth], 0,
                   sizeof(catalogSession.history[0]));
        } else {
            catalogSession.relation = previousRelation;
            snprintf(catalogSession.relationLabel,
                     sizeof(catalogSession.relationLabel), "%s",
                     previousRelationLabel);
        }
        return;
    }

    if (pendingCatalogOpen) {
        pendingCatalogOpen = false;
        pendingCatalogDelta = 0;
        if (uiScreen != UiScreen::Catalog || catalogRowCount() == 0) {
            Serial.println("ERROR CATALOG_SELECT reason=no-row");
            return;
        }
        const CatalogRow row = catalogRowAt(catalogSession.selected);
        if (row.kind == CatalogRowKind::BulkDownload) {
            if (!prepareBulkDownloadSession()) {
                refreshCatalogFrame("bulk-nothing-missing");
            } else {
                displayBulkDownloadConfirm("catalog-relation");
            }
            return;
        }
        if (row.href == nullptr || row.href[0] == '\0') {
            Serial.println("ERROR CATALOG_SELECT reason=no-link");
            return;
        }
        if (row.kind == CatalogRowKind::PreviousPage ||
            row.kind == CatalogRowKind::NextPage) {
            displayCatalogUrl(row.href, false, false,
                              row.kind == CatalogRowKind::NextPage
                                  ? "page-next"
                                  : "page-previous");
            return;
        }
        if (row.entry != nullptr &&
            row.entry->kind == OpdsEntryKind::Navigation) {
            displayCatalogUrl(row.href, true, false, "section");
            return;
        }
        if (row.entry == nullptr || row.entry->kind != OpdsEntryKind::Book) {
            Serial.println("ERROR CATALOG_SELECT reason=invalid-book-row");
            return;
        }
        openRemoteBookCard(*row.entry,
                           catalogSession.owner == TopLevelTab::Search
                               ? "search"
                               : "catalog");
        return;
    }

    if (pendingCatalogDelta != 0 &&
        millis() - pendingCatalogNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingCatalogDelta;
    pendingCatalogDelta = 0;
    if (delta == 0) {
        return;
    }
    const size_t count = catalogRowCount();
    if (uiScreen != UiScreen::Catalog || count == 0) {
        Serial.println("ERROR CATALOG_NAVIGATION reason=no-rows");
        return;
    }
    int32_t target = static_cast<int32_t>(catalogSession.selected) + delta;
    target = max(0, target);
    target = min(target, static_cast<int32_t>(count - 1));
    if (static_cast<size_t>(target) == catalogSession.selected) {
        Serial.printf("CATALOG BOUNDARY selected=%lu/%lu\n",
                      static_cast<unsigned long>(catalogSession.selected + 1),
                      static_cast<unsigned long>(count));
        return;
    }
    catalogSession.selected = static_cast<size_t>(target);
    refreshCatalogFrame("selection");
}

void processSearchActions() {
    if (pendingSearchBack) {
        pendingSearchBack = false;
        pendingSearchSelect = false;
        pendingSearchDelta = 0;
        if (uiScreen != UiScreen::Search) {
            Serial.println("ERROR SEARCH_BACK reason=not-search");
            return;
        }
        if (searchSession.phase == SearchPhase::Letter) {
            searchSession.phase = SearchPhase::Range;
            searchSession.selected = searchSession.range;
            searchSession.firstVisible = 0;
            displaySearch(false, "step-back");
        } else if (searchSession.phase == SearchPhase::Range) {
            searchSession.phase = SearchPhase::Scope;
            searchSession.selected = static_cast<size_t>(searchSession.scope);
            searchSession.firstVisible = 0;
            displaySearch(false, "step-back");
        } else {
            pendingTopLevelTarget = TopLevelTab::Home;
            pendingTopLevelOpen = true;
        }
        return;
    }

    if (pendingSearchSelect) {
        pendingSearchSelect = false;
        pendingSearchDelta = 0;
        if (uiScreen != UiScreen::Search || searchRowCount() == 0) {
            Serial.println("ERROR SEARCH_SELECT reason=no-row");
            return;
        }
        if (searchSession.phase == SearchPhase::Scope) {
            searchSession.scope =
                static_cast<SearchScope>(searchSession.selected);
            searchSession.phase = SearchPhase::Range;
            searchSession.selected = 0;
            searchSession.firstVisible = 0;
            displaySearch(false, "scope-selected");
        } else if (searchSession.phase == SearchPhase::Range) {
            searchSession.range = searchSession.selected;
            searchSession.phase = SearchPhase::Letter;
            searchSession.selected = 0;
            searchSession.firstVisible = 0;
            displaySearch(false, "range-selected");
        } else {
            snprintf(searchSession.prefix, sizeof(searchSession.prefix), "%s",
                     searchRowLabel(searchSession.selected));
            executeSearch();
        }
        return;
    }

    if (pendingSearchDelta != 0 &&
        millis() - pendingSearchNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingSearchDelta;
    pendingSearchDelta = 0;
    if (delta == 0) {
        return;
    }
    const size_t count = searchRowCount();
    if (uiScreen != UiScreen::Search || count == 0) {
        Serial.println("ERROR SEARCH_NAVIGATION reason=no-rows");
        return;
    }
    int32_t target = static_cast<int32_t>(searchSession.selected) + delta;
    target = max(0, target);
    target = min(target, static_cast<int32_t>(count - 1));
    if (static_cast<size_t>(target) == searchSession.selected) {
        Serial.printf("SEARCH BOUNDARY selected=%lu/%lu\n",
                      static_cast<unsigned long>(searchSession.selected + 1),
                      static_cast<unsigned long>(count));
        return;
    }
    searchSession.selected = static_cast<size_t>(target);
    displaySearch(false, "selection");
}

void processLocalLibraryActions() {
    if (pendingLocalLibraryBack) {
        pendingLocalLibraryBack = false;
        pendingSelectedBookOpen = false;
        pendingLibraryDelta = 0;
        if (uiScreen != UiScreen::LocalLibrary) {
            Serial.println("ERROR LIBRARY_BACK reason=not-library");
            return;
        }
        if (localLibrarySession.phase == LocalLibraryPhase::Books &&
            localLibrarySession.section == LocalLibrarySection::Search) {
            displaySearch(false, "offline-results-back");
            return;
        }
        if (localLibrarySession.phase == LocalLibraryPhase::Books &&
            localSectionIsGrouping(localLibrarySession.section)) {
            char groupLabel[128]{};
            snprintf(groupLabel, sizeof(groupLabel), "%s",
                     localLibrarySession.groupLabel);
            localLibrarySession.phase = LocalLibraryPhase::Groups;
            localLibrarySession.selected = 0;
            localLibrarySession.firstVisible = 0;
            rebuildLocalLibraryView();
            for (size_t index = 0; index < localLibrarySession.groupCount;
                 ++index) {
                if (strcmp(localLibrarySession.groups[index], groupLabel) ==
                    0) {
                    localLibrarySession.selected = index;
                    break;
                }
            }
            displayLocalLibrary(false, "group-back");
            return;
        }
        if (localLibrarySession.phase != LocalLibraryPhase::Sections) {
            localLibrarySession.phase = LocalLibraryPhase::Sections;
            localLibrarySession.selected = static_cast<size_t>(localLibrarySession.section);
            localLibrarySession.section = LocalLibrarySection::All;
            localLibrarySession.firstVisible = 0;
            localLibrarySession.groupLabel[0] = '\0';
            rebuildLocalLibraryView();
            displayLocalLibrary(false, "sections-back");
            return;
        }
        pendingTopLevelTarget = TopLevelTab::Home;
        pendingTopLevelOpen = true;
        return;
    }

    if (pendingLibraryDelta != 0 &&
        millis() - pendingLibraryNavigationAt < kNavigationCoalesceMs) {
        return;
    }
    const int8_t delta = pendingLibraryDelta;
    pendingLibraryDelta = 0;
    if (delta != 0) {
        const size_t count = localLibraryRowCount();
        if (uiScreen != UiScreen::LocalLibrary || count == 0) {
            Serial.println("ERROR LIBRARY_NAVIGATION reason=no-active-list");
            return;
        }
        int32_t requested =
            static_cast<int32_t>(localLibrarySession.selected) + delta;
        requested = max(0, requested);
        requested = min(requested, static_cast<int32_t>(count - 1));
        const size_t target = static_cast<size_t>(requested);
        if (target == localLibrarySession.selected) {
            Serial.printf("LIBRARY BOUNDARY selected=%lu/%lu\n",
                          static_cast<unsigned long>(
                              localLibrarySession.selected + 1),
                          static_cast<unsigned long>(count));
            return;
        }
        localLibrarySession.selected = target;
        displayLocalLibrary(false, "selection");
        return;
    }

    if (pendingSelectedBookOpen) {
        pendingSelectedBookOpen = false;
        const size_t count = localLibraryRowCount();
        if (uiScreen != UiScreen::LocalLibrary || count == 0 ||
            localLibrarySession.selected >= count) {
            Serial.println("ERROR LIBRARY_OPEN_BOOK reason=no-selection");
            return;
        }
        if (localLibrarySession.phase == LocalLibraryPhase::Sections) {
            if (localLibrarySession.selected == 9) { openWifiSettings(); return; }
            if (localLibrarySession.selected == 8) {
                if (AutomaticSync::request(provisioningActive || bookUpload.active() || BookPreparation::busy() || pendingPowerSleep))
                    displayLocalLibrary(false, "library-sync-started");
                else displayLocalLibrary(false, "sync-rejected");
                return;
            }
            localLibrarySession.section =
                static_cast<LocalLibrarySection>(
                    localLibrarySession.selected);
            localLibrarySession.phase =
                localSectionIsGrouping(localLibrarySession.section)
                    ? LocalLibraryPhase::Groups
                    : LocalLibraryPhase::Books;
            localLibrarySession.selected = 0;
            localLibrarySession.firstVisible = 0;
            localLibrarySession.groupLabel[0] = '\0';
            rebuildLocalLibraryView();
            displayLocalLibrary(false, "section-open");
            return;
        }
        if (localLibrarySession.phase == LocalLibraryPhase::Groups) {
            snprintf(localLibrarySession.groupLabel,
                     sizeof(localLibrarySession.groupLabel), "%s",
                     localLibrarySession.groups[localLibrarySession.selected]);
            localLibrarySession.phase = LocalLibraryPhase::Books;
            localLibrarySession.selected = 0;
            localLibrarySession.firstVisible = 0;
            rebuildLocalLibraryView();
            displayLocalLibrary(false, "group-open");
            return;
        }
        const LocalBookEntry *entry =
            localVisibleEntry(localLibrarySession.selected);
        if (entry == nullptr) {
            Serial.println("ERROR LIBRARY_OPEN_BOOK reason=invalid-row");
            return;
        }
        const LocalBookEntry selected = *entry;
        libraryInteractionAt = millis();
        openLocalBookCard(selected, UiScreen::LocalLibrary, "library");
    }
}

void processIdleLibraryPreparation() {
    if (uiScreen != UiScreen::LocalLibrary ||
        !localLibrarySession.loaded || bookUpload.active() ||
        pendingSelectedBookOpen || pendingLibraryDelta != 0 ||
        millis() - libraryInteractionAt < kIdlePreparationDelayMs) {
        return;
    }

    const uint32_t now = millis();
    const LocalBookEntry *candidate = nullptr;
    for (size_t index = 0;
         index < localLibrarySession.info.loadedCount; ++index) {
        const LocalBookEntry &entry = localLibrarySession.entries[index];
        if (entry.detailsReady && entry.paginationReady) {
            continue;
        }
        const bool retryDeferred =
            strcmp(entry.id, preparationRetryBookId) == 0 &&
            static_cast<int32_t>(now - preparationRetryAt) < 0;
        if (!retryDeferred) {
            candidate = &entry;
            break;
        }
    }
    if (candidate == nullptr) {
        return;
    }

    char bookId[33]{};
    snprintf(bookId, sizeof(bookId), "%s", candidate->id);
    const bool detailsReady = candidate->detailsReady;
    const bool paginationReady = candidate->paginationReady;
    libraryInteractionAt = now;

    const uint32_t startedAt = millis();
    BookPreparationResult &preparation = bookPreparationWorkspace;
    if (!detailsReady) {
        Fb2CacheInfo &enriched = preparation.cache;
        memset(&enriched, 0, sizeof(enriched));
        if (!Fb2Cache::build(bookId, enriched)) {
            snprintf(preparationRetryBookId,
                     sizeof(preparationRetryBookId), "%s", bookId);
            preparationRetryAt = millis() + kPreparationRetryDelayMs;
            Serial.printf(
                "ERROR BOOK_METADATA_IDLE id=%s reason=%s retry_ms=%lu\n",
                bookId, enriched.error,
                static_cast<unsigned long>(kPreparationRetryDelayMs));
            return;
        }
        Serial.printf(
            "BOOK METADATA IDLE COMPLETE id=%s annotation=%lu series=%s "
            "duration_ms=%lu display_refresh=false\n",
            bookId, static_cast<unsigned long>(strlen(enriched.annotation)),
            enriched.series[0] == '\0' ? "none" : enriched.series,
            static_cast<unsigned long>(millis() - startedAt));
    }

    if (paginationReady) {
        if (strcmp(preparationRetryBookId, bookId) == 0) {
            preparationRetryBookId[0] = '\0';
            preparationRetryAt = 0;
        }
        displayLocalLibrary(true, "idle-enriched");
        return;
    }
    if (!prepareBookArtifacts(bookId, "idle", preparation)) {
        snprintf(preparationRetryBookId, sizeof(preparationRetryBookId),
                 "%s", bookId);
        preparationRetryAt = millis() + kPreparationRetryDelayMs;
        Serial.printf(
            "ERROR BOOK_PREPARATION_IDLE id=%s reason=%s retry_ms=%lu\n",
            bookId, preparation.error,
            static_cast<unsigned long>(kPreparationRetryDelayMs));
        return;
    }

    if (strcmp(preparationRetryBookId, bookId) == 0) {
        preparationRetryBookId[0] = '\0';
        preparationRetryAt = 0;
    }
    Serial.printf(
        "BOOK PREPARATION IDLE COMPLETE id=%s total_ms=%lu pages=%lu "
        "chapters=%lu\n",
        bookId, static_cast<unsigned long>(millis() - startedAt),
        static_cast<unsigned long>(preparation.pagination.pageCount),
        static_cast<unsigned long>(preparation.pagination.chapterCount));
    displayLocalLibrary(true,
                        detailsReady ? "idle-prepared" : "idle-enriched");
}

bool testPsram() {
    diagnostics.psramDetected = psramFound();
    diagnostics.psramBytes = ESP.getPsramSize();
    diagnostics.freePsramBytes = ESP.getFreePsram();
    diagnostics.flashBytes = ESP.getFlashChipSize();

    if (!diagnostics.psramDetected || diagnostics.psramBytes < kPsramTestBytes) {
        return false;
    }

    auto *test = static_cast<uint8_t *>(heap_caps_malloc(
        kPsramTestBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (test == nullptr) {
        return false;
    }

    for (size_t index = 0; index < kPsramTestBytes; ++index) {
        test[index] = static_cast<uint8_t>((index * 31U + 17U) & 0xFFU);
    }

    bool passed = true;
    for (size_t index = 0; index < kPsramTestBytes; ++index) {
        const uint8_t expected = static_cast<uint8_t>((index * 31U + 17U) & 0xFFU);
        if (test[index] != expected) {
            passed = false;
            break;
        }
    }

    heap_caps_free(test);
    diagnostics.freePsramBytes = ESP.getFreePsram();
    return passed;
}

bool testSdAtomicWrite() {
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    diagnostics.sdMounted = false;
    for (uint8_t attempt = 0; attempt < 3 && !diagnostics.sdMounted;
         ++attempt) {
        SD.end();
        delay(100 + static_cast<uint32_t>(attempt) * 150U);
        diagnostics.sdMounted = SD.begin(SD_CS, SPI);
        if (diagnostics.sdMounted && SD.cardType() == CARD_NONE) {
            diagnostics.sdMounted = false;
        }
    }
    if (!diagnostics.sdMounted || SD.cardType() == CARD_NONE) {
        diagnostics.sdMounted = false;
        return false;
    }

    diagnostics.sdBytes = SD.cardSize();
    constexpr char directory[] = "/reader-diagnostics";
    constexpr char partialPath[] = "/reader-diagnostics/write-test.partial";
    constexpr char finalPath[] = "/reader-diagnostics/write-test.ok";

    if (!SD.exists(directory) && !SD.mkdir(directory)) {
        return false;
    }

    SD.remove(partialPath);
    SD.remove(finalPath);

    File output = SD.open(partialPath, FILE_WRITE);
    if (!output) {
        return false;
    }

    uint8_t block[256];
    for (size_t index = 0; index < sizeof(block); ++index) {
        block[index] = static_cast<uint8_t>(index ^ 0xA5U);
    }

    bool passed = true;
    for (int blockIndex = 0; blockIndex < 4; ++blockIndex) {
        if (output.write(block, sizeof(block)) != sizeof(block)) {
            passed = false;
            break;
        }
    }
    output.flush();
    output.close();

    if (!passed || !SD.rename(partialPath, finalPath)) {
        SD.remove(partialPath);
        return false;
    }

    File input = SD.open(finalPath, FILE_READ);
    if (!input || input.size() != sizeof(block) * 4U) {
        if (input) {
            input.close();
        }
        SD.remove(finalPath);
        return false;
    }

    for (int blockIndex = 0; blockIndex < 4 && passed; ++blockIndex) {
        for (size_t index = 0; index < sizeof(block); ++index) {
            if (input.read() != static_cast<int>(block[index])) {
                passed = false;
                break;
            }
        }
    }
    input.close();
    SD.remove(finalPath);
    return passed;
}

bool probeRtc() {
    Wire.begin(BOARD_SDA, BOARD_SCL);
    Wire.beginTransmission(0x51);
    return Wire.endTransmission() == 0;
}

void measureBattery() {
    analogReadResolution(12);
    analogSetPinAttenuation(BATT_PIN, ADC_11db);
    delay(120);

    uint32_t rawTotal = 0;
    uint32_t millivoltTotal = 0;
    constexpr uint16_t sampleCount = 32;
    for (uint16_t sample = 0; sample < sampleCount; ++sample) {
        rawTotal += analogRead(BATT_PIN);
        millivoltTotal += analogReadMilliVolts(BATT_PIN);
        delay(3);
    }

    diagnostics.batteryRaw = rawTotal / sampleCount;
    diagnostics.batteryMillivolts = (millivoltTotal / sampleCount) * 2U;
    diagnostics.batteryPercent =
        batteryPercentFromMillivolts(diagnostics.batteryMillivolts);
    nextBatterySampleAt = millis() + 5000U;
    updateBatteryPolicy();
    batterySampleCount = 0;
    batteryRawTotal = batteryMvTotal = 0;
}

void pollBatteryTelemetry() {
    if (static_cast<int32_t>(millis() - nextBatterySampleAt) < 0) {
        return;
    }
    // Spread ADC averaging across loop iterations; do not sleep for 200 ms
    // or redraw an idle e-ink screen just to update its battery indicator.
    batteryRawTotal += analogRead(BATT_PIN);
    batteryMvTotal += analogReadMilliVolts(BATT_PIN);
    if (++batterySampleCount == 32) {
        diagnostics.batteryRaw = batteryRawTotal / 32U;
        diagnostics.batteryMillivolts = (batteryMvTotal / 32U) * 2U;
        diagnostics.batteryPercent =
            batteryPercentFromMillivolts(diagnostics.batteryMillivolts);
        batterySampleCount = 0;
        batteryRawTotal = batteryMvTotal = 0;
        nextBatterySampleAt = millis() + 5000U;
        updateBatteryPolicy();
    } else {
        nextBatterySampleAt = millis() + 3U;
    }
}

void printBatteryStatus(const char *mode) {
    Serial.printf(
        "BATTERY STATUS mode=%s millivolts=%u raw=%u percent=%u "
        "plausible=%s usb_power=unknown\n",
        mode, diagnostics.batteryMillivolts, diagnostics.batteryRaw,
        static_cast<unsigned>(diagnostics.batteryPercent),
        diagnostics.batteryMillivolts >= 3000 &&
                diagnostics.batteryMillivolts <= 4500
            ? "true"
            : "false");
}

void printDiagnostics() {
    Serial.printf(
        "DIAG {\"firmware\":\"%s\",\"version\":\"%s\","
        "\"psram_bytes\":%u,\"psram_test\":%s,\"flash_bytes\":%u,"
        "\"sd_mounted\":%s,\"sd_bytes\":%llu,\"sd_write\":%s,"
        "\"rtc\":%s,\"battery_mv\":%u,\"battery_raw\":%u,"
        "\"battery_percent\":%u,"
        "\"wake_cause\":%d,\"reset_reason\":%d,\"boot_count\":%lu}\n",
        kFirmwareName, kFirmwareVersion,
        static_cast<unsigned>(diagnostics.psramBytes),
        diagnostics.psramPassed ? "true" : "false",
        static_cast<unsigned>(diagnostics.flashBytes),
        diagnostics.sdMounted ? "true" : "false",
        static_cast<unsigned long long>(diagnostics.sdBytes),
        diagnostics.sdWritePassed ? "true" : "false",
        diagnostics.rtcDetected ? "true" : "false",
        diagnostics.batteryMillivolts, diagnostics.batteryRaw,
        static_cast<unsigned>(diagnostics.batteryPercent),
        static_cast<int>(esp_sleep_get_wakeup_cause()),
        static_cast<int>(diagnostics.resetReason),
        static_cast<unsigned long>(bootCount));
}

// One parent action per double click. Editors handle their own sub-level first.
void queueBackAction() {
    switch(uiScreen) {
        case UiScreen::Reader: pendingTopLevelTarget=readerReturnTab;pendingTopLevelOpen=true;break;
        case UiScreen::ReaderMenu: pendingReaderMenuBack=true;break;
        case UiScreen::Contents: pendingContentsBack=true;break;
        case UiScreen::Bookmarks: pendingReaderBookmarksBack=true;break;
        case UiScreen::ReadingSettings: pendingReadingSettingsBack=true;break;
        case UiScreen::OrderedSelector: pendingOrderedSelectorBack=true;break;
        case UiScreen::LocalLibrary: pendingLocalLibraryBack=true;break;
        case UiScreen::Catalog: pendingCatalogBack=true;break;
        case UiScreen::Search: pendingSearchBack=true;break;
        case UiScreen::Favorites: pendingFavoritesBack=true;break;
        case UiScreen::Download: pendingDownloadBack=true;break;
        case UiScreen::BulkDownloadConfirm: pendingBulkDownloadBack=true;break;
        case UiScreen::BookCard: pendingBookCardBack=true;break;
        case UiScreen::Annotation: pendingAnnotationBack=true;break;
        case UiScreen::TopLevel: pendingTopLevelTarget=TopLevelTab::Home;pendingTopLevelOpen=true;break;
        default: break; // Home is the root; Back does not reopen it.
    }
}

#include "sync_progress_ui.inc"
#include "battery_ui.inc"

void emitInput(const char *button, const char *gesture, const char *source) {
    if (physicalInputs != nullptr && xTaskGetCurrentTaskHandle() != uiTaskHandle) {
        const QueuedInput event{button, gesture, millis()};
        if (xQueueSend(physicalInputs, &event, 0) != pdTRUE)
            Serial.println("INPUT OVERFLOW rejected=true");
        return;
    }
    lastActivityAt = millis();
    if(batteryProtectionRequested)return;
    if(batteryWarningVisible) {
        if(!strcmp(button,"CENTER"))dismissBatteryWarning();
        if(strcmp(button,"POWER"))return;
        dismissBatteryWarning();
    }
    if (!wifiTextEntry.load()) snprintf(lastInput, sizeof(lastInput), "%s %s", button, gesture);
    ++inputCount;
    if (!wifiTextEntry.load()) Serial.printf("EVENT %s %s source=%s count=%lu\n", button, gesture, source,
                  static_cast<unsigned long>(inputCount));
    if (strcmp(button, "POWER") == 0) {
        if (wifiScreen()) closeWifiSettings();
        if(collectionTextEntry.load()){collectionTextEntry=false;collectionKeyboard.wipe();}
        pendingPowerSleep = true;
        AutomaticSync::setPaused(true);
        BookPreparation::cancel();
        return;
    }
    if (!strcmp(button, "CENTER") && !strcmp(gesture, "LONG")) {
        if (uiScreen == UiScreen::Sections) return;
        if (wifiScreen()) closeWifiSettings();
        if (collectionTextEntry.load()) { collectionTextEntry=false; collectionKeyboard.wipe(); }
        if (uiScreen == UiScreen::Reader || uiScreen == UiScreen::ReaderMenu ||
            uiScreen == UiScreen::Contents || uiScreen == UiScreen::Bookmarks ||
            uiScreen == UiScreen::ReadingSettings || uiScreen == UiScreen::OrderedSelector)
            activeTopLevelTab = readerReturnTab;
        if (uiScreen == UiScreen::DeviceSettings || uiScreen == UiScreen::Wifi)
            activeTopLevelTab = TopLevelTab::OnDevice;
        pendingTopLevelTarget = activeTopLevelTab;
        pendingTabsOpen = pendingTopLevelOpen = true;
        BookPreparation::cancel();
        if (AutomaticSync::busy()) AutomaticSync::cancel();
        return;
    }
    if(collectionTextEntry.load()){inputCollectionName(button,gesture);return;}
    if (wifiScreen()) { inputWifiSettings(button,gesture); return; }
    if (BookPreparation::busy()) {
        if (strcmp(button, "CENTER") == 0 && strcmp(gesture, "DOUBLE") == 0) {
            BookPreparation::cancel();
            Serial.println("BOOK CANCEL requested=true");
        }
        return;
    }
    if (AutomaticSync::busy()) {
        if (strcmp(button, "CENTER") == 0) AutomaticSync::cancel();
        return;
    }
    if (handleTabNavigation(button, gesture)) return;
    if (!strcmp(button,"CENTER") && !strcmp(gesture,"DOUBLE")) {queueBackAction();return;}
    if (handleSectionBackFocus(button, gesture)) return;
    if (strcmp(button, "CLEAN") == 0) {
        pendingPanelClean = true;
        return;
    }

    const bool verticalNavigation =
        strcmp(button, "UP") == 0 || strcmp(button, "DOWN") == 0;
    if (strcmp(gesture, "SHORT") == 0 && verticalNavigation) {
        if (uiScreen == UiScreen::Reader) {
            noteReaderPageTurn();
        } else if (isUiNavigationScreen(uiScreen)) {
            noteUiNavigationClick();
        }
    }

    if (strcmp(gesture, "SHORT") == 0) {
        if (uiScreen == UiScreen::Reader) {
            if (strcmp(button, "UP") == 0) {
                pendingPageDelta = static_cast<int8_t>(
                    max(-20, static_cast<int>(pendingPageDelta) - 1));
                pendingPageNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingPageDelta = static_cast<int8_t>(
                    min(20, static_cast<int>(pendingPageDelta) + 1));
                pendingPageNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingReaderMenuOpen = true;
            }
        } else if (uiScreen == UiScreen::ReaderMenu) {
            if (strcmp(button, "UP") == 0) {
                pendingReaderMenuDelta = static_cast<int8_t>(
                    max(-8, static_cast<int>(pendingReaderMenuDelta) - 1));
                pendingReaderMenuNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingReaderMenuDelta = static_cast<int8_t>(
                    min(8, static_cast<int>(pendingReaderMenuDelta) + 1));
                pendingReaderMenuNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingReaderMenuSelect = true;
            }
        } else if (uiScreen == UiScreen::Contents) {
            if (strcmp(button, "UP") == 0) {
                pendingContentsDelta = static_cast<int8_t>(
                    max(-32, static_cast<int>(pendingContentsDelta) - 1));
                pendingContentsNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingContentsDelta = static_cast<int8_t>(
                    min(32, static_cast<int>(pendingContentsDelta) + 1));
                pendingContentsNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingContentsSelect = true;
            }
        } else if (uiScreen == UiScreen::Bookmarks) {
            if (strcmp(button, "UP") == 0) {
                pendingReaderBookmarksDelta = static_cast<int8_t>(max(
                    -32, static_cast<int>(pendingReaderBookmarksDelta) - 1));
                pendingReaderBookmarksNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingReaderBookmarksDelta = static_cast<int8_t>(min(
                    32, static_cast<int>(pendingReaderBookmarksDelta) + 1));
                pendingReaderBookmarksNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingReaderBookmarksSelect = true;
            }
        } else if (uiScreen == UiScreen::ReadingSettings) {
            if (strcmp(button, "UP") == 0) {
                pendingReadingSettingsDelta = static_cast<int8_t>(max(
                    -4, static_cast<int>(pendingReadingSettingsDelta) - 1));
                pendingReadingSettingsNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingReadingSettingsDelta = static_cast<int8_t>(min(
                    4, static_cast<int>(pendingReadingSettingsDelta) + 1));
                pendingReadingSettingsNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingReadingSettingsSelect = true;
            }
        } else if (uiScreen == UiScreen::OrderedSelector) {
            if (strcmp(button, "UP") == 0) {
                pendingOrderedSelectorDelta = static_cast<int8_t>(max(
                    -4, static_cast<int>(pendingOrderedSelectorDelta) - 1));
                pendingOrderedSelectorNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingOrderedSelectorDelta = static_cast<int8_t>(min(
                    4, static_cast<int>(pendingOrderedSelectorDelta) + 1));
                pendingOrderedSelectorNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingOrderedSelectorSelect = true;
            }
        } else if (uiScreen == UiScreen::LocalLibrary) {
            if (strcmp(button, "UP") == 0) {
                pendingLibraryDelta = static_cast<int8_t>(
                    max(-31, static_cast<int>(pendingLibraryDelta) - 1));
                pendingLibraryNavigationAt = millis();
                libraryInteractionAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingLibraryDelta = static_cast<int8_t>(
                    min(31, static_cast<int>(pendingLibraryDelta) + 1));
                pendingLibraryNavigationAt = millis();
                libraryInteractionAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingSelectedBookOpen = true;
                libraryInteractionAt = millis();
            }
        } else if (uiScreen == UiScreen::Home) {
            if (strcmp(button, "UP") == 0) {
                pendingHomeDelta = static_cast<int8_t>(
                    max(-3, static_cast<int>(pendingHomeDelta) - 1));
                pendingHomeNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingHomeDelta = static_cast<int8_t>(
                    min(3, static_cast<int>(pendingHomeDelta) + 1));
                pendingHomeNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingHomeBookOpen = true;
            }
        } else if (uiScreen == UiScreen::Catalog) {
            if (strcmp(button, "UP") == 0) {
                pendingCatalogDelta = static_cast<int8_t>(
                    max(-32, static_cast<int>(pendingCatalogDelta) - 1));
                pendingCatalogNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingCatalogDelta = static_cast<int8_t>(
                    min(32, static_cast<int>(pendingCatalogDelta) + 1));
                pendingCatalogNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingCatalogOpen = true;
            }
        } else if (uiScreen == UiScreen::BulkDownloadConfirm) {
            if (strcmp(button, "UP") == 0) {
                pendingBulkDownloadDelta = -1;
                pendingBulkDownloadNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingBulkDownloadDelta = 1;
                pendingBulkDownloadNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingBulkDownloadSelect = true;
            }
        } else if (uiScreen == UiScreen::Search) {
            if (strcmp(button, "UP") == 0) {
                pendingSearchDelta = static_cast<int8_t>(
                    max(-12, static_cast<int>(pendingSearchDelta) - 1));
                pendingSearchNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingSearchDelta = static_cast<int8_t>(
                    min(12, static_cast<int>(pendingSearchDelta) + 1));
                pendingSearchNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingSearchSelect = true;
            }
        } else if (uiScreen == UiScreen::Download) {
            if (strcmp(button, "CENTER") == 0 && downloadFailureVisible) {
                pendingDownloadRetry = true;
            }
        } else if (uiScreen == UiScreen::BookCard) {
            if (strcmp(button, "UP") == 0) {
                pendingBookCardDelta = static_cast<int8_t>(
                    max(-8, static_cast<int>(pendingBookCardDelta) - 1));
                pendingBookCardNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingBookCardDelta = static_cast<int8_t>(
                    min(8, static_cast<int>(pendingBookCardDelta) + 1));
                pendingBookCardNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingBookCardSelect = true;
            }
        } else if (uiScreen == UiScreen::Annotation) {
            if (strcmp(button, "UP") == 0) {
                pendingAnnotationDelta = static_cast<int8_t>(
                    max(-8, static_cast<int>(pendingAnnotationDelta) - 1));
                pendingAnnotationNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingAnnotationDelta = static_cast<int8_t>(
                    min(8, static_cast<int>(pendingAnnotationDelta) + 1));
                pendingAnnotationNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingAnnotationBack = true;
            }
        } else if (uiScreen == UiScreen::Favorites) {
            if (strcmp(button, "UP") == 0) {
                pendingFavoritesDelta = static_cast<int8_t>(
                    max(-16, static_cast<int>(pendingFavoritesDelta) - 1));
                pendingFavoritesNavigationAt = millis();
            } else if (strcmp(button, "DOWN") == 0) {
                pendingFavoritesDelta = static_cast<int8_t>(
                    min(16, static_cast<int>(pendingFavoritesDelta) + 1));
                pendingFavoritesNavigationAt = millis();
            } else if (strcmp(button, "CENTER") == 0) {
                pendingFavoritesSelect = true;
            }
        } else if (uiScreen == UiScreen::TopLevel) {
            if (strcmp(button, "CENTER") == 0) {
                Serial.printf("TAB SELECT tab=%s action=pending-data\n",
                              topLevelTabCode(activeTopLevelTab));
            }
        } else if (strcmp(button, "CENTER") == 0) {
            pendingTopLevelTarget = TopLevelTab::Home;
            pendingTopLevelOpen = true;
        }
    } else if (strcmp(gesture, "LONG") == 0 && verticalNavigation) {
        const int8_t direction=!strcmp(button,"UP")?-1:1;
        if(uiScreen==UiScreen::Reader) {
            pendingChapterDelta=static_cast<int8_t>(max(-10,min(10,static_cast<int>(pendingChapterDelta)+direction)));
            pendingChapterNavigationAt=millis();
        }
    }
}

uint32_t sleepContextChecksum(const SleepContext &context) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(&context);
    uint32_t hash = 2166136261U;
    const size_t length = sizeof(SleepContext) - sizeof(context.checksum);
    for (size_t index = 0; index < length; ++index) {
        hash ^= bytes[index];
        hash *= 16777619U;
    }
    return hash;
}

bool validSleepContext() {
    return sleepContext.magic == kSleepContextMagic &&
           sleepContext.version == kSleepContextVersion &&
           sleepContext.checksum == sleepContextChecksum(sleepContext);
}

void saveSleepContext() {
    SleepContext context{};
    context.magic = kSleepContextMagic;
    context.version = kSleepContextVersion;
    context.screen = static_cast<uint8_t>(uiScreen);
    context.activeTab = static_cast<uint8_t>(activeTopLevelTab);
    context.readerReturnTab = static_cast<uint8_t>(readerReturnTab);
    if (readerSession.active && (uiScreen == UiScreen::Reader || uiScreen == UiScreen::ReaderMenu ||
        uiScreen == UiScreen::Contents || uiScreen == UiScreen::Bookmarks ||
        uiScreen == UiScreen::ReadingSettings || uiScreen == UiScreen::OrderedSelector)) {
        context.screen=static_cast<uint8_t>(UiScreen::Reader);
        snprintf(context.bookId, sizeof(context.bookId), "%s",
                 readerSession.bookId);
        context.pageCount = readerSession.pageCount;
        context.chapterCount = readerSession.chapterCount;
    }
    context.checksum = sleepContextChecksum(context);
    sleepContext = context;
}

bool selectSleepBook(char *bookId, size_t capacity) {
    if (bookId == nullptr || capacity == 0) {
        return false;
    }
    bookId[0] = '\0';
    if (readerSession.active &&
        BookUploadReceiver::validBookId(readerSession.bookId) &&
        localBookPresent(readerSession.bookId)) {
        snprintf(bookId, capacity, "%s", readerSession.bookId);
        return true;
    }

    RecentBookList recent{};
    if (!RecentBooks::load(recent)) {
        Serial.printf("WARNING SLEEP_FRAME recent=false reason=%s\n",
                      recent.error);
        return false;
    }
    for (size_t index = 0; index < recent.count; ++index) {
        if (BookUploadReceiver::validBookId(recent.ids[index]) &&
            localBookPresent(recent.ids[index])) {
            snprintf(bookId, capacity, "%s", recent.ids[index]);
            return true;
        }
    }
    return false;
}

bool renderSleepFrame(const char *reason) {
    if (framebuffer == nullptr || !displayInitialized) {
        return false;
    }
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr) {
        return false;
    }

    memset(framebuffer, 0xFF, kFramebufferBytes);
    drawScreenChrome(scratch, "ABYSS // READER", I18n::tr("ПАУЗА"));

    char bookId[33]{};
    Fb2CacheInfo metadata{};
    CoverBitmap cover{};
    CoverCacheInfo coverInfo{};
    const bool hasBook = selectSleepBook(bookId, sizeof(bookId));
    const bool hasMetadata = hasBook && Fb2Cache::load(bookId, metadata);
    bool hasCover = hasBook && CoverCache::load(bookId, cover, coverInfo);
    if (!hasCover && hasBook && CoverCache::sourcePresent(bookId)) {
        CoverCacheInfo buildInfo{};
        if (CoverCache::build(bookId, buildInfo)) {
            hasCover = CoverCache::load(bookId, cover, coverInfo);
        } else {
            Serial.printf("WARNING SLEEP_FRAME cover=false id=%s reason=%s\n",
                          bookId, buildInfo.error);
        }
    }

    if (hasCover) {
        drawPortraitRoundedRect(94, 132, 352, 484, 18, 9, 15, 1);
        drawPortraitCoverFitted(cover, 270, 374, 324, 456);

        const char *title = hasMetadata && metadata.title[0] != '\0'
                                ? metadata.title
                                : I18n::tr("ТЕКУЩАЯ КНИГА");
        PortraitTextLines titleLines{};
        wrapPortraitText(&UiCondensed16Bold, title, 460, 3, titleLines);
        const int32_t firstBaseline =
            titleLines.count <= 1 ? 680 : (titleLines.count == 2 ? 664 : 648);
        for (size_t index = 0; index < titleLines.count; ++index) {
            const int32_t width =
                measurePortraitText(&UiCondensed16Bold, titleLines.line[index]);
            drawPortraitText(scratch, &UiCondensed16Bold,
                             titleLines.line[index],
                             max(36, (kPortraitWidth - width) / 2),
                             firstBaseline + static_cast<int32_t>(index) * 28);
        }
        if (hasMetadata && metadata.author[0] != '\0') {
            char author[144]{};
            fitPortraitText(&UiCondensed13, metadata.author, 430, author,
                            sizeof(author));
            const int32_t width = measurePortraitText(&UiCondensed13, author);
            drawPortraitText(scratch, &UiCondensed13, author,
                             max(54, (kPortraitWidth - width) / 2), 758, 5);
        }
    } else {
        drawPortraitText(scratch, &UiCondensed9, I18n::tr("ДОМАШНЯЯ БИБЛИОТЕКА"), 54,
                         238, 6);
        drawPortraitText(scratch, &UiCondensed22Medium, "ABYSS", 54, 328);
        fillPortraitRoundedRect(54, 370, 432, 168, 22, 0);
        drawPortraitText(scratch, &UiCondensed16Bold, I18n::tr("ЧИТАЛКА"), 92, 444, 15,
                         0);
        drawPortraitText(scratch, &UiCondensed13, I18n::tr("КНИГИ ЖДУТ ВАС"), 92, 494,
                         11, 0);
    }

    fillPortraitRect(36, 842, 468, 1, 11);
    drawPortraitCircle(61, 886, 15, 5, 15, 2);
    drawPortraitLine(61, 869, 61, 886, 5, 3);
    drawPortraitText(scratch, &UiCondensed13, I18n::tr("СПЯЩИЙ РЕЖИМ"), 92, 892, 4);
    drawPortraitText(scratch, &UiCondensed9,
                     hasCover ? I18n::tr("ОБЛОЖКА СОХРАНЕНА НА ЭКРАНЕ")
                              : I18n::tr("ДО ВСТРЕЧИ СЛЕДУЮЩЕЙ ГЛАВЕ"),
                     92, 924, 7);

    CoverCache::release(cover);
    heap_caps_free(scratch);
    Serial.printf("POWER SLEEP_FRAME ready=true id=%s cover=%s reason=%s\n",
                  hasBook ? bookId : "none", hasCover ? "true" : "false",
                  reason == nullptr ? "unspecified" : reason);
    return true;
}

bool displaySleepFrame(const char *reason) {
    if (!renderSleepFrame(reason)) {
        Serial.println("WARNING POWER SLEEP_FRAME ready=false");
        return false;
    }
    displayRefresh.requestHardClearBeforeNextRefresh();
    lastDisplayRefresh =
        displayRefresh.refresh(framebuffer, DisplayRefreshMode::QualityFull);
    hasDisplayRefresh = true;
    printDisplayRefresh("SLEEP", lastDisplayRefresh);
    return lastDisplayRefresh.ok;
}

bool restoreWakeState() {
    const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    if (wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED && !batteryResumeLoaded) {
        return false;
    }

    const bool contextValid = validSleepContext();
    Serial.printf("POWER WAKE cause=%d context_valid=%s\n",
                  static_cast<int>(wakeCause),
                  contextValid ? "true" : "false");
    if (contextValid && sleepContext.activeTab <=
                            static_cast<uint8_t>(TopLevelTab::Favorites)) {
        activeTopLevelTab =
            static_cast<TopLevelTab>(sleepContext.activeTab);
    }
    if (contextValid && sleepContext.readerReturnTab <=
                            static_cast<uint8_t>(TopLevelTab::Favorites)) {
        readerReturnTab =
            static_cast<TopLevelTab>(sleepContext.readerReturnTab);
    }

    if (contextValid &&
        sleepContext.screen == static_cast<uint8_t>(UiScreen::Reader) &&
        BookUploadReceiver::validBookId(sleepContext.bookId)) {
        ReaderPaginationInfo pagination{};
        ReaderProgress progress{};
        if (ReaderPagination::load(sleepContext.bookId, pagination) &&
            ReaderPagination::loadProgress(sleepContext.bookId,
                                           pagination.pageCount, progress)) {
            readerSession = ReaderSession{};
            readerSession.active = true;
            snprintf(readerSession.bookId, sizeof(readerSession.bookId), "%s",
                     sleepContext.bookId);
            readerSession.currentPage = progress.currentPage;
            readerSession.pageCount = pagination.pageCount;
            readerSession.chapterCount = pagination.chapterCount;
            uiScreen = UiScreen::Reader;
            const bool readerRestored = displayReaderPage(
                readerSession.currentPage, "wake-from-sleep-frame");
            Serial.printf(
                "POWER WAKE RESTORE screen=reader id=%s page=%lu/%lu "
                "redraw=true panel=sleep-frame ok=%s\n",
                readerSession.bookId,
                static_cast<unsigned long>(readerSession.currentPage),
                static_cast<unsigned long>(readerSession.pageCount),
                readerRestored ? "true" : "false");
            if (readerRestored) {
                return true;
            }
            readerSession = ReaderSession{};
        }
        Serial.printf(
            "POWER WAKE RESTORE screen=reader ok=false id=%s "
            "fallback=home\n",
            sleepContext.bookId);
    }

    const bool homeRestored = displayHome(true, "wake");
    Serial.printf("POWER WAKE RESTORE screen=home ok=%s redraw=true\n",
                  homeRestored ? "true" : "false");
    return homeRestored;
}

void printPowerStatus() {
    Serial.printf(
        "POWER STATUS screen=%u idle_ms=%lu auto_sleep_ms=%lu "
        "wake_pin=%u wake_level=%s context_valid=%s switch=%s level=%d\n",
        static_cast<unsigned>(uiScreen),
        static_cast<unsigned long>(idleBeforeLastCommand),
        static_cast<unsigned long>((activeReaderSettings.sleepMinutes * 60U * 1000U)),
        static_cast<unsigned>(kPowerPin),
        sleepSwitchLatching.load() && digitalRead(kPowerPin) == LOW ? "high" : "low",
        validSleepContext() ? "true" : "false",
        sleepSwitchLatching.load() ? "latching" : "momentary", digitalRead(kPowerPin));
}

void enterDeepSleep(const char *reason, uint32_t timerWakeSeconds) {
    if (WifiSetup::active()) closeWifiSettings();
    AutomaticSync::setPaused(true);
    if (AutomaticSync::busy() || BookPreparation::busy()) {
        BookPreparation::cancel();
        pendingPowerSleep = true;
        AutomaticSync::setPaused(true);
        return;
    }
    saveSleepContext();
    const bool sleepFrameVisible = displaySleepFrame(reason);
    Serial.printf(
        "POWER DEEP_SLEEP reason=%s wake=POWER_GPIO10 timer_seconds=%lu "
        "screen=%u reader=%s sleep_frame=%s\n",
        reason, static_cast<unsigned long>(timerWakeSeconds),
        static_cast<unsigned>(uiScreen),
        readerSession.active ? readerSession.bookId : "none",
        sleepFrameVisible ? "true" : "false");
    Serial.flush();
    networkService.disconnect();
    SD.end();
    displayRefresh.shutdown();
    pinMode(kPowerPin, INPUT_PULLUP);
    const int wakeLevel = sleepSwitchLatching.load() && digitalRead(kPowerPin) == LOW ? 1 : 0;
    Serial.printf("POWER WAKE_ARM level=%d trigger=%d switch=%s\n",
                  digitalRead(kPowerPin), wakeLevel, sleepSwitchLatching.load() ? "latching" : "momentary");
    // EXT0 samples RTC IO during deep sleep. The digital-domain pull-up alone
    // does not keep this input HIGH: configure the RTC-domain pull explicitly.
    rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(kPowerPin));
    rtc_gpio_pullup_en(static_cast<gpio_num_t>(kPowerPin));
    // Sleep switch is latching: wake on the opposite stable level. A fixed
    // LOW wake would immediately wake again whenever the switch stays LOW.
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(kPowerPin),
                                wakeLevel);
    if (timerWakeSeconds > 0) {
        esp_sleep_enable_timer_wakeup(
            static_cast<uint64_t>(timerWakeSeconds) * 1000000ULL);
    }
    delay(50);
    esp_deep_sleep_start();
}

void processPowerAction() {
    if (!pendingPowerSleep || AutomaticSync::busy() || BookPreparation::busy()) return;
    pendingPowerSleep = false;
    enterDeepSleep("power-button");
}

#include "battery_shutdown.inc"

bool uiActionPending() {
    return pendingPageDelta != 0 || pendingChapterDelta != 0 ||
           pendingLibraryDelta != 0 || pendingHomeDelta != 0 ||
           pendingTopLevelDelta != 0 || pendingCatalogDelta != 0 ||
           pendingSearchDelta != 0 || pendingBookCardDelta != 0 ||
           pendingAnnotationDelta != 0 || pendingFavoritesDelta != 0 ||
           pendingReaderMenuDelta != 0 || pendingContentsDelta != 0 ||
           pendingReaderBookmarksDelta != 0 ||
           pendingReadingSettingsDelta != 0 ||
           pendingOrderedSelectorDelta != 0 || pendingBulkDownloadDelta != 0 ||
           pendingSelectedBookOpen || pendingLocalLibraryBack ||
           pendingHomeBookOpen || pendingTopLevelOpen || pendingCatalogOpen ||
           pendingCatalogBack || pendingSearchSelect || pendingSearchBack ||
           pendingDownloadRetry || pendingDownloadBack ||
           pendingBookCardSelect || pendingBookCardBack ||
           pendingAnnotationBack || pendingFavoritesSelect ||
           pendingFavoritesBack || pendingReaderMenuOpen ||
           pendingReaderMenuSelect || pendingReaderMenuBack ||
           pendingContentsSelect || pendingContentsBack ||
           pendingReaderBookmarksSelect || pendingReaderBookmarksBack ||
           pendingReadingSettingsSelect || pendingReadingSettingsBack ||
           pendingOrderedSelectorSelect || pendingOrderedSelectorBack ||
           pendingBulkDownloadSelect || pendingBulkDownloadBack ||
           pendingCatalogRefresh || pendingUsbReaderOpenId[0] != '\0' ||
           pendingUsbReaderPage || pendingPanelClean || renderPending;
}

void processPanelCleanAction() {
    if (!pendingPanelClean) {
        return;
    }
    pendingPanelClean = false;

    if (!displayInitialized || framebuffer == nullptr ||
        uiScreen == UiScreen::None || !hasDisplayRefresh) {
        Serial.println(
            "ERROR PANEL_CLEAN reason=no-restorable-frame");
        return;
    }

    Serial.printf("PANEL CLEAN START screen=%u action=hard-clear-restore\n",
                  static_cast<unsigned>(uiScreen));
    Serial.flush();

    const DisplayRefreshResult cleanResult = displayRefresh.repairToWhite();
    printDisplayRefresh("PANEL_CLEAN_WHITE", cleanResult);
    if (!cleanResult.ok) {
        Serial.println("PANEL CLEAN COMPLETE ok=false restored=false");
        return;
    }
    readerTurnsSinceClean = 0;
    uiNavigationClicksSinceClean = 0;

    const DisplayRefreshMode restoreMode =
        uiScreen == UiScreen::Reader ? DisplayRefreshMode::ReaderText
                                     : DisplayRefreshMode::QualityFull;
    lastDisplayRefresh = displayRefresh.refresh(framebuffer, restoreMode);
    hasDisplayRefresh = lastDisplayRefresh.ok;
    printDisplayRefresh("PANEL_CLEAN_RESTORE", lastDisplayRefresh);
    Serial.printf("PANEL CLEAN COMPLETE ok=%s restored=%s\n",
                  lastDisplayRefresh.ok ? "true" : "false",
                  lastDisplayRefresh.ok ? "true" : "false");
}

void processAutoSleep() {
    if (uiScreen == UiScreen::None || uiScreen == UiScreen::Preparation ||
        uiScreen == UiScreen::Download || bookUpload.active() ||
        provisioningActive || uiActionPending()) {
        return;
    }
    if (millis() - lastActivityAt >= (activeReaderSettings.sleepMinutes * 60U * 1000U)) {
        enterDeepSleep("idle-30m");
    }
}

void processIdleBookCardCover() {
    constexpr uint32_t kCoverPreparationDelayMs = 1500;
    if (uiScreen != UiScreen::BookCard || !bookCardSession.active ||
        bookCardSession.coverPreparationAttempted || bookUpload.active() ||
        provisioningActive || uiActionPending() ||
        millis() - bookCardSession.displayedAt <
            kCoverPreparationDelayMs) {
        return;
    }

    bookCardSession.coverPreparationAttempted = true;
    if (!BookUploadReceiver::validBookId(bookCardSession.bookId)) {
        Serial.println(
            "COVER IDLE skipped=true reason=book-id-unavailable");
        return;
    }

    CoverBitmap existing{};
    CoverCacheInfo existingInfo{};
    if (CoverCache::load(bookCardSession.bookId, existing, existingInfo)) {
        CoverCache::release(existing);
        Serial.printf("COVER IDLE id=%s reused=true refresh=false\n",
                      bookCardSession.bookId);
        return;
    }

    if (CoverCache::sourcePresent(bookCardSession.bookId)) {
        CoverCacheInfo prepared{};
        const bool ok = CoverCache::build(bookCardSession.bookId, prepared);
        Serial.printf(
            "COVER IDLE id=%s source=sd prepared=%s size=%ux%u "
            "refresh=false reason=%s\n",
            bookCardSession.bookId, ok ? "true" : "false",
            prepared.width, prepared.height,
            ok ? "none" : prepared.error);
        return;
    }

    if (bookCardSession.local) {
        Serial.printf(
            "COVER IDLE id=%s skipped=true reason=source-missing-local\n",
            bookCardSession.bookId);
        return;
    }
    const char *coverUrl =
        bookCardSession.remoteEntry.coverHref[0] != '\0'
            ? bookCardSession.remoteEntry.coverHref
            : bookCardSession.remoteEntry.thumbnailHref;
    if (coverUrl[0] == '\0') {
        Serial.printf(
            "COVER IDLE id=%s skipped=true reason=url-unavailable\n",
            bookCardSession.bookId);
        return;
    }

    BookDownloadResult download{};
    if (!networkService.downloadCover(coverUrl, bookCardSession.bookId,
                                      download)) {
        Serial.printf(
            "WARNING COVER_IDLE_DOWNLOAD id=%s http=%d duration_ms=%lu "
            "reason=%s refresh=false\n",
            bookCardSession.bookId, download.httpCode,
            static_cast<unsigned long>(download.durationMs),
            download.error);
        return;
    }
    CoverCacheInfo prepared{};
    const bool ok = CoverCache::build(bookCardSession.bookId, prepared);
    Serial.printf(
        "COVER IDLE DOWNLOAD id=%s bytes=%lu duration_ms=%lu "
        "prepared=%s size=%ux%u refresh=false reason=%s\n",
        bookCardSession.bookId,
        static_cast<unsigned long>(download.responseBytes),
        static_cast<unsigned long>(download.durationMs),
        ok ? "true" : "false", prepared.width, prepared.height,
        ok ? "none" : prepared.error);
}

void printStorageRecoveryStatus(const char *source) {
    Serial.printf(
        "STORAGE RECOVERY source=%s ran=%s ok=%s books=%u restored=%u "
        "partials_removed=%u rollbacks_removed=%u errors=%u\n",
        source, storageRecoveryRan ? "true" : "false",
        storageRecoveryReport.ok ? "true" : "false",
        static_cast<unsigned>(storageRecoveryReport.booksScanned),
        static_cast<unsigned>(storageRecoveryReport.filesRestored),
        static_cast<unsigned>(storageRecoveryReport.partialsRemoved),
        static_cast<unsigned>(storageRecoveryReport.rollbackCopiesRemoved),
        static_cast<unsigned>(storageRecoveryReport.errors));
}

bool cleanupMalformedBookFixture() {
    constexpr char bookId[] = "recovery-malformed";
    bool passed = BookUploadReceiver::invalidateDerivedFiles(bookId);
    constexpr const char *paths[] = {
        "/books/recovery-malformed/book.fb2",
        "/books/recovery-malformed/book.fb2.part",
        "/books/recovery-malformed/book.fb2.old",
    };
    for (const char *path : paths) {
        passed = (!SD.exists(path) || SD.remove(path)) && passed;
    }
    if (SD.exists("/books/recovery-malformed/cache")) {
        passed = SD.rmdir("/books/recovery-malformed/cache") && passed;
    }
    if (SD.exists("/books/recovery-malformed")) {
        passed = SD.rmdir("/books/recovery-malformed") && passed;
    }
    return passed;
}

bool runMalformedBookTest(MalformedBookTestResult &result) {
    result = MalformedBookTestResult{};
    constexpr char bookId[] = "recovery-malformed";
    constexpr char sourcePath[] = "/books/recovery-malformed/book.fb2";
    if (!cleanupMalformedBookFixture() ||
        !BookUploadReceiver::ensureBookDirectory(bookId)) {
        snprintf(result.parserError, sizeof(result.parserError),
                 "fixture-create-failed");
        return false;
    }

    File source = SD.open(sourcePath, FILE_WRITE);
    if (!source) {
        snprintf(result.parserError, sizeof(result.parserError),
                 "fixture-open-failed");
        cleanupMalformedBookFixture();
        return false;
    }
    constexpr char fragment[] = "<broken>";
    bool written = true;
    for (uint8_t index = 0; index < 64; ++index) {
        written = source.write(
                      reinterpret_cast<const uint8_t *>(fragment),
                      sizeof(fragment) - 1) == sizeof(fragment) - 1 &&
                  written;
    }
    source.flush();
    source.close();
    if (!written) {
        snprintf(result.parserError, sizeof(result.parserError),
                 "fixture-write-failed");
        cleanupMalformedBookFixture();
        return false;
    }

    Fb2CacheInfo cacheInfo{};
    result.parserRejected = !Fb2Cache::build(bookId, cacheInfo);
    snprintf(result.parserError, sizeof(result.parserError), "%s",
             cacheInfo.error[0] == '\0' ? "none" : cacheInfo.error);
    File original = SD.open(sourcePath, FILE_READ);
    result.originalPreserved = original && original.size() == 512;
    if (original) {
        original.close();
    }
    result.partialsAbsent =
        !SD.exists("/books/recovery-malformed/cache/content-v1.txt") &&
        !SD.exists("/books/recovery-malformed/cache/content-v1.txt.part") &&
        !SD.exists("/books/recovery-malformed/metadata.json") &&
        !SD.exists("/books/recovery-malformed/metadata.json.part");
    result.cleanupPassed = cleanupMalformedBookFixture();
    result.ok = result.parserRejected && result.originalPreserved &&
                result.partialsAbsent && result.cleanupPassed;
    return result.ok;
}

bool handleProvisioningCommand(char *line) {
    if (strncmp(line, "PROVISION", 9) != 0) {
        return false;
    }

    const size_t commandLength = strnlen(line, sizeof(serialLine));
    const auto wipeCommand = [line, commandLength]() {
        if (commandLength > 0) {
            memset(line, 0, commandLength);
        }
    };

    if (strcmp(line, "PROVISION PAIR BEGIN") == 0) {
        char error[64]{};
        memset(&stagedProvisioning, 0, sizeof(stagedProvisioning));
        ProvisioningStore::load(stagedProvisioning, error, sizeof(error));
        WifiCredential wifi{};
        if (WifiCredentials::load(wifi)) {
            memcpy(stagedProvisioning.wifiSsid, wifi.ssid, sizeof(wifi.ssid));
            memcpy(stagedProvisioning.wifiPassword, wifi.password, sizeof(wifi.password));
        }
        WifiCredentials::wipe(&wifi, sizeof(wifi));
        stagedProvisioningMask = 0x03;
        provisioningActive = true;
        wipeCommand();
        Serial.printf("PROVISION PAIR READY device=%s\n", AutomaticSync::deviceId());
        return true;
    }
    if (strcmp(line, "PROVISION BEGIN") == 0) {
        memset(&stagedProvisioning, 0, sizeof(stagedProvisioning));
        stagedProvisioningMask = 0;
        provisioningActive = true;
        wipeCommand();
        Serial.printf("OK PROVISION BEGIN schema=%u\n",
                      static_cast<unsigned>(kProvisioningSchemaVersion));
        return true;
    }

    if (strcmp(line, "PROVISION ABORT") == 0) {
        memset(&stagedProvisioning, 0, sizeof(stagedProvisioning));
        stagedProvisioningMask = 0;
        provisioningActive = false;
        wipeCommand();
        Serial.println("OK PROVISION ABORT staged=false");
        return true;
    }

    if (strncmp(line, "PROVISION FIELD ", 16) == 0) {
        char fieldName[32]{};
        char encoded[385]{};
        char trailing = '\0';
        const int fields = sscanf(line + 16, "%31s %384s %c", fieldName,
                                  encoded, &trailing);
        if (!provisioningActive || fields != 2) {
            memset(encoded, 0, sizeof(encoded));
            wipeCommand();
            Serial.println("ERROR PROVISION reason=invalid-field-command");
            return true;
        }

        char *target = nullptr;
        size_t capacity = 0;
        uint8_t mask = 0;
        if (strcmp(fieldName, "SSID") == 0) {
            target = stagedProvisioning.wifiSsid;
            capacity = sizeof(stagedProvisioning.wifiSsid);
            mask = 0x01;
        } else if (strcmp(fieldName, "WIFI_PASSWORD") == 0) {
            target = stagedProvisioning.wifiPassword;
            capacity = sizeof(stagedProvisioning.wifiPassword);
            mask = 0x02;
        } else if (strcmp(fieldName, "OPDS_URL") == 0) {
            target = stagedProvisioning.opdsUrl;
            capacity = sizeof(stagedProvisioning.opdsUrl);
            mask = 0x04;
        } else if (strcmp(fieldName, "OPDS_USERNAME") == 0) {
            target = stagedProvisioning.opdsUsername;
            capacity = sizeof(stagedProvisioning.opdsUsername);
            mask = 0x08;
        } else if (strcmp(fieldName, "OPDS_PASSWORD") == 0) {
            target = stagedProvisioning.opdsPassword;
            capacity = sizeof(stagedProvisioning.opdsPassword);
            mask = 0x10;
        }

        char error[64]{};
        size_t decodedBytes = 0;
        const bool accepted =
            target != nullptr &&
            ProvisioningStore::decodeBase64Field(
                encoded, target, capacity, decodedBytes, error, sizeof(error));
        memset(encoded, 0, sizeof(encoded));
        wipeCommand();
        if (!accepted) {
            if (target != nullptr && capacity > 0) {
                memset(target, 0, capacity);
            }
            Serial.println("ERROR PROVISION reason=invalid-field-value");
            return true;
        }
        stagedProvisioningMask |= mask;
        Serial.printf("OK PROVISION FIELD name=%s accepted=true\n",
                      fieldName);
        return true;
    }

    if (strcmp(line, "PROVISION COMMIT") == 0) {
        wipeCommand();
        if (!provisioningActive ||
            stagedProvisioningMask != kProvisioningAllFieldsMask) {
            Serial.println("ERROR PROVISION reason=incomplete-staging");
            return true;
        }
        char error[64]{};
        const bool saved = ProvisioningStore::save(
            stagedProvisioning, error, sizeof(error));
        ProvisioningConfig verified{};
        const bool loaded =
            saved && ProvisioningStore::load(verified, error, sizeof(error));
        const bool matches =
            loaded && memcmp(&verified, &stagedProvisioning,
                             sizeof(stagedProvisioning)) == 0;
        memset(&verified, 0, sizeof(verified));
        memset(&stagedProvisioning, 0, sizeof(stagedProvisioning));
        stagedProvisioningMask = 0;
        provisioningActive = false;
        networkService.invalidateConfiguration();
        if (!matches) {
            Serial.println("ERROR PROVISION reason=commit-verification-failed");
            return true;
        }
        Serial.printf(
            "PROVISION COMMIT COMPLETE configured=true schema=%u\n",
            static_cast<unsigned>(kProvisioningSchemaVersion));
        return true;
    }

    if (strcmp(line, "PROVISION STATUS") == 0) {
        wipeCommand();
        ProvisioningConfig config{};
        char error[64]{};
        const bool configured =
            ProvisioningStore::load(config, error, sizeof(error));
        memset(&config, 0, sizeof(config));
        Serial.printf(
            "PROVISION STATUS configured=%s schema=%u fields=%s\n",
            configured ? "true" : "false",
            static_cast<unsigned>(kProvisioningSchemaVersion),
            configured ? "complete" : "none");
        return true;
    }

    if (strcmp(line, "PROVISION CLEAR CONFIRM") == 0) {
        wipeCommand();
        char error[64]{};
        const bool cleared = ProvisioningStore::clear(error, sizeof(error));
        memset(&stagedProvisioning, 0, sizeof(stagedProvisioning));
        stagedProvisioningMask = 0;
        provisioningActive = false;
        networkService.invalidateConfiguration();
        Serial.printf("PROVISION CLEAR COMPLETE ok=%s configured=false\n",
                      cleared ? "true" : "false");
        return true;
    }

    wipeCommand();
    Serial.println("ERROR PROVISION reason=invalid-command");
    return true;
}

bool handleFallbackWifiCommand(char *line) {
    if (strncmp(line, "WIFI FALLBACK", 13) != 0) {
        return false;
    }
    const size_t commandLength = strnlen(line, sizeof(serialLine));
    const auto wipeCommand = [line, commandLength]() {
        if (commandLength > 0) {
            memset(line, 0, commandLength);
        }
    };

    if (strcmp(line, "WIFI FALLBACK STATUS") == 0) {
        WifiCredential credential{};
        char error[64]{};
        const bool configured = ProvisioningStore::loadFallbackWifi(
            credential, error, sizeof(error));
        memset(&credential, 0, sizeof(credential));
        wipeCommand();
        Serial.printf("WIFI FALLBACK STATUS configured=%s\n",
                      configured ? "true" : "false");
        return true;
    }

    if (strcmp(line, "WIFI FALLBACK CLEAR CONFIRM") == 0) {
        wipeCommand();
        char error[64]{};
        const bool cleared = ProvisioningStore::clearFallbackWifi(
            error, sizeof(error));
        networkService.disconnect();
        Serial.printf("WIFI FALLBACK CLEAR COMPLETE ok=%s\n",
                      cleared ? "true" : "false");
        return true;
    }

    if (strncmp(line, "WIFI FALLBACK SET ", 18) == 0) {
        char encodedSsid[65]{};
        char encodedPassword[129]{};
        char trailing = '\0';
        const int fields = sscanf(line + 18, "%64s %128s %c", encodedSsid,
                                  encodedPassword, &trailing);
        WifiCredential credential{};
        char error[64]{};
        size_t decodedBytes = 0;
        bool accepted = fields == 2 &&
                        ProvisioningStore::decodeBase64Field(
                            encodedSsid, credential.ssid,
                            sizeof(credential.ssid), decodedBytes, error,
                            sizeof(error));
        accepted = accepted && ProvisioningStore::decodeBase64Field(
                                   encodedPassword, credential.password,
                                   sizeof(credential.password), decodedBytes,
                                   error, sizeof(error));
        memset(encodedSsid, 0, sizeof(encodedSsid));
        memset(encodedPassword, 0, sizeof(encodedPassword));
        wipeCommand();
        const bool saved =
            accepted && ProvisioningStore::saveFallbackWifi(
                            credential, error, sizeof(error));
        memset(&credential, 0, sizeof(credential));
        networkService.disconnect();
        Serial.printf("WIFI FALLBACK SET COMPLETE ok=%s\n",
                      saved ? "true" : "false");
        return true;
    }

    wipeCommand();
    Serial.println("ERROR WIFI_FALLBACK reason=invalid-command");
    return true;
}

void handleSerialCommand(char *line) {
    if ((batteryWarningVisible || batteryProtectionRequested) &&
        strncmp(line,"INPUT ",6) && strcmp(line,"PING") && strcmp(line,"BATTERY STATUS") &&
        strcmp(line,"POWER STATUS") && strcmp(line,"DIAG STATUS") && strcmp(line,"DISPLAY CAPTURE")) {
        Serial.println("ERROR BATTERY_NOTICE_ACTIVE"); return;
    }
    if (strcmp(line, "SETTINGS OPEN") == 0) { openWifiSettings(); return; }
    if (WifiSetup::active() && strncmp(line, "INPUT ", 6) != 0 &&
        strcmp(line, "PING") != 0 && strcmp(line, "SYNC STATUS") != 0 &&
        strcmp(line, "DIAG") != 0 && strcmp(line, "WIFI STATUS") != 0 &&
        strcmp(line, "DISPLAY CAPTURE") != 0 && strncmp(line, "BATTERY PREVIEW ",16) != 0 &&
        strcmp(line, "SETTINGS CLOSE") != 0) {
        memset(line, 0, strlen(line));
        Serial.println("ERROR WIFI_SETTINGS_ACTIVE close-settings-first=true");
        return;
    }
    if (strcmp(line, "SETTINGS CLOSE") == 0) {
        if (WifiSetup::active()) { closeWifiSettings(); displayHome(false, "settings-close"); }
        return;
    }
    if (strcmp(line, "WIFI STATUS") == 0) {
        Serial.printf("WIFI STATUS active=%s state=%u networks=%u radio=%s\n",
            WifiSetup::active() ? "true" : "false", static_cast<unsigned>(WifiSetup::state()),
            WifiSetup::count(), WiFi.getMode() == WIFI_OFF ? "off" : "on");
        return;
    }
    while (*line == ' ') {
        ++line;
    }
    idleBeforeLastCommand = millis() - lastActivityAt;
    lastActivityAt = millis();

    if (strcmp(line, "SYNC NOW") == 0) {
        const bool ok = AutomaticSync::request(provisioningActive || bookUpload.active() ||
                                              BookPreparation::busy() || pendingPowerSleep);
        Serial.printf("SYNC REQUEST accepted=%s reason=%s\n", ok ? "true" : "false",
                        ok ? "none" : AutomaticSync::busy() ? "sync-busy"
                        : ReaderSyncPolicy::errorCode(AutomaticSync::error()));
        if (ok) {
            busyFrame = 0; busyPainted = false; busyNextFrameAt = 0;
            if (uiScreen == UiScreen::Home) displayHome(false, "manual-sync");
        }
        return;
    }
    if (strcmp(line, "SYNC CANCEL") == 0) {
        AutomaticSync::cancel();
        return;
    }
    // A preparation or import owns SD. Only non-mutating status and queued
    // input (including POWER/cancel) remain available until it releases SD.
    if ((BookPreparation::busy() || AutomaticSync::busy()) &&
        strcmp(line, "PING") != 0 && strcmp(line, "SYNC STATUS") != 0 &&
        strcmp(line, "SYNC PAUSE") != 0 && strncmp(line, "INPUT ", 6) != 0) {
        Serial.println("ERROR STORAGE_BUSY retry-after-completion=true");
        return;
    }

    if (strcmp(line, "SYNC PAUSE") == 0) {
        AutomaticSync::setPaused(true);
        Serial.printf("SYNC PAUSED busy=%s\n", AutomaticSync::busy() ? "true" : "false");
        return;
    }
    if (strcmp(line, "SYNC RESUME") == 0) {
        AutomaticSync::setPaused(false);
        AutomaticSync::start();
        Serial.println("SYNC RESUMED");
        return;
    }
    if (strcmp(line, "SYNC STATUS") == 0) {
        const auto progress = AutomaticSync::progress();
        Serial.printf("SYNC STATUS device=%s busy=%s paused=%s status=%u reason=%s http=%d stage=%u done=%lu total=%lu percent=%d elapsed_s=%lu\n",
                      AutomaticSync::deviceId(), AutomaticSync::busy() ? "true" : "false",
                      AutomaticSync::isPaused() ? "true" : "false", static_cast<unsigned>(AutomaticSync::status()),
                      ReaderSyncPolicy::errorCode(AutomaticSync::error()), AutomaticSync::httpCode(),
                      unsigned(progress.stage),static_cast<unsigned long>(progress.done),static_cast<unsigned long>(progress.total),
                      SyncProgress::percent(progress),static_cast<unsigned long>((millis()-progress.startedAt)/1000));
        return;
    }
    // Operator-only storage/radio operations cannot race a background import.
    // Pause persists through multi-command provisioning and binary USB uploads.
    const char *exclusivePrefixes[] = {"PROVISION ", "WIFI ", "NETWORK ",
        "OPDS ", "BOOK ", "UPLOAD ", "CACHE ", "COVER ", "SD ",
        "STORAGE ", "DOWNLOAD ", "BATTERY LOAD"};
    for (const char *prefix : exclusivePrefixes) {
        if (strncmp(line, prefix, strlen(prefix)) == 0 && !ReaderSyncPolicy::isReadOnlyStatus(line)) {
            AutomaticSync::setPaused(true);
            if (AutomaticSync::busy()) {
                Serial.println("ERROR SYNC_BUSY retry-after-current-transfer=true");
                return;
            }
            Serial.println("SYNC PAUSED source=usb resume=SYNC_RESUME");
            break;
        }
    }

    if (handleProvisioningCommand(line) || handleFallbackWifiCommand(line)) {
        return;
    }

    if (strcmp(line, "STORAGE RECOVERY STATUS") == 0) {
        printStorageRecoveryStatus("usb-status");
        return;
    }

    if (strcmp(line, "STORAGE RECOVERY RUN CONFIRM") == 0) {
        storageRecoveryRan = true;
        const bool recovered =
            diagnostics.sdMounted &&
            StorageRecovery::run(storageRecoveryReport);
        localLibrarySession.loaded = false;
        homeSession.loaded = false;
        downloadQueueLoaded = false;
        printStorageRecoveryStatus("usb-run");
        Serial.printf("STORAGE RECOVERY COMPLETE ok=%s\n",
                      recovered ? "true" : "false");
        return;
    }

    if (strcmp(line, "STORAGE RECOVERY TEST ARM CONFIRM") == 0) {
        const bool armed = diagnostics.sdMounted &&
                           StorageRecovery::armSelfTest();
        localLibrarySession.loaded = false;
        homeSession.loaded = false;
        Serial.printf(
            "STORAGE RECOVERY TEST armed=%s next=SYSTEM_RESTART_CONFIRM\n",
            armed ? "true" : "false");
        return;
    }

    if (strcmp(line, "STORAGE RECOVERY TEST VERIFY") == 0) {
        StorageRecoverySelfTestResult result{};
        const bool verified = diagnostics.sdMounted &&
                              StorageRecovery::verifySelfTest(result);
        localLibrarySession.loaded = false;
        homeSession.loaded = false;
        Serial.printf(
            "STORAGE RECOVERY TEST verified=%s book=%s state=%s "
            "published=%s leftovers_absent=%s cleanup=%s\n",
            verified ? "true" : "false",
            result.recoveredBook ? "true" : "false",
            result.recoveredState ? "true" : "false",
            result.preservedPublishedMetadata ? "true" : "false",
            result.leftoversAbsent ? "true" : "false",
            result.cleanupPassed ? "true" : "false");
        return;
    }

    if (strcmp(line, "STORAGE MALFORMED TEST CONFIRM") == 0) {
        MalformedBookTestResult result{};
        const bool passed = diagnostics.sdMounted &&
                            runMalformedBookTest(result);
        localLibrarySession.loaded = false;
        homeSession.loaded = false;
        Serial.printf(
            "STORAGE MALFORMED TEST ok=%s rejected=%s original=%s "
            "partials_absent=%s cleanup=%s parser_error=%s\n",
            passed ? "true" : "false",
            result.parserRejected ? "true" : "false",
            result.originalPreserved ? "true" : "false",
            result.partialsAbsent ? "true" : "false",
            result.cleanupPassed ? "true" : "false",
            result.parserError[0] == '\0' ? "unknown" : result.parserError);
        return;
    }

    if (strncmp(line, "BOOK PUT ", 9) == 0) {
        char bookId[33]{};
        char sha256[65]{};
        unsigned long expectedBytes = 0;
        char trailing = '\0';
        const int fields = sscanf(line + 9, "%32s %lu %64s %c", bookId,
                                  &expectedBytes, sha256, &trailing);
        if (fields != 3) {
            Serial.println("ERROR BOOK_PUT reason=invalid-command");
            return;
        }
        if (!diagnostics.sdMounted) {
            Serial.println("ERROR BOOK_PUT reason=sd-unavailable");
            return;
        }
        bookUpload.start(bookId, static_cast<uint32_t>(expectedBytes), sha256,
                         Serial);
        return;
    }

    if (strcmp(line, "BOOK LIST") == 0) {
        File root = SD.open("/books", FILE_READ);
        unsigned count = 0;
        while (root) {
            File item = root.openNextFile(FILE_READ);
            if (!item) break;
            char id[33]{};
            const char *name = item.name();
            const char *base = strrchr(name, '/');
            const bool valid = BookUploadReceiver::validBookId(base ? base + 1 : name);
            if (valid) snprintf(id, sizeof(id), "%s", base ? base + 1 : name);
            const bool directory = item.isDirectory();
            item.close();
            if (directory && valid) {
                Serial.printf("BOOK ITEM id=%s\n", id);
                ++count;
            }
        }
        root.close();
        Serial.printf("BOOK LIST COMPLETE count=%u\n", count);
        return;
    }
    if (strncmp(line, "BOOK REMOVE ", 12) == 0) {
        char id[33]{}, confirmation[16]{}, extra = '\0';
        if (sscanf(line + 12, "%32s %15s %c", id, confirmation, &extra) != 2 ||
            strcmp(confirmation, "CONFIRM") || !BookUploadReceiver::validBookId(id)) {
            Serial.println("ERROR BOOK_REMOVE reason=invalid-confirmed-command");
            return;
        }
        const bool ok = BookUploadReceiver::archiveBook(id);
        if (ok) {
            if (strcmp(readerSession.bookId, id) == 0) readerSession.active = false;
            homeSession.loaded = false;
            localLibrarySession.loaded = false;
        }
        Serial.printf("BOOK REMOVE COMPLETE id=%s ok=%s recoverable=trash\n", id, ok ? "true" : "false");
        return;
    }
    if (strncmp(line, "BOOK STATUS ", 12) == 0) {
        const char *bookId = line + 12;
        char path[96]{};
        char partialPath[96]{};
        char oldPath[96]{};
        if (!BookUploadReceiver::bookPath(bookId, path, sizeof(path))) {
            Serial.println("ERROR BOOK_STATUS reason=invalid-book-id");
            return;
        }
        BookUploadReceiver::partialBookPath(bookId, partialPath,
                                            sizeof(partialPath));
        snprintf(oldPath, sizeof(oldPath), "/books/%s/book.fb2.old", bookId);
        File book = SD.open(path, FILE_READ);
        if (!book) {
            Serial.printf(
                "BOOK STATUS id=%s present=false part=%s old=%s\n", bookId,
                SD.exists(partialPath) ? "true" : "false",
                SD.exists(oldPath) ? "true" : "false");
            return;
        }
        Serial.printf(
            "BOOK STATUS id=%s present=true bytes=%lu part=%s old=%s "
            "path=%s\n",
            bookId, static_cast<unsigned long>(book.size()),
            SD.exists(partialPath) ? "true" : "false",
            SD.exists(oldPath) ? "true" : "false", path);
        book.close();
        return;
    }

    if (strncmp(line, "BOOK CACHE CLEAR ", 17) == 0) {
        char bookId[33]{};
        char confirmation[16]{};
        char trailing = '\0';
        const int fields = sscanf(line + 17, "%32s %15s %c", bookId,
                                  confirmation, &trailing);
        if (fields != 2 || strcmp(confirmation, "CONFIRM") != 0 ||
            !BookUploadReceiver::validBookId(bookId)) {
            Serial.println(
                "ERROR BOOK_CACHE_CLEAR reason=invalid-confirmed-command");
            return;
        }
        const bool cleared = BookUploadReceiver::invalidateDerivedFiles(bookId);
        localLibrarySession.loaded = false;
        homeSession.loaded = false;
        if (readerSession.active &&
            strcmp(readerSession.bookId, bookId) == 0) {
            readerSession = ReaderSession{};
            uiScreen = UiScreen::None;
        }
        Serial.printf("BOOK CACHE CLEAR COMPLETE id=%s ok=%s original=kept\n",
                      bookId, cleared ? "true" : "false");
        return;
    }

    if (strncmp(line, "BOOK PREPARE ", 13) == 0) {
        const char *bookId = line + 13;
        if (!BookUploadReceiver::validBookId(bookId)) {
            Serial.println("ERROR BOOK_PREPARE reason=invalid-book-id");
            return;
        }
        Serial.printf("OK BOOK PREPARE id=%s\n", bookId);
        Serial.flush();
        const uint32_t startedAt = millis();
        BookPreparationResult &preparation = bookPreparationWorkspace;
        if (!prepareBookArtifacts(bookId, "usb", preparation)) {
            Serial.printf("ERROR BOOK_PREPARE reason=%s id=%s\n",
                          preparation.error, bookId);
            return;
        }
        localLibrarySession.loaded = false;
        Serial.printf(
            "BOOK PREPARE COMPLETE id=%s cache_reused=%s "
            "pagination_reused=%s duration_ms=%lu pages=%lu chapters=%lu "
            "title=%s author=%s\n",
            bookId, preparation.cacheReused ? "true" : "false",
            preparation.paginationReused ? "true" : "false",
            static_cast<unsigned long>(millis() - startedAt),
            static_cast<unsigned long>(preparation.pagination.pageCount),
            static_cast<unsigned long>(preparation.pagination.chapterCount),
            preparation.cache.title, preparation.cache.author);
        return;
    }

    if (strcmp(line, "DOWNLOAD STATUS") == 0) {
        if (!ensureDownloadQueueLoaded()) {
            Serial.println("DOWNLOAD STATUS loaded=false radio=off");
        } else if (downloadQueue.count == 0) {
            Serial.println(
                "DOWNLOAD STATUS loaded=true jobs=0 active=false radio=off");
        } else {
            const BookDownloadJob &job = downloadQueue.jobs[0];
            Serial.printf(
                "DOWNLOAD STATUS loaded=true jobs=%lu active=%s id=%s "
                "attempts=%u reason=%s radio=off\n",
                static_cast<unsigned long>(downloadQueue.count),
                uiScreen == UiScreen::Download ? "true" : "false",
                job.bookId, static_cast<unsigned>(job.attempts),
                job.lastError[0] == '\0' ? "pending" : job.lastError);
        }
        return;
    }

    if (strcmp(line, "DOWNLOAD RETRY") == 0) {
        if (!ensureDownloadQueueLoaded() || downloadQueue.count == 0) {
            Serial.println("ERROR DOWNLOAD_RETRY reason=no-queued-job");
            return;
        }
        activeDownloadJob = downloadQueue.jobs[0];
        executeDownloadJob(activeDownloadJob, "usb-retry");
        return;
    }

    if (strcmp(line, "DOWNLOAD CANCEL CONFIRM") == 0) {
        if (!ensureDownloadQueueLoaded() || downloadQueue.count == 0) {
            Serial.println("DOWNLOAD CANCEL COMPLETE removed=false jobs=0");
            return;
        }
        char bookId[33]{};
        snprintf(bookId, sizeof(bookId), "%s", downloadQueue.jobs[0].bookId);
        const bool removed = DownloadQueue::remove(bookId, downloadQueue);
        Serial.printf("DOWNLOAD CANCEL COMPLETE removed=%s id=%s jobs=%lu\n",
                      removed ? "true" : "false", bookId,
                      static_cast<unsigned long>(downloadQueue.count));
        return;
    }

    if (strcmp(line, "HOME OPEN") == 0 ||
        strcmp(line, "HOME REFRESH") == 0) {
        pendingTopLevelTarget = TopLevelTab::Home;
        pendingTopLevelOpen = true;
        return;
    }

    if (strcmp(line, "HOME STATUS") == 0) {
        if (!homeSession.loaded) {
            Serial.println("HOME STATUS loaded=false");
        } else {
            const auto action = HomeLayout::action(homeSession.selected, homeSession.count, homeSession.addedCount);
            const LocalBookEntry *book = nullptr;
            if (action == HomeLayout::Action::Continue || action == HomeLayout::Action::ReadingCard)
                book = &homeSession.entries[homeSession.selected - HomeLayout::kFirstBook];
            else if (action == HomeLayout::Action::AddedCard)
                book = &homeSession.added[homeSession.selected - HomeLayout::kFirstBook - homeSession.count];
            for (size_t i = 0; i < homeSession.addedCount; ++i)
                Serial.printf("HOME ADDED slot=%u id=%s added_at=%lu\n", static_cast<unsigned>(i + 1),
                    homeSession.added[i].id, static_cast<unsigned long>(homeSession.added[i].addedAt));
            Serial.printf("HOME STATUS loaded=true active=%s books=%lu recent=%lu added=%lu selected=%lu action=%s id=%s\n",
                uiScreen == UiScreen::Home ? "true" : "false",
                static_cast<unsigned long>(homeSession.localBookCount),
                static_cast<unsigned long>(homeSession.count),
                static_cast<unsigned long>(homeSession.addedCount),
                static_cast<unsigned long>(homeSession.selected + 1), HomeLayout::actionName(action),
                book ? book->id : "-");
        }
        return;
    }

    if (strcmp(line, "TAB STATUS") == 0) {
        Serial.printf("TAB STATUS active=%s screen=%u\n",
                      topLevelTabCode(activeTopLevelTab),
                      static_cast<unsigned>(uiScreen));
        return;
    }

    if (strcmp(line, "CATALOG OPEN") == 0) {
        pendingTopLevelTarget = TopLevelTab::Catalog;
        pendingTopLevelOpen = true;
        return;
    }

    if (strcmp(line, "CATALOG REFRESH") == 0) {
        pendingCatalogRefresh = true;
        return;
    }

    if (strcmp(line, "CATALOG STATUS") == 0) {
        if (!catalogSession.loaded || catalogSession.feed == nullptr) {
            Serial.println("CATALOG STATUS loaded=false radio=off");
        } else {
            Serial.printf(
                "CATALOG STATUS loaded=true active=%s owner=%s entries=%lu "
                "rows=%lu selected=%lu history=%lu next=%s previous=%s "
                "truncated=%s radio=off\n",
                uiScreen == UiScreen::Catalog ? "true" : "false",
                topLevelTabCode(catalogSession.owner),
                static_cast<unsigned long>(catalogSession.feed->entryCount),
                static_cast<unsigned long>(catalogRowCount()),
                static_cast<unsigned long>(catalogRowCount() == 0
                                               ? 0
                                               : catalogSession.selected + 1),
                static_cast<unsigned long>(catalogSession.historyDepth),
                catalogSession.feed->nextHref[0] == '\0' ? "false" : "true",
                catalogSession.feed->previousHref[0] == '\0' ? "false"
                                                              : "true",
                catalogSession.feed->truncated ? "true" : "false");
        }
        return;
    }

    if (strcmp(line, "SEARCH OPEN") == 0) {
        pendingTopLevelTarget = TopLevelTab::Search;
        pendingTopLevelOpen = true;
        return;
    }

    if (strcmp(line, "SEARCH STATUS") == 0) {
        Serial.printf(
            "SEARCH STATUS active=%s phase=%u scope=%s selected=%lu/%lu "
            "prefix=%s radio=off\n",
            uiScreen == UiScreen::Search ? "true" : "false",
            static_cast<unsigned>(searchSession.phase),
            searchScopeLabel(searchSession.scope),
            static_cast<unsigned long>(searchRowCount() == 0
                                           ? 0
                                           : searchSession.selected + 1),
            static_cast<unsigned long>(searchRowCount()),
            searchSession.prefix[0] == '\0' ? "none" : searchSession.prefix);
        return;
    }

    if (strcmp(line, "LIBRARY OPEN") == 0 ||
        strcmp(line, "LIBRARY REFRESH") == 0) {
        pendingTopLevelTarget = TopLevelTab::OnDevice;
        pendingTopLevelOpen = true;
        return;
    }

    if (strcmp(line, "LIBRARY STATUS") == 0) {
        if (!localLibrarySession.loaded) {
            Serial.println("LIBRARY STATUS loaded=false");
        } else {
            const size_t rows = localLibraryRowCount();
            const LocalBookEntry *selected =
                localVisibleEntry(localLibrarySession.selected);
            Serial.printf(
                "LIBRARY STATUS loaded=true active=%s owner=%s phase=%u "
                "section=%u books=%lu rows=%lu selected=%lu id=%s title=%s\n",
                uiScreen == UiScreen::LocalLibrary ? "true" : "false",
                topLevelTabCode(localLibrarySession.owner),
                static_cast<unsigned>(localLibrarySession.phase),
                static_cast<unsigned>(localLibrarySession.section),
                static_cast<unsigned long>(
                    localLibrarySession.info.loadedCount),
                static_cast<unsigned long>(rows),
                static_cast<unsigned long>(
                    rows == 0 ? 0 : localLibrarySession.selected + 1),
                selected == nullptr ? "none" : selected->id,
                selected == nullptr ? "none" : selected->title);
        }
        return;
    }

    if (strncmp(line, "READER OPEN ", 12) == 0) {
        const char *bookId = line + 12;
        if (!BookUploadReceiver::validBookId(bookId)) {
            Serial.println("ERROR READER_OPEN reason=invalid-book-id");
            return;
        }
        snprintf(pendingUsbReaderOpenId, sizeof(pendingUsbReaderOpenId),
                 "%s", bookId);
        return;
    }

    for (char *cursor = line; *cursor != '\0'; ++cursor) {
        if (*cursor >= 'a' && *cursor <= 'z') {
            *cursor = static_cast<char>(*cursor - ('a' - 'A'));
        }
    }

    if (!strcmp(line,"BATTERY PREVIEW WARNING")) {
        batteryWarningPending=true;Serial.println("BATTERY PREVIEW warning=queued");return;
    }
    if (!strcmp(line,"BATTERY PREVIEW CHARGE")) {
        // Save the current frame so OK restores it; preview never sleeps or writes NVS.
        if(!batteryUnderlay)batteryUnderlay=static_cast<uint8_t*>(heap_caps_malloc(kFramebufferBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(batteryUnderlay) {memcpy(batteryUnderlay,framebuffer,kFramebufferBytes);batteryWarningVisible=paintBatteryNotice(true);}
        Serial.println("BATTERY PREVIEW charge=true shutdown=false");return;
    }
    if (strcmp(line, "PING") == 0) {
        Serial.printf("PONG %s %s\n", kFirmwareName, kFirmwareVersion);
    } else if (strcmp(line, "STORAGE STATUS") == 0) {
        const uint64_t total = SD.totalBytes(), used = SD.usedBytes();
        Serial.printf("STORAGE STATUS mounted=%s total=%llu used=%llu free=%llu internal_free=%lu\n",
            diagnostics.sdMounted ? "true" : "false", total, used, total >= used ? total - used : 0,
            static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    } else if (strcmp(line, "DIAG STATUS") == 0) {
        printDiagnostics();
    } else if (strcmp(line, "NETWORK STATUS") == 0) {
        const NetworkStatus network = networkService.status();
        Serial.printf(
            "NETWORK STATUS configured=%s connected=%s time_valid=%s "
            "rssi=%ld radio=%s reason=%s\n",
            network.configured ? "true" : "false",
            network.connected ? "true" : "false",
            network.timeValid ? "true" : "false",
            static_cast<long>(network.rssi),
            network.connected ? "on" : "off",
            network.error[0] == '\0' ? "none" : network.error);
    } else if (strcmp(line, "NETWORK CONNECT FALLBACK") == 0) {
        networkService.disconnect();
        Serial.println("OK NETWORK CONNECT source=fallback credentials=stored");
        Serial.flush();
        NetworkStatus network{};
        const bool connected = networkService.connect(network, 20000, 2);
        Serial.printf(
            "NETWORK CONNECT COMPLETE ok=%s source=fallback time_valid=%s "
            "rssi=%ld duration_ms=%lu reason=%s\n",
            connected ? "true" : "false",
            network.timeValid ? "true" : "false",
            static_cast<long>(network.rssi),
            static_cast<unsigned long>(network.connectDurationMs),
            network.error[0] == '\0' ? "none" : network.error);
    } else if (strcmp(line, "NETWORK CONNECT") == 0) {
        Serial.println("OK NETWORK CONNECT credentials=stored");
        Serial.flush();
        NetworkStatus network{};
        const bool connected = networkService.connect(network);
        Serial.printf(
            "NETWORK CONNECT COMPLETE ok=%s time_valid=%s rssi=%ld "
            "duration_ms=%lu reason=%s\n",
            connected ? "true" : "false",
            network.timeValid ? "true" : "false",
            static_cast<long>(network.rssi),
            static_cast<unsigned long>(network.connectDurationMs),
            network.error[0] == '\0' ? "none" : network.error);
    } else if (strcmp(line, "NETWORK DISCONNECT") == 0) {
        networkService.disconnect();
        Serial.println("NETWORK DISCONNECT COMPLETE radio=off");
    } else if (strcmp(line, "OPDS TEST") == 0) {
        Serial.println("OK OPDS TEST tls=required auth=stored radio=temporary");
        Serial.flush();
        const OpdsProbeResult probe = networkService.probeRoot();
        Serial.printf(
            "OPDS TEST COMPLETE ok=%s http=%d tls=%s tls_error=%d "
            "atom=%s entries=%lu "
            "bytes=%lu duration_ms=%lu radio=off reason=%s\n",
            probe.ok ? "true" : "false", probe.httpCode,
            probe.tlsVerified ? "verified" : "failed", probe.tlsError,
            probe.atomFeed ? "true" : "false",
            static_cast<unsigned long>(probe.entries),
            static_cast<unsigned long>(probe.responseBytes),
            static_cast<unsigned long>(probe.durationMs),
            probe.error[0] == '\0' ? "none" : probe.error);
    } else if (strcmp(line, "SYSTEM RESTART CONFIRM") == 0) {
        Serial.println("SYSTEM RESTARTING reason=usb-confirmed");
        Serial.flush();
        delay(100);
        ESP.restart();
    } else if (strcmp(line, "READER STATUS") == 0) {
        if (readerSession.active) {
            ReaderChapterEntry chapter{};
            const bool hasChapter = ReaderPagination::currentChapter(
                readerSession.bookId, readerSession.currentPage, chapter);
            Serial.printf(
                "READER STATUS active=true id=%s page=%lu/%lu "
                "chapter=%lu/%lu chapter_page=%lu\n",
                readerSession.bookId,
                static_cast<unsigned long>(readerSession.currentPage),
                static_cast<unsigned long>(readerSession.pageCount),
                static_cast<unsigned long>(hasChapter ? chapter.ordinal : 0),
                static_cast<unsigned long>(readerSession.chapterCount),
                static_cast<unsigned long>(hasChapter ? chapter.page : 0));
        } else {
            Serial.println("READER STATUS active=false");
        }
    } else if (strcmp(line, "READER CHAPTER STATUS") == 0) {
        ReaderChapterEntry chapter{};
        if (!readerSession.active) {
            Serial.println("ERROR READER_CHAPTER reason=no-active-book");
        } else {
            const bool hasCurrent = ReaderPagination::currentChapter(
                readerSession.bookId, readerSession.currentPage, chapter);
            if (hasCurrent) {
                Serial.printf(
                    "READER CHAPTER current=%lu/%lu page=%lu title=%s\n",
                    static_cast<unsigned long>(chapter.ordinal),
                    static_cast<unsigned long>(readerSession.chapterCount),
                    static_cast<unsigned long>(chapter.page), chapter.title);
            } else {
                ReaderChapterEntry firstChapter{};
                if (ReaderPagination::adjacentChapter(
                        readerSession.bookId, readerSession.currentPage, 1,
                        firstChapter)) {
                    Serial.printf(
                        "READER CHAPTER current=0/%lu page=%lu "
                        "title=Вступление next=%lu\n",
                        static_cast<unsigned long>(
                            readerSession.chapterCount),
                        static_cast<unsigned long>(
                            readerSession.currentPage),
                        static_cast<unsigned long>(firstChapter.page));
                } else {
                    Serial.println(
                        "ERROR READER_CHAPTER reason=index-read-failed");
                }
            }
        }
    } else if (strcmp(line, "READER CHAPTER NEXT") == 0) {
        pendingChapterDelta = 1;
    } else if (strcmp(line, "READER CHAPTER PREVIOUS") == 0 ||
               strcmp(line, "READER CHAPTER PREV") == 0) {
        pendingChapterDelta = -1;
    } else if (strncmp(line, "READER PAGE ", 12) == 0) {
        char *end = nullptr;
        const unsigned long page = strtoul(line + 12, &end, 10);
        if (end == line + 12 || *end != '\0') {
            Serial.println("ERROR READER_PAGE reason=invalid-command");
        } else {
            pendingUsbReaderPageNumber = static_cast<uint32_t>(page);
            pendingUsbReaderPage = true;
        }
    } else if (strcmp(line, "DIAG REFRESH") == 0) {
        Serial.println("ERROR DISPLAY_QUARANTINED command=DIAG_REFRESH");
    } else if (strcmp(line, "DISPLAY CLEAN") == 0) {
        pendingPanelClean = true;
        Serial.println(
            "OK PANEL CLEAN queued=true action=hard-clear-restore");
    } else if (strcmp(line, "DISPLAY REPAIR WHITE") == 0 ||
               strcmp(line, "DISPLAY RECOVER") == 0) {
        Serial.println("OK DISPLAY REPAIR WHITE mode=panel-repair-white "
                       "expected_seconds=30");
        Serial.flush();
        lastDisplayRefresh = displayRefresh.repairToWhite();
        hasDisplayRefresh = true;
        printDisplayRefresh("REFRESH", lastDisplayRefresh);
    } else if (strcmp(line, "DISPLAY CALIBRATE A") == 0) {
        if (!buildDisplayCalibrationFrameA()) {
            Serial.println("ERROR DISPLAY_CALIBRATION_BUILD_FAILED");
        } else {
            Serial.println("OK DISPLAY CALIBRATE A "
                           "mode=calibration-grayscale one_shot=true "
                           "orientation=portrait-540x960");
            Serial.flush();
            lastDisplayRefresh =
                displayRefresh.drawCalibrationOnWhite(framebuffer);
            hasDisplayRefresh = true;
            printDisplayRefresh("REFRESH", lastDisplayRefresh);
        }
    } else if (strcmp(line, "DISPLAY TRANSITION B") == 0) {
        if (!buildDisplayCalibrationFrameB()) {
            Serial.println("ERROR DISPLAY_TRANSITION_BUILD_FAILED");
        } else {
            Serial.println("OK DISPLAY TRANSITION B "
                           "mode=calibration-transition-full one_shot=true "
                           "orientation=portrait-540x960");
            Serial.flush();
            lastDisplayRefresh = displayRefresh.drawFullTransition(framebuffer);
            hasDisplayRefresh = true;
            printDisplayRefresh("REFRESH", lastDisplayRefresh);
        }
    } else if (strcmp(line, "DISPLAY FULL STRESS 6") == 0) {
        if (fullStressCompleted) {
            Serial.println("ERROR DISPLAY_STRESS_ALREADY_COMPLETED");
        } else {
            Serial.println("OK DISPLAY FULL STRESS 6 "
                           "mode=quality-full sequence=A-B-A-B-A-B");
            Serial.flush();
            bool allPassed = true;
            for (uint8_t pass = 0; pass < 6; ++pass) {
                const bool frameBuilt = (pass & 1) == 0
                                            ? buildDisplayCalibrationFrameA()
                                            : buildDisplayCalibrationFrameB();
                if (!frameBuilt) {
                    Serial.printf("ERROR DISPLAY_STRESS_BUILD pass=%u\n",
                                  static_cast<unsigned>(pass + 1));
                    allPassed = false;
                    break;
                }
                lastDisplayRefresh = displayRefresh.refresh(
                    framebuffer, DisplayRefreshMode::QualityFull);
                hasDisplayRefresh = true;
                printDisplayRefresh("STRESS", lastDisplayRefresh);
                if (!lastDisplayRefresh.ok) {
                    allPassed = false;
                    break;
                }
                delay(100);
            }
            fullStressCompleted = allPassed;
            Serial.printf("DISPLAY STRESS COMPLETE ok=%s final=B\n",
                          allPassed ? "true" : "false");
        }
    } else if (strcmp(line, "DISPLAY POLICY") == 0) {
        Serial.printf("DISPLAY POLICY dashboard=quarantined "
                      "auto_render=false region=disabled "
                      "repair=lilygo-screen-repair calibration=one-shot "
                      "transition=clear-plus-full-one-shot "
                      "normal=clear-plus-full-grayscale "
                      "orientation=portrait-540x960 debt=%lu\n",
                      static_cast<unsigned long>(
                          displayRefresh.regionalRefreshesSinceFull()));
    } else if (strcmp(line, "DISPLAY STATUS") == 0) {
        if (hasDisplayRefresh) {
            printDisplayRefresh("STATUS", lastDisplayRefresh);
        } else {
            Serial.println("DISPLAY STATUS unavailable=true");
        }
    } else if (strcmp(line, "DISPLAY CAPTURE") == 0) {
        if (!framebuffer || !displayRefresh.flush()) {
            Serial.println("ERROR DISPLAY_CAPTURE reason=not-ready");
            return;
        }
        Serial.printf("DISPLAY CAPTURE BEGIN bytes=%u\n", static_cast<unsigned>(kFramebufferBytes));
        Serial.write(framebuffer, kFramebufferBytes);
        Serial.flush();
        Serial.println("\nDISPLAY CAPTURE COMPLETE");
    } else if (strcmp(line, "DISPLAY SNAPSHOT STATUS") == 0) {
        printFramebufferSnapshot();
    } else if (strcmp(line, "BATTERY STATUS") == 0) {
        networkService.disconnect();
        delay(100);
        measureBattery();
        printBatteryStatus("radio-off");
    } else if (strcmp(line, "BATTERY LOAD TEST") == 0) {
        networkService.disconnect();
        delay(150);
        measureBattery();
        const uint16_t idleMillivolts = diagnostics.batteryMillivolts;
        const uint16_t idleRaw = diagnostics.batteryRaw;
        NetworkStatus network{};
        const bool connected = networkService.connect(network);
        measureBattery();
        const uint16_t radioMillivolts = diagnostics.batteryMillivolts;
        const uint16_t radioRaw = diagnostics.batteryRaw;
        networkService.disconnect();
        Serial.printf(
            "BATTERY LOAD TEST connected=%s idle_mv=%u idle_raw=%u "
            "radio_mv=%u radio_raw=%u delta_mv=%ld rssi=%ld radio=off\n",
            connected ? "true" : "false", idleMillivolts, idleRaw,
            radioMillivolts, radioRaw,
            static_cast<long>(radioMillivolts) -
                static_cast<long>(idleMillivolts),
            static_cast<long>(network.rssi));
    } else if (strcmp(line, "POWER STATUS") == 0) {
        printPowerStatus();
    } else if (strcmp(line, "POWER SLEEP CONFIRM") == 0 ||
               strcmp(line, "DIAG SLEEP") == 0) {
        enterDeepSleep("usb-confirmed");
    } else if (strcmp(line, "POWER TEST SLEEP 3") == 0) {
        enterDeepSleep("usb-test", 3);
    } else if (strcmp(line, "INPUT UP SHORT") == 0) {
        emitInput("UP", "SHORT", "usb");
    } else if (strcmp(line, "INPUT UP LONG") == 0) {
        emitInput("UP", "LONG", "usb");
    } else if (strcmp(line, "INPUT DOWN SHORT") == 0) {
        emitInput("DOWN", "SHORT", "usb");
    } else if (strcmp(line, "INPUT DOWN LONG") == 0) {
        emitInput("DOWN", "LONG", "usb");
    } else if (strcmp(line, "INPUT CENTER SHORT") == 0) {
        emitInput("CENTER", "SHORT", "usb");
    } else if (strcmp(line, "INPUT CENTER LONG") == 0) {
        emitInput("CENTER", "LONG", "usb");
    } else if (strcmp(line, "INPUT CENTER DOUBLE") == 0) {
        emitInput("CENTER", "DOUBLE", "usb");
    } else if (strcmp(line, "INPUT POWER SHORT") == 0) {
        emitInput("POWER", "SHORT", "usb");
    } else if (*line != '\0') {
        // Never reflect arbitrary serial input: a mistyped provisioning line
        // may contain a reversible credential payload.
        Serial.println("ERROR UNKNOWN_COMMAND");
    }
}

void pollSerial() {
    if (bookUpload.active()) {
        bookUpload.poll(Serial, Serial);
        return;
    }

    while (Serial.available() > 0) {
        const char value = static_cast<char>(Serial.read());
        if (value == '\r') {
            continue;
        }
        if (value == '\n') {
            serialLine[serialLength] = '\0';
            handleSerialCommand(serialLine);
            serialLength = 0;
            serialLine[0] = '\0';
            if (bookUpload.active()) {
                return;
            }
            continue;
        }
        if (serialLength < sizeof(serialLine) - 1) {
            serialLine[serialLength++] = value;
        } else {
            serialLength = 0;
            serialLine[0] = '\0';
            Serial.println("ERROR COMMAND_TOO_LONG");
        }
    }
}

void beginButton(ButtonTracker &button) {
    if (&button == &powerButton) rtc_gpio_deinit(static_cast<gpio_num_t>(button.pin));
    pinMode(button.pin, INPUT_PULLUP);
    button.rawLevel = digitalRead(button.pin);
    button.stableLevel = button.rawLevel;
    button.rawChangedAt = millis();
}

void pollButton(ButtonTracker &button) {
    const uint32_t now = millis();
    const bool currentLevel = digitalRead(button.pin);

    if (&button == &powerButton && suppressPowerUntilRelease) {
        if (currentLevel != button.rawLevel) {
            button.rawLevel = currentLevel;
            button.rawChangedAt = now;
        }
        if (button.rawLevel == HIGH &&
            now - button.rawChangedAt >= kDebounceMs) {
            button.stableLevel = HIGH;
            button.clickPending = false;
            suppressPowerUntilRelease = false;
            lastActivityAt = now;
            Serial.println("POWER WAKE BUTTON release_suppressed=true");
        }
        return;
    }

    if (currentLevel != button.rawLevel) {
        button.rawLevel = currentLevel;
        button.rawChangedAt = now;
    }

    if (button.stableLevel != button.rawLevel &&
        now - button.rawChangedAt >= kDebounceMs) {
        button.stableLevel = button.rawLevel;
        if (&button == &centerButton) {
            // Handled below by OkGesture on every poll, including while held.
        } else if (&button == &powerButton && sleepSwitchLatching.load()) {
            // A latching switch changes state once; it has no click duration.
            emitInput("POWER", "LONG", "gpio-switch");
        } else if (button.stableLevel == LOW) {
            button.pressedAt = now;
            if (&button == &upButton || &button == &downButton) {
                emitInput(button.name, "SHORT", "gpio");
                button.repeat.pressed(now);
            }
        } else {
            const uint32_t duration = now - button.pressedAt;
            if (&button == &upButton || &button == &downButton) {
                // UP/DOWN act on press. Holding repeats; release never adds a step.
                button.clickPending = false;
            } else if (&button == &cleanButton) {
                // Dedicated board button: short presses do not navigate.
                button.clickPending = false;
                if (duration >= kPanelCleanHoldMs) {
                    emitInput("CLEAN", "LONG", "gpio");
                    Serial.printf(
                        "EVENT CLEAN source=gpio pin=21 hold_ms=%lu queued=true\n",
                        static_cast<unsigned long>(duration));
                }
            } else if (duration >= kLongPressMs) {
                button.clickPending = false;
                emitInput(button.name, "LONG", "gpio");
            } else if (button.allowDouble) {
                if (button.clickPending &&
                    now - button.clickReleasedAt <= kDoublePressMs) {
                    button.clickPending = false;
                    emitInput(button.name, "DOUBLE", "gpio");
                } else {
                    button.clickPending = true;
                    button.clickReleasedAt = now;
                }
            } else {
                emitInput(button.name, "SHORT", "gpio");
            }
        }
    }

    if (&button == &centerButton) {
        const auto event = button.okGesture.update(button.stableLevel == LOW, now);
        if (event != OkGesture::Event::None)
            emitInput("CENTER", event == OkGesture::Event::Short ? "SHORT" :
                event == OkGesture::Event::Double ? "DOUBLE" : "LONG", "gpio");
        return;
    }
    if ((&button == &upButton || &button == &downButton) &&
        button.stableLevel == LOW && button.rawLevel == LOW &&
        button.repeat.held(now)) {
        emitInput(button.name, "SHORT", "gpio");
    }
    if (button.clickPending && now - button.clickReleasedAt > kDoublePressMs) {
        button.clickPending = false;
        emitInput(button.name, "SHORT", "gpio");
    }
}

void pollPhysicalInputs(void *) {
    for (;;) {
        pollButton(upButton);
        pollButton(downButton);
        pollButton(centerButton);
        pollButton(powerButton);
        pollButton(cleanButton);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

}  // namespace

void setup() {
    uiTaskHandle = xTaskGetCurrentTaskHandle();
    ++bootCount;
    diagnostics.resetReason = esp_reset_reason();
    Serial.begin(115200);
    const uint32_t serialStart = millis();
    while (!Serial && millis() - serialStart < 1500) {
        delay(10);
    }

    Serial.printf("BOOT %s %s board=Screen-4.7-S3-V2.4\n",
                  kFirmwareName, kFirmwareVersion);

    diagnostics.psramPassed = testPsram();
    if (diagnostics.psramPassed && ReaderTlsMemory::initialize()) {
        Serial.println("TLS MEMORY large_buffers=psram small_allocations=internal verified_tls=true");
    } else {
        Serial.println("WARNING TLS_MEMORY using-sdk-default=true");
    }
    localLibrarySession.entries = static_cast<LocalBookEntry *>(
        heap_caps_calloc(kLocalLibraryCapacity, sizeof(LocalBookEntry),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    localLibrarySession.groups = static_cast<char (*)[128]>(
        heap_caps_calloc(kLocalLibraryCapacity,
                         sizeof(localLibrarySession.groups[0]),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (localLibrarySession.entries == nullptr ||
        localLibrarySession.groups == nullptr) {
        Serial.println("FATAL LOCAL_LIBRARY_ALLOCATION_FAILED");
        return;
    }
    diagnostics.sdWritePassed = testSdAtomicWrite();
    storageRecoveryRan = diagnostics.sdMounted;
    if (storageRecoveryRan) {
        StorageRecovery::run(storageRecoveryReport);
    }
    if (diagnostics.sdMounted) {
        if (!ReaderSettingsStore::load(activeReaderSettings)) {
            Serial.printf("WARNING READING_SETTINGS_BOOT reason=%s\n",
                          activeReaderSettings.error);
            activeReaderSettings = ReaderSettings{};
            ReaderSettingsStore::apply(activeReaderSettings);
        }
        readingSettingsSession.settings = activeReaderSettings;
    }
    sleepSwitchLatching.store(activeReaderSettings.sleepLatching);
    diagnostics.rtcDetected = probeRtc();

    // The build places the large EPD buffer in PSRAM directly. Boot/wake must
    // never start Wi-Fi just to influence the display allocator.
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    displayInitialized = displayRefresh.begin();
    measureBattery();

    framebuffer = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (framebuffer == nullptr) {
        Serial.println("FATAL FRAMEBUFFER_ALLOCATION_FAILED");
        displayRefresh.shutdown();
        return;
    }

    beginButton(upButton);
    beginButton(downButton);
    beginButton(centerButton);
    beginButton(powerButton);
    beginButton(cleanButton);
    suppressPowerUntilRelease = !sleepSwitchLatching.load() &&
        esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 &&
        powerButton.stableLevel == LOW;
    lastActivityAt = millis();

    batteryResumeLoaded=readBatteryResume();
    const bool plausibleBattery=diagnostics.batteryMillivolts>=2500 && diagnostics.batteryMillivolts<=4500;
    if(plausibleBattery && ((batteryResumeLoaded && diagnostics.batteryPercent<=BatteryPolicy::ResumePercent) || diagnostics.batteryPercent<=BatteryPolicy::ProtectionPercent)) {
        // Confirm a cold-boot low reading before opening books or starting workers.
        measureBattery();measureBattery();
        if(diagnostics.batteryPercent<=BatteryPolicy::ProtectionPercent || (batteryResumeLoaded && diagnostics.batteryPercent<=BatteryPolicy::ResumePercent))
            enterBatteryProtection(batteryResumeLoaded);
    }
    printDiagnostics();
    printStorageRecoveryStatus("boot");
    ProvisioningConfig bootConfiguration{};
    char provisioningError[64]{};
    const bool provisioned = ProvisioningStore::load(
        bootConfiguration, provisioningError, sizeof(provisioningError));
    memset(&bootConfiguration, 0, sizeof(bootConfiguration));
    Serial.printf("PROVISION BOOT configured=%s schema=%u radio=off\n",
                  provisioned ? "true" : "false",
                  static_cast<unsigned>(kProvisioningSchemaVersion));
    const bool wakeStateRestored = restoreWakeState();
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED && !wakeStateRestored) {
        // The exact H716 backend is now stable enough to draw one intentional
        // frame on cold boot.  Do not clear in DisplayRefreshController::begin:
        // that used to leave a reset device white because setup then preserved
        // the freshly cleared panel.  A dedicated six-second CENTER hold is
        // the explicit ghost-removal path.
        const bool coldBootRendered =
            displayInitialized && framebuffer != nullptr &&
            displayHome(true, "cold-boot");
        Serial.printf("DISPLAY COLD_BOOT rendered=%s screen=%u\n",
                      coldBootRendered ? "true" : "false",
                      static_cast<unsigned>(uiScreen));
    } else {
        Serial.printf("DISPLAY WAKE restored=%s screen=%u\n",
                      wakeStateRestored ? "true" : "false",
                      static_cast<unsigned>(uiScreen));
    }
    if(batteryResumeLoaded && wakeStateRestored)clearBatteryResume();
    Serial.println("READY USB commands: PING | DIAG STATUS | "
                   "STORAGE RECOVERY STATUS|RUN|TEST | "
                   "STORAGE MALFORMED TEST | "
                   "BOOK PUT ... | BOOK STATUS ... | BOOK PREPARE ... | "
                   "BOOK CACHE CLEAR ... CONFIRM | "
                   "DOWNLOAD STATUS|RETRY|CANCEL CONFIRM | "
                    "HOME OPEN|REFRESH|STATUS | TAB STATUS | "
                   "CATALOG OPEN|REFRESH|STATUS | SEARCH OPEN|STATUS | "
                   "LIBRARY OPEN|REFRESH|STATUS | "
                   "READER OPEN ... | READER STATUS | READER PAGE ... | "
                   "READER CHAPTER STATUS|NEXT|PREVIOUS | "
                   "DISPLAY CLEAN | DISPLAY REPAIR WHITE | "
                   "DISPLAY CALIBRATE A | "
                   "DISPLAY TRANSITION B | "
                   "DISPLAY FULL STRESS 6 | "
                   "DISPLAY POLICY | DISPLAY STATUS | DISPLAY SNAPSHOT STATUS | "
                   "PROVISION BEGIN|FIELD|COMMIT|STATUS|ABORT | "
                   "NETWORK STATUS|CONNECT|DISCONNECT | OPDS TEST | "
                   "BATTERY STATUS|LOAD TEST | POWER STATUS|SLEEP CONFIRM | "
                   "POWER TEST SLEEP 3 | SYSTEM RESTART CONFIRM | "
                   "DIAG SLEEP | INPUT ...");
    physicalInputs = xQueueCreate(32, sizeof(QueuedInput));
    if (physicalInputs != nullptr) {
        xTaskCreatePinnedToCore(pollPhysicalInputs, "reader_keys", 4096, nullptr,
                                2, &inputTaskHandle, 1);
    }
    if (provisioned && diagnostics.sdMounted) AutomaticSync::start();
}

void loop() {
    pollBatteryTelemetry();
    pollSerial();
    if (inputTaskHandle != nullptr) {
        QueuedInput event{};
        // One semantic event per frame: selection and its following press
        // cannot be reordered by draining the entire queue before dispatch.
        if (xQueueReceive(physicalInputs, &event, 0) == pdTRUE) {
            Serial.printf("INPUT DISPATCH latency_ms=%lu\n",
                static_cast<unsigned long>(millis() - event.capturedAt));
            emitInput(event.button, event.gesture, "gpio");
        }
    } else {
        pollButton(upButton);
        pollButton(downButton);
        pollButton(centerButton);
        pollButton(powerButton);
        pollButton(cleanButton);
    }
    pollWifiSettings();
    if(!batteryProtectionRequested && showPendingBatteryWarning()) {delay(2);return;}
    bool prepared = false, cancelled = false;
    if (BookPreparation::takeResult(prepared, cancelled)) {
        snprintf(operationNotice, sizeof(operationNotice), "%s",
            prepared ? "" : cancelled ? I18n::tr("ПОДГОТОВКА ОТМЕНЕНА") : I18n::tr("НЕ УДАЛОСЬ ПОДГОТОВИТЬ КНИГУ"));
        if (preparingSettings) {
            preparingSettings = false;
            ReaderSettingsStore::apply(beforePreparationSettings);
            if (restoringSettings) {
                restoringSettings = false;
                if (!prepared) readerSession.active = false;
                displayReadingSettings(false, "settings-restored");
            } else if (prepared && !pendingPowerSleep) {
                completingSettings = true;
                applyOrderedReaderSetting();
                completingSettings = false;
            } else {
                // Cancellation may race the atomic publication of the new
                // layout. Restore the old cache before allowing old-layout
                // navigation or sleep, rather than using mismatched offsets.
                ReaderPaginationInfo previousPages{};
                if (!ReaderPagination::load(readerSession.bookId, previousPages) &&
                    BookPreparation::start(readerSession.bookId)) {
                    preparingSettings = true;
                    restoringSettings = true;
                } else displayReadingSettings(false, "preparation-cancelled");
            }
        } else {
            bookCardSession.preparing = false;
            if (prepared && !pendingPowerSleep) openReaderBook(preparingBookId, "worker-complete");
            else if (uiScreen == UiScreen::BookCard) displayBookCard("prepare-failed-or-cancelled");
            else if (uiScreen == UiScreen::Home) displayHome(false, "prepare-failed-or-cancelled");
        }
        preparingBookId[0] = '\0';
        if (!pendingPowerSleep && !BookPreparation::busy()) AutomaticSync::setPaused(false);
    }
    if(processBatteryProtection()) {delay(2);return;}
    if (BookPreparation::busy() || AutomaticSync::busy()) {
        // Worker exclusively owns SD parsing/writes; UI still handles cancel,
        // power and the loading indicator, without concurrent cache mutation.
        if (AutomaticSync::busy()) drawSyncProgress();
        else drawBusyIndicator();
        delay(2);
        return;
    }
    if (AutomaticSync::takeFinished()) {
        syncReturnCleanupPending = syncProgressVisible;
        syncProgressVisible = false;
        AutomaticSync::takeLibraryChanged();
        char currentPath[96]{};
        if (readerSession.active && BookUploadReceiver::bookPath(readerSession.bookId, currentPath, sizeof(currentPath)) &&
            !SD.exists(currentPath)) readerSession.active = false;
        homeSession.loaded = false;
        localLibrarySession.loaded = false;
        favoritesSession.loaded=false;
        if (uiScreen == UiScreen::Home) displayHome(true, "manual-sync-finished");
        if (uiScreen == UiScreen::LocalLibrary) displayLocalLibrary(true,"manual-sync-finished");
        else if (uiScreen != UiScreen::Home) displayTopLevelTab(activeTopLevelTab,true,"manual-sync-finished");
    }
    if (AutomaticSync::takeLibraryChanged()) {
        homeSession.loaded = false;
        localLibrarySession.loaded = false;
    }
    if (!AutomaticSync::busy() && !uiActionPending() && millis() - lastActivityAt > 2000) {
        if (uiScreen == UiScreen::Home && !homeSession.loaded) displayHome(true, "auto-download");
        if (uiScreen == UiScreen::LocalLibrary && !localLibrarySession.loaded) displayLocalLibrary(true, "auto-download");
    }
    processPanelCleanAction();
    processTopLevelActions();
    if (BookPreparation::busy() || AutomaticSync::busy()) { delay(2); return; }
    processBulkDownloadActions();
    processCatalogActions();
    processSearchActions();
    processDownloadActions();
    processBookCardActions();
    if (BookPreparation::busy()) { delay(2); return; }
    processAnnotationActions();
    processFavoritesActions();
    processLocalLibraryActions();
    processReaderMenuActions();
    processContentsActions();
    processReaderBookmarksActions();
    processReadingSettingsActions();
    processOrderedSelectorActions();
    if (BookPreparation::busy()) { delay(2); return; }
    // Cold pagination runs in the cancellable worker; idle prefetch below
    // only renders already paginated neighbouring pages and never uses Wi-Fi.
    processReaderNavigation();
    if (BookPreparation::busy()) { delay(2); return; }
    processReaderChapterNavigation();
    processPowerAction();
    processAutoSleep();

    if (renderPending && displayInitialized && framebuffer != nullptr) {
        renderDashboard(pendingRefresh);
    }

    prefetchReaderPage();

    delay(2);
}
