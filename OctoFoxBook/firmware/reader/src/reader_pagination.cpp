#include "reader_pagination.h"
#include "work_progress.h"

#include <ArduinoJson.h>
#include <SD.h>

#include <string.h>

#include "book_upload.h"
#include "fb2_cache.h"
#include "reader_text.h"

namespace {

struct StateRam : ArduinoJson::Allocator {
    void *allocate(size_t n) override {
#ifdef ARDUINO
        return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        return malloc(n);
#endif
    }
    void *reallocate(void *p, size_t n) override {
#ifdef ARDUINO
        return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        return realloc(p, n);
#endif
    }
    void deallocate(void *p) override { free(p); }
} stateRam;

constexpr char kPagesSignature[] = "ABYSS_FB2_PAGES\t1";
constexpr char kChaptersSignature[] = "ABYSS_FB2_CHAPTERS\t2";
constexpr char kIndexMagic[8] = {'A', 'B', 'P', 'G', 'I', 'D', 'X', '1'};
constexpr uint32_t kIndexVersion = 1;
constexpr int32_t kPortraitWidth = 540;
constexpr size_t kContentReadBufferBytes = 32768;
constexpr size_t kPagesWriteBufferBytes = 32768;
constexpr size_t kIndexWriteBufferBytes = 8192;
constexpr size_t kChaptersWriteBufferBytes = 8192;

struct IndexHeader {
    char magic[8]{};
    uint32_t version = 0;
    uint32_t entryBytes = 0;
    uint32_t pageCount = 0;
    uint32_t pagesBytes = 0;
    uint32_t sourceRecords = 0;
};

static_assert(sizeof(ReaderPageIndexEntry) == 12,
              "Reader page index format changed");

void setError(ReaderPaginationInfo &info, const char *message) {
    snprintf(info.error, sizeof(info.error), "%s", message);
}

void setError(ReaderProgress &progress, const char *message) {
    snprintf(progress.error, sizeof(progress.error), "%s", message);
}

void setError(ReaderUserState &state, const char *message) {
    snprintf(state.error, sizeof(state.error), "%s", message);
}

bool publishFile(const char *partialPath, const char *finalPath,
                 const char *oldPath) {
    SD.remove(oldPath);
    const bool hadPrevious = SD.exists(finalPath);
    if (hadPrevious && !SD.rename(finalPath, oldPath)) {
        return false;
    }
    if (!SD.rename(partialPath, finalPath)) {
        if (hadPrevious) {
            SD.rename(oldPath, finalPath);
        }
        return false;
    }
    if (hadPrevious) {
        SD.remove(oldPath);
    }
    return true;
}

bool makeCachePath(const char *bookId, const char *name, char *target,
                   size_t capacity) {
    if (!BookUploadReceiver::validBookId(bookId) || target == nullptr ||
        capacity == 0) {
        return false;
    }
    const int written = snprintf(target, capacity, "/books/%s/cache/%s",
                                 bookId, name);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool makeStatePath(const char *bookId, const char *suffix, char *target,
                   size_t capacity) {
    if (!BookUploadReceiver::validBookId(bookId) || target == nullptr ||
        capacity == 0) {
        return false;
    }
    const int written = snprintf(target, capacity,
                                 "/books/%s/reader-state.json%s", bookId,
                                 suffix);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

bool loadStateDocument(const char *bookId, uint32_t pageCount,
                       JsonDocument &document) {
    char path[96]{};
    if (!makeStatePath(bookId, "", path, sizeof(path))) {
        return false;
    }
    File input = SD.open(path, FILE_READ);
    if (!input) {
        return false;
    }
    const DeserializationError jsonError = deserializeJson(document, input);
    input.close();
    const char *schema = document["schema"].as<const char *>();
    const char *stateBookId = document["book_id"].as<const char *>();
    const char *layout = document["layout"].as<const char *>();
    const uint32_t version = document["version"].as<uint32_t>();
    const uint32_t page = document["current_page"] | 0U;
    const uint32_t savedPageCount = document["page_count"] | 0U;
    if (jsonError || schema == nullptr || stateBookId == nullptr ||
        layout == nullptr || strcmp(schema, "abyss-reader-state") ||
        version != 1 || strcmp(stateBookId, bookId)) return false;
    if (strcmp(layout, readerLayout().id) || savedPageCount != pageCount) {
        ReaderPageIndexEntry logical{};
        logical.sourceRecord = document["logical"]["record"] | 0U;
        logical.sourceByte = document["logical"]["byte"] | 0U;
        uint32_t mapped = 1;
        if (!ReaderPagination::pageForLogical(bookId, logical, mapped)) return false;
        document["current_page"] = mapped;
        for (JsonObject mark : document["bookmarks"].as<JsonArray>()) {
            logical.sourceRecord = mark["logical"]["record"] | 0U;
            logical.sourceByte = mark["logical"]["byte"] | 0U;
            if (!ReaderPagination::pageForLogical(bookId, logical, mapped)) return false;
            mark["page"] = mapped;
        }
        document["layout"] = readerLayout().id;
        document["page_count"] = pageCount;
        return true;
    }
    return page > 0 && page <= pageCount;
}

bool publishStateDocument(const char *bookId, JsonDocument &document) {
    if (document.overflowed()) return false;
    char partialPath[104]{};
    char finalPath[96]{};
    char oldPath[104]{};
    if (!makeStatePath(bookId, "", finalPath, sizeof(finalPath)) ||
        !makeStatePath(bookId, ".part", partialPath, sizeof(partialPath)) ||
        !makeStatePath(bookId, ".old", oldPath, sizeof(oldPath))) {
        return false;
    }
    SD.remove(partialPath);
    File output = SD.open(partialPath, FILE_WRITE);
    if (!output) {
        return false;
    }
    const bool written = serializeJsonPretty(document, output) == measureJsonPretty(document);
    output.flush();
    output.close();
    if (!written || !publishFile(partialPath, finalPath, oldPath)) {
        SD.remove(partialPath);
        return false;
    }
    return true;
}

bool chaptersPath(const char *bookId, char *target, size_t capacity) {
    return makeCachePath(bookId, "chapters-v2.txt", target, capacity);
}

struct ChapterStructure {
    uint16_t minimumDepth = UINT16_MAX;
    uint32_t minimumDepthCount = 0;
    bool hasDeeperTitles = false;

    void add(uint16_t depth) {
        if (depth < minimumDepth) {
            hasDeeperTitles = minimumDepth != UINT16_MAX;
            minimumDepth = depth;
            minimumDepthCount = 1;
        } else if (depth == minimumDepth) {
            ++minimumDepthCount;
        } else {
            hasDeeperTitles = true;
        }
    }

    bool skipSoleWrapper(uint16_t depth) const {
        return minimumDepthCount == 1 && hasDeeperTitles &&
               depth == minimumDepth;
    }
};

bool scanChapterStructure(File &content, uint32_t recordsOffset,
                          ChapterStructure &structure) {
    if (!content.seek(recordsOffset)) {
        return false;
    }
    bool headingPending = false;
    uint16_t pendingDepth = 0;
    while (content.available()) {
        if (workCancelled()) return false;
        String record = content.readStringUntil('\n');
        if (record.endsWith("\r")) {
            record.remove(record.length() - 1);
        }
        if (record.length() < 2 || record[1] != '\t') {
            continue;
        }
        const char type = record[0];
        if (type == 'S') {
            pendingDepth = static_cast<uint16_t>(record.substring(2).toInt());
            headingPending = pendingDepth > 0;
        } else if (type == 'H' && headingPending) {
            structure.add(pendingDepth);
            headingPending = false;
        } else if (type == 'P' || type == 'I') {
            headingPending = false;
        }
    }
    return content.seek(recordsOffset);
}

bool parseChapterRecord(const String &record, ReaderChapterEntry &entry) {
    if (!record.startsWith("C\t")) {
        return false;
    }
    const int ordinalEnd = record.indexOf('\t', 2);
    const int pageEnd =
        ordinalEnd >= 0 ? record.indexOf('\t', ordinalEnd + 1) : -1;
    const int sourceEnd =
        pageEnd >= 0 ? record.indexOf('\t', pageEnd + 1) : -1;
    if (ordinalEnd < 0 || pageEnd < 0 || sourceEnd < 0 ||
        sourceEnd + 1 >= static_cast<int>(record.length())) {
        return false;
    }
    entry = ReaderChapterEntry{};
    entry.ordinal = record.substring(2, ordinalEnd).toInt();
    entry.page = record.substring(ordinalEnd + 1, pageEnd).toInt();
    entry.sourceRecord = record.substring(pageEnd + 1, sourceEnd).toInt();
    const String title = record.substring(sourceEnd + 1);
    title.toCharArray(entry.title, sizeof(entry.title));
    return entry.ordinal > 0 && entry.page > 0 && entry.title[0] != '\0';
}

bool validateChapterFile(File &chapters, uint32_t pageCount,
                         uint32_t &chapterCount) {
    chapterCount = 0;
    String signature = chapters.readStringUntil('\n');
    signature.trim();
    String layout = chapters.readStringUntil('\n');
    layout.trim();
    String expectedLayout = "LAYOUT\t";
    expectedLayout += readerLayout().id;
    if (signature != kChaptersSignature || layout != expectedLayout) {
        return false;
    }

    uint32_t previousPage = 0;
    while (chapters.available()) {
        String record = chapters.readStringUntil('\n');
        record.trim();
        if (record.isEmpty()) {
            continue;
        }
        ReaderChapterEntry entry{};
        if (!parseChapterRecord(record, entry) ||
            entry.ordinal != chapterCount + 1 || entry.page > pageCount ||
            entry.page <= previousPage) {
            return false;
        }
        ++chapterCount;
        previousPage = entry.page;
    }
    return chapterCount > 0;
}

class PageWriter {
public:
    PageWriter(File &pages, File &index, File &chapters,
               ReaderPaginationInfo &info)
        : pages_(pages), index_(index), chapters_(chapters), info_(info) {}

    bool heading(const String &text, uint32_t sourceRecord,
                 bool chapterStart) {
        if (pageOpen_ && bodyWritten_) {
            if (!beginPage(sourceRecord, 0)) {
                return false;
            }
        } else if (!pageOpen_ && !beginPage(sourceRecord, 0)) {
            return false;
        }
        pages_.print("H\t");
        pages_.println(text);
        if (chapterStart && lastChapterPage_ != info_.pageCount) {
            ++info_.chapterCount;
            chapters_.print("C\t");
            chapters_.print(info_.chapterCount);
            chapters_.write(static_cast<uint8_t>('\t'));
            chapters_.print(info_.pageCount);
            chapters_.write(static_cast<uint8_t>('\t'));
            chapters_.print(sourceRecord);
            chapters_.write(static_cast<uint8_t>('\t'));
            chapters_.println(text);
            lastChapterPage_ = info_.pageCount;
        }
        const ReaderLayoutConfig &layout = readerLayout();
        const int32_t headingBaseline =
            layout.headingBaseline + static_cast<int32_t>(headingLines_) *
                                         layout.lineAdvance;
        ++headingLines_;
        baseline_ = headingBaseline + layout.headingToBody;
        contentWritten_ = true;
        return pages_.getWriteError() == 0;
    }

    bool line(const String &text, bool indent, uint32_t sourceRecord,
              uint32_t sourceByte) {
        if (!pageOpen_ || baseline_ > readerLayout().lastBaseline) {
            if (!beginPage(sourceRecord, sourceByte)) {
                return false;
            }
        }
        pages_.print("L\t");
        pages_.print(indent ? '1' : '0');
        pages_.write(static_cast<uint8_t>('\t'));
        pages_.println(text);
        baseline_ += readerLayout().lineAdvance;
        bodyWritten_ = true;
        contentWritten_ = true;
        return pages_.getWriteError() == 0;
    }

    bool gap(uint8_t pixels) {
        if (!pageOpen_ || !bodyWritten_) {
            return true;
        }
        pages_.print("G\t");
        pages_.println(pixels);
        baseline_ += pixels;
        return pages_.getWriteError() == 0;
    }

    bool finish() {
        return contentWritten_ && info_.pageCount > 0 &&
               pages_.getWriteError() == 0 && index_.getWriteError() == 0 &&
               chapters_.getWriteError() == 0;
    }

    bool ensureFallbackChapter() {
        if (info_.chapterCount > 0) {
            return true;
        }
        info_.chapterCount = 1;
        chapters_.println("C\t1\t1\t0\tНачало");
        return chapters_.getWriteError() == 0;
    }

private:
    bool beginPage(uint32_t sourceRecord, uint32_t sourceByte) {
        ReaderPageIndexEntry entry{};
        entry.fileOffset = static_cast<uint32_t>(pages_.position());
        entry.sourceRecord = sourceRecord;
        entry.sourceByte = sourceByte;
        if (index_.write(reinterpret_cast<const uint8_t *>(&entry),
                         sizeof(entry)) != sizeof(entry)) {
            return false;
        }
        ++info_.pageCount;
        pages_.print("PAGE\t");
        pages_.print(info_.pageCount);
        pages_.write(static_cast<uint8_t>('\t'));
        pages_.print(sourceRecord);
        pages_.write(static_cast<uint8_t>('\t'));
        pages_.println(sourceByte);
        pageOpen_ = true;
        bodyWritten_ = false;
        contentWritten_ = false;
        headingLines_ = 0;
        baseline_ = readerLayout().firstBaseline;
        return pages_.getWriteError() == 0;
    }

    File &pages_;
    File &index_;
    File &chapters_;
    ReaderPaginationInfo &info_;
    int32_t baseline_ = 0;
    uint16_t headingLines_ = 0;
    uint32_t lastChapterPage_ = 0;
    bool pageOpen_ = false;
    bool bodyWritten_ = false;
    bool contentWritten_ = false;
};

bool emitWord(PageWriter &writer, String &line, int32_t &lineWidth,
              uint32_t &lineStart, const String &word, uint32_t wordStart,
              bool &firstLine, uint32_t sourceRecord) {
    static const int32_t spaceWidth = measureReaderUtf8(String(" "));
    const int32_t wordWidth = measureReaderUtf8(word);
    const int32_t candidateWidth =
        line.isEmpty() ? wordWidth : lineWidth + spaceWidth + wordWidth;
    const ReaderLayoutConfig &layout = readerLayout();
    const int32_t availableWidth =
        kPortraitWidth - layout.leftMargin - layout.rightMargin -
        (firstLine ? layout.firstLineIndent : 0);
    if (!line.isEmpty() && candidateWidth > availableWidth) {
        if (!writer.line(line, firstLine, sourceRecord, lineStart)) {
            return false;
        }
        firstLine = false;
        line = word;
        lineWidth = wordWidth;
        lineStart = wordStart;
    } else {
        if (line.isEmpty()) {
            lineStart = wordStart;
            line = word;
        } else {
            line += ' ';
            line += word;
        }
        lineWidth = candidateWidth;
    }
    return true;
}

bool wrapParagraph(PageWriter &writer, String paragraph,
                   uint32_t sourceRecord) {
    paragraph.replace("\xC2\xA0", " ");
    paragraph.trim();
    if (paragraph.isEmpty()) {
        return true;
    }

    String line;
    String word;
    line.reserve(160);
    word.reserve(96);
    int32_t lineWidth = 0;
    uint32_t lineStart = 0;
    uint32_t wordStart = 0;
    bool firstLine = true;
    size_t cursor = 0;
    while (cursor < paragraph.length()) {
        if (workCancelled()) return false;
        const size_t start = cursor;
        const uint32_t codePoint = decodeReaderUtf8(
            paragraph.c_str(), paragraph.length(), cursor);
        if (!isReaderLayoutSpace(codePoint)) {
            if (word.isEmpty()) {
                wordStart = static_cast<uint32_t>(start);
            }
            word.concat(paragraph.c_str() + start,
                        static_cast<unsigned int>(cursor - start));
            continue;
        }
        if (word.isEmpty()) {
            continue;
        }
        if (!emitWord(writer, line, lineWidth, lineStart, word, wordStart,
                      firstLine, sourceRecord)) {
            return false;
        }
        word = "";
    }
    if (!word.isEmpty() &&
        !emitWord(writer, line, lineWidth, lineStart, word, wordStart,
                  firstLine, sourceRecord)) {
        return false;
    }
    if (!line.isEmpty() &&
        !writer.line(line, firstLine, sourceRecord, lineStart)) {
        return false;
    }
    return writer.gap(readerLayout().paragraphGap);
}

bool readHeader(File &index, IndexHeader &header) {
    if (index.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) !=
        sizeof(header)) {
        return false;
    }
    return memcmp(header.magic, kIndexMagic, sizeof(kIndexMagic)) == 0 &&
           header.version == kIndexVersion &&
           header.entryBytes == sizeof(ReaderPageIndexEntry) &&
           header.pageCount > 0;
}

}  // namespace

const char *ReaderPagination::layoutId() {
    return readerLayout().id;
}

bool ReaderPagination::pagesPath(const char *bookId, char *target,
                                 size_t capacity) {
    return makeCachePath(bookId, "pages-v1.txt", target, capacity);
}

bool ReaderPagination::build(const char *bookId, ReaderPaginationInfo &info) {
    info = ReaderPaginationInfo{};
    if (!BookUploadReceiver::validBookId(bookId)) {
        setError(info, "invalid-book-id");
        return false;
    }

    char contentPath[96]{};
    char finalPagesPath[96]{};
    char partialPagesPath[104]{};
    char oldPagesPath[104]{};
    char finalIndexPath[96]{};
    char partialIndexPath[104]{};
    char oldIndexPath[104]{};
    char finalChaptersPath[96]{};
    char partialChaptersPath[104]{};
    char oldChaptersPath[104]{};
    if (!Fb2Cache::contentPath(bookId, contentPath, sizeof(contentPath)) ||
        !pagesPath(bookId, finalPagesPath, sizeof(finalPagesPath)) ||
        !makeCachePath(bookId, "pages-v1.idx", finalIndexPath,
                       sizeof(finalIndexPath)) ||
        !chaptersPath(bookId, finalChaptersPath,
                      sizeof(finalChaptersPath))) {
        setError(info, "path-failed");
        return false;
    }
    snprintf(partialPagesPath, sizeof(partialPagesPath), "%s.part",
             finalPagesPath);
    snprintf(oldPagesPath, sizeof(oldPagesPath), "%s.old", finalPagesPath);
    snprintf(partialIndexPath, sizeof(partialIndexPath), "%s.part",
             finalIndexPath);
    snprintf(oldIndexPath, sizeof(oldIndexPath), "%s.old", finalIndexPath);
    snprintf(partialChaptersPath, sizeof(partialChaptersPath), "%s.part",
             finalChaptersPath);
    snprintf(oldChaptersPath, sizeof(oldChaptersPath), "%s.old",
             finalChaptersPath);
    SD.remove(partialPagesPath);
    SD.remove(partialIndexPath);
    SD.remove(partialChaptersPath);

    File content = SD.open(contentPath, FILE_READ);
    File pages = SD.open(partialPagesPath, FILE_WRITE);
    File index = SD.open(partialIndexPath, FILE_WRITE);
    File chapters = SD.open(partialChaptersPath, FILE_WRITE);
    if (!content || !pages || !index || !chapters) {
        content.close();
        pages.close();
        index.close();
        chapters.close();
        SD.remove(partialPagesPath);
        SD.remove(partialIndexPath);
        SD.remove(partialChaptersPath);
        setError(info, "open-failed");
        return false;
    }
    content.setBufferSize(kContentReadBufferBytes);
    pages.setBufferSize(kPagesWriteBufferBytes);
    index.setBufferSize(kIndexWriteBufferBytes);
    chapters.setBufferSize(kChaptersWriteBufferBytes);

    IndexHeader placeholder{};
    if (index.write(reinterpret_cast<const uint8_t *>(&placeholder),
                    sizeof(placeholder)) != sizeof(placeholder)) {
        content.close();
        pages.close();
        index.close();
        chapters.close();
        SD.remove(partialPagesPath);
        SD.remove(partialIndexPath);
        SD.remove(partialChaptersPath);
        setError(info, "index-header-write-failed");
        return false;
    }
    pages.println(kPagesSignature);
    pages.print("LAYOUT\t");
    pages.println(readerLayout().id);
    chapters.println(kChaptersSignature);
    chapters.print("LAYOUT\t");
    chapters.println(readerLayout().id);

    String signature = content.readStringUntil('\n');
    signature.trim();
    content.readStringUntil('\n');  // TITLE
    content.readStringUntil('\n');  // AUTHOR
    if (signature != "ABYSS_FB2_TEXT\t1") {
        content.close();
        pages.close();
        index.close();
        chapters.close();
        SD.remove(partialPagesPath);
        SD.remove(partialIndexPath);
        SD.remove(partialChaptersPath);
        setError(info, "content-cache-version");
        return false;
    }

    const uint32_t recordsOffset = static_cast<uint32_t>(content.position());
    ChapterStructure chapterStructure{};
    if (!scanChapterStructure(content, recordsOffset, chapterStructure)) {
        content.close();
        pages.close();
        index.close();
        chapters.close();
        SD.remove(partialPagesPath);
        SD.remove(partialIndexPath);
        SD.remove(partialChaptersPath);
        setError(info, "chapter-scan-failed");
        return false;
    }

    PageWriter writer(pages, index, chapters, info);
    bool sectionFound = false;
    bool headingPending = false;
    uint16_t pendingSectionDepth = 0;
    bool passed = true;
    uint32_t sourceRecord = 0;
    while (content.available()) {
        reportWorkProgress();
        if (workCancelled()) { passed = false; setError(info, "cancelled"); break; }
        String record = content.readStringUntil('\n');
        if (record.endsWith("\r")) {
            record.remove(record.length() - 1);
        }
        if (record.length() < 2 || record[1] != '\t') {
            continue;
        }
        ++sourceRecord;
        const char type = record[0];
        const String value = record.substring(2);
        if (type == 'S') {
            sectionFound = true;
            pendingSectionDepth =
                static_cast<uint16_t>(value.toInt());
            headingPending = pendingSectionDepth > 0;
            continue;
        }
        if (!sectionFound) {
            continue;
        }
        if (type == 'H') {
            const bool chapterStart =
                headingPending &&
                !chapterStructure.skipSoleWrapper(pendingSectionDepth);
            passed = writer.heading(value, sourceRecord, chapterStart);
            headingPending = false;
        } else if (type == 'P' || type == 'I') {
            headingPending = false;
            passed = wrapParagraph(writer, value, sourceRecord);
        } else if (type == 'E') {
            passed = writer.gap(20);
        }
        if (!passed) {
            break;
        }
    }
    info.sourceRecords = sourceRecord;
    passed = passed && sectionFound && writer.finish() &&
             writer.ensureFallbackChapter();
    content.close();

    pages.flush();
    info.pagesBytes = static_cast<uint32_t>(pages.size());
    pages.close();
    chapters.flush();
    chapters.close();

    IndexHeader header{};
    memcpy(header.magic, kIndexMagic, sizeof(kIndexMagic));
    header.version = kIndexVersion;
    header.entryBytes = sizeof(ReaderPageIndexEntry);
    header.pageCount = info.pageCount;
    header.pagesBytes = info.pagesBytes;
    header.sourceRecords = info.sourceRecords;
    if (!index.seek(0) ||
        index.write(reinterpret_cast<const uint8_t *>(&header),
                    sizeof(header)) != sizeof(header)) {
        passed = false;
    }
    index.flush();
    index.close();

    if (!passed) {
        SD.remove(partialPagesPath);
        SD.remove(partialIndexPath);
        SD.remove(partialChaptersPath);
        setError(info, "pagination-build-failed");
        return false;
    }
    if (!publishFile(partialPagesPath, finalPagesPath, oldPagesPath) ||
        !publishFile(partialIndexPath, finalIndexPath, oldIndexPath) ||
        !publishFile(partialChaptersPath, finalChaptersPath,
                     oldChaptersPath)) {
        SD.remove(partialPagesPath);
        SD.remove(partialIndexPath);
        SD.remove(partialChaptersPath);
        setError(info, "pagination-publish-failed");
        return false;
    }
    info.ok = true;
    return true;
}

bool ReaderPagination::load(const char *bookId, ReaderPaginationInfo &info) {
    info = ReaderPaginationInfo{};
    char pagesPathValue[96]{};
    char indexPath[96]{};
    char chaptersPathValue[96]{};
    if (!pagesPath(bookId, pagesPathValue, sizeof(pagesPathValue)) ||
        !makeCachePath(bookId, "pages-v1.idx", indexPath,
                       sizeof(indexPath)) ||
        !chaptersPath(bookId, chaptersPathValue,
                      sizeof(chaptersPathValue))) {
        setError(info, "path-failed");
        return false;
    }
    File pages = SD.open(pagesPathValue, FILE_READ);
    File index = SD.open(indexPath, FILE_READ);
    File chapters = SD.open(chaptersPathValue, FILE_READ);
    if (!pages || !index || !chapters) {
        pages.close();
        index.close();
        chapters.close();
        setError(info, "pagination-not-found");
        return false;
    }
    IndexHeader header{};
    String signature = pages.readStringUntil('\n');
    signature.trim();
    String layout = pages.readStringUntil('\n');
    layout.trim();
    String expectedLayout = "LAYOUT\t";
    expectedLayout += readerLayout().id;
    const bool pageFilesValid =
        signature == kPagesSignature && layout == expectedLayout &&
        readHeader(index, header) && pages.size() == header.pagesBytes &&
        index.size() == sizeof(IndexHeader) +
                            header.pageCount * sizeof(ReaderPageIndexEntry);
    uint32_t chapterCount = 0;
    const bool valid = pageFilesValid &&
                       validateChapterFile(chapters, header.pageCount,
                                           chapterCount);
    pages.close();
    index.close();
    chapters.close();
    if (!valid) {
        setError(info, "pagination-invalid");
        return false;
    }
    info.ok = true;
    info.pageCount = header.pageCount;
    info.chapterCount = chapterCount;
    info.pagesBytes = header.pagesBytes;
    info.sourceRecords = header.sourceRecords;
    return true;
}

bool ReaderPagination::pageEntry(const char *bookId, uint32_t page,
                                 ReaderPageIndexEntry &entry) {
    ReaderPaginationInfo info{};
    if (!load(bookId, info) || page == 0 || page > info.pageCount) {
        return false;
    }
    char indexPath[96]{};
    if (!makeCachePath(bookId, "pages-v1.idx", indexPath,
                       sizeof(indexPath))) {
        return false;
    }
    File index = SD.open(indexPath, FILE_READ);
    if (!index) {
        return false;
    }
    const uint32_t offset = sizeof(IndexHeader) +
                            (page - 1) * sizeof(ReaderPageIndexEntry);
    const bool passed = index.seek(offset) &&
                        index.read(reinterpret_cast<uint8_t *>(&entry),
                                   sizeof(entry)) == sizeof(entry);
    index.close();
    return passed;
}

bool ReaderPagination::pageForLogical(
    const char *bookId, const ReaderPageIndexEntry &logical,
    uint32_t &page) {
    page = 1;
    ReaderPaginationInfo info{};
    if (!load(bookId, info)) {
        return false;
    }
    char indexPath[96]{};
    if (!makeCachePath(bookId, "pages-v1.idx", indexPath,
                       sizeof(indexPath))) {
        return false;
    }
    File index = SD.open(indexPath, FILE_READ);
    if (!index) {
        return false;
    }
    IndexHeader header{};
    if (!readHeader(index, header)) {
        index.close();
        return false;
    }
    ReaderPageIndexEntry candidate{};
    for (uint32_t ordinal = 1; ordinal <= header.pageCount; ++ordinal) {
        if (index.read(reinterpret_cast<uint8_t *>(&candidate),
                       sizeof(candidate)) != sizeof(candidate)) {
            index.close();
            return false;
        }
        const bool afterTarget =
            candidate.sourceRecord > logical.sourceRecord ||
            (candidate.sourceRecord == logical.sourceRecord &&
             candidate.sourceByte > logical.sourceByte);
        if (afterTarget) {
            break;
        }
        page = ordinal;
    }
    index.close();
    return true;
}

bool ReaderPagination::currentChapter(const char *bookId, uint32_t page,
                                      ReaderChapterEntry &entry) {
    entry = ReaderChapterEntry{};
    ReaderPaginationInfo info{};
    if (!load(bookId, info) || page == 0 || page > info.pageCount) {
        return false;
    }
    char path[96]{};
    if (!chaptersPath(bookId, path, sizeof(path))) {
        return false;
    }
    File chapters = SD.open(path, FILE_READ);
    if (!chapters) {
        return false;
    }
    chapters.readStringUntil('\n');
    chapters.readStringUntil('\n');
    bool found = false;
    while (chapters.available()) {
        String record = chapters.readStringUntil('\n');
        record.trim();
        ReaderChapterEntry candidate{};
        if (!parseChapterRecord(record, candidate)) {
            continue;
        }
        if (candidate.page > page) {
            break;
        }
        entry = candidate;
        found = true;
    }
    chapters.close();
    return found;
}

bool ReaderPagination::chapterEntry(const char *bookId, uint32_t ordinal,
                                    ReaderChapterEntry &entry) {
    entry = ReaderChapterEntry{};
    ReaderPaginationInfo info{};
    if (!load(bookId, info) || ordinal == 0 || ordinal > info.chapterCount) {
        return false;
    }
    char path[96]{};
    if (!chaptersPath(bookId, path, sizeof(path))) {
        return false;
    }
    File chapters = SD.open(path, FILE_READ);
    if (!chapters) {
        return false;
    }
    chapters.readStringUntil('\n');
    chapters.readStringUntil('\n');
    bool found = false;
    while (chapters.available()) {
        String record = chapters.readStringUntil('\n');
        record.trim();
        ReaderChapterEntry candidate{};
        if (!parseChapterRecord(record, candidate)) {
            continue;
        }
        if (candidate.ordinal == ordinal) {
            entry = candidate;
            found = true;
            break;
        }
        if (candidate.ordinal > ordinal) {
            break;
        }
    }
    chapters.close();
    return found;
}

bool ReaderPagination::adjacentChapter(const char *bookId, uint32_t page,
                                       int8_t direction,
                                       ReaderChapterEntry &entry) {
    entry = ReaderChapterEntry{};
    ReaderPaginationInfo info{};
    if (direction == 0 || !load(bookId, info) || page == 0 ||
        page > info.pageCount) {
        return false;
    }
    char path[96]{};
    if (!chaptersPath(bookId, path, sizeof(path))) {
        return false;
    }
    File chapters = SD.open(path, FILE_READ);
    if (!chapters) {
        return false;
    }
    chapters.readStringUntil('\n');
    chapters.readStringUntil('\n');
    bool found = false;
    while (chapters.available()) {
        String record = chapters.readStringUntil('\n');
        record.trim();
        ReaderChapterEntry candidate{};
        if (!parseChapterRecord(record, candidate)) {
            continue;
        }
        if (direction > 0) {
            if (candidate.page > page) {
                entry = candidate;
                found = true;
                break;
            }
        } else if (candidate.page < page) {
            entry = candidate;
            found = true;
        } else {
            break;
        }
    }
    chapters.close();
    return found;
}

bool ReaderPagination::loadProgress(const char *bookId, uint32_t pageCount,
                                    ReaderProgress &progress) {
    progress = ReaderProgress{};
    JsonDocument document(&stateRam);
    if (!loadStateDocument(bookId, pageCount, document)) {
        setError(progress, "invalid-state");
        return false;
    }
    const uint32_t page = document["current_page"] | 0U;
    progress.found = true;
    progress.finished = document["finished"] | false;
    progress.currentPage = page;
    progress.pageCount = pageCount;
    progress.logical.sourceRecord = document["logical"]["record"] | 0U;
    progress.logical.sourceByte = document["logical"]["byte"] | 0U;
    return true;
}

bool ReaderPagination::loadPortableState(const char *bookId,
                                         ReaderProgress &progress,
                                         ReaderUserState &state) {
    progress = ReaderProgress{};
    state = ReaderUserState{};
    char path[96]{};
    if (!makeStatePath(bookId, "", path, sizeof(path))) {
        setError(progress, "invalid-book-id");
        setError(state, "invalid-book-id");
        return false;
    }
    File input = SD.open(path, FILE_READ);
    if (!input) {
        setError(progress, "state-not-found");
        setError(state, "state-not-found");
        return false;
    }
    JsonDocument document(&stateRam);
    const DeserializationError jsonError = deserializeJson(document, input);
    input.close();
    const char *schema = document["schema"].as<const char *>();
    const char *stateBookId = document["book_id"].as<const char *>();
    if (jsonError || schema == nullptr || stateBookId == nullptr ||
        strcmp(schema, "abyss-reader-state") != 0 ||
        (document["version"] | 0U) != 1 ||
        strcmp(stateBookId, bookId) != 0) {
        setError(progress, "invalid-state");
        setError(state, "invalid-state");
        return false;
    }
    progress.found = true;
    progress.finished = document["finished"] | false;
    progress.currentPage = document["current_page"] | 1U;
    progress.pageCount = document["page_count"] | 0U;
    progress.logical.sourceRecord = document["logical"]["record"] | 0U;
    progress.logical.sourceByte = document["logical"]["byte"] | 0U;
    state.found = true;
    state.finished = progress.finished;
    JsonArrayConst bookmarks = document["bookmarks"].as<JsonArrayConst>();
    for (JsonObjectConst bookmark : bookmarks) {
        if (state.bookmarkCount >= kReaderBookmarkCapacity) {
            break;
        }
        ReaderBookmark candidate{};
        candidate.page = bookmark["page"] | 0U;
        candidate.logical.sourceRecord =
            bookmark["logical"]["record"] | 0U;
        candidate.logical.sourceByte = bookmark["logical"]["byte"] | 0U;
        const char *title = bookmark["title"].as<const char *>();
        if (candidate.page == 0 || title == nullptr || title[0] == '\0') {
            continue;
        }
        snprintf(candidate.title, sizeof(candidate.title), "%s", title);
        state.bookmarks.push_back(candidate);
        ++state.bookmarkCount;
    }
    return true;
}

bool ReaderPagination::saveProgress(
    const char *bookId, uint32_t page, uint32_t pageCount,
    const ReaderPageIndexEntry &logical, ReaderProgress &progress) {
    progress = ReaderProgress{};
    if (page == 0 || page > pageCount) {
        setError(progress, "page-out-of-range");
        return false;
    }
    JsonDocument document(&stateRam);
    if (!loadStateDocument(bookId, pageCount, document)) {
        char path[96]{};
        makeStatePath(bookId, "", path, sizeof(path));
        // Corrupt/unsupported existing state is never replaced by an empty
        // document: that would silently discard bookmarks and sync identities.
        if (SD.exists(path)) { setError(progress, "invalid-existing-state"); return false; }
        document.clear();
    }
    const bool samePage = (document["current_page"] | 0U) == page &&
                          (document["logical"]["record"] | 0U) != 0;
    document["schema"] = "abyss-reader-state";
    document["version"] = 1;
    document["book_id"] = bookId;
    document["layout"] = readerLayout().id;
    document["current_page"] = page;
    document["page_count"] = pageCount;
    if (!samePage) {
        document["logical"]["record"] = logical.sourceRecord;
        document["logical"]["byte"] = logical.sourceByte;
    }
    if (!publishStateDocument(bookId, document)) {
        setError(progress, "state-publish-failed");
        return false;
    }
    progress.found = true;
    progress.finished = document["finished"] | false;
    progress.currentPage = page;
    progress.pageCount = pageCount;
    progress.logical.sourceRecord = document["logical"]["record"] | 0U;
    progress.logical.sourceByte = document["logical"]["byte"] | 0U;
    return true;
}

bool ReaderPagination::loadUserState(const char *bookId, uint32_t pageCount,
                                     ReaderUserState &state) {
    state = ReaderUserState{};
    JsonDocument document(&stateRam);
    if (!loadStateDocument(bookId, pageCount, document)) {
        setError(state, "invalid-state");
        return false;
    }
    state.found = true;
    state.finished = document["finished"] | false;
    JsonArrayConst bookmarks = document["bookmarks"].as<JsonArrayConst>();
    for (JsonObjectConst bookmark : bookmarks) {
        if (state.bookmarkCount >= kReaderBookmarkCapacity) {
            break;
        }
        ReaderBookmark candidate{};
        candidate.page = bookmark["page"] | 0U;
        candidate.logical.sourceRecord = bookmark["logical"]["record"] | 0U;
        candidate.logical.sourceByte = bookmark["logical"]["byte"] | 0U;
        const char *title = bookmark["title"].as<const char *>();
        if (candidate.page == 0 || candidate.page > pageCount ||
            title == nullptr || title[0] == '\0') {
            continue;
        }
        snprintf(candidate.title, sizeof(candidate.title), "%s", title);
        state.bookmarks.push_back(candidate);
        ++state.bookmarkCount;
    }
    return true;
}

bool ReaderPagination::toggleBookmark(
    const char *bookId, uint32_t page, uint32_t pageCount,
    const ReaderPageIndexEntry &logical, const char *title, bool &added,
    ReaderUserState &state) {
    added = false;
    state = ReaderUserState{};
    if (page == 0 || page > pageCount || title == nullptr || title[0] == '\0') {
        setError(state, "invalid-bookmark");
        return false;
    }
    JsonDocument document(&stateRam);
    if (!loadStateDocument(bookId, pageCount, document)) {
        setError(state, "invalid-state");
        return false;
    }
    JsonArray bookmarks;
    if (document["bookmarks"].is<JsonArray>()) {
        bookmarks = document["bookmarks"].as<JsonArray>();
    } else {
        bookmarks = document["bookmarks"].to<JsonArray>();
    }
    size_t found = SIZE_MAX;
    size_t index = 0;
    for (JsonObjectConst bookmark : bookmarks) {
        if ((bookmark["page"] | 0U) == page) {
            found = index;
            break;
        }
        ++index;
    }
    if (found != SIZE_MAX) {
        bookmarks.remove(found);
    } else {
        if (bookmarks.size() >= kReaderBookmarkCapacity) {
            setError(state, "bookmark-capacity");
            return false;
        }
        JsonObject bookmark = bookmarks.add<JsonObject>();
        bookmark["page"] = page;
        bookmark["logical"]["record"] = logical.sourceRecord;
        bookmark["logical"]["byte"] = logical.sourceByte;
        bookmark["title"] = title;
        added = true;
    }
    if (!publishStateDocument(bookId, document)) {
        setError(state, "state-publish-failed");
        return false;
    }
    return loadUserState(bookId, pageCount, state);
}

bool ReaderPagination::setFinished(const char *bookId, uint32_t pageCount,
                                   bool finished,
                                   ReaderUserState &state) {
    state = ReaderUserState{};
    JsonDocument document(&stateRam);
    if (!loadStateDocument(bookId, pageCount, document)) {
        setError(state, "invalid-state");
        return false;
    }
    document["finished"] = finished;
    if (!publishStateDocument(bookId, document)) {
        setError(state, "state-publish-failed");
        return false;
    }
    return loadUserState(bookId, pageCount, state);
}
