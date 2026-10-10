"""Translate FB2 cache records to the web reader's text-node anchors.

Offsets on the device are UTF-8 bytes; DOM offsets are UTF-16 code units.
Page numbers and percentages are deliberately never used as text positions.
"""
from html.parser import HTMLParser
import re


def normalize(value):
    return re.sub(r'[\x00-\x20\x7f]+', ' ', value).strip(' ')


def annotate(root):
    """Match the first-body record numbering of FB2 decoder 4."""
    counter = 0
    def visit(node):
        nonlocal counter
        name = node.tag.rsplit('}', 1)[-1]
        if name in ('section', 'empty-line', 'image'):
            counter += 1
        for child in node:
            visit(child)
        if name in ('p', 'subtitle', 'text-author') and normalize(''.join(node.itertext())):
            counter += 1
            node.set('data-abyss-record', str(counter))
    body = next((n for n in root if n.tag.rsplit('}', 1)[-1] == 'body'), None)
    if body is not None:
        visit(body)


def units(value):
    return len(value.encode('utf-16-le')) // 2


class _Blocks(HTMLParser):
    def __init__(self, fragment):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.blocks = []
        self.serial = 0
        self.feed(fragment)

    def handle_starttag(self, tag, attrs):
        self.serial += 1
        attrs = dict(attrs)
        parent = self.stack[-1] if self.stack else ('', 0, None)
        identity = self.serial if tag in ('p', 'h1', 'h2', 'h3', 'h4', 'blockquote', 'li') else parent[1]
        record = int(attrs['data-abyss-record']) if 'data-abyss-record' in attrs else parent[2]
        if tag not in ('br', 'img', 'hr', 'input', 'meta', 'link'):
            self.stack.append((tag, identity, record))

    def handle_endtag(self, tag):
        for index in range(len(self.stack)-1, -1, -1):
            if self.stack[index][0] == tag:
                del self.stack[index:]
                break

    def handle_data(self, text):
        if not text:
            return
        _, identity, record = self.stack[-1] if self.stack else ('', 0, None)
        if not self.blocks or self.blocks[-1]['id'] != identity:
            self.blocks.append(dict(id=identity, text='', nodes=[]))
        block = self.blocks[-1]
        block['nodes'].append((record, units(block['text']), text))
        block['text'] += text


class AnchorMap:
    def __init__(self, chapters):
        self.blocks = [_Blocks(chapter['html']).blocks for chapter in chapters]
        self.records = {}
        for chapter, blocks in enumerate(self.blocks):
            for block, item in enumerate(blocks):
                for record, start, text in item['nodes']:
                    if record is not None:
                        value = self.records.setdefault(record, dict(text='', nodes=[]))
                        value['nodes'].append((len(value['text']), chapter, block, start, text))
                        value['text'] += text

    @staticmethod
    def boundaries(raw):
        # Each emitted character points back to its first raw character.
        result = []; indexes = []; pending = None
        for index, char in enumerate(raw):
            if ord(char) <= 32 or ord(char) == 127:
                if result and pending is None: pending = index
                continue
            if pending is not None:
                result.append(' '); indexes.append(pending); pending = None
            result.append(char); indexes.append(index)
        return ''.join(result), indexes + [len(raw)]

    def to_web(self, record, byte):
        value = self.records.get(record)
        if not value or type(byte) is not int or byte < 0:
            raise ValueError('Unmapped text position')
        normalized, indexes = self.boundaries(value['text'])
        encoded = normalized.encode('utf-8')
        if byte > len(encoded): raise ValueError('Text position out of range')
        # Reject middle-of-codepoint offsets instead of shifting the bookmark.
        count = len(encoded[:byte].decode('utf-8'))
        raw = indexes[count]
        nodes = value['nodes']
        node = next((n for n in nodes if n[0] <= raw < n[0]+len(n[4])), nodes[-1])
        start, chapter, block, char, text = node
        return chapter, dict(block=block, char=char+units(text[:max(0, raw-start)]))

    def to_native(self, chapter, anchor):
        if type(chapter) is not int or not isinstance(anchor, dict):
            raise ValueError('No exact text anchor')
        try:
            block = self.blocks[chapter][anchor['block']]
            char = anchor['char']
            if chapter < 0 or anchor['block'] < 0 or type(char) is not int or not 0 <= char <= units(block['text']):
                raise ValueError('Text position out of range')
            node = next((n for n in block['nodes'] if n[1] <= char < n[1]+units(n[2])), block['nodes'][-1])
            record, start, text = node
            value = self.records[record]
            fragment = text.encode('utf-16-le')[:(char-start)*2].decode('utf-16-le')
            matching = next(n for n in value['nodes'] if n[1:4] == (chapter, anchor['block'], start))
            raw = matching[0] + len(fragment)
            normalized, indexes = self.boundaries(value['text'])
            count = sum(index < raw for index in indexes[:-1])
            return dict(record=record, byte=len(normalized[:count].encode('utf-8')))
        except (IndexError, KeyError, TypeError, UnicodeError) as error:
            raise ValueError('Unmapped text position') from error
