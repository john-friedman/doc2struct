"""Render one document's standardized rows as Markdown using the C engine."""
from ._engine import _native


def convert_standardized_text_to_markdown(standardized_text, *, redundant='blockquote'):
    """Return Markdown from an iterable of standardized row dictionaries.

    Rows must be in document order; filing metadata and custom columns are ignored.
    ``redundant`` is 'blockquote' (default), 'plain', or 'omit'. Tables always use
    pipes, an empty header, and every source row; ragged rows are padded. Cell and
    paragraph layout whitespace is collapsed to spaces, and literal Markdown is
    escaped. Table notes retain their original markers. List matrices contain
    [relative_depth, item_text] string pairs; unsupported legal markers remain
    literal inside bullets. Images become alt-text placeholders. The generated
    opening 'introduction' title is suppressed, and headings beyond level 5 become
    bold paragraphs. Returns an empty string for an empty document.

    Save the returned string with UTF-8 encoding when a file is required.
    """
    return _native.markdown(standardized_text, redundant)
