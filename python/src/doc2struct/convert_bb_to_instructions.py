"""External OCR words/regions to ordinary native instructions.

Each page supplies positive width/height and words (or regions). Entries supply
str text and bbox=[left, top, right, bottom], in page pixels with a top-left
origin. Optional flags use bold=1, italic=2, underline=4, strikethrough=128.
Optional page dpi provides physical scale; otherwise a 1056px reference page
height is assumed. Fonts are estimated from clustered visible line heights.
Individual word boxes provide more evidence than multiword regions; character
positions inside a region are never invented. OCR is performed by the caller.
"""
from ._engine import _native, prepare


def convert_bb_to_instructions(pages, rules=None):
    """Resolve boxes into ordinary instructions without retaining coordinates.

    Pages are in document order; boxes may be unordered. Blocks and lines are
    document-wide. soft_break hints describe geometry independently of rules;
    join_wrapped_lines controls their use during standardization. adapters.bb
    controls optional prose/table judgments, including the criteria enabled by
    BB:PARSEBENCH. Pass the same rules here and to
    convert_instructions_to_standardized_text for staged conversion.
    """
    return _native.bb_instructions(pages, prepare(rules))
