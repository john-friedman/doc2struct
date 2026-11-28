"""Convenience constants read from the C library's canonical defaults."""
import json
from .. import _native

_DEFAULTS = json.loads(_native.defaults_json())
_ADAPTERS = _DEFAULTS['adapters']
DEFAULT_HIDE_HIDDEN_ATTR = _ADAPTERS['html']['hide_hidden_attr']
DEFAULT_HIDE_VISIBILITY = _ADAPTERS['html']['hide_visibility']
DEFAULT_MAX_SPAN = _ADAPTERS['html']['max_span']
DEFAULT_TEXT_TAB_SIZE = _ADAPTERS['text']['tab_size']
DEFAULT_PARTIALSGML_TAB_SIZE = _ADAPTERS['partialsgml']['tab_size']
# The empty fallback permits importing the source tree before rebuilding an
# older native extension. Rebuilt distributions read these from defaults.json.
_PDF = _ADAPTERS.get('pdf', {})
DEFAULT_PDF_CLIP_TO_PAGE = _PDF.get('clip_to_page', False)
DEFAULT_PDF_DETECT_DECORATIONS = _PDF.get('detect_decorations', False)
DEFAULT_PDF_LAYOUT = _PDF.get('layout', 'default')
DEFAULT_PDF_FONT_RECOVERY = _PDF.get('font_recovery', 'default')
DEFAULT_BB_ADAPTER = _ADAPTERS.get('bb', {})
DEFAULT_HEADER_STYLES = tuple(_DEFAULTS['header_styles'])
_REMOVE = _DEFAULTS['remove']
_TABLES = _DEFAULTS['tables']
_LISTS = _DEFAULTS['lists']
_TEXT = _DEFAULTS['text']
_HEADINGS = _DEFAULTS['headings']
DEFAULT_PRESERVE_STYLES = tuple(_TEXT['preserve_styles'])
DEFAULT_JOIN_WRAPPED_LINES = _TEXT['join_wrapped_lines']
DEFAULT_WRAP_HYPHENATION = _TEXT['wrap_hyphenation']
DEFAULT_INFER_HEADINGS = _HEADINGS['infer']
DEFAULT_INFERRED_HEADING_STYLES = tuple(_HEADINGS['styles'])
DEFAULT_HEADING_MAX_WORDS = _HEADINGS['max_words']
DEFAULT_MERGE_ADJACENT_HEADINGS = _HEADINGS['merge_adjacent']
DEFAULT_PROTECT_YEARS = _TABLES['protect_years']
DEFAULT_FORMATTING_MAX_GAP = _TABLES['formatting_max_gap']
_TEXT = _DEFAULTS['text']
_HEADINGS = _DEFAULTS['headings']
DEFAULT_PRESERVE_STYLES = tuple(_TEXT['preserve_styles'])
DEFAULT_JOIN_WRAPPED_LINES = _TEXT['join_wrapped_lines']
DEFAULT_WRAP_HYPHENATION = _TEXT['wrap_hyphenation']
DEFAULT_INFER_HEADINGS = _HEADINGS['infer']
DEFAULT_INFERRED_HEADING_STYLES = tuple(_HEADINGS['styles'])
DEFAULT_HEADING_MAX_WORDS = _HEADINGS['max_words']
DEFAULT_MERGE_ADJACENT_HEADINGS = _HEADINGS['merge_adjacent']
DEFAULT_PROTECT_YEARS = _TABLES['protect_years']
DEFAULT_FORMATTING_MAX_GAP = _TABLES['formatting_max_gap']
DEFAULT_LIST_GAP_ROWS = _LISTS['max_gap_rows']
DEFAULT_HEADING_USE_FONT_SIZE = _HEADINGS.get('use_font_size', True)
DEFAULT_HEADING_SPLIT_BEFORE = tuple(_HEADINGS.get('split_before', ()))
DEFAULT_RECOGNIZE_INFERRED_HEADINGS = _HEADINGS.get('recognize_inferred', False)
DEFAULT_LINKED_REGIONS = tuple(_REMOVE.get('linked_regions', ()))
DEFAULT_PROTECT_WRAPPED_HEADINGS = _TEXT.get('protect_wrapped_headings', False)
DEFAULT_LIST_TAIL_ROWS = _LISTS['max_tail_rows']
STANDARD_REMOVE = tuple(tuple(p) for p in _REMOVE['patterns'])
DEFAULT_REPEAT_THRESHOLD = _REMOVE['repeat_threshold']
DEFAULT_PAGE_LABEL_REGEX = _REMOVE['page_label_regex']
DEFAULT_FOOTNOTE_REGEX = _TABLES['footnote_regex']
DEFAULT_ANNOTATION_EXCLUDE_REGEX = _TABLES['annotation_exclude_regex']
DEFAULT_FOOTNOTE_LINES = _TABLES['footnote_lines']
DEFAULT_PREAMBLE_LINES = _TABLES['preamble_lines']
DEFAULT_POSTAMBLE_LINES = _TABLES['postamble_lines']
DEFAULT_SINGLE_ROW_TO_TEXT = _TABLES['single_row_to_text']
DEFAULT_TITLE_FROM_SECTION = _TABLES['title_from_section']
DEFAULT_UNWRAP_LAYOUT = _TABLES['unwrap_layout']
KNOWN_TABLE_STEPS = tuple(_TABLES['steps'])

# styles a header rule may require of the line's first piece
KNOWN_STYLES = ('bold', 'italic', 'underline', 'centered', 'all_caps', 'proper_case', 'fake_table', 'strikethrough')

# columns every output already has; custom columns may not reuse them
RESERVED_COLUMNS = ('id', 'level', 'type', 'text', 'matrix', 'cleaned', 'preamble',
                    'footnotes', 'postamble', 'runs', 'cell_runs')
