"""Shared, bounded book extraction and conservative repairs of legacy FB2 XML."""
import bz2
import gzip
import io
import lzma
import re
import struct
import zipfile
import xml.etree.ElementTree as ET
from html.entities import name2codepoint
from pathlib import PurePosixPath

MAX_DOWNLOAD_BYTES = 128 * 1024 * 1024
MAX_DOCUMENT_BYTES = 128 * 1024 * 1024
MAX_ARCHIVE_ENTRIES = 256
FB2_NS = "http://www.gribuser.ru/xml/fictionbook/2.0"
XLINK_NS = "http://www.w3.org/1999/xlink"


class BookFormatError(ValueError):
    pass


class BookSizeError(BookFormatError):
    pass


def bounded_read(stream, limit):
    data = stream.read(limit + 1)
    if len(data) > limit:
        raise BookSizeError("Распакованная книга превышает лимит 128 МБ")
    return data


def extract_book(payload, *, max_archive_bytes=MAX_DOWNLOAD_BYTES,
                 max_document_bytes=MAX_DOCUMENT_BYTES, max_entries=MAX_ARCHIVE_ENTRIES):
    if len(payload) > max_archive_bytes:
        raise BookSizeError("Файл загрузки превышает лимит 128 МБ")
    count = 1
    try:
        if payload.startswith(b"PK"):
            with zipfile.ZipFile(io.BytesIO(payload)) as archive:
                infos = archive.infolist()
                count = len(infos)
                if not 1 <= count <= max_entries:
                    raise BookFormatError("Слишком много файлов в ZIP")
                candidates = []
                total = 0
                for member in infos:
                    path = PurePosixPath(member.filename.replace('\\', '/'))
                    if (path.is_absolute() or '..' in path.parts or
                        re.match(r'^[A-Za-z]:', str(path)) or '\x00' in member.filename or
                        ((member.external_attr >> 16) & 0o170000) == 0o120000):
                        raise BookFormatError("Небезопасный путь или ссылка внутри ZIP")
                    if member.flag_bits & 1:
                        raise BookFormatError("ZIP защищён паролем")
                    total += member.file_size
                    if total > max_document_bytes:
                        raise BookSizeError("Содержимое ZIP превышает лимит 128 МБ")
                    if member.file_size > max(member.compress_size, 1) * 500:
                        raise BookFormatError("Превышен защитный коэффициент сжатия ZIP")
                    # AppleDouble sidecars are not second books, even when ending in .fb2.
                    if member.is_dir() or '__MACOSX' in path.parts or path.name.startswith('._'):
                        continue
                    if path.suffix.lower() == '.fb2':
                        candidates.append(member)
                if len(candidates) != 1:
                    raise BookFormatError("В ZIP должен быть один FB2; служебные файлы и папки разрешены")
                member = candidates[0]
                if member.compress_type == zipfile.ZIP_LZMA:
                    # ZIP-LZMA otherwise accepts attacker-sized decoder dictionaries.
                    start = member.header_offset
                    name_size, extra_size = struct.unpack_from('<HH', payload, start + 26)
                    data_start = start + 30 + name_size + extra_size
                    if len(payload) < data_start + 9 or struct.unpack_from('<H', payload, data_start + 2)[0] != 5:
                        raise BookFormatError("Некорректный заголовок ZIP-LZMA")
                    if struct.unpack_from('<I', payload, data_start + 5)[0] > 64 * 1024 * 1024:
                        raise BookFormatError("Слишком большой словарь ZIP-LZMA")
                with archive.open(candidates[0]) as stream:
                    data = bounded_read(stream, max_document_bytes)
        elif payload.startswith(b'\x1f\x8b'):
            with gzip.GzipFile(fileobj=io.BytesIO(payload)) as stream:
                data = bounded_read(stream, max_document_bytes)
        elif payload.startswith(b'BZh'):
            with bz2.BZ2File(io.BytesIO(payload)) as stream:
                data = bounded_read(stream, max_document_bytes)
        elif payload.startswith(b'\xfd7zXZ\x00'):
            # Bound the decoder dictionary as well as the output.
            decoder = lzma.LZMADecompressor(memlimit=64 * 1024 * 1024)
            data = decoder.decompress(payload, max_length=max_document_bytes + 1)
            if len(data) > max_document_bytes:
                raise BookSizeError("Распакованная книга превышает лимит 128 МБ")
            if not decoder.eof or decoder.unused_data:
                raise BookFormatError("Неполный или составной XZ-файл")
        else:
            data = payload
        if len(data) > max_document_bytes:
            raise BookSizeError("Распакованная книга превышает лимит 128 МБ")
        return data, count
    except (zipfile.BadZipFile, RuntimeError, NotImplementedError, OSError, EOFError, struct.error) as error:
        raise BookFormatError("Повреждённый архив или неподдерживаемый метод сжатия") from error


def parse_xml(payload, *, max_bytes=MAX_DOCUMENT_BYTES):
    if not payload or len(payload) > max_bytes:
        raise BookSizeError("Пустой FB2 или превышен лимит 128 МБ")
    if re.search(rb'<!\s*(DOCTYPE|ENTITY)', payload.replace(b'\x00', b''), re.I):
        raise BookFormatError("FB2 с DTD или внешними сущностями не поддерживается")
    parser = ET.XMLPullParser(events=('start', 'end'))
    count = depth = 0
    root = None
    for offset in range(0, len(payload), 32768):
        parser.feed(payload[offset:offset + 32768])
        for event, node in parser.read_events():
            if event == 'start':
                if root is None:
                    root = node
                depth += 1
                count += 1
                if depth > 128 or count > 500000:
                    raise BookFormatError("Слишком сложная структура FB2")
            else:
                depth -= 1
    parser.close()
    if root is None or root.tag not in ('FictionBook', '{' + FB2_NS + '}FictionBook'):
        raise BookFormatError("Ожидалась книга FB2, а не HTML/PDF или другой формат")
    return root


def repair_inline_paragraphs(text):
    """Split inline formatting crossing paragraph boundaries, never recover text.

    E.g. <p><strong>A</p><p>B</strong></p>. Only explicitly closed inline
    spans can continue into another paragraph; structural damage still fails.
    All non-markup bytes and all paragraph boundaries remain unchanged.
    """
    tokens = re.split(r'(<!\[CDATA\[.*?\]\]>|<!--.*?-->|<[^<>"\']*(?:"[^"]*"[^<>"\']*|\'[^\']*\'[^<>"\']*)*>)', text, flags=re.S)
    inline = {'strong', 'emphasis', 'strikethrough', 'sub', 'sup', 'code'}
    paragraphs = {'p', 'v', 'subtitle'}
    stack, pending, output = [], [], []
    changed = False
    for token_index, token in enumerate(tokens):
        match = re.fullmatch(r'<(/?)([\w:.-]+)(?:\s[^<>]*)?\s*(/?)>', token, re.S)
        if not match:
            if pending and token.strip() and not token.startswith('<!--'):
                return text, False
            output.append(token)
            continue
        closing, name, single = match.groups()
        local = name.rsplit(':', 1)[-1]
        if closing:
            if pending:
                if pending[-1][0] != name:
                    return text, False
                pending.pop()  # It was already closed before </p>.
                changed = True
                continue
            if not stack:
                return text, False
            if stack[-1][0] != name:
                # An orphan closing formatting tag has no text or structural
                # meaning. Ignore it only when that span is nowhere open.
                if local in inline and not any(n == name for n, _ in stack):
                    changed = True
                    continue
                if local not in paragraphs:
                    return text, False
                carried = []
                while stack and stack[-1][0].rsplit(':', 1)[-1] in inline:
                    carried.append(stack.pop())
                if not stack or stack[-1][0] != name or not carried:
                    return text, False
                output.extend('</' + n + '>' for n, _ in carried)
                pending = list(reversed(carried))
                changed = True
            stack.pop()
            output.append(token)
        else:
            if pending and (local not in paragraphs or single):
                return text, False
            output.append(token)
            if not single and not token.endswith('/>'):
                stack.append((name, token))
            if pending:
                for entry in pending:
                    output.append(entry[1])
                    stack.append(entry)
                pending = []
        if len(stack) > 128:
            return text, False
    return (''.join(output), changed) if not stack and not pending else (text, False)


def normalize_fb2(payload, *, max_bytes=MAX_DOCUMENT_BYTES):
    """Never recover missing closing tags or truncated text. Preserve originals at publication."""
    try:
        parse_xml(payload, max_bytes=max_bytes)
        return payload, ()
    except ET.ParseError:
        pass
    encoding = re.search(rb'encoding\s*=\s*[\'"]([^\'"]+)', payload[:256], re.I)
    codec = encoding[1].decode('ascii') if encoding else 'utf-8-sig'
    if payload.startswith((b'\xff\xfe', b'\xfe\xff')):
        codec = 'utf-16'
    if codec.lower().replace('_', '-') not in {'utf-8', 'utf-8-sig', 'utf-16', 'utf-16le',
        'utf-16be', 'windows-1251', 'cp1251', 'koi8-r', 'iso-8859-1'}:
        raise BookFormatError("Неизвестная кодировка FB2")
    try:
        text = payload.decode(codec)
    except (UnicodeError, LookupError) as error:
        raise BookFormatError("Байты FB2 не соответствуют заявленной кодировке") from error
    repairs = []
    if text.lstrip('\ufeff \r\n\t').startswith('<?xml') and not text.startswith('<?xml'):
        text = text.lstrip('\ufeff \r\n\t')
        repairs.append('xml-leading-whitespace')
    parts = re.split(r'(<!\[CDATA\[.*?\]\]>|<!--.*?-->)', text, flags=re.S)
    for i in range(0, len(parts), 2):
        old = parts[i]
        value = re.sub(r'&([A-Za-z][A-Za-z0-9]+);',
                       lambda m: '&#' + str(name2codepoint[m[1]]) + ';' if m[1] not in {'amp','lt','gt','apos','quot'} and m[1] in name2codepoint else m[0], old)
        value = re.sub(r'&(?!amp;|lt;|gt;|apos;|quot;|#\d+;|#x[0-9a-fA-F]+;)', '&amp;', value)
        if value != old:
            repairs.append('xml-entities')
        old = value
        def attributes(match):
            tokens = re.split(r'("[^"]*"|\'[^\']*\')', match[0])
            for index in range(0, len(tokens) - 1, 2):
                tokens[index] = re.sub(r'(\s[\w:.-]+)\s*={2,}\s*$', r'\1=', tokens[index])
            return ''.join(tokens)
        value = re.sub(r'<[A-Za-z_][^<>]*>', attributes, value)
        if value != old:
            repairs.append('xml-attribute-equals')
        parts[i] = value
    text = ''.join(parts)
    # Only the conventional FB2 link prefixes; arbitrary namespaces are never invented.
    for prefix in ('l', 'xlink'):
        if re.search(r'\s' + prefix + r':href\s*=', text) and not re.search(r'xmlns:' + prefix + r'\s*=', text):
            text, changed = re.subn(r'<FictionBook(?=[\s>])', '<FictionBook xmlns:' + prefix + '="' + XLINK_NS + '"', text, count=1)
            if changed:
                repairs.append('xml-xlink-namespace')
    # A normalized document is canonical UTF-8, without lossy decode/recovery.
    text = re.sub(r'(<\?xml[^>]*encoding\s*=\s*)[\'"][^\'"]+[\'"]', r'\1"UTF-8"', text, count=1, flags=re.I)
    result = text.encode('utf-8')
    try:
        parse_xml(result, max_bytes=max_bytes)
    except ET.ParseError as error:
        fixed, changed = repair_inline_paragraphs(text)
        if not changed:
            raise BookFormatError("FB2 повреждён: автоматическое исправление без потери текста невозможно") from error
        result = fixed.encode('utf-8')
        try:
            parse_xml(result, max_bytes=max_bytes)
        except ET.ParseError as second:
            raise BookFormatError("FB2 повреждён: автоматическое исправление без потери текста невозможно") from second
        repairs.append('xml-inline-paragraphs')
    return result, tuple(dict.fromkeys(repairs))
