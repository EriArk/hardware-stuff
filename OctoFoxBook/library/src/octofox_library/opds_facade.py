"""Bounded BookLore OPDS client; no external-source discovery or importer."""
from collections.abc import Iterator, Mapping, Sequence
from dataclasses import dataclass
from http import HTTPStatus
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode, urlsplit
from urllib.request import Request, urlopen
import xml.etree.ElementTree as ET

ATOM = "http://www.w3.org/2005/Atom"
DC = "http://purl.org/dc/terms/"
CALIBRE = "http://calibre.kovidgoyal.net/2009/metadata"
PREFIX = "/api/v1/opds"

@dataclass(frozen=True, slots=True)
class FacadeSettings:
    upstream_url: str
    public_origin: str
    timeout_seconds: int = 20
    maximum_xml_bytes: int = 32 * 1024 * 1024

    def __post_init__(self):
        for value in (self.upstream_url, self.public_origin):
            parsed = urlsplit(value)
            if parsed.scheme not in {"http", "https"} or not parsed.hostname or parsed.username or parsed.password:
                raise ValueError("Expected an HTTP(S) URL without embedded credentials")
            if parsed.query or parsed.fragment:
                raise ValueError("URL query and fragment are not supported")
        if urlsplit(self.upstream_url).path.rstrip("/") != PREFIX:
            raise ValueError("BookLore upstream must end in /api/v1/opds")
        if urlsplit(self.public_origin).path not in {"", "/"}:
            raise ValueError("Public origin must not include a path")

def _tag(namespace: str, name: str) -> str:
    return f"{{{namespace}}}{name}"


def _atom(name: str) -> str:
    return _tag(ATOM, name)


class UpstreamFailure(RuntimeError):
    def __init__(self, status: int, message: str = "upstream request failed") -> None:
        super().__init__(message)
        self.status = status


class UpstreamClient:
    def __init__(self, settings: FacadeSettings) -> None:
        self.settings = settings
        self.base_url = settings.upstream_url.rstrip("/")

    def _url(self, suffix: str, query: Sequence[tuple[str, str]] = ()) -> str:
        if suffix and not suffix.startswith("/"):
            raise ValueError("upstream suffix must be empty or absolute-path relative")
        url = f"{self.base_url}{suffix}"
        if query:
            url = f"{url}?{urlencode(query, doseq=True, quote_via=quote)}"
        return url

    def open(
        self,
        suffix: str,
        query: Sequence[tuple[str, str]],
        authorization: str | None,
        *,
        method: str = "GET",
        request_headers: Mapping[str, str] | None = None,
    ):
        headers = {
            "Accept": "application/atom+xml, application/xml;q=0.9, */*;q=0.1",
            "User-Agent": "OctoFox-Library/0.1",
        }
        if authorization:
            headers["Authorization"] = authorization
        if request_headers:
            headers.update(request_headers)
        request = Request(self._url(suffix, query), headers=headers, method=method)
        try:
            return urlopen(request, timeout=self.settings.timeout_seconds)
        except HTTPError as error:
            error.close()
            raise UpstreamFailure(error.code) from error
        except (TimeoutError, URLError) as error:
            raise UpstreamFailure(HTTPStatus.BAD_GATEWAY) from error

    def fetch_xml(
        self,
        suffix: str,
        query: Sequence[tuple[str, str]],
        authorization: str | None,
    ) -> ET.Element:
        with self.open(suffix, query, authorization) as response:
            content_type = response.headers.get_content_type()
            if content_type not in {
                "application/atom+xml",
                "application/xml",
                "application/opensearchdescription+xml",
                "text/xml",
            }:
                raise UpstreamFailure(HTTPStatus.BAD_GATEWAY, "upstream did not return XML")
            payload = response.read(self.settings.maximum_xml_bytes + 1)
        if len(payload) > self.settings.maximum_xml_bytes:
            raise UpstreamFailure(HTTPStatus.BAD_GATEWAY, "upstream XML is too large")
        try:
            return ET.fromstring(payload)
        except ET.ParseError as error:
            raise UpstreamFailure(HTTPStatus.BAD_GATEWAY, "upstream XML is invalid") from error

    def iter_catalog_entries(
        self,
        authorization: str | None,
        *,
        query: str | None = None,
    ) -> Iterator[ET.Element]:
        for page in range(1, 5001):
            parameters = [("page", str(page)), ("size", "100")]
            if query is not None:
                parameters.insert(0, ("q", query))
            feed = self.fetch_xml("/catalog", parameters, authorization)
            entries = feed.findall(_atom("entry"))
            yield from entries
            has_next = any(
                link.get("rel") == "next" for link in feed.findall(_atom("link"))
            )
            if not has_next:
                return
        raise UpstreamFailure(HTTPStatus.BAD_GATEWAY, "upstream pagination did not terminate")
