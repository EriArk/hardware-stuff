"""Bounded, offline EPUB 2/3 -> FB2 reading copy; never execute book HTML/CSS.

The original EPUB remains the downloadable source. Spine order, not ZIP order,
defines the text. No extraction to disk, network requests or external entities.
"""
import base64
import hashlib
import io
import posixpath
import re
import zipfile
import xml.etree.ElementTree as ET
from html import unescape
from urllib.parse import unquote, urlsplit

from octofox_library.book_formats import BookFormatError, BookSizeError, FB2_NS, XLINK_NS, MAX_DOCUMENT_BYTES

EPUB_MIME = 'application/epub+zip'
MAX_ENTRIES = 4096


def is_epub(data):
    if not data.startswith(b'PK'):
        return False
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            return 'META-INF/container.xml' in archive.namelist()
    except zipfile.BadZipFile:
        return False


def xml(data):
    # EPUB XHTML often contains the standard HTML DOCTYPE; do not load it.
    clean = data.replace(b'\x00', b'')
    if re.search(rb'<!\s*ENTITY|<!\s*DOCTYPE[^>]*\[', clean, re.I):
        raise BookFormatError('EPUB с XML-сущностями не поддерживается')
    if b'\x00' in data:
        data = data.decode('utf-16').encode('utf-8')
        data = re.sub(rb'encoding=["\'][^"\']+["\']', b'encoding="utf-8"', data, count=1)
    data = re.sub(rb'<!DOCTYPE[^>]*>', b'', data, flags=re.I)
    parser = ET.XMLPullParser(events=('start', 'end'))
    depth = nodes = 0
    root = None
    try:
        for start in range(0, len(data), 65536):
            parser.feed(data[start:start + 65536])
            for event, node in parser.read_events():
                if event == 'start':
                    depth += 1
                    nodes += 1
                    if root is None:
                        root = node
                    if depth > 96 or nodes > 250000:
                        raise BookFormatError('Слишком сложный документ EPUB')
                else:
                    depth -= 1
        parser.close()
    except ET.ParseError as error:
        raise BookFormatError('Повреждён XML внутри EPUB') from error
    if root is None:
        raise BookFormatError('Пустой документ EPUB')
    return root


def resource(base, href):
    value = urlsplit(href)
    path = unquote(value.path)
    if value.scheme or value.netloc or value.query or '\\' in path or '\x00' in path or path.startswith('/'):
        raise BookFormatError('Внешние ресурсы EPUB не поддерживаются')
    result = posixpath.normpath(posixpath.join(posixpath.dirname(base), path)) if path else base
    if result in {'.', '..'} or result.startswith('../') or ':' in result:
        raise BookFormatError('Небезопасный путь внутри EPUB')
    return result


def epub_to_fb2(payload, *, max_bytes=MAX_DOCUMENT_BYTES):
    if len(payload) > max_bytes:
        raise BookSizeError('EPUB превышает допустимый размер')
    try:
        with zipfile.ZipFile(io.BytesIO(payload)) as archive:
            return _convert(archive, max_bytes)
    except (zipfile.BadZipFile, KeyError, NotImplementedError, RuntimeError, UnicodeError) as error:
        raise BookFormatError('Повреждённый или неподдерживаемый EPUB') from error


def _convert(archive, limit):
    members = archive.infolist()
    if not 1 <= len(members) <= MAX_ENTRIES:
        raise BookFormatError('Слишком много файлов внутри EPUB')
    names, total = set(), 0
    for member in members:
        name = member.filename
        if (name in names or name.startswith('/') or '\\' in name or ':' in name or
                '..' in name.split('/') or '\x00' in name or
                ((member.external_attr >> 16) & 0o170000) == 0o120000):
            raise BookFormatError('Небезопасный или повторный путь внутри EPUB')
        names.add(name)
        if member.flag_bits & 1 or member.compress_type not in {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}:
            raise BookFormatError('Зашифрованный или неподдерживаемый EPUB')
        total += member.file_size
        if total > limit:
            raise BookSizeError('Распакованный EPUB превышает лимит 128 МБ')
        if member.file_size > max(1, member.compress_size) * 500:
            raise BookFormatError('Слишком высокая степень сжатия EPUB')

    def read(name):
        with archive.open(name) as stream:
            result = stream.read(limit + 1)
        if len(result) > limit:
            raise BookSizeError('Ресурс EPUB превышает допустимый размер')
        return result

    if read('mimetype').strip() != EPUB_MIME.encode():
        raise BookFormatError('Это не книга EPUB')
    container = xml(read('META-INF/container.xml'))
    rootfile = container.find('./{*}rootfiles/{*}rootfile')
    if rootfile is None:
        raise BookFormatError('В EPUB нет описания книги')
    package_path = resource('', rootfile.get('full-path', ''))
    package = xml(read(package_path))
    meta, manifest, spine = (package.find('{*}' + name) for name in ('metadata', 'manifest', 'spine'))
    if meta is None or manifest is None or spine is None:
        raise BookFormatError('В EPUB нет метаданных или порядка чтения')
    items = {item.get('id'): item for item in manifest}
    if len(items) != len(manifest):
        raise BookFormatError('Повторные идентификаторы в EPUB')
    encrypted = set()
    if 'META-INF/encryption.xml' in names:
        for node in xml(read('META-INF/encryption.xml')).iter():
            if node.tag.rsplit('}', 1)[-1] == 'CipherReference':
                encrypted.add(resource('', node.get('URI', '')))
    if any(n.get('property') == 'rendition:layout' and n.text == 'pre-paginated' for n in meta):
        raise BookFormatError('EPUB с фиксированной вёрсткой пока не поддерживается; нужен текстовый EPUB')

    def element(parent, tag, text=None, **attrs):
        node = ET.SubElement(parent, tag, attrs)
        node.text = text
        return node

    def values(name):
        return [' '.join(n.itertext()).strip() for n in meta if n.tag.rsplit('}', 1)[-1] == name]

    # The ESP32 probes for literal <FictionBook; don't mutate ElementTree's
    # process-global namespace registry (also used concurrently by OPDS).
    root = ET.Element('FictionBook', {'xmlns': FB2_NS, 'xmlns:xlink': XLINK_NS})
    info = element(element(root, 'description'), 'title-info')
    for genre in values('subject')[:100]:
        element(info, 'genre', genre[:200])
    for author in values('creator')[:30]:
        element(element(info, 'author'), 'nickname', author[:500])
    title = next((s for s in values('title') if s), '')
    if not title:
        raise BookFormatError('В EPUB не указано название книги')
    element(info, 'book-title', title[:500])
    description = '\n'.join(values('description'))
    if description:
        # OPF descriptions sometimes contain escaped HTML. Keep text only.
        description = unescape(re.sub('<[^>]*>', ' ', description))
        element(element(info, 'annotation'), 'p', description[:16000])
    series = next((n.get('content', '') for n in meta if n.get('name') == 'calibre:series'), '')
    number = next((n.get('content', '') for n in meta if n.get('name') == 'calibre:series_index'), '')
    for collection in meta:
        if collection.get('property') == 'belongs-to-collection':
            refinements = [n for n in meta if n.get('refines') == '#' + collection.get('id', '')]
            if any(n.get('property') == 'collection-type' and n.text == 'series' for n in refinements):
                series = collection.text or ''
                number = next((n.text or '' for n in refinements if n.get('property') == 'group-position'), '')
                break
    binaries = {}

    def image_ref(base, href):
        path = resource(base, href)
        if path in encrypted:
            raise BookFormatError('EPUB защищён DRM: нужна книга без защиты')
        if path in binaries:
            return binaries[path][0]
        data = read(path)
        from octofox_library.web_uploads import image_type
        mime = image_type(data)
        if not mime or len(data) > 4 * 1024 * 1024:
            return ''
        identity = 'img-' + hashlib.sha256(path.encode()).hexdigest()[:24]
        binaries[path] = (identity, mime, data)
        return identity

    cover_id = next((n.get('content') for n in meta if n.get('name') == 'cover'), '')
    cover = next((n for n in manifest if 'cover-image' in n.get('properties', '').split()), items.get(cover_id))
    if cover is not None:
        identity = image_ref(package_path, cover.get('href', ''))
        if identity:
            element(element(info, 'coverpage'), 'image').set('xlink:href', '#' + identity)
    element(info, 'lang', next(iter(values('language')), 'und'))
    if series:
        element(info, 'sequence', name=series[:500], number=number[:30])

    body = element(root, 'body')
    inline = {'em': 'emphasis', 'i': 'emphasis', 'b': 'strong', 'strong': 'strong',
              's': 'strikethrough', 'del': 'strikethrough', 'sup': 'sup', 'sub': 'sub'}
    blocks = {'p', 'div', 'section', 'article', 'li', 'blockquote', 'pre', 'tr', 'dl', 'dt', 'dd',
              'ul', 'ol', 'table', 'thead', 'tbody', 'tfoot', 'header', 'footer', 'main', 'figure', 'figcaption'}
    blocks.update({'h1', 'h2', 'h3', 'h4', 'h5', 'h6'})
    skip = {'script', 'style', 'head', 'noscript', 'iframe', 'object', 'form', 'nav', 'audio', 'video'}

    def append_text(node, text):
        if text:
            if len(node):
                node[-1].tail = (node[-1].tail or '') + text
            else:
                node.text = (node.text or '') + text

    def content(source, target, chapter_path):
        append_text(target, source.text)
        for child in source:
            tag = child.tag.rsplit('}', 1)[-1].lower()
            if tag not in skip and 'hidden' not in child.attrib and child.get('aria-hidden') != 'true':
                if tag in {'img', 'image'}:
                    href = child.get('src') or child.get('{' + XLINK_NS + '}href') or child.get('href', '')
                    if href:
                        identity = image_ref(chapter_path, href)
                        if identity:
                            element(target, 'image').set('xlink:href', '#' + identity)
                elif tag == 'br':
                    append_text(target, '\n')
                elif tag == 'hr':
                    element(target, 'empty-line')
                elif tag in inline:
                    content(child, element(target, inline[tag]), chapter_path)
                elif re.fullmatch('h[1-6]', tag):
                    content(child, element(target, 'subtitle'), chapter_path)
                elif tag in blocks:
                    # Containers with blocks are flattened, preventing nested <p>.
                    has_blocks = any(n is not child and n.tag.rsplit('}', 1)[-1] in blocks for n in child.iter())
                    dest = target if has_blocks else element(target, 'p')
                    content(child, dest, chapter_path)
                elif tag in {'td', 'th'}:
                    content(child, target, chapter_path)
                    append_text(target, ' | ')
                else:
                    content(child, target, chapter_path)
            append_text(target, child.tail)

    for ref in spine:
        item = items.get(ref.get('idref'))
        if item is None:
            raise BookFormatError('В EPUB отсутствует часть книги из оглавления')
        if item.get('media-type') not in {'application/xhtml+xml', 'text/html'}:
            raise BookFormatError('EPUB содержит неподдерживаемую нетекстовую главу')
        path = resource(package_path, item.get('href', ''))
        if path in encrypted:
            raise BookFormatError('EPUB защищён DRM: нужна книга без защиты')
        document = xml(read(path))
        text_body = document.find('{*}body')
        if text_body is None:
            raise BookFormatError('В главе EPUB отсутствует текст')
        section = element(body, 'section')
        heading = next((n for n in text_body.iter() if n.tag.rsplit('}', 1)[-1] in {'h1', 'h2'}), None)
        # Existing headings supply chapter labels without duplicating the text.
        content(text_body, section, path)
        if heading is not None and len(section) and section[0].tag == 'subtitle':
            paragraph = section[0]
            section.remove(paragraph)
            paragraph.tag = 'p'
            chapter_title = ET.Element('title')
            chapter_title.append(paragraph)
            section.insert(0, chapter_title)
        elif not any((t or '').strip() for t in section.itertext()) and section.find('.//image') is None:
            body.remove(section)
        # Loose text / inline spans under <body> are legal XHTML, but FB2 wants
        # paragraphs. Preserve order without nested <p> or dropped tails.
        children, leading = list(section), section.text
        section.clear()
        pending = None
        if leading and leading.strip():
            pending = element(section, 'p', leading)
        for child in children:
            tail = child.tail
            child.tail = None
            if child.tag in {'title', 'subtitle', 'p', 'empty-line', 'image'}:
                section.append(child)
                pending = None
            else:
                if pending is None:
                    pending = element(section, 'p')
                pending.append(child)
            if tail and tail.strip():
                if pending is None:
                    pending = element(section, 'p')
                append_text(pending, tail)
    if not any(t.strip() for t in body.itertext()):
        raise BookFormatError('В EPUB не найден текст для чтения')
    for identity, mime, data in binaries.values():
        node = element(root, 'binary', base64.b64encode(data).decode(), id=identity)
        node.set('content-type', mime)
    result = ET.tostring(root, encoding='utf-8', xml_declaration=True)
    if len(result) > limit:
        raise BookSizeError('Подготовленная книга превышает лимит 128 МБ')
    return result


def reading_copy(payload):
    return epub_to_fb2(payload) if is_epub(payload) else payload
