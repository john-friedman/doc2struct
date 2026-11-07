#include "doc2struct.h"
#include "scan.h"
#include <string.h>
#include <stdio.h>

const char *ds_cleanup_names[] = {
    "trimmed_cell_whitespace", "merged_formatting_chars", "merged_numeric_ranges",
    "removed_empty_rows", "removed_empty_columns", "removed_span_rows",
    "removed_span_columns", "merged_header_rows",
    "unresolved_formatting_cells", "unresolved_header_alignment",
    "ambiguous_text_attachment", NULL
};

enum {
    CELL_PRESENT = 1, CELL_NUMERIC = 2, CELL_YEAR = 4, CELL_DIGIT = 8,
    CELL_PREFIX = 16, CELL_SUFFIX = 32, CELL_FORMATTING = 64
};
typedef struct {
    Matrix *matrix;
    unsigned char *facts;
    int formatting;
} TableState;

static unsigned char cell_facts(Str, int);
static void replace_cell(TableState *table, size_t index, Str text) {
    table->matrix->cells[index] = text;
    table->facts[index] = cell_facts(text, table->formatting);
}

static int numeric(Str text) {
    size_t i, k;
    int digits = 0, missing = 0;
    text = ds_strip(text, 1);
    for (i = 0; i < text.n; i += k) {
        uint32_t c = ds_utf8(text.p + i, text.n - i, &k);
        if (c >= '0' && c <= '9') {
            digits = 1;
            continue;
        }
        if (c == '-' || c == 0x2013 || c == 0x2014)
            missing = 1;
        if (c == '$' || c == '(' || c == ')' || c == '.' || c == ',' || c == '%' ||
            c == '+' || c == '-' || c == 0x2013 || c == 0x2014 || c == 0xa3 ||
            c == 0x20ac || c == 0xa5 || c == '*' || ds_space(c))
            continue;
        /* A number may carry a parenthesized alphabetic footnote reference. */
        if (digits && i && text.p[i - 1] == '(' && text.p[text.n - 1] == ')' &&
            text.n - i <= 5) {
            size_t q;
            for (q = i; q + 1 < text.n; q++)
                if (!((text.p[q] >= 'a' && text.p[q] <= 'z') ||
                      (text.p[q] >= 'A' && text.p[q] <= 'Z')))
                    break;
            if (q + 1 == text.n)
                return 1;
        }
        return 0;
    }
    return digits || missing;
}
static int year(Str text) {
    size_t i;
    if (text.n != 4 || !((text.p[0] == '1' && text.p[1] == '9') ||
                         (text.p[0] == '2' && text.p[1] == '0')))
        return 0;
    for (i = 2; i < 4; i++)
        if (text.p[i] < '0' || text.p[i] > '9')
            return 0;
    return 1;
}
static int same_cell(Str a, Str b) { return a.p == b.p && a.n == b.n; }
static int has_digit(Str text) {
    size_t i = 0;
    for (; text.n - i >= 16; i += 16)
        if (ds_ascii_masks(text.p + i).digit) return 1;
    for (; i < text.n; i++)
        if (text.p[i] >= '0' && text.p[i] <= '9')
            return 1;
    return 0;
}
static int dash(Str text) {
    return ds_is(text, "-") || ds_is(text, "\342\200\223") || ds_is(text, "\342\200\224");
}
/* Expanded colspans repeat the same source slice. When attaching formatting
 * to that value, retain one logical value rather than an unformatted copy. */
static void clear_copies(TableState *table, size_t row, size_t col) {
    Matrix *m = table->matrix;
    size_t c;
    Str source = m->cells[row * m->nc + col];
    for (c = 0; c < m->nc; c++)
        if (c != col && same_cell(source, m->cells[row * m->nc + c]))
            replace_cell(table, row * m->nc + c, ds_str(""));
}

/* A right-aligned year can label a range spread across formatting cells.
 * Join two endpoints only within one year group and without Low/High-style
 * subheaders. Other multi-value columns remain unchanged. */
static int merge_ranges(Document *d, Row *row, TableState *table) {
    Matrix *m = &row->matrix;
    size_t h, c, n = 0, i, g, *ends;
    if (m->nc < 3)
        return 1;
    ends = ds_alloc(&d->arena, m->nc * sizeof(size_t));
    if (!ends)
        return 0;
    for (h = 0; h < m->nr && h < 12; h++) {
        Str previous = {0};
        n = 0;
        for (c = 0; c < m->nc; c++) {
            Str value = m->cells[h * m->nc + c];
            if (!(table->facts[h * m->nc + c] & CELL_YEAR))
                continue;
            if (n && c == ends[n - 1] + 1 && same_cell(value, previous))
                ends[n - 1] = c;
            else
                ends[n++] = c;
            previous = value;
        }
        if (n >= 2)
            break;
    }
    if (h == m->nr || h == 12)
        return 1;
    for (g = 0; g < n; g++) {
        size_t start = g ? ends[g - 1] + 1 : 0;
        int subheader = 0, data = 0;
        for (i = h + 1; i < m->nr; i++) {
            size_t count = 0, low = 0, high = 0;
            int text_after_value = 0, labels = 0;
            for (c = start; c <= ends[g]; c++) {
                Str value = m->cells[i * m->nc + c];
                if (!value.n)
                    continue;
                if (table->facts[i * m->nc + c] & CELL_NUMERIC) {
                    if (!count)
                        low = c;
                    high = c;
                    count++;
                } else {
                    if (count)
                        text_after_value = 1;
                    /* The first column is normally the observation label. */
                    if (c > start)
                        labels = 1;
                }
            }
            if (!data && !count && labels)
                subheader = 1;
            if (count)
                data = 1;
            if (count == 2 && !subheader && !text_after_value) {
                Str last = m->cells[i * m->nc + high];
                size_t step, endpoint;
                uint32_t first = ds_utf8(last.p, last.n, &step);
                endpoint = step;
                if (endpoint < last.n && last.p[endpoint] == '$')
                    endpoint++;
                if ((first == '-' || first == 0x2013) && endpoint < last.n &&
                    last.p[endpoint] >= '0' && last.p[endpoint] <= '9') {
                    replace_cell(table, i * m->nc + high,
                                 ds_cat(&d->arena, m->cells[i * m->nc + low], last));
                    replace_cell(table, i * m->nc + low, ds_str(""));
                    row->cleaned |= DS_MERGED_RANGES;
                }
            }
        }
    }
    return !d->arena.error[0];
}

static void diagnose(Row *row, const TableState *table) {
    Matrix *m = &row->matrix;
    size_t i, j, h;
    for (i = 0; i < m->nr * m->nc; i++)
        if (table->facts[i] & CELL_FORMATTING)
            row->cleaned |= DS_TABLE_FORMATTING;
    for (h = 0; h < m->nr && h < 8; h++) {
        size_t years = 0;
        for (j = 0; j < m->nc; j++)
            years += (table->facts[h * m->nc + j] & CELL_YEAR) != 0;
        if (years < 2)
            continue;
        for (j = 1; j < m->nc; j++) {
            if (m->cells[h * m->nc + j].n)
                continue;
            for (i = h + 1; i < m->nr; i++)
                if (table->facts[i * m->nc + j] & CELL_NUMERIC) {
                    row->cleaned |= DS_TABLE_ALIGNMENT;
                    break;
                }
        }
        break;
    }
}

static int blank(Str s) { return !s.n || ds_is(s, "-") || ds_is(s, "\342\200\223"); }
static int format(Str s, int which) {
    size_t i = 0, k;
    int found = 0;
    for (; i < s.n; i += k) {
        uint32_t c = ds_utf8(s.p + i, s.n - i, &k);
        int allowed = which == 0   ? (c == '$' || c == '(' || c == 0x2013 || c == '-')
                      : which == 1 ? (c == ')' || c == '%' || c == 0x2013 || c == '-')
                                   : (c == '$' || c == '(' || c == ')' || c == '%' || c == 0x2013 ||
                                      c == '-');
        if (!allowed)
            return which == 2;
        found = 1;
    }
    return which == 2 ? 0 : found;
}

/* The original classifiers define the semantics, including Unicode currency
 * and alphabetic footnotes. Compute their results once per cell version. */
static unsigned char cell_facts(Str text, int formatting) {
    unsigned char facts = 0;
    if (!text.n) return facts;
    if (!blank(text)) facts |= CELL_PRESENT;
    if (numeric(text)) facts |= CELL_NUMERIC;
    if (year(text)) facts |= CELL_YEAR;
    if (formatting) {
        if (has_digit(text)) facts |= CELL_DIGIT;
        if (format(text, 0)) facts |= CELL_PREFIX;
        if (format(text, 1)) facts |= CELL_SUFFIX;
    }
    if (text.n == 1 && (text.p[0] == '$' || text.p[0] == '(' || text.p[0] == ')'))
        facts |= CELL_FORMATTING;
    return facts;
}

static int enabled(const Rules *r, unsigned i) { return (r->table_steps & (1u << i)) != 0; }
static void select_rows(TableState *table, unsigned char *keep) {
    Matrix *m = table->matrix;
    size_t i, j = 0;
    for (i = 0; i < m->nr; i++)
        if (keep[i]) {
            if (i != j) {
                memmove(m->cells + j * m->nc, m->cells + i * m->nc, m->nc * sizeof(Str));
                memmove(table->facts + j * m->nc, table->facts + i * m->nc, m->nc);
            }
            j++;
        }
    m->nr = j;
}
static void select_cols(TableState *table, unsigned char *keep) {
    Matrix *m = table->matrix;
    size_t i, j, k = 0, nc = 0;
    for (j = 0; j < m->nc; j++)
        nc += keep[j] != 0;
    for (i = 0; i < m->nr; i++)
        for (j = 0; j < m->nc; j++)
            if (keep[j]) {
                table->facts[k] = table->facts[i * m->nc + j];
                m->cells[k++] = m->cells[i * m->nc + j];
            }
    m->nc = nc;
}
static int clean_table(Document *d, const Rules *r, Row *row, Vec *scratch) {
    Matrix *m = &row->matrix;
    TableState table = {m, NULL, enabled(r, 1)};
    size_t i, j, k, count = m->nr * m->nc;
    unsigned char *keep, *data_columns;
    row->cleaned = 0;
    /* Facts are scratch data: retain one buffer across this document's tables. */
    if (!scratch->p || count > scratch->cap) {
        size_t capacity = count;
        if (scratch->cap <= SIZE_MAX / 2 && scratch->cap * 2 > capacity)
            capacity = scratch->cap * 2;
        scratch->p = ds_alloc_uninit(&d->arena, capacity);
        if (!scratch->p) return 0;
        scratch->cap = capacity;
    }
    table.facts = scratch->p;
    for (i = 0; i < m->nr * m->nc; i++) {
        Str stripped = ds_strip(m->cells[i], 1);
        if (stripped.n != m->cells[i].n)
            row->cleaned |= DS_TRIMMED_CELLS;
        m->cells[i] = stripped;
        table.facts[i] = i && same_cell(stripped, m->cells[i - 1]) ? table.facts[i - 1] :
                        cell_facts(stripped, table.formatting);
    }
    if (enabled(r, 1)) {
        data_columns = ds_alloc(&d->arena, m->nc);
        if (!data_columns)
            return 0;
        for (i = 0; i < m->nr; i++)
            for (j = 0; j < m->nc; j++) {
                unsigned char facts = table.facts[i * m->nc + j];
                if ((facts & (CELL_DIGIT | CELL_NUMERIC | CELL_YEAR)) == (CELL_DIGIT | CELL_NUMERIC))
                    data_columns[j] = 1;
            }
        for (i = 0; i < m->nr; i++)
            for (j = 0; j < m->nc; j++) {
                Str t = m->cells[i * m->nc + j];
                if ((table.facts[i * m->nc + j] & CELL_PREFIX) && !(dash(t) && data_columns[j]))
                    for (k = j + 1; k < m->nc; k++)
                        if (m->cells[i * m->nc + k].n) {
                            Str next = m->cells[i * m->nc + k];
                            if (table.facts[i * m->nc + k] & (CELL_NUMERIC | CELL_PREFIX)) {
                                clear_copies(&table, i, j);
                                clear_copies(&table, i, k);
                                replace_cell(&table, i * m->nc + k, ds_cat(&d->arena, t, next));
                                replace_cell(&table, i * m->nc + j, ds_str(""));
                                row->cleaned |= DS_MERGED_FORMATTING;
                            }
                            break;
                        }
            }
        for (i = 0; i < m->nr; i++)
            for (j = 0; j < m->nc; j++) {
                Str t = m->cells[i * m->nc + j];
                if ((table.facts[i * m->nc + j] & CELL_SUFFIX) && !(dash(t) && data_columns[j]))
                    for (k = j; k; k--)
                        if (m->cells[i * m->nc + k - 1].n) {
                            if (table.facts[i * m->nc + k - 1] & CELL_NUMERIC) {
                                clear_copies(&table, i, j);
                                clear_copies(&table, i, k - 1);
                                replace_cell(&table, i * m->nc + k - 1,
                                             ds_cat(&d->arena, m->cells[i * m->nc + k - 1], t));
                                replace_cell(&table, i * m->nc + j, ds_str(""));
                                row->cleaned |= DS_MERGED_FORMATTING;
                            }
                            break;
                        }
            }
        if (!merge_ranges(d, row, &table))
            return 0;
    }
    keep = ds_alloc(&d->arena, m->nr > m->nc ? m->nr : m->nc);
    if (!keep)
        return 0;
    if (enabled(r, 2)) {
        for (i = 0; i < m->nr; i++)
            keep[i] = ds_flags_any(table.facts + i * m->nc, m->nc, CELL_PRESENT);
        {
            size_t before = m->nr;
            select_rows(&table, keep);
            if (before != m->nr)
                row->cleaned |= DS_EMPTY_ROWS;
        }
    }
    if (enabled(r, 3) && m->nr && m->nc) {
        size_t missing = m->nc;
        memset(keep, 0, m->nc);
        for (i = 0; i < m->nr && missing; i++)
            missing -= ds_flags_columns(keep, table.facts + i * m->nc, m->nc, CELL_PRESENT);
        {
            size_t before = m->nc;
            select_cols(&table, keep);
            if (before != m->nc)
                row->cleaned |= DS_EMPTY_COLUMNS;
        }
    }
    if (enabled(r, 4)) {
        memset(keep, 1, m->nr);
        for (i = m->nr; i > 1; i--) {
            size_t a = i - 1, b = i - 2;
            for (j = 0; j < m->nc; j++)
                if ((table.facts[a * m->nc + j] & CELL_PRESENT) &&
                    !same_cell(m->cells[a * m->nc + j], m->cells[b * m->nc + j]))
                    break;
            if (j == m->nc)
                keep[a] = 0;
        }
        {
            size_t before = m->nr;
            select_rows(&table, keep);
            if (before != m->nr)
                row->cleaned |= DS_SPAN_ROWS;
        }
    }
    if (enabled(r, 5)) {
        memset(keep, 1, m->nr);
        for (i = 0; i + 1 < m->nr; i++) {
            for (j = 0; j < m->nc; j++)
                if ((table.facts[i * m->nc + j] & CELL_PRESENT) &&
                    !same_cell(m->cells[i * m->nc + j], m->cells[(i + 1) * m->nc + j]))
                    break;
            if (j == m->nc)
                keep[i] = 0;
        }
        {
            size_t before = m->nr;
            select_rows(&table, keep);
            if (before != m->nr)
                row->cleaned |= DS_SPAN_ROWS;
        }
    }
    for (k = 6; k <= 7; k++)
        if (enabled(r, (unsigned)k) && m->nr) {
            memset(keep, 1, m->nc);
            for (j = 0; j + 1 < m->nc; j++) {
                size_t a = k == 6 ? j : j + 1, b = k == 6 ? j + 1 : j;
                for (i = 0; i < m->nr; i++)
                    if ((table.facts[i * m->nc + a] & CELL_PRESENT) &&
                        !same_cell(m->cells[i * m->nc + a], m->cells[i * m->nc + b]))
                        break;
                if (i == m->nr)
                    keep[a] = 0;
            }
            {
                size_t before = m->nc;
                select_cols(&table, keep);
                if (before != m->nc)
                    row->cleaned |= DS_SPAN_COLUMNS;
            }
        }
    if (enabled(r, 8) && m->nr >= 2) {
        int header = 1;
        for (i = 0; i < 2 * m->nc; i++)
            if ((table.facts[i] & (CELL_NUMERIC | CELL_YEAR)) == CELL_NUMERIC)
                header = 0;
        for (j = 0; header && j + 1 < m->nc; j++)
            if (m->cells[j].n && same_cell(m->cells[j], m->cells[j + 1]))
                break;
        if (header && j + 1 < m->nc) {
            for (j = 0; j < m->nc; j++)
                if (!ds_eq(m->cells[j], m->cells[m->nc + j])) {
                    Str parts[] = {m->cells[j], m->cells[m->nc + j]};
                    replace_cell(&table, j, ds_join(&d->arena, parts, 2, ds_str("\n")));
                }
            if (m->nr > 2) {
                memmove(m->cells + m->nc, m->cells + 2 * m->nc, (m->nr - 2) * m->nc * sizeof(Str));
                memmove(table.facts + m->nc, table.facts + 2 * m->nc, (m->nr - 2) * m->nc);
            }
            m->nr--;
            row->cleaned |= DS_MERGED_HEADERS;
        }
    }
    diagnose(row, &table);
    return !d->arena.error[0];
}
static int text_row(Row *r) { return ds_is(r->type, "text") || ds_is(r->type, "textsmall"); }
static int annotation_text(Document *d, const Rules *r, Row *row) {
    return text_row(row) && (!r->annotation_exclude ||
           !ds_match(d, r->annotation_exclude, ds_strip(row->text, 1), 0));
}
static int contains(Str text, const char *part) {
    size_t i, n = strlen(part);
    for (i = 0; i + n <= text.n; i++)
        if (!memcmp(text.p + i, part, n))
            return 1;
    return 0;
}
static int introduction(Document *d, Str text) {
    Str lower = ds_lower(&d->arena, ds_strip(text, 1));
    if (!lower.n)
        return 0;
    return lower.p[lower.n - 1] == ':' || contains(lower, "as follows") ||
           contains(lower, "following table") || contains(lower, "presented below") ||
           contains(lower, "summarized below") || contains(lower, "shown below") ||
           contains(lower, "set forth below") || contains(lower, "in millions") ||
           contains(lower, "in thousands") || contains(lower, "in billions");
}
static int next_table(Row *rows, size_t n, size_t start) {
    while (start < n && text_row(rows + start))
        start++;
    return start < n && ds_is(rows[start].type, "table");
}
static int continuation(Document *d, Row *note, Row *candidate, Str previous) {
    Run *a = note->nitems ? &note->items[0].run : NULL;
    Run *b = candidate->nitems ? &candidate->items[0].run : NULL;
    Str text = ds_strip(candidate->text, 1);
    previous = ds_strip(previous, 1);
    if (!text.n || introduction(d, text) || !ds_eq(note->type, candidate->type))
        return 0;
    if (a && b && a->has_font && b->has_font && a->font != b->font)
        return 0;
    if (a && b && a->has_indent && b->has_indent && b->indent > a->indent + 2)
        return 1;
    /* Additional paragraphs need evidence of continuation, beyond proximity. */
    return (text.p[0] >= 'a' && text.p[0] <= 'z') ||
           (previous.n && previous.p[previous.n - 1] != '.' &&
            previous.p[previous.n - 1] != ';' && previous.p[previous.n - 1] != ':');
}
static int add_note(Document *d, Vec *notes, Str id, Str text) {
    Note *n = ds_push(&d->arena, notes, sizeof(Note));
    if (!n)
        return 0;
    *n = (Note){id, text};
    return 1;
}
static int notes_table(Document *d, const Rules *r, Matrix *m, Vec *notes) {
    size_t i, original = notes->n;
    if (!m->nr || m->nc != 2)
        return 0;
    for (i = 0; i < m->nr; i++) {
        Str marker = ds_strip(m->cells[i * 2], 0);
        if (r->annotation_exclude) {
            Str pieces[] = {marker, ds_strip(m->cells[i * 2 + 1], 0)};
            if (ds_match(d, r->annotation_exclude, ds_join(&d->arena, pieces, 2, ds_str(" ")), 0)) {
                notes->n = original;
                return 0;
            }
        }
        if (!ds_match(d, r->footnote, marker, 0)) {
            notes->n = original;
            return 0;
        }
        if (!r->footnote->groups) {
            snprintf(d->arena.error, sizeof(d->arena.error),
                     "footnote regex must have a capture group");
            return 0;
        }
        if (ds_group(d, marker, 0).n != marker.n) {
            notes->n = original;
            return 0;
        }
        if (!add_note(d, notes, ds_group(d, marker, 1), ds_strip(m->cells[i * 2 + 1], 0)))
            return 0;
    }
    return 1;
}
static int absorb(Document *d, const Rules *r) {
    Row *rows = d->rows.p;
    size_t i, n = d->rows.n;
    /* Reserve introductions for every table before an earlier table can
     * consume them as footnotes or postamble. Between tables, ambiguous text
     * stays in the document rather than being assigned by traversal order. */
    if (r->preamble_lines > 0)
        for (i = 0; i < n; i++) {
            size_t start = i, boundary, chosen = i, j;
            Vec parts = {0};
            if (!ds_is(rows[i].type, "table"))
                continue;
            while (start && i - start < (size_t)r->preamble_lines &&
                   annotation_text(d, r, rows + start - 1))
                start--;
            boundary = start;
            while (boundary && annotation_text(d, r, rows + boundary - 1))
                boundary--;
            if (!boundary || !ds_is(rows[boundary - 1].type, "table"))
                chosen = start;
            else
                for (j = i; j > start; j--)
                    if (introduction(d, rows[j - 1].text)) {
                        chosen = j - 1;
                        break;
                    }
            for (j = chosen; j < i; j++) {
                Str *part = ds_push(&d->arena, &parts, sizeof(Str));
                if (!part)
                    return 0;
                *part = rows[j].text;
                rows[j].removed = 1;
            }
            if (parts.n)
                rows[i].preamble = ds_join(&d->arena, parts.p, parts.n, ds_str(" "));
        }
    for (i = 0; i < n; i++) {
        Row *row = rows + i;
        size_t j, k, last = i;
        Vec parts = {0}, notes = {0};
        if (row->removed || !ds_is(row->type, "table"))
            continue;
        if (r->footnote) {
            j = i + 1;
            while (j < n && !rows[j].removed) {
                Row *x = rows + j;
                Str marker, id, text;
                size_t extra = 0;
                if (ds_is(x->type, "table")) {
                    if (!notes_table(d, r, &x->matrix, &notes))
                        break;
                    x->removed = 1;
                    last = j++;
                    continue;
                }
                if (!annotation_text(d, r, x))
                    break;
                marker = ds_strip(x->text, 0);
                if (!ds_match(d, r->footnote, marker, 0))
                    break;
                if (!r->footnote->groups) {
                    snprintf(d->arena.error, sizeof(d->arena.error),
                             "footnote regex must have a capture group");
                    return 0;
                }
                id = ds_group(d, marker, 1);
                text = ds_group(d, marker, 0);
                text = ds_strip((Str){marker.p + text.n, marker.n - text.n}, 0);
                x->removed = 1;
                k = j + 1;
                while (k < n && r->footnote_lines > 0 && extra < (size_t)r->footnote_lines &&
                       annotation_text(d, r, rows + k) && !rows[k].removed &&
                       continuation(d, x, rows + k, text) &&
                       !ds_match(d, r->footnote, ds_strip(rows[k].text, 0), 0)) {
                    Str fragments[] = {text, ds_strip(rows[k].text, 0)};
                    text = ds_join(&d->arena, fragments, 2, ds_str(" "));
                    rows[k].removed = 1;
                    extra++;
                    k++;
                }
                if (!add_note(d, &notes, id, text))
                    return 0;
                last = k - 1;
                j = k;
            }
            row->notes = notes.p;
            row->nnotes = notes.n;
        }
        if (r->postamble_lines > 0) {
            parts = (Vec){0};
            j = last + 1;
            while (j < n && parts.n < (size_t)r->postamble_lines && annotation_text(d, r, rows + j) &&
                   !rows[j].removed) {
                if (next_table(rows, n, j)) {
                    row->cleaned |= DS_TABLE_ATTACHMENT;
                    break;
                }
                Str *p = ds_push(&d->arena, &parts, sizeof(Str));
                if (!p)
                    return 0;
                *p = rows[j].text;
                rows[j].removed = 1;
                j++;
            }
            if (parts.n)
                row->postamble = ds_join(&d->arena, parts.p, parts.n, ds_str(" "));
        }
    }
    return !d->arena.error[0];
}
int ds_tables(Document *d, const Rules *r) {
    Row *rows = d->rows.p;
    Vec scratch = {0};
    size_t i, j, out = 0;
    for (i = 0; i < d->rows.n; i++) {
        Row row = rows[i];
        if (ds_is(row.type, "table")) {
            if (!clean_table(d, r, &row, &scratch))
                return 0;
            if (!row.matrix.nr)
                continue;
            if (r->single_row_to_text && row.matrix.nr == 1) {
                row.type = row.klass = ds_str("text");
                row.text = ds_join(&d->arena, row.matrix.cells, row.matrix.nc, ds_str(" "));
                row.matrix = (Matrix){0};
                row.cleaned = -1;
            }
        }
        rows[out++] = row;
    }
    d->rows.n = out;
    if (!absorb(d, r))
        return 0;
    if (r->title_from_section) {
        Row *head = NULL, *table = NULL;
        size_t count = 0;
        for (i = 0; i <= d->rows.n; i++) {
            if (i < d->rows.n && rows[i].removed)
                continue;
            if (i == d->rows.n || ds_is(rows[i].type, "title")) {
                if (head && count == 1 && !table->text.n)
                    table->text = head->text;
                head = i < d->rows.n ? rows + i : NULL;
                table = NULL;
                count = 0;
            } else if (ds_is(rows[i].type, "table")) {
                table = rows + i;
                count++;
            }
        }
    }
    for (i = 0, j = 0; i < d->rows.n; i++)
        if (!rows[i].removed) {
            rows[j] = rows[i];
            rows[j].id = j;
            j++;
        }
    d->rows.n = j;
    return !d->arena.error[0];
}
