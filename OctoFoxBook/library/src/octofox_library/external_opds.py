"""Bounded OPDS 1.x reader. Remote addresses are checked and pinned per request."""
import http.client
import ipaddress
import re
import socket
import ssl
import time
import xml.etree.ElementTree as ET
from urllib.parse import urljoin, urlsplit, urlunsplit, quote

from .web_errors import WebError

ATOM = '{http://www.w3.org/2005/Atom}'
ACQUIRE = 'http://opds-spec.org/acquisition'
FORMATS = {'application/epub+zip': 'EPUB', 'application/fb2+zip': 'FB2 ZIP',
           'application/x-fictionbook+xml': 'FB2', 'application/fb2+xml': 'FB2',
           'application/x-fictionbook+zip': 'FB2 ZIP'}


def valid_url(value):
    if not isinstance(value, str) or len(value) > 4096 or re.search(r'[\x00-\x20\\]', value):
        raise WebError(400, 'Invalid catalog address.')
    try:
        p = urlsplit(value)
        if p.scheme not in ('https', 'http') or not p.hostname or p.username or p.password or p.port == 0:
            raise ValueError()
        host = p.hostname.encode('idna').decode('ascii')
        netloc = ('[' + host + ']' if ':' in host else host) + (f':{p.port}' if p.port else '')
        path = quote(p.path or '/', safe="/%:@!$&'()*+,;=-._~")
        query = quote(p.query, safe="/%?:@!$&'()*+,;=-._~[]")
        return urlunsplit((p.scheme, netloc, path, query, ''))
    except (ValueError, UnicodeError):
        raise WebError(400, 'Invalid catalog address.') from None


def origin(url):
    p = urlsplit(valid_url(url))
    return p.scheme, p.hostname, p.port or (443 if p.scheme == 'https' else 80)


def public_addresses(host, port):
    addresses = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    def allowed(address):
        ip = ipaddress.ip_address(address)
        return ip.is_global and not (ip.is_multicast or ip.is_reserved or getattr(ip, 'is_site_local', False))
    if not addresses or any(not allowed(a[4][0]) for a in addresses):
        raise WebError(400, 'Catalog addresses must be on the public Internet.')
    return addresses


class Fetcher:
    def __init__(self, resolve=public_addresses):
        self.resolve = resolve

    def fetch(self, url, *, root='', authorization='', limit=4 * 1024 * 1024, progress=None):
        deadline = time.monotonic() + 90
        for _ in range(6):
            url = valid_url(url)
            p = urlsplit(url)
            port = p.port or (443 if p.scheme == 'https' else 80)
            addresses = self.resolve(p.hostname, port)
            connection = http.client.HTTPConnection(p.hostname, port, timeout=15)
            sock = None
            try:
                # Connect to the validated numeric address, never resolve the host again.
                for address in addresses[:4]:
                    if time.monotonic() > deadline:
                        raise TimeoutError()
                    try:
                        sock = socket.socket(address[0], socket.SOCK_STREAM)
                        sock.settimeout(15)
                        sock.connect(address[4])
                        break
                    except OSError:
                        if sock:
                            sock.close()
                        sock = None
                if sock is None:
                    raise OSError('connect')
                if p.scheme == 'https':
                    sock = ssl.create_default_context().wrap_socket(sock, server_hostname=p.hostname)
                connection.sock = sock
                headers = {'User-Agent': 'OctoFox-Library/OPDS', 'Accept-Encoding': 'identity',
                           'Accept': 'application/atom+xml, application/epub+zip, */*'}
                if authorization and root and origin(url) == origin(root):
                    headers['Authorization'] = authorization
                connection.request('GET', (p.path or '/') + ('?' + p.query if p.query else ''), headers=headers)
                response = connection.getresponse()
                if response.status in (301, 302, 303, 307, 308):
                    destination = valid_url(urljoin(url, response.getheader('Location', '')))
                    if p.scheme == 'https' and urlsplit(destination).scheme != 'https':
                        raise WebError(400, 'The catalog redirected to an insecure address.')
                    url = destination
                    continue
                if response.status in (401, 403):
                    raise WebError(400, 'The catalog refused access. Check its login and password.')
                if response.status != 200:
                    raise WebError(502, 'The catalog could not provide this page or book.')
                if response.getheader('Content-Encoding', 'identity') != 'identity':
                    raise WebError(502, 'Unsupported compressed server response.')
                total = int(response.getheader('Content-Length', '0'))
                if total < 0 or total > limit:
                    raise WebError(413, 'This file is too large.')
                data = bytearray()
                while True:
                    if time.monotonic() > deadline:
                        raise TimeoutError()
                    if progress:
                        progress(len(data), total)
                    chunk = response.read(min(65536, limit + 1 - len(data)))
                    if not chunk:
                        break
                    data.extend(chunk)
                    if len(data) > limit:
                        raise WebError(413, 'This file is too large.')
                if total and len(data) != total:
                    raise WebError(502, 'The download was interrupted. Please retry.')
                if progress:
                    progress(len(data), total)
                return bytes(data), url, response.getheader('Content-Type', '').split(';')[0]
            except (OSError, ValueError, http.client.HTTPException):
                raise WebError(502, 'Cannot connect to the catalog. Please retry.') from None
            finally:
                connection.close()
                if sock:
                    sock.close()
        raise WebError(502, 'Too many catalog redirects.')


def xml(data):
    if b'\x00' in data or re.search(br'<!\s*(?:DOCTYPE|ENTITY)', data, re.I):
        raise WebError(400, 'Unsafe catalog document.')
    try:
        return ET.fromstring(data)
    except ET.ParseError:
        raise WebError(400, 'This address did not return an OPDS catalog.') from None


def parse_feed(data, url):
    root = xml(data)
    if root.tag not in (ATOM + 'feed', ATOM + 'entry'):
        raise WebError(400, 'This address did not return an OPDS 1.x catalog.')

    def text(node, tag):
        child = node.find(ATOM + tag)
        return ''.join(child.itertext()).strip()[:1500] if child is not None else ''

    def links(node):
        base = urljoin(url, root.get('{http://www.w3.org/XML/1998/namespace}base', ''))
        if node is not root:
            base = urljoin(base, node.get('{http://www.w3.org/XML/1998/namespace}base', ''))
        result = []
        for link in node.findall(ATOM + 'link'):
            href = urljoin(base, link.get('href', ''))
            if urlsplit(href).scheme in ('http', 'https'):
                result.append({**link.attrib, 'href': href})
        return result

    top = links(root)
    entries = [root] if root.tag == ATOM + 'entry' else root.findall(ATOM + 'entry')
    if len(entries) > 500:
        raise WebError(413, 'This catalog page has too many entries.')
    items = []
    for entry in entries:
        ls = links(entry)
        downloads = []
        for link in ls:
            mime = link.get('type', '').split(';')[0]
            fmt = FORMATS.get(mime)
            if not fmt and mime in ('application/zip', 'application/octet-stream') and urlsplit(link['href']).path.lower().endswith('.fb2.zip'):
                fmt = 'FB2 ZIP'
            if fmt and link.get('rel') in (ACQUIRE, ACQUIRE + '/open-access'):
                downloads.append({'url': link['href'], 'format': fmt})
        nav = next((l['href'] for l in ls if 'application/atom+xml' in l.get('type', '')
                    and l.get('rel', 'subsection') in ('subsection', 'alternate', 'http://opds-spec.org/acquisition')), '')
        partial = any(l.get('rel') == 'alternate' and 'application/atom+xml' in l.get('type', '') for l in ls)
        # Gutenberg exposes each publication as a one-book navigation feed.
        gutenberg = (urlsplit(url).hostname in ('www.gutenberg.org', 'm.gutenberg.org')
                    and urlsplit(nav).hostname in ('www.gutenberg.org', 'm.gutenberg.org')
                    and bool(re.fullmatch(r'/ebooks/\d+\.opds', urlsplit(nav).path)))
        partial = partial or gutenberg
        author = ', '.join(text(a, 'name') for a in entry.findall(ATOM + 'author'))
        listing_author = text(entry, 'content') if gutenberg else ''
        # Authorless Gutenberg entries sometimes put a download count here.
        if not author and not re.fullmatch(r'[\d,\s]+ downloads?', listing_author):
            author = listing_author
        items.append({'identity': text(entry, 'id') or nav or (downloads[0]['url'] if downloads else text(entry, 'title')),
                      'title': text(entry, 'title') or 'Untitled',
                      'author': author,
                      'summary': re.sub('<[^>]+>', '', text(entry, 'summary') or text(entry, 'content'))[:600],
                      'downloads': downloads, 'navigate': nav, 'detail': nav if partial else '',
                      'cover': next((l['href'] for l in ls if l.get('rel') == 'http://opds-spec.org/image/thumbnail'), '')})
    return {'title': text(root, 'title'), 'url': url, 'items': items,
            'next': next((l['href'] for l in top if l.get('rel') == 'next'), ''),
            'previous': next((l['href'] for l in top if l.get('rel') in ('previous', 'prev')), ''),
            'search': next((l for l in top if l.get('rel') == 'search'), None)}


def search_url(data, base, terms):
    root = xml(data)
    template = next((e.get('template', '') for e in root.iter() if e.tag.endswith('}Url')
                     and e.get('type', '').startswith('application/atom+xml')), '')
    destination = expand_search(urljoin(base, template), terms)
    # Gutenberg's published OpenSearch descriptor still points to its retired
    # mobile hostname, which intermittently returns gateway errors. The same
    # documented OPDS path is served on its current HTTPS catalog origin.
    parsed = urlsplit(destination)
    if urlsplit(base).hostname == 'www.gutenberg.org' and parsed.hostname == 'm.gutenberg.org':
        destination = urlunsplit(('https', 'www.gutenberg.org', parsed.path, parsed.query, ''))
    return destination


def expand_search(template, terms):
    if '{searchTerms}' not in template or not terms.strip() or len(terms) > 200:
        raise WebError(400, 'This catalog does not provide a supported search.')
    template = template.replace('{searchTerms}', quote(terms.strip(), safe=''))
    template = re.sub(r'\{[^{}]+\?\}', '', template)
    if '{' in template:
        raise WebError(400, 'This catalog does not provide a supported search.')
    return valid_url(template)
