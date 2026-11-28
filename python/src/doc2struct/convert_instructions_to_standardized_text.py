"""Run native standardization on inspectable Python instruction rows.

A list is one row with type="list". Its matrix contains string pairs
[relative_depth, item_text], retaining any source markers in item_text.
The row's level places the whole list in the document; its text is empty.
Before grouping, lists.max_gap_rows bounds matching text rows between list
items. A confirmed chain may absorb lists.max_tail_rows after its last item.
Counts use pre-merge rows and never cross a different type, level or payload
style/font/indent. Set max_gap_rows=0 to disable or max_tail_rows=0 for no tail.

For tables, cleaned is a list of actual cleanup changes and unresolved issues.
An empty list means nothing was recorded, not that the table is verified.
Other row types have cleaned=None.

Text matching the legacy remove rules is retained with type="redundant" in
source order. These rows do not create sections or become table annotations.

After table processing, adjacent text or textsmall rows of the same type and
level are joined with spaces. Type/level changes and all other types end the block.

When headings.merge_adjacent is enabled, adjacent titles at the same level
also join when their recognized identities agree (or the following fragment has no identity)
and their first source runs match
in style flags, font size and indentation. This runs before table-title inheritance
and retains an existing recognized identity and every source item's metadata.
Joined inferred fragments are checked against HeaderRules to recognize wrapped
headings; this adds their identity while retaining the already-built nesting.
headings.recognize_inferred additionally assigns a configured identity to a
complete inferred title, retaining its inferred level. It matches the full
caption after the row has already passed heading inference.

text.preserve_styles opts into an additional runs field on non-table rows:
[{"text": "plain ", "flags": 0}, {"text": "italic", "flags": 2}]. The text
field remains plain. Flags are bold=1, italic=2, underline=4 and strikethrough=128; combined styles
use their bitwise OR. The existing Markdown renderer consumes these runs.
Matrix remains a 2D array of plain strings. Tables can carry cell_runs entries
with row/col coordinates and styled runs. Styles follow retained cells through
row/column selection; cell rewrites that cannot retain alignment use plain text.
No spans are inferred during standardization.

headings.infer, styles and max_words control fallback heading inference; regex
HeaderRules still take priority. max_words=0 means unlimited.
HeaderRule.repeat="redundant" preserves recognized headings through frequency
cleanup, then marks identical or bare repetitions redundant while that section
is active. A different section or enclosing part allows the identifier to recur.
repeat="active" also treats a differently worded heading with the same active
identity as a continuation; this is useful for explicit amendment directives.
headings.split_before contains regexes on normalized heading text. A row must
match at its beginning; later matches start separate rows with source styles
retained. Distinct recognized headings do not merge back together.
remove.linked_regions contains navigation-marker regexes. A marker and the
following consecutive fully linked rows become redundant; ordinary unlinked
content ends the region. The text remains in the output.
text.join_wrapped_lines joins geometry-marked continuations before heading
inference. wrap_hyphenation="preserve" keeps a trailing hyphen without adding a
space; "remove" drops it when the continuation starts with a lowercase letter.
PDF and bounding-box adapters emit optional soft_break hints for this purpose.
tables.protect_years prevents numeric suffixes from attaching to year headers;
formatting_max_gap bounds blank cells crossed to join numeric prefixes/suffixes
(-1 is unlimited; 0 permits only directly adjacent cells).

Extra columns, including class and standardized_title, are always opt-in. A text
column can extract a regex capture or use value to emit a literal/replacement.
Header rules establish structure before column patterns examine the output rows.
"""
from ._engine import _native, prepare

DEFAULT_COLUMNS = ('id', 'level', 'type', 'text', 'matrix', 'cleaned', 'preamble', 'footnotes', 'postamble')
RULE_COLUMNS = DEFAULT_COLUMNS


def convert_instructions_to_standardized_text(instructions, rules=None):
    return _native.standardize(instructions, prepare(rules), rules is not None)
