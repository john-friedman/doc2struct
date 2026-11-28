#ifndef DOC2STRUCT_PUBLIC_H
#define DOC2STRUCT_PUBLIC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DOC2STRUCT_VERSION "0.1.8"
#define DOC2STRUCT_DEFAULT_CACHE_SIZE ((size_t)8 * 1024 * 1024)
typedef struct doc2struct_rules doc2struct_rules;
typedef struct doc2struct_parser doc2struct_parser;
typedef struct doc2struct_document doc2struct_document;
typedef struct doc2struct_item doc2struct_item;
typedef struct doc2struct_regex doc2struct_regex;
typedef struct doc2struct_style doc2struct_style;
typedef struct { const char *p; size_t n; } doc2struct_string;
typedef struct { char message[256]; } doc2struct_error;
enum { DOC2STRUCT_TEXT, DOC2STRUCT_START, DOC2STRUCT_END };
enum { DOC2STRUCT_BOLD=1, DOC2STRUCT_ITALIC=2, DOC2STRUCT_UNDERLINE=4,
       DOC2STRUCT_STRIKETHROUGH=128, DOC2STRUCT_TEXT_STYLES=135 };
enum { DOC2STRUCT_STYLE, DOC2STRUCT_COLSPAN, DOC2STRUCT_ROWSPAN, DOC2STRUCT_HREF,
       DOC2STRUCT_SRC, DOC2STRUCT_ALT, DOC2STRUCT_HIDDEN, DOC2STRUCT_ATTRIBUTE_COUNT };
typedef struct {
    doc2struct_string value[DOC2STRUCT_ATTRIBUTE_COUNT];
    doc2struct_style *prepared_style; /* Library-owned; leave NULL on input. */
} doc2struct_attributes;
typedef struct {
    int kind;
    doc2struct_string value;
    doc2struct_attributes *attrs;
} doc2struct_token;
typedef struct { const void *chunk; size_t index; } doc2struct_token_iterator;

/* Records are borrowed views, valid until their document/rules are freed.
 * Only records returned by add_token/add_run may be populated by the caller. */
typedef struct {
    size_t block, line, row, col, rowspan, colspan;
    doc2struct_string text, href, tag, src;
    unsigned flags;
    double font, indent;
    int in_table, has_font, has_indent;
    int soft_break; /* Layout geometry identifies a possible wrapped continuation. */
    int logical_cell; /* Cell boundaries are already resolved; keep numeric marks in their cells. */
    size_t page;
    double bbox[4]; /* Optional top-left CSS-pixel x0,y0,x1,y1 source bounds. */
    int has_bbox;
} doc2struct_run;
typedef struct { doc2struct_string text; unsigned flags; } doc2struct_text_run;
typedef struct { doc2struct_text_run *runs; size_t count; } doc2struct_cell_style;
typedef struct {
    doc2struct_string *cells; size_t nr, nc;
    doc2struct_cell_style *styles; /* Optional parallel array; cells stay plain strings. */
} doc2struct_matrix;
typedef struct { doc2struct_string id, text; } doc2struct_note;

/* Optional styled slices of plain row text; matrix cells remain strings. */
typedef struct {
    size_t id, level;
    doc2struct_string type, klass, title, text, preamble, postamble;
    doc2struct_matrix matrix;
    int cleaned, removed;
    doc2struct_item *items;
    size_t nitems;
    doc2struct_note *notes;
    size_t nnotes;
    unsigned style_mask;
    doc2struct_text_run *text_runs;
    size_t ntext_runs;
    int logical_cells;
} doc2struct_row;
typedef struct {
    doc2struct_string name, source, dtype, which;
    doc2struct_string *types;
    size_t ntypes;
    int filter_types;
    doc2struct_regex *rx; /* Opaque compiled pattern; use column_text to match. */
    doc2struct_string *classes;
    size_t nclasses;
    int filter_classes;
    doc2struct_string value; /* Optional regex replacement; NULL means capture extraction. */
} doc2struct_column;

/* NULL name loads defaults. Names and document aliases are case-insensitive.
 * JSON accepts a partial rules object; defaults and validation live in C.
 * Rules are immutable after creation and may be shared by independent parsers. */
doc2struct_rules *doc2struct_rules_preset(const char *name, doc2struct_error *error);
doc2struct_rules *doc2struct_rules_json(const char *json, size_t size, doc2struct_error *error);
void doc2struct_rules_free(doc2struct_rules *rules);
const doc2struct_column *doc2struct_rules_columns(const doc2struct_rules *, size_t *count);
/* Embedded canonical JSON, valid for the lifetime of the library. */
size_t doc2struct_preset_count(void);
const char *doc2struct_preset_namespace(size_t index);
doc2struct_string doc2struct_preset_pack(size_t index);
doc2struct_string doc2struct_defaults_json(void);
/* Resolve a partial object or preset to a fully defaulted JSON rules object.
 * Free the returned buffer with doc2struct_buffer_free. */
char *doc2struct_rules_resolve(const char *input, size_t size, int preset,
                             size_t *output_size, doc2struct_error *error);
/* Append extra headers/columns. Override nondefault adapter settings and replace
 * nondefault rule sections. Inputs are JSON objects; output uses buffer_free. */
char *doc2struct_rules_merge_json(doc2struct_string base, doc2struct_string extra,
                                size_t *output_size, doc2struct_error *error);

doc2struct_parser *doc2struct_parser_new(size_t cache_size, doc2struct_error *error);
void doc2struct_parser_clear(doc2struct_parser *);
void doc2struct_parser_close(doc2struct_parser *);
void doc2struct_parser_free(doc2struct_parser *);
size_t doc2struct_parser_cache_limit(const doc2struct_parser *);
size_t doc2struct_parser_cached_bytes(const doc2struct_parser *);

/* Input and rules must outlive the result. A parser must outlive every result
 * using its cache. Each parser is single-owner: serialize its operations,
 * including releasing cached documents. Independent parsers may run in parallel.
 * Pass NULL parser to use an independent document allocation. Input settings
 * come from the compiled rules' adapters section. */
doc2struct_document *doc2struct_parse(doc2struct_parser *, doc2struct_string input,
                                    const doc2struct_rules *, doc2struct_error *error);
/* Required doc2struct PDFium bridge runtime (ABI 2). Pass an absolute UTF-8
 * path to a private copy, not a library initialized by another wrapper. Initialization
 * is idempotent for that path; the engine lives until process exit. PDFium calls
 * are serialized internally; layout/standardization do not hold its lock. */
int doc2struct_pdfium_init(const char *library_path, doc2struct_error *error);
/* Embedded text only (no OCR). Input bytes need only live until return.
 * Password-protected documents requiring a password are unsupported.
 * Results use the same rows/runs as HTML. */
doc2struct_document *doc2struct_parse_pdf(doc2struct_parser *, doc2struct_string input,
                                        const doc2struct_rules *, doc2struct_error *error);
doc2struct_document *doc2struct_pdf_instructions(doc2struct_parser *, doc2struct_string input,
                                               const doc2struct_rules *, doc2struct_error *error);
/* Opt-in layout selection; NULL follows rules.adapters.pdf.layout, while
 * "pdf_adapter" explicitly selects the original layout.
 * "pdf_adapter_experimental" uses text and vector rules to reconstruct tables
 * and matches vector glyphs when embedded font Unicode maps are missing.
 * Both adapters produce the same standardized output schema. */
doc2struct_document *doc2struct_parse_pdf_with_adapter(doc2struct_parser *, doc2struct_string input,
        const doc2struct_rules *, const char *adapter, doc2struct_error *error);
doc2struct_document *doc2struct_pdf_instructions_with_adapter(doc2struct_parser *, doc2struct_string input,
        const doc2struct_rules *, const char *adapter, doc2struct_error *error);
/* External OCR word/region boxes, in page pixels with a top-left origin.
 * Boxes may be unordered; pages are in document order. Text must be UTF-8.
 * Optional flags use DOC2STRUCT_TEXT_STYLES. dpi=0 assumes a 1056px reference
 * page height; a positive dpi supplies physical scale. No OCR is performed.
 * Geometry determines layout and is not included in the resulting runs/rows.
 * Inputs need only live until return; the result owns its text. */
typedef struct {
    doc2struct_string text;
    double left, top, right, bottom;
    unsigned flags;
} doc2struct_bb_word;

typedef struct {
    double width, height, dpi;
    const doc2struct_bb_word *words;
    size_t count;
} doc2struct_bb_page;

doc2struct_document *doc2struct_parse_bb(doc2struct_parser *, const doc2struct_bb_page *, size_t count,
                                       const doc2struct_rules *, doc2struct_error *error);
doc2struct_document *doc2struct_bb_instructions(doc2struct_parser *, const doc2struct_bb_page *, size_t count,
                                              const doc2struct_rules *, doc2struct_error *error);
/* Individual text document bodies; the caller selects the syntax. Text mode
 * treats angle brackets literally. Partial SGML consumes known layout tags,
 * retaining unknown tags/field names. Both return the same runs as HTML/PDF.
 * Input is UTF-8, with the HTML frontend's Windows-1252 fallback for bytes.
 * adapters.text.tab_size and adapters.partialsgml.tab_size are 1..256 (default 8).
 * Indent uses 8px character cells.
 * Results own their text. Input bytes need only live until return. */
doc2struct_document *doc2struct_text_instructions(doc2struct_parser *, doc2struct_string input,
                                                const doc2struct_rules *, doc2struct_error *error);
doc2struct_document *doc2struct_partialsgml_instructions(doc2struct_parser *, doc2struct_string input,
                                                       const doc2struct_rules *, doc2struct_error *error);
/* Direct paths retain native instructions through standardization. Rules must
 * outlive the result; text is document-owned, as on the instruction-only paths. */
doc2struct_document *doc2struct_parse_text(doc2struct_parser *, doc2struct_string input,
                                         const doc2struct_rules *, doc2struct_error *error);
doc2struct_document *doc2struct_parse_partialsgml(doc2struct_parser *, doc2struct_string input,
                                                const doc2struct_rules *, doc2struct_error *error);
/* Page geometry is in points, with a top-left origin after page rotation.
 * Page views are borrowed and valid only during the callback. Returning zero
 * cancels extraction. Callbacks run outside the engine lock. */
typedef struct {
    uint32_t unicode;
    unsigned flags; /* DOC2STRUCT_BOLD/ITALIC/UNDERLINE/STRIKETHROUGH */
    int space_before;
    double left, top, right, bottom, x, y, font, dx, dy;
} doc2struct_pdf_glyph;
typedef struct { double left, top, right, bottom, thickness; unsigned structural; } doc2struct_pdf_rule;
typedef struct {
    size_t index, count;
    double width, height;
    const doc2struct_pdf_glyph *glyphs;
    const doc2struct_pdf_rule *rules;
    size_t rule_count;
} doc2struct_pdf_page;
typedef int (*doc2struct_pdf_page_sink)(void *, const doc2struct_pdf_page *);
int doc2struct_pdf_visit(doc2struct_string input,
                        doc2struct_pdf_page_sink sink, void *context, doc2struct_error *error);
void doc2struct_document_free(doc2struct_document *);
void doc2struct_document_detach_cache(doc2struct_document *);
const char *doc2struct_document_error(const doc2struct_document *);
const doc2struct_row *doc2struct_document_rows(const doc2struct_document *, size_t *count);
/* Standard columns and string-valued text/header_title columns. Other custom
 * columns use row_item/row_links/column_applies/column_text below.
 * with_rules is retained for compatibility; columns always govern the schema. */
char *doc2struct_document_json(doc2struct_document *, const doc2struct_rules *, int with_rules,
                              size_t *size, doc2struct_error *error);
/* Render rows in their supplied document order. Pipe tables use an empty
 * header and retain every source row. Layout whitespace is collapsed; source
 * Markdown is escaped. All supplied source headings are rendered.
 * Heading depths above six become bold paragraphs. Images retain their alt text
 * as placeholders. Table captions matching the current heading are not repeated.
 * Returns a NUL-terminated UTF-8 buffer (including for empty input); free with
 * doc2struct_buffer_free. output_size excludes the terminating NUL.
 * BLOCKQUOTE is the default presentation; PLAIN retains unmarked text and OMIT
 * drops only redundant rows. Input rows and all their fields are borrowed. */
enum { DOC2STRUCT_REDUNDANT_BLOCKQUOTE, DOC2STRUCT_REDUNDANT_PLAIN, DOC2STRUCT_REDUNDANT_OMIT };
char *doc2struct_convert_standardized_text_to_markdown(const doc2struct_row *rows, size_t count,
        int redundant, size_t *output_size, doc2struct_error *error);
void doc2struct_buffer_free(void *);

/* Staged parsing API used by adapters and native diagnostic tools. */
doc2struct_document *doc2struct_document_new(doc2struct_error *);
doc2struct_string doc2struct_document_copy(doc2struct_document *, doc2struct_string);
int doc2struct_tokenize(doc2struct_document *, doc2struct_string);
size_t doc2struct_token_count(const doc2struct_document *);
const doc2struct_token *doc2struct_token_next(const doc2struct_document *, doc2struct_token_iterator *);
doc2struct_token *doc2struct_add_token(doc2struct_document *);
doc2struct_attributes *doc2struct_add_attributes(doc2struct_document *);
int doc2struct_normalize(doc2struct_document *, const doc2struct_rules *);
doc2struct_run *doc2struct_add_run(doc2struct_document *);
const doc2struct_run *doc2struct_document_runs(const doc2struct_document *, size_t *count);
int doc2struct_standardize(doc2struct_document *, const doc2struct_rules *);
const doc2struct_run *doc2struct_row_item(const doc2struct_row *, size_t index);
const doc2struct_string *doc2struct_row_links(const doc2struct_row *, size_t index, size_t *count);
/* Tests row type/class filters; table rows never receive custom columns. */
int doc2struct_column_applies(const doc2struct_column *, const doc2struct_row *);
int doc2struct_column_text(doc2struct_document *, const doc2struct_column *, doc2struct_string,
                          doc2struct_string *result);
const char *doc2struct_cleanup_name(size_t index);

#ifdef __cplusplus
}
#endif
#endif
