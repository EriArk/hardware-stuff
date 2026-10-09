#include "opds_parser.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct XmlElement {
    size_t openStart = 0;
    size_t openEnd = 0;
    size_t contentStart = 0;
    size_t contentEnd = 0;
    size_t after = 0;
    bool selfClosing = false;
};

void setError(char *target, size_t capacity, const char *value) {
    if (target != nullptr && capacity > 0) {
        snprintf(target, capacity, "%s", value == nullptr ? "" : value);
    }
}

bool isNameByte(char value) {
    return isalnum(static_cast<unsigned char>(value)) || value == '_' ||
           value == '-' || value == ':' || value == '.';
}

bool localNameEquals(const char *start, size_t length, const char *wanted) {
    const char *local = start;
    for (size_t index = 0; index < length; ++index) {
        if (start[index] == ':') {
            local = start + index + 1;
        }
    }
    return strlen(wanted) == static_cast<size_t>(start + length - local) &&
           strncmp(local, wanted, start + length - local) == 0;
}

size_t findTagEnd(const char *xml, size_t start, size_t limit) {
    char quote = '\0';
    for (size_t index = start; index < limit; ++index) {
        const char value = xml[index];
        if (quote != '\0') {
            if (value == quote) {
                quote = '\0';
            }
        } else if (value == '\'' || value == '"') {
            quote = value;
        } else if (value == '>') {
            return index;
        }
    }
    return limit;
}

bool findElement(const char *xml, size_t length, const char *wanted,
                 size_t from, size_t limit, XmlElement &element) {
    limit = min(limit, length);
    for (size_t cursor = from; cursor < limit;) {
        const char *found = static_cast<const char *>(
            memchr(xml + cursor, '<', limit - cursor));
        if (found == nullptr) {
            return false;
        }
        const size_t openStart = static_cast<size_t>(found - xml);
        size_t nameStart = openStart + 1;
        if (nameStart >= limit) {
            return false;
        }
        if (xml[nameStart] == '!' || xml[nameStart] == '?' ||
            xml[nameStart] == '/') {
            const size_t skipped = findTagEnd(xml, nameStart, limit);
            cursor = skipped < limit ? skipped + 1 : limit;
            continue;
        }
        const size_t nameBegin = nameStart;
        while (nameStart < limit && isNameByte(xml[nameStart])) {
            ++nameStart;
        }
        const size_t nameLength = nameStart - nameBegin;
        const size_t openEnd = findTagEnd(xml, nameStart, limit);
        if (openEnd >= limit) {
            return false;
        }
        if (nameLength == 0 ||
            !localNameEquals(xml + nameBegin, nameLength, wanted)) {
            cursor = openEnd + 1;
            continue;
        }

        size_t beforeEnd = openEnd;
        while (beforeEnd > nameStart &&
               isspace(static_cast<unsigned char>(xml[beforeEnd - 1]))) {
            --beforeEnd;
        }
        element = XmlElement{};
        element.openStart = openStart;
        element.openEnd = openEnd;
        element.contentStart = openEnd + 1;
        element.selfClosing = beforeEnd > nameStart &&
                              xml[beforeEnd - 1] == '/';
        if (element.selfClosing) {
            element.contentEnd = element.contentStart;
            element.after = openEnd + 1;
            return true;
        }

        size_t closeCursor = openEnd + 1;
        while (closeCursor < limit) {
            const char *close = static_cast<const char *>(
                memchr(xml + closeCursor, '<', limit - closeCursor));
            if (close == nullptr) {
                return false;
            }
            const size_t closeStart = static_cast<size_t>(close - xml);
            if (closeStart + 2 < limit && xml[closeStart + 1] == '/') {
                size_t closeName = closeStart + 2;
                const size_t closeNameBegin = closeName;
                while (closeName < limit && isNameByte(xml[closeName])) {
                    ++closeName;
                }
                if (localNameEquals(xml + closeNameBegin,
                                    closeName - closeNameBegin, wanted)) {
                    const size_t closeEnd =
                        findTagEnd(xml, closeName, limit);
                    if (closeEnd >= limit) {
                        return false;
                    }
                    element.contentEnd = closeStart;
                    element.after = closeEnd + 1;
                    return true;
                }
            }
            const size_t skipped = findTagEnd(xml, closeStart + 1, limit);
            closeCursor = skipped < limit ? skipped + 1 : limit;
        }
        return false;
    }
    return false;
}

bool appendByte(char *target, size_t capacity, size_t &used, char value) {
    if (used + 1 >= capacity) {
        return false;
    }
    target[used++] = value;
    target[used] = '\0';
    return true;
}

bool appendCodePoint(char *target, size_t capacity, size_t &used,
                     uint32_t codePoint) {
    if (codePoint <= 0x7FU) {
        return appendByte(target, capacity, used,
                          static_cast<char>(codePoint));
    }
    if (codePoint <= 0x7FFU) {
        return appendByte(target, capacity, used,
                          static_cast<char>(0xC0U | (codePoint >> 6U))) &&
               appendByte(target, capacity, used,
                          static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    if (codePoint <= 0xFFFFU) {
        return appendByte(target, capacity, used,
                          static_cast<char>(0xE0U | (codePoint >> 12U))) &&
               appendByte(target, capacity, used,
                          static_cast<char>(0x80U |
                                            ((codePoint >> 6U) & 0x3FU))) &&
               appendByte(target, capacity, used,
                          static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    if (codePoint <= 0x10FFFFU) {
        return appendByte(target, capacity, used,
                          static_cast<char>(0xF0U | (codePoint >> 18U))) &&
               appendByte(target, capacity, used,
                          static_cast<char>(0x80U |
                                            ((codePoint >> 12U) & 0x3FU))) &&
               appendByte(target, capacity, used,
                          static_cast<char>(0x80U |
                                            ((codePoint >> 6U) & 0x3FU))) &&
               appendByte(target, capacity, used,
                          static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    return appendByte(target, capacity, used, '?');
}

bool decodeEntity(const char *source, size_t length, uint32_t &codePoint) {
    if (length == 3 && strncmp(source, "amp", 3) == 0) {
        codePoint = '&';
    } else if (length == 2 && strncmp(source, "lt", 2) == 0) {
        codePoint = '<';
    } else if (length == 2 && strncmp(source, "gt", 2) == 0) {
        codePoint = '>';
    } else if (length == 4 && strncmp(source, "quot", 4) == 0) {
        codePoint = '"';
    } else if (length == 4 && strncmp(source, "apos", 4) == 0) {
        codePoint = '\'';
    } else if (length == 4 && strncmp(source, "nbsp", 4) == 0) {
        codePoint = ' ';
    } else if (length >= 2 && source[0] == '#') {
        char buffer[16]{};
        const bool hexadecimal = source[1] == 'x' || source[1] == 'X';
        const size_t numberStart = hexadecimal ? 2 : 1;
        const size_t numberLength = length - numberStart;
        if (numberLength == 0 || numberLength >= sizeof(buffer)) {
            return false;
        }
        memcpy(buffer, source + numberStart, numberLength);
        char *end = nullptr;
        codePoint = strtoul(buffer, &end, hexadecimal ? 16 : 10);
        if (end == buffer || *end != '\0') {
            return false;
        }
    } else {
        return false;
    }
    return true;
}

void decodeText(const char *source, size_t length, char *target,
                size_t capacity, bool stripTags) {
    if (target == nullptr || capacity == 0) {
        return;
    }
    target[0] = '\0';
    size_t used = 0;
    bool pendingSpace = false;
    size_t cursor = 0;
    while (cursor < length && used + 1 < capacity) {
        if (stripTags && source[cursor] == '<') {
            if (cursor + 9 <= length &&
                strncmp(source + cursor, "<![CDATA[", 9) == 0) {
                const char *end = strstr(source + cursor + 9, "]]>");
                if (end == nullptr || end > source + length) {
                    break;
                }
                const size_t innerLength =
                    static_cast<size_t>(end - (source + cursor + 9));
                char inner[256]{};
                decodeText(source + cursor + 9, innerLength, inner,
                           sizeof(inner), true);
                for (size_t index = 0; inner[index] != '\0'; ++index) {
                    if (!appendByte(target, capacity, used, inner[index])) {
                        break;
                    }
                }
                cursor = static_cast<size_t>(end - source) + 3;
                continue;
            }
            const char *end = static_cast<const char *>(
                memchr(source + cursor, '>', length - cursor));
            if (end == nullptr) {
                break;
            }
            pendingSpace = used > 0;
            cursor = static_cast<size_t>(end - source) + 1;
            continue;
        }
        if (source[cursor] == '&') {
            const char *end = static_cast<const char *>(
                memchr(source + cursor + 1, ';', length - cursor - 1));
            if (end != nullptr) {
                uint32_t codePoint = 0;
                const size_t entityLength =
                    static_cast<size_t>(end - (source + cursor + 1));
                if (decodeEntity(source + cursor + 1, entityLength,
                                 codePoint)) {
                    if (codePoint == ' ') {
                        pendingSpace = used > 0;
                    } else {
                        if (pendingSpace && used > 0) {
                            appendByte(target, capacity, used, ' ');
                        }
                        pendingSpace = false;
                        appendCodePoint(target, capacity, used, codePoint);
                    }
                    cursor = static_cast<size_t>(end - source) + 1;
                    continue;
                }
            }
        }
        const unsigned char value =
            static_cast<unsigned char>(source[cursor++]);
        if (isspace(value)) {
            pendingSpace = used > 0;
            continue;
        }
        if (pendingSpace && used > 0) {
            appendByte(target, capacity, used, ' ');
        }
        pendingSpace = false;
        appendByte(target, capacity, used, static_cast<char>(value));
    }
}

bool copyAttribute(const char *xml, const XmlElement &element,
                   const char *wanted, char *target, size_t capacity) {
    if (target == nullptr || capacity == 0) {
        return false;
    }
    target[0] = '\0';
    size_t cursor = element.openStart + 1;
    while (cursor < element.openEnd && isNameByte(xml[cursor])) {
        ++cursor;
    }
    while (cursor < element.openEnd) {
        while (cursor < element.openEnd &&
               isspace(static_cast<unsigned char>(xml[cursor]))) {
            ++cursor;
        }
        if (cursor >= element.openEnd || xml[cursor] == '/') {
            break;
        }
        const size_t nameStart = cursor;
        while (cursor < element.openEnd && isNameByte(xml[cursor])) {
            ++cursor;
        }
        const size_t nameLength = cursor - nameStart;
        while (cursor < element.openEnd &&
               isspace(static_cast<unsigned char>(xml[cursor]))) {
            ++cursor;
        }
        if (cursor >= element.openEnd || xml[cursor] != '=') {
            while (cursor < element.openEnd &&
                   !isspace(static_cast<unsigned char>(xml[cursor]))) {
                ++cursor;
            }
            continue;
        }
        ++cursor;
        while (cursor < element.openEnd &&
               isspace(static_cast<unsigned char>(xml[cursor]))) {
            ++cursor;
        }
        if (cursor >= element.openEnd ||
            (xml[cursor] != '\'' && xml[cursor] != '"')) {
            continue;
        }
        const char quote = xml[cursor++];
        const size_t valueStart = cursor;
        while (cursor < element.openEnd && xml[cursor] != quote) {
            ++cursor;
        }
        if (nameLength == strlen(wanted) &&
            strncmp(xml + nameStart, wanted, nameLength) == 0) {
            decodeText(xml + valueStart, cursor - valueStart, target, capacity,
                       false);
            return true;
        }
        if (cursor < element.openEnd) {
            ++cursor;
        }
    }
    return false;
}

void copyElementText(const char *xml, size_t length, const char *wanted,
                     size_t from, size_t limit, char *target,
                     size_t capacity) {
    XmlElement element{};
    if (!findElement(xml, length, wanted, from, limit, element)) {
        if (target != nullptr && capacity > 0) {
            target[0] = '\0';
        }
        return;
    }
    decodeText(xml + element.contentStart,
               element.contentEnd - element.contentStart, target, capacity,
               true);
}

void assignFeedLink(OpdsFeed &feed, const char *rel, const char *href) {
    if (strcmp(rel, "self") == 0) {
        snprintf(feed.selfHref, sizeof(feed.selfHref), "%s", href);
    } else if (strcmp(rel, "start") == 0) {
        snprintf(feed.startHref, sizeof(feed.startHref), "%s", href);
    } else if (strcmp(rel, "previous") == 0) {
        snprintf(feed.previousHref, sizeof(feed.previousHref), "%s", href);
    } else if (strcmp(rel, "next") == 0) {
        snprintf(feed.nextHref, sizeof(feed.nextHref), "%s", href);
    } else if (strcmp(rel, "search") == 0) {
        snprintf(feed.searchHref, sizeof(feed.searchHref), "%s", href);
    }
}

}  // namespace

bool OpdsParser::parse(const String &xmlString, OpdsFeed &feed) {
    // OpdsFeed intentionally lives in PSRAM. A value-initialized temporary is
    // larger than the Arduino loop task stack, so clear it in place.
    memset(&feed, 0, sizeof(feed));
    const char *xml = xmlString.c_str();
    const size_t length = xmlString.length();
    if (xml == nullptr || length == 0) {
        setError(feed.error, sizeof(feed.error), "empty-document");
        return false;
    }

    XmlElement feedElement{};
    if (!findElement(xml, length, "feed", 0, length, feedElement)) {
        setError(feed.error, sizeof(feed.error), "missing-feed");
        return false;
    }

    XmlElement firstEntry{};
    const bool hasEntry = findElement(xml, length, "entry",
                                      feedElement.contentStart,
                                      feedElement.contentEnd, firstEntry);
    const size_t feedMetadataEnd =
        hasEntry ? firstEntry.openStart : feedElement.contentEnd;
    copyElementText(xml, length, "title", feedElement.contentStart,
                    feedMetadataEnd, feed.title, sizeof(feed.title));

    size_t linkCursor = feedElement.contentStart;
    XmlElement link{};
    while (findElement(xml, length, "link", linkCursor, feedMetadataEnd,
                       link)) {
        char rel[96]{};
        char href[kOpdsUrlCapacity]{};
        copyAttribute(xml, link, "rel", rel, sizeof(rel));
        copyAttribute(xml, link, "href", href, sizeof(href));
        if (rel[0] != '\0' && href[0] != '\0') {
            assignFeedLink(feed, rel, href);
        }
        linkCursor = link.after;
    }

    size_t entryCursor = feedElement.contentStart;
    XmlElement entryElement{};
    while (findElement(xml, length, "entry", entryCursor,
                       feedElement.contentEnd, entryElement)) {
        entryCursor = entryElement.after;
        if (feed.entryCount >= kOpdsMaxEntries) {
            feed.truncated = true;
            continue;
        }
        OpdsEntry &entry = feed.entries[feed.entryCount];
        copyElementText(xml, length, "title", entryElement.contentStart,
                        entryElement.contentEnd, entry.title,
                        sizeof(entry.title));
        copyElementText(xml, length, "id", entryElement.contentStart,
                        entryElement.contentEnd, entry.id, sizeof(entry.id));
        copyElementText(xml, length, "content", entryElement.contentStart,
                        entryElement.contentEnd, entry.summary,
                        sizeof(entry.summary));
        if (entry.summary[0] == '\0') {
            copyElementText(xml, length, "summary", entryElement.contentStart,
                            entryElement.contentEnd, entry.summary,
                            sizeof(entry.summary));
        }
        copyElementText(xml, length, "series", entryElement.contentStart,
                        entryElement.contentEnd, entry.series,
                        sizeof(entry.series));
        copyElementText(xml, length, "series_index",
                        entryElement.contentStart, entryElement.contentEnd,
                        entry.seriesNumber, sizeof(entry.seriesNumber));

        XmlElement authorElement{};
        if (findElement(xml, length, "author", entryElement.contentStart,
                        entryElement.contentEnd, authorElement)) {
            copyElementText(xml, length, "name", authorElement.contentStart,
                            authorElement.contentEnd, entry.author,
                            sizeof(entry.author));
        }

        size_t entryLinkCursor = entryElement.contentStart;
        XmlElement entryLink{};
        while (findElement(xml, length, "link", entryLinkCursor,
                           entryElement.contentEnd, entryLink)) {
            char rel[128]{};
            char href[kOpdsUrlCapacity]{};
            copyAttribute(xml, entryLink, "rel", rel, sizeof(rel));
            copyAttribute(xml, entryLink, "href", href, sizeof(href));
            if (strcmp(rel, "subsection") == 0 && href[0] != '\0') {
                snprintf(entry.href, sizeof(entry.href), "%s", href);
                entry.kind = OpdsEntryKind::Navigation;
            } else if (strncmp(rel, "http://opds-spec.org/acquisition",
                               strlen("http://opds-spec.org/acquisition")) ==
                           0 &&
                       href[0] != '\0') {
                snprintf(entry.acquisitionHref,
                         sizeof(entry.acquisitionHref), "%s", href);
                entry.kind = OpdsEntryKind::Book;
            } else if (strcmp(rel, "http://opds-spec.org/image") == 0 &&
                       href[0] != '\0') {
                snprintf(entry.coverHref, sizeof(entry.coverHref), "%s",
                         href);
            } else if (strcmp(rel,
                              "http://opds-spec.org/image/thumbnail") == 0 &&
                       href[0] != '\0') {
                snprintf(entry.thumbnailHref, sizeof(entry.thumbnailHref),
                         "%s", href);
            } else if (strcmp(rel, "urn:abyss-reader:author") == 0 &&
                       href[0] != '\0') {
                snprintf(entry.authorHref, sizeof(entry.authorHref), "%s",
                         href);
            } else if (strcmp(rel, "urn:abyss-reader:series") == 0 &&
                       href[0] != '\0') {
                snprintf(entry.seriesHref, sizeof(entry.seriesHref), "%s",
                         href);
            }
            entryLinkCursor = entryLink.after;
        }
        if (entry.title[0] == '\0') {
            snprintf(entry.title, sizeof(entry.title), "БЕЗ НАЗВАНИЯ");
        }
        ++feed.entryCount;
    }

    if (feed.title[0] == '\0') {
        snprintf(feed.title, sizeof(feed.title), "КАТАЛОГ");
    }
    setError(feed.error, sizeof(feed.error), "");
    return true;
}
