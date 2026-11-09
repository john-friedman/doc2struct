# doc2struct

doc2struct is an algorithm that can parse documents fast on one thread. It has been released in alpha, due to demand. Will be polished.

![Parsing a document](image.png)

**installation**

python 
```
pip install doc2struct
```

c 
```
git clone https://github.com/john-friedman/doc2struct.git
cd doc2struct
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --config Release --prefix ./install
```

**quickstart**

```python
from doc2struct import parse_html, visualize_standardized_text
# e.g. https://www.sec.gov/Archives/edgar/data/1318605/000095017022000796/tsla-20211231.htm
with open("10-K.html","rb") as f:
    html = f.read()

standardized_text = parse_html(html, rules="SEC:10-K")
visualize_standardized_text(standardized_text)
```


It emits a compact data structure, preserving nesting, suitable for storage as parquet.

| Column | Meaning | Example |
|---|---|---|
| id | Sequential output row number, starting at `0` | `3` |
| level | Nesting depth within the document | `2` |
| type | `title`, `text`, `textsmall`, `table`, `image`, `list`, or `redundant` | `"table"` |
| text | Text content; image alt text for images; associated title for tables. `null` for lists | `"Consolidated Balance Sheets"` |
| matrix | Tables: rows of cell strings. Lists: `[relative_depth, item_text]` pairs, both strings. `null` otherwise | `[["Year", "Revenue"], ["2025", "100"]]` |
| cleaned | Table cleanup changes and unresolved issues, represented as a list of strings. `null` for other types | `["removed_empty_rows", "merged_header_rows"]` |
| preamble | Table rows only: associated text before the table, or `null` | `"Amounts in millions."` |
| footnotes | Table rows only: list of `[id, text]` pairs, or `null` | `[["(1)", "Includes discontinued operations."]]` |
| postamble | Table rows only: associated text following the table’s footnotes, or `null` | `"See accompanying notes to the financial statements."` |


It supports customizable rulesets, that allow the algorithm to be easily tweaked to process different documents better. For example, the 10-K preset produces:

| Column | Meaning | Example |
|---|---|---|
| class | SEC classification | `"item"` |
| standardized_title | A nicer title | `"item1a"` |

```json
"10k": {
      "columns": [
        {
          "name": "class",
          "source": "text",
          "types": [
            "title"
          ],
          "pattern": "(?i)^item\\b",
          "value": "item"
        },
        {
          "name": "standardized_title",
          "source": "text",
          "types": [
            "title"
          ],
          "pattern": "(?i)^item\\s*(\\d+)\\.?([a-z])?(?![a-z])",
          "value": "item\\1\\L\\2\\E"
        }
      ],
      "headers": [
        {
          "name": "part",
          "regex": "^part\\s*([ivx]+)$",
          "level": 0,
          "requires": []
        },
        {
          "name": "signatures",
          "regex": "^signatures?\\.*$",
          "level": 0
        },
        {
          "name": "item",
          "regex": "^item\\s*(\\d+)\\.?([a-z])?(?![a-z])",
          "level": 1,
          "requires": []
        }
      ]
    }
```

The ideal use case for doc2struct is as part of a [document router](https://datamule.xyz/articles/exploring-document-parsing-router-architecture/) that uses LLMs to tweak the rulesets depending on the corpus.

The first usecase of doc2struct will be processing every SEC html, pdf, and text file (10TB+) for about $0.04 (AWS spot estimate using c6gd.medium).

## Functions

```python
# Parse directly to standardized text
standardized_text = parse_html(html, rules=None)
standardized_text = parse_pdf(pdf, rules=None)
standardized_text = parse_text(text, rules=None)
standardized_text = parse_partialsgml(text, rules=None)

# Convert to instructions
instructions = convert_html_to_instructions(html, rules=None)
instructions = convert_pdf_to_instructions(pdf, rules=None)
instructions = convert_text_to_instructions(text, rules=None)
instructions = convert_partialsgml_to_instructions(text, rules=None)

# Convert instructions to standardized text
standardized_text = convert_instructions_to_standardized_text(
    instructions, rules=None
)

# Visualize
visualize_instructions(
    instructions,
    filename="instructions_visualization.html",
    open_browser=True,
)
visualize_standardized_text(
    standardized_text,
    filename="standardized_text_visualization.html",
    open_browser=True,
)

# Saving output
import polars as pl
from doc2struct import parse_html

standardized_text = parse_html(html, rules="SEC:10-K")
pl.from_dicts(
    standardized_text, infer_schema_length=None
).write_parquet("document.parquet")

# writing output to markdown
convert_standardized_text_to_markdown(standardized_text, redundant="blockquote")
```


## Modalities supported

- HTML
- PDF with a text layer
- TXT
- Partial SGML (TEXT with SGML mixed in), e.g. SEC form 345 documents from 1999

## How it works

doc2struct takes a modality and converts it to instructions using the modality specific adapter. Instructions can be thought of as a 'block' that contains just the sufficient information necessary to later construct a tree hierarchy. Instructions are then converted to standardized text.

### HTML -> Standardized text

**HTML tokenization**

Only essential information is kept. Uses SIMD.

Takes html bytes (UTF-8, falling back to cp1252), outputs tokens.

`(kind, text/tag, attributes including style)`

| kind | meaning |
|---|---|
| 0 | TEXT |
| 1 | START |
| 2 | END |

```
(1, 'p',  {'style': 'font-weight:bold'})   
(0, 'Item 1 & 2', None)                    
(2, 'p',  None)                            
(1, 'br', {})                              
(1, 'img', {'src': 'a.png', 'alt': 'logo'})
(0, '\n', None)                            
(1, 'td', {'colspan': '2'})                
(0, 'Total', None)
(2, 'td', None)
(1, 'script', {})                          
(2, 'script', None)                       
```

**Normalization**

Normalizes tokens into instructions with these columns:

`block, line, row, col, span, text, flags, font_size, indent, href, tag, src`

| Column | Meaning |
|---|---|
| block | one id per plain line, or one shared id for a whole table |
| line | the visual line |
| row, col | position in a table, empty outside tables |
| span | `RxC` (rowspan x colspan), only when the cell is bigger than 1x1 |
| text | the text of the run. For an image, its alt |
| flags | bitmask: bold 1, italic 2, underline 4, centered 8, all_caps=16 and proper_case=32 |
| font_size | px, from the nearest enclosing font-size |
| indent | px, from the nearest enclosing element's own text-indent, padding and margin |
| href | link target of the enclosing `<a>` |
| tag | innermost enclosing block tag |
| src | image source, set only on image instruction cells |

| block | line | row | col | span | text | flags | font_size | indent | href | tag | src |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0 |  |  |  | `ITEM 1. BUSINESS` | 9 | 16 |  |  | p |  |
| 1 | 1 |  |  |  | `We make ` | 0 |  | 24 |  | p |  |
| 1 | 1 |  |  |  | `software` | 2 |  | 24 |  | p |  |
| 1 | 1 |  |  |  | `, see ` | 0 |  | 24 |  | p |  |
| 1 | 1 |  |  |  | `Note 4` | 0 |  | 24 | /ex.htm | p |  |
| 1 | 1 |  |  |  | `.` | 0 |  | 24 |  | p |  |
| 2 | 2 |  |  |  | `Company logo` | 0 |  |  |  |  | logo.png |
| 2 | 2 |  |  |  | `\n` | 0 |  |  |  |  |  |
| 3 | 3 | 0 | 0 |  | `Revenue` | 0 |  |  |  | td |  |
| 3 | 4 | 0 | 1 | 1x2 | `2021` | 1 |  |  |  | td |  |
| 3 | 5 | 1 | 0 |  | `Cloud` | 0 |  |  |  | td |  |
| 3 | 6 | 1 | 0 |  | `services` | 0 |  |  |  | td |  |
| 3 | 7 | 1 | 1 |  |  | 0 |  |  |  |  |  |
| 3 | 8 | 1 | 2 |  | `5` | 0 |  |  |  | td |  |

**Convert instructions to standardized text**

Takes instructions, converts them into standardized text, suitable for storage in a parquet file. Supports new columns.

Default

| Column | Meaning | Example |
|---|---|---|
| id | Sequential output row number, starting at `0` | `3` |
| level | Nesting depth within the document | `2` |
| type | `title`, `text`, `textsmall`, `table`, `image`, `list`, or `redundant` | `"table"` |
| text | Text content; image alt text for images; associated title for tables. `null` for lists | `"Consolidated Balance Sheets"` |
| matrix | Tables: rows of cell strings. Lists: `[relative_depth, item_text]` pairs, both strings. `null` otherwise | `[["Year", "Revenue"], ["2025", "100"]]` |
| cleaned | Table cleanup changes and unresolved issues, represented as a list of strings. `null` for other types | `["removed_empty_rows", "merged_header_rows"]` |
| preamble | Table rows only: associated text before the table, or `null` | `"Amounts in millions."` |
| footnotes | Table rows only: list of `[id, text]` pairs, or `null` | `[["(1)", "Includes discontinued operations."]]` |
| postamble | Table rows only: associated text following the table’s footnotes, or `null` | `"See accompanying notes to the financial statements."` |



With the 10-K preset

| Column | Meaning | Example |
|---|---|---|
| id | Sequential output row number, starting at `0` | `3` |
| level | Nesting depth within the document | `2` |
| type | `title`, `text`, `textsmall`, `table`, `image`, `list`, or `redundant` | `"table"` |
| text | Text content; image alt text for images; associated title for tables. `null` for lists | `"Consolidated Balance Sheets"` |
| matrix | Tables: rows of cell strings. Lists: `[relative_depth, item_text]` pairs, both strings. `null` otherwise | `[["Year", "Revenue"], ["2025", "100"]]` |
| cleaned | Table cleanup changes and unresolved issues, represented as a list of strings. `null` for other types | `["removed_empty_rows", "merged_header_rows"]` |
| preamble | Table rows only: associated text before the table, or `null` | `"Amounts in millions."` |
| footnotes | Table rows only: list of `[id, text]` pairs, or `null` | `[["(1)", "Includes discontinued operations."]]` |
| postamble | Table rows only: associated text following the table’s footnotes, or `null` | `"See accompanying notes to the financial statements."` |
| class | Classification assigned by the SEC rules | `"item"` |
| standardized_title | Normalized identifier for a recognized heading; otherwise `null` | `"item1a"` |


### PDF

**PDF to instructions**

Left to right reading orientation, y used to calculate new lines. In the future this will change to support other reading alignments.

**Convert instructions to standardized text**

Same as for HTML.

### TXT

**TXT to instructions**

TXT is naively assumed to be in order.

**Convert instructions to standardized text**

Same as for HTML.

### Partial SGML

**Partial SGML to instructions**

Partial SGML is naively assumed to be in order.

**Convert instructions to standardized text**

Same as for HTML.

### Instruction definition

| Term | Meaning |
|---|---|
| instruction cell | a single run of text with one style, or an image |
| instruction | one block: all instruction cells that share a `block` id. A plain line of text is one instruction. A whole table is one instruction. |
| instructions | the full list of instruction cells for a document, in document order |

## Table definition

A table is one output row. Its `matrix` holds the cleaned data and its other columns hold what belongs to it.

**Title.** The text calculated to be the table's title.

**Preamble** Text before a table, related to the table.

**Footnotes.** Text right after the table such as(`*`, `(a)`, `1`, `†`) is moved into `footnotes` as `[id, text]` pairs.

**Postamble** Text after a table, related to the table.

Example

```csv
id,level,type,text,matrix,preamble,footnotes,postamble
0,0,title,Segment results,,,,
1,1,text,Revenue grew in every segment.,,,,
2,1,table,Segment results,"[[""Segment"",""2024"",""2023""],[""Cloud"",""105.4(a)"",""87.9""],[""Gaming"",""23.5"",""15.5(b)""]]",(In millions),"[[""(a)"",""Includes acquired businesses.""],[""(b)"",""Restated.""]]",Source: company filings.
3,1,text,Operating income also rose.,,,,
```

## Rulesets

Rules change how the algorithm and adapters work.

Here is the full default ruleset (as of 11/3/2026)

```json
{
  "name": "default",
  "headers": [],
  "header_styles": ["bold", "all_caps", "proper_case"],
  "remove": {
    "patterns": [
      [
        "^\\d+$",
        false
      ],
      [
        "^-\\d+-$",
        false
      ],
      [
        "(^\\s*\\d+\\s*\\||\\|\\s*\\d+\\s*$)",
        false
      ],
      [
        "^_+$",
        false
      ],
      [
        "^●(?:\\s*●)*$",
        false
      ],
      [
        "^table\\s+of\\s+contents$",
        true
      ]
    ],
    "repeat_threshold": 20,
    "page_label_regex": "^f-\\d{1,4}$"
  },
  "tables": {
    "steps": [
      "validate_structure",
      "merge_formatting_chars",
      "remove_empty_rows",
      "remove_empty_columns",
      "remove_subset_rows_bottom_to_top",
      "remove_subset_rows_top_to_bottom",
      "remove_subset_columns_left_to_right",
      "remove_subset_columns_right_to_left",
      "merge_duplicate_header_rows_down"
    ],
    "single_row_to_text": true,
    "title_from_section": true,
    "footnote_regex": "^(\\*+|\\((?:[a-zA-Z]|\\d{1,3}|[ivxlcdmIVXLCDM]{2,4})\\)|\\d{1,3}(?:[.)](?!\\d)|(?=\\s|$))|†+)",
    "annotation_exclude_regex": "(?i)^(?:\\(\\d{3}\\)\\s*\\d{3}[-.\\s]\\d{4}|[1-9]\\d{2,4}\\s+(?:[\\w.\\x27-]+\\s+){1,5}(?:street|avenue|road|boulevard|drive|lane|way|st\\.?|ave\\.?|rd\\.?|blvd\\.?)\\b)",
    "footnote_lines": 3,
    "preamble_lines": 7,
    "postamble_lines": 7,
    "unwrap_layout": false
  },
  "lists": {
    "max_gap_rows": 3,
    "max_tail_rows": 1
  },
  "columns": []
}
```

### html_tokenizer

- Skips comments, doctype, and the contents of script, style and title.
- Keeps: style, colspan, rowspan, href, src, alt and hidden.
- Decodes entities and lowercases tag names.
- Self-closed non-void tags get a START and an END.

### token_normalizer

- Hidden: skips display:none elements and everything inside them. With options, it also skips the hidden attribute and visibility:hidden.
- Body only: ignores everything before the first non-head tag.
- Lines: a line ends at the end of p, div, h1–h6 or li, or at `<br>`.
- Styles: reads bold, italic, underline and centered from the tags and inline style, and takes the nearest font_size and indent.
- Links: the nearest `<a>` sets href.
- Tables: nested tables are flattened into the cell, spans are capped at max_span, and empty cells get a placeholder instruction cell.
- Repair: implied end tags for p, li, td and tr, and end tags never pop past a table.
- Whitespace: whitespace-only text at the start of a line is dropped.


### convert_instructions_to_standardized_text

- Merging: adjacent instruction cells in the same line with the same style are joined into one piece of text.
- Fake tables: a table with only one non-empty row is marked as not a table.
- Cleaning: page numbers, "table of contents" links and text repeated about 20 or more times (running headers and footers) are marked as redundant.
- Header rules: determine nesting.
- Column rules: determine making new columns
- Predicted headers: what doc2struct thinks might be a header.
- Small text: text with a smaller font size than most of the document becomes `textsmall`.
- Nesting: lines are placed under the nearest earlier header with a lower level. `level` is the nesting depth.
- Tables: table rules.

## Presets

doc2struct has a default set of rules in presets/defaults.json, it also has SEC filing presets in presets/sec_filings.json for 900+ SEC document types.

## Adding adapters.

It should be fairly easy to add new adapters for different modalities. OCR -> instructions is possible, but a dedicated vision LLM might perform better there.

## Timeline

- July 2024: [SEC Parsers](https://github.com/john-friedman/SEC-Parsers) released.
    - At the time, the best open source LLM parser could parse one Tesla 10-K root form in ten minutes.
    - SEC Parsers could do 2 per second.
    - It was forked early on by [fintool](https://www.ycombinator.com/companies/fintool).
- December 2024: [doc2dict](https://github.com/john-friedman/doc2dict/) released.
    - Generalized to parse any sort of html files, not just specific SEC files.
    - Added customizable rulesets.
- May 2025: [doc2dict](https://github.com/john-friedman/doc2dict/) updated.
    - Added PDF parsing.
    - This version was used for extracting [43 billion tokens for LLM training](https://www.teraflopai.com/blog/sec-edgar-case-study) at a cost of $1.10.
- February 2026: [doc2dict](https://github.com/john-friedman/doc2dict/) updated.
    - HTML parsing speed doubled.
    - At this point, many companies working with SEC data pointed LLMs to copy/refine doc2dict for their own parsers.
- October 2026: [doc2struct](https://github.com/john-friedman/doc2struct) released.
    - HTML parsing speed benchmarked at 500 MB/s 
    - PDF parsing speed benchmarked at 25 MB/s 
    - TXT at 60 MB/s
    - Rulesets defined for 900+ SEC document types.

## Process

- Work began with me reading thousands of SEC html files. That allowed me to write a system of general rules to parse more and more html files in the way a human (or LLM) would.
- I initially used Claude to write code. Then switched to writing by hand, as I got better at coding. I alternated between writing code manually and describing the algorithm to Claude.
- In October 2026, I settled on the final algorithm and data structure. Then used Sol/Astra to flesh out / write most of the code. I have barely inspected the code. This is new for me.
- I have also tasked coding agent loops with adding rulesets for most files across the SEC corpus. El nuevo mundo.

> Note: this work has been interrupted by my other commitments. The algorithm was (mostly) completed by May 2026.


## Benchmarks

> Note: since this benchmark was first run, performance has been increased by ~50% on x86.

| data | type | size | time | throughput |
|---|---|---|---|---|
| Every 2025 10-K and 10-K/A | html | 19,085.105 MB | 39,962 ms | 477.6 MB/s |
| Every 2020 8-K and 8-K/A | html | 2,765.038 MB | 12,474 ms | 221.7 MB/s |
| Every 10-K, 10-Q, 8-K as well as /A for MSFT | html | 288.175 MB | 514 ms | 560.7 MB/s |
| 100 10-K and 10-K/A PDFs | pdf | 213.225 MB | 8,520 ms | 25 MB/s |
| 1,000 pre-2000 filing documents | partial SGML | 36.957 MB | 898 ms | 41.2 MB/s |
| 1,000 pre-2000 filing documents | text | 36.957 MB | 590 ms | 62.7 MB/s |

Every 2025 10-K and 10-K/A is about 19,085,104,953 characters or about ~5 billion tokens. 

> Note: someone suggested 'Tolkiens' as a measure. Assuming TXT speeds, this yields about 800 LOTRs per second.

## Future Improvements

I barely understand SIMD. I somewhat understand HTML's DOM. I do not understand PDF. Hopefully, my understanding (or a LLM) will improve much over the next year. I think:

1. ~4x improvement to speed for HTML is plausible
2. ~20x improvement to speed for PDF is plausible
3. Massive improvement in quality is plausible 

## Is doc2struct useful?

It is for me. Hope it is for you as well. It is not the right tool for many use cases. PDF support will remain minimal.

## Acknowledgements

- [Justin Lokos](https://www.linkedin.com/in/justinlokos/): for coding loop suggestions.
- [simdjson team](https://github.com/simdjson/simdjson): Daniel Lemire, Geoff Langdale, John Keiser, Paul Dreik, Francisco Thiesen and others for inspiration on how to apply SIMD.
- [lexbor](https://github.com/lexbor/lexbor/tree/f4cbbcd91359a0ec9499e3ce7e263de629482d61): Alexander Borisov for writing an excellent html parsing library that was used as the backbone of doc2dict.
- [xmltodict](https://github.com/martinblech/xmltodict): Martin Blech's excellent XML library.
- [Rysana](https://rysana.com/): for general inspiration on optimization and language choice.

And many OSS contributors:

dermonito, AnirudhJM24, GuangzheLeLe Wang, Owen Gordon, Mike Meyers, Jeff Meyers, l0pl0p, d6tdev, xingren23, Norman Niemer, paraknell, Wanxuan Li, cardal4, Matthew Massicotte, Benedetto Leto, everling, Alex Braid, Muhsin Tcheifa, Benjamin Cance, mjc0608, hrishikesh kamath, a-dafinca, Henry Meng, fountainheadpro, Michael Haggerty, fmmoret, Jerry Bard, Rian Dolphin, Gal Skarishevsky, Rui A, Denis Lima E Alves, Bill McDonald, rbarman, msharifbd, eltociear, Athe-kunal, and others.