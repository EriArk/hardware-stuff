bool syncProgressVisible = false;
void drawSyncProgress() {
    static SyncProgress::Snapshot shown;
    static uint32_t shownAt = 0;
    static bool shownCancelling = false;
    const auto progress = AutomaticSync::progress();
    const bool cancelling = AutomaticSync::cancelRequested();
    const uint32_t elapsed = millis() - shownAt;
    if (displayRefresh.busy()) return;
    if (syncProgressVisible && (elapsed < 1500 ||
        (!SyncProgress::changed(progress, shown) && cancelling == shownCancelling && elapsed < 10000))) return;
    auto *scratch = static_cast<uint8_t *>(heap_caps_malloc(kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scratch || !framebuffer) { heap_caps_free(scratch); return; }
    using Stage = SyncProgress::Stage;
    const char *stage = "";
    switch (progress.stage) {
        case Stage::Connecting: stage=I18n::text("Connecting to your library","Подключаемся к библиотеке");break;
        case Stage::CollectionsUpload: stage=I18n::text("Sending collection changes","Отправляем коллекции");break;
        case Stage::ReadingUpload: stage=I18n::text("Saving reading progress","Сохраняем позиции чтения");break;
        case Stage::CheckingBooks: stage=I18n::text("Checking for book updates","Проверяем обновления книг");break;
        case Stage::Download: stage=I18n::text("Downloading a book","Загружаем книгу");break;
        case Stage::Preparing: stage=I18n::text("Preparing the book","Подготавливаем книгу");break;
        case Stage::Cover: stage=I18n::text("Downloading the cover","Загружаем обложку");break;
        case Stage::Verifying: stage=I18n::text("Checking the saved file","Проверяем сохранённый файл");break;
        case Stage::Removing: stage=I18n::text("Updating books on device","Обновляем книги на читалке");break;
        case Stage::ReadingDownload: stage=I18n::text("Updating positions & bookmarks","Обновляем позиции и закладки");break;
        case Stage::CollectionsDownload: stage=I18n::text("Updating your collections","Обновляем ваши коллекции");break;
    }
    char detail[96]{};
    if (progress.stage == Stage::ReadingUpload || progress.stage == Stage::ReadingDownload) {
        snprintf(detail,sizeof(detail),I18n::text("Books checked: %lu / %lu","Проверено книг: %lu / %lu"),
            static_cast<unsigned long>(progress.done),static_cast<unsigned long>(progress.total));
    } else if (progress.stage == Stage::Download || progress.stage == Stage::Cover) {
        if(progress.total)snprintf(detail,sizeof(detail),"%lu / %lu KB",
            static_cast<unsigned long>(progress.done/1024),static_cast<unsigned long>((progress.total+1023)/1024));
        else snprintf(detail,sizeof(detail),I18n::text("Received: %lu KB","Получено: %lu КБ"),static_cast<unsigned long>(progress.done/1024));
    } else snprintf(detail,sizeof(detail),"%s",I18n::text("Your library is being updated.","Обновляем вашу библиотеку."));
    BookishUI::Sync view;
    view.stage=stage;view.detail=detail;view.percent=SyncProgress::percent(progress);
    view.downloaded=progress.downloaded;view.elapsedSeconds=(millis()-progress.startedAt)/1000;
    view.cancelling=cancelling;
    ReaderBookishCanvas canvas(scratch);
    BookishUI::syncProgress(canvas,view);
    heap_caps_free(scratch);
    // Only the changing stage/counter/bar/timer area is cleared on updates.
    const Rect_t region{370,30,320,480}; // Portrait y=270..590.
    const auto result=displayRefresh.refresh(framebuffer,syncProgressVisible?
        DisplayRefreshMode::RecoveryRegion:DisplayRefreshMode::RecoveryFull,region);
    if(result.ok) {
        syncProgressVisible=true;shown=progress;shownAt=millis();shownCancelling=cancelling;
        Serial.printf("SYNC UI stage=%u done=%lu total=%lu percent=%d elapsed_s=%u cancelling=%s\n",
            unsigned(progress.stage),static_cast<unsigned long>(progress.done),
            static_cast<unsigned long>(progress.total),view.percent,view.elapsedSeconds,cancelling?"true":"false");
    }
}
