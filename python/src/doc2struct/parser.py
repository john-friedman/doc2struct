"""Document bytes to standardized rows, with native intermediate storage.

HTML, PDF, text and SGML inputs must be bytes; returned text is str.
Read file contents with Path.read_bytes() or open(path, 'rb').read().
parse_bb instead accepts externally recognized text and bounding boxes.
"""
from ._engine import _native, prepare


def parse_html(html: bytes, rules=None):
    """Parse HTML bytes using rules.adapters.html, reusing bounded C memory."""
    return _native.parse_html(html, prepare(rules), rules is not None)


def clear_parser_cache():
    """Release the default parser's cached memory; subsequent calls remain usable."""
    _native.clear_parser_cache()


def parse_pdf(pdf: bytes, rules=None, *, adapter=None):
    """Parse PDF bytes through native extraction, layout, and standardization.

    Extracts embedded text; image-only pages require OCR elsewhere.
    Set adapter="pdf_adapter_experimental" to opt into geometric table layout.
    rules="PDF:PARSEBENCH" selects that layout plus the bundled pdf_parsebench
    extraction and standardization policies. OCR is not performed.
    """
    if adapter is None:
        return _native.parse_pdf(pdf, prepare(rules), rules is not None)
    return _native.parse_pdf(pdf, prepare(rules), rules is not None, None, adapter)


def parse_text(text: bytes, rules=None):
    """Parse plain-text bytes directly to standardized rows in C.

    Uses rules.adapters.text.tab_size; encoding matches convert_text_to_instructions.
    Uses the default parser's bounded native memory cache.
    """
    return _native.parse_text(text, prepare(rules), rules is not None)


def parse_bb(pages, rules=None):
    """Resolve external OCR boxes and standardize entirely in C.

    Each page has positive width/height and words (or regions) containing str
    text and bbox=[left, top, right, bottom], in page pixels with a top-left
    origin. Pages are in document order; boxes may be unordered. Optional dpi
    supplies physical scale, and optional flags carry existing text styles.
    Geometry determines ordinary instructions and is not included in output.
    OCR is performed by the caller. No Python instruction list is constructed.
    """
    return _native.parse_bb(pages, prepare(rules), rules is not None)


def parse_partialsgml(text: bytes, rules=None):
    """Parse caller-identified partial SGML bytes to standardized rows in C.

    Uses rules.adapters.partialsgml.tab_size; encoding matches the instruction converter.
    """
    return _native.parse_partialsgml(text, prepare(rules), rules is not None)


class Parser:
    """Reuse up to cache_size bytes of C memory between sequential parses.

    Rules and returned rows have the same behavior as parse_html. Each instance
    has one owner; calls on that instance must not overlap. close() releases
    cached blocks, and clear() releases them while keeping the parser usable.
    """
    __slots__ = ('_handle',)

    def __init__(self, cache_size=8 * 1024 * 1024):
        self._handle = _native.parser_new(cache_size)

    def parse_html(self, html: bytes, rules=None):
        return _native.parse_html(html, prepare(rules), rules is not None, self._handle)

    def parse_pdf(self, pdf: bytes, rules=None, *, adapter=None):
        """Parse PDF bytes using this parser's cache and optional layout adapter."""
        if adapter is None:
            return _native.parse_pdf(pdf, prepare(rules), rules is not None, self._handle)
        return _native.parse_pdf(pdf, prepare(rules), rules is not None, self._handle, adapter)

    def parse_text(self, text: bytes, rules=None):
        """Parse plain-text bytes using this parser's native memory cache."""
        return _native.parse_text(text, prepare(rules), rules is not None, self._handle)

    def parse_bb(self, pages, rules=None):
        """Parse external OCR boxes using this parser's native memory cache."""
        return _native.parse_bb(pages, prepare(rules), rules is not None, self._handle)

    def parse_partialsgml(self, text: bytes, rules=None):
        """Parse partial SGML bytes using this parser's native memory cache."""
        return _native.parse_partialsgml(text, prepare(rules), rules is not None, self._handle)

    @property
    def cache_size(self):
        return _native.parser_info(self._handle)[0]

    @property
    def cached_bytes(self):
        return _native.parser_info(self._handle)[1]

    @property
    def closed(self):
        return _native.parser_info(self._handle)[2]

    def clear(self):
        _native.parser_clear(self._handle)

    def close(self):
        _native.parser_close(self._handle)

    def __enter__(self):
        if self.closed:
            raise ValueError('Parser is closed')
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()
