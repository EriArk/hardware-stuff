// Application-side adapter: the layout module does not perform filesystem I/O.
const GFXfont *bookishFont(BookishUI::Font font) {
    switch (font) {
        case BookishUI::Font::Small: return &BookishArimo10;
        case BookishUI::Font::Body: return &BookishArimo18;
        case BookishUI::Font::Control: return &BookishArimo22;
        case BookishUI::Font::Heading: return &BookishLora25;
        case BookishUI::Font::Title: return &BookishLora31;
        case BookishUI::Font::Hero: return &BookishLora39;
    }
    return &BookishArimo18;
}

struct HomeCoverSlot {
    char id[33]{};
    CoverBitmap bitmap{};
};
HomeCoverSlot homeCovers[HomeLayout::kReadingCapacity + HomeLayout::kAddedCapacity];
void invalidateHomeCovers() {
    for (auto &slot : homeCovers) {
        CoverCache::release(slot.bitmap);
        slot.id[0] = '\0';
    }
}
const CoverBitmap *homeCover(const char *id) {
    if (!id || !*id) return nullptr;
    for (auto &slot : homeCovers)
        if (strcmp(slot.id, id) == 0) return slot.bitmap.pixels ? &slot.bitmap : nullptr;
    // The worker exclusively owns SD during sync/preparation. Repainting its
    // status must never race a partially published cover or touch the SD bus.
    if (AutomaticSync::busy() || BookPreparation::busy()) return nullptr;
    for (auto &slot : homeCovers) {
        if (slot.id[0]) continue;
        snprintf(slot.id, sizeof(slot.id), "%s", id);
        CoverCacheInfo info{};
        if (CoverCache::load(id, slot.bitmap, info)) return &slot.bitmap;
        return nullptr;
    }
    return nullptr;
}

class ReaderBookishCanvas final : public BookishUI::Canvas {
public:
    explicit ReaderBookishCanvas(uint8_t *scratch) : scratch_(scratch) {}
    void box(int x, int y, int w, int h, int radius, uint8_t border,
             uint8_t fill, int thickness = 1) override {
        if (radius) drawPortraitRoundedRect(x, y, w, h, radius, border, fill, thickness);
        else {
            fillPortraitRect(x, y, w, h, fill);
            if (border != fill) drawPortraitRect(x, y, w, h, border, thickness);
        }
    }
    void text(BookishUI::Font font, const char *value, int x, int baseline,
              int width, uint8_t ink = 0, uint8_t paper = 15) override {
        char fitted[256]{};
        fitPortraitText(bookishFont(font), value, width, fitted, sizeof(fitted));
        drawPortraitText(scratch_, bookishFont(font), fitted, x, baseline, ink, paper, true);
    }
    void title(const char *value, int x, int baseline, int width) override {
        PortraitTextLines lines{};
        wrapPortraitText(&BookishLora31, value, width, 2, lines);
        for (size_t i = 0; i < lines.count; ++i)
            drawPortraitText(scratch_, &BookishLora31, lines.line[i], x,
                             baseline + i * 37, 0, 15, true);
    }
    void cover(const char *id, int x, int y, int w, int h) override {
        const CoverBitmap *bitmap = homeCover(id);
        if (bitmap) {
            drawPortraitCoverFitted(*bitmap, x + w / 2, y + h / 2, w, h);
        } else {
            // An honest neutral book silhouette, never an unrelated cover.
            box(x, y, w, h, 2, 10, 15);
            box(x + 4, y + 4, 1, h - 8, 0, 10, 10);
            box(x + w / 4, y + h / 3, w / 2, 1, 0, 5, 5);
            box(x + w / 4, y + h / 3 + 6, w / 2, 1, 0, 10, 10);
        }
    }
    void logo(int x, int y) override {
        for (int row = 0; row < kBookishLogoHeight; ++row)
            for (int col = 0; col < kBookishLogoWidth; ++col)
                setPortraitPixel(x + col, y + row, kBookishLogo[row * kBookishLogoWidth + col]);
    }
private:
    uint8_t *scratch_;
};

bool renderBookishHome(int tabFocus = -1) {
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(
        kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scratch || !framebuffer) { heap_caps_free(scratch); return false; }
    memset(framebuffer, 0xFF, kFramebufferBytes);
    BookishUI::Home view{};
    char statuses[3][48]{};
    auto book = [&](const LocalBookEntry &entry, size_t selection, size_t status) {
        formatLocalBookStatus(entry, statuses[status], sizeof(statuses[status]));
        return BookishUI::Book{entry.id, entry.title, entry.author, statuses[status],
            static_cast<uint8_t>(localBookProgressPercent(entry)), tabFocus < 0 && homeSession.selected == selection};
    };
    BookishUI::Book hero{};
    if (homeSession.count) {
        hero = book(homeSession.entries[0], HomeLayout::kFirstBook, 0);
        view.hero = &hero;
    }
    const auto action = HomeLayout::action(homeSession.selected, homeSession.count, homeSession.addedCount);
    view.added = action == HomeLayout::Action::AddedCard || homeSession.count < 2;
    if (view.added) {
        view.rowCount = homeSession.addedCount;
        for (size_t i = 0; i < view.rowCount; ++i)
            view.rows[i] = book(homeSession.added[i], HomeLayout::kFirstBook + homeSession.count + i, i + 1);
    } else {
        view.rowCount = homeSession.count - 1;
        for (size_t i = 0; i < view.rowCount; ++i)
            view.rows[i] = book(homeSession.entries[i + 1], HomeLayout::kFirstBook + i + 1, i + 1);
    }
    view.empty = homeSession.localBookCount == 0;
    view.syncSelected = action == HomeLayout::Action::Sync && tabFocus < 0;
    view.settingsSelected = action == HomeLayout::Action::Settings && tabFocus < 0;
    view.syncing = AutomaticSync::busy();
    view.tabFocus = tabFocus;
    if (tabFocus >= 0)
        for (unsigned i = 0; i < kVisibleTabCount; ++i)
            if (kVisibleTabs[i] == activeTopLevelTab) view.activeTab = i;
    const char *notice = "";
    switch (AutomaticSync::status()) {
        case AutomaticSync::Status::Failed: notice = ReaderSyncPolicy::errorLabel(AutomaticSync::error()); break;
        case AutomaticSync::Status::Cancelling: notice = "Завершаю синхронизацию…"; break;
        case AutomaticSync::Status::Complete: notice = "Библиотека обновлена"; break;
        case AutomaticSync::Status::More: notice = "Есть ещё книги — продолжите синхронизацию"; break;
        case AutomaticSync::Status::Cancelled: notice = "Синхронизация отменена"; break;
        case AutomaticSync::Status::Running: notice = "Синхронизация…"; break;
        default: break;
    }
    view.notice = operationNotice[0] ? operationNotice : notice;
    ReaderBookishCanvas canvas(scratch);
    BookishUI::home(canvas, view);
    heap_caps_free(scratch);
    return true;
}
