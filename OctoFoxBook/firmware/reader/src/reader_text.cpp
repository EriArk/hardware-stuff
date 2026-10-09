#include "reader_text.h"

#include "reader_roboto14.h"
#include "reader_roboto16.h"
#include "reader_roboto18.h"

namespace {

ReaderTextSize activeTextSize = ReaderTextSize::Medium;
ReaderLineSpacing activeLineSpacing = ReaderLineSpacing::Normal;

const char *layoutId(ReaderTextSize size, ReaderLineSpacing spacing) {
    static constexpr const char *ids[3][3] = {
        {"reader-roboto14-compact-540x960-v2",
         "reader-roboto14-normal-540x960-v2",
         "reader-roboto14-airy-540x960-v2"},
        {"reader-roboto16-compact-540x960-v2",
         "reader-roboto16-normal-540x960-v2",
         "reader-roboto16-airy-540x960-v2"},
        {"reader-roboto18-compact-540x960-v2",
         "reader-roboto18-normal-540x960-v2",
         "reader-roboto18-airy-540x960-v2"},
    };
    return ids[static_cast<uint8_t>(size)][static_cast<uint8_t>(spacing)];
}

const GFXfont *layoutFont(ReaderTextSize size) {
    switch (size) {
        case ReaderTextSize::Small:
            return &ReaderRoboto14;
        case ReaderTextSize::Large:
            return &ReaderRoboto18;
        case ReaderTextSize::Medium:
            return &ReaderRoboto16;
    }
    return &ReaderRoboto16;
}

int32_t normalLineAdvance(ReaderTextSize size) {
    switch (size) {
        case ReaderTextSize::Small:
            return 36;
        case ReaderTextSize::Large:
            return 48;
        case ReaderTextSize::Medium:
            return 42;
    }
    return 42;
}

}  // namespace

void setReaderLayout(ReaderTextSize textSize,
                     ReaderLineSpacing lineSpacing) {
    activeTextSize = textSize;
    activeLineSpacing = lineSpacing;
}

ReaderTextSize readerTextSize() {
    return activeTextSize;
}

ReaderLineSpacing readerLineSpacing() {
    return activeLineSpacing;
}

const ReaderLayoutConfig &readerLayout() {
    static ReaderLayoutConfig config{};
    const int32_t baseAdvance = normalLineAdvance(activeTextSize);
    const int32_t spacingDelta =
        activeLineSpacing == ReaderLineSpacing::Compact
            ? -4
            : (activeLineSpacing == ReaderLineSpacing::Airy ? 5 : 0);
    config.font = layoutFont(activeTextSize);
    config.id = layoutId(activeTextSize, activeLineSpacing);
    config.firstBaseline = activeTextSize == ReaderTextSize::Small
                               ? 62
                               : (activeTextSize == ReaderTextSize::Large
                                      ? 74
                                      : 68);
    config.headingBaseline = config.firstBaseline;
    config.lineAdvance = baseAdvance + spacingDelta;
    config.headingToBody = config.lineAdvance + 14;
    config.paragraphGap =
        activeLineSpacing == ReaderLineSpacing::Compact
            ? 5
            : (activeLineSpacing == ReaderLineSpacing::Airy ? 11 : 8);
    return config;
}

const GFXfont *readerBodyFont() {
    return readerLayout().font;
}

uint32_t decodeReaderUtf8(const char *text, size_t length, size_t &cursor) {
    if (cursor >= length) {
        return 0;
    }
    const uint8_t first = static_cast<uint8_t>(text[cursor++]);
    if (first < 0x80) {
        return first;
    }

    uint32_t codePoint = 0;
    size_t continuationCount = 0;
    if ((first & 0xE0) == 0xC0) {
        codePoint = first & 0x1F;
        continuationCount = 1;
    } else if ((first & 0xF0) == 0xE0) {
        codePoint = first & 0x0F;
        continuationCount = 2;
    } else if ((first & 0xF8) == 0xF0) {
        codePoint = first & 0x07;
        continuationCount = 3;
    } else {
        return '?';
    }
    if (cursor + continuationCount > length) {
        cursor = length;
        return '?';
    }
    for (size_t index = 0; index < continuationCount; ++index) {
        const uint8_t next = static_cast<uint8_t>(text[cursor++]);
        if ((next & 0xC0) != 0x80) {
            return '?';
        }
        codePoint = (codePoint << 6) | (next & 0x3F);
    }
    return codePoint;
}

int32_t measureReaderUtf8(const String &text) {
    int32_t width = 0;
    size_t cursor = 0;
    const GFXfont *font = readerBodyFont();
    while (cursor < text.length()) {
        const uint32_t codePoint =
            decodeReaderUtf8(text.c_str(), text.length(), cursor);
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

bool isReaderLayoutSpace(uint32_t codePoint) {
    return codePoint == ' ' || codePoint == '\t' || codePoint == 0xA0;
}
