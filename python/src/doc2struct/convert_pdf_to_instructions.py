"""PDF geometry and layout stay native until instruction export."""
from ._engine import _native, prepare


def convert_pdf_to_instructions(pdf, rules=None, *, adapter=None):
    """Convert PDF bytes to the same instruction dictionaries produced by HTML.

    Font sizes and indentation are converted from PDF points to CSS pixels.
    Optional soft_break=True marks a geometric continuation for the
    text.join_wrapped_lines standardization rule; no text is joined here.
    Set adapter="pdf_adapter_experimental" to opt into geometric table layout.
    Its table instructions carry logical_cell=True so standardization preserves
    resolved cell boundaries instead of merging numeric marks across cells.

    rules="PDF:PARSEBENCH" selects the bundled pdf_parsebench profile and its
    experimental layout automatically. It enables page-bound filtering, vector
    underline/strikethrough detection and font recovery. Instructions also carry
    page (zero-based) and bbox=(x0,y0,x1,y1), in top-left CSS pixels. Pass the same
    rules to convert_instructions_to_standardized_text to preserve formatting.
    """
    if adapter is None:
        return _native.pdf_instructions(pdf, prepare(rules))
    return _native.pdf_instructions(pdf, prepare(rules), adapter)
