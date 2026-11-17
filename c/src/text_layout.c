#include "doc2struct.h"
#include "scan.h"
#include <string.h>
#include <stdio.h>

/* Both frontends produce ordinary runs. SGML recognition is opt-in and only
 * consumes known layout/formatting tags; unknown tags remain source text. */
enum { TL_TEXT, TL_TABLE, TL_END_TABLE, TL_PAGE, TL_MARKERS };
typedef struct { size_t column; unsigned flags; } TextStyle;
typedef struct {
    Str text, raw;
    size_t source, indent, width;
    size_t nfields, numbers, ntabs;
    unsigned flags;
    int kind, rule, analyzed, has_digit;
    Vec markers;
    Vec styles;
} TextLine;
typedef struct {
    Document *document;
    Vec lines;
    size_t tab_size, source;
    unsigned flags;
    int partial, comment;
    int stream, pending_active;
    Run pending;
} TextLayout;

static int horizontal(uint32_t c) { return c != '\n' && c != '\r' && c != '\f' && ds_space(c); }
static inline uint32_t text_utf8(const char *p, size_t n, size_t *step) {
    if (n && (unsigned char)p[0] < 128) { *step = 1; return (unsigned char)p[0]; }
    return ds_utf8(p, n, step);
}
static int tag_is(Str name, const char *word) {
    size_t i, n = strlen(word);
    if (name.n != n) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name.p[i];
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (c != (unsigned char)word[i]) return 0;
    }
    return 1;
}
static int add_line(TextLayout *layout, TextLine line) {
    TextLine *out = ds_push(&layout->document->arena, &layout->lines, sizeof(TextLine));
    if (!out) return 0;
    *out = line;
    return 1;
}
static int rule_line(Str text) {
    size_t i, count = 0;
    for (i = 0; i < text.n; i++) {
        unsigned char c = (unsigned char)text.p[i];
        if (c == ' ' || c == '\t') continue;
        if (c != '-' && c != '=' && c != '_' && c != '+' && c != '|') return 0;
        count++;
    }
    return count >= 3;
}
/* Tabs advance to stops; UTF-8 code points count as one monospaced column.
 * Font size remains unknown. Indentation uses an eight-pixel character cell,
 * keeping native list indentation tolerances compatible with HTML/PDF. */
static int finish_line(TextLayout *layout, Str raw, Vec markers, Vec styles, unsigned flags) {
    Arena *arena = &layout->document->arena;
    TextLine line = {0};
    size_t i, k, used = 0, column = 0, right = 0, capacity = raw.n, leading = 1;
    char *buffer;
    if (ds_text_span(raw.p, raw.n, 0) == raw.n) {
        line.text = line.raw = raw;
        line.source = layout->source;
        line.indent = ds_text_spaces(raw.p, raw.n);
        line.width = raw.n;
        while (line.width && raw.p[line.width - 1] == ' ') line.width--;
        line.flags = flags; line.markers = markers; line.styles = styles;
        line.kind = markers.n && !line.width ? TL_MARKERS : TL_TEXT;
        line.rule = rule_line(raw);
        return add_line(layout, line);
    }
    for (i = 0; i < raw.n; i++) if (raw.p[i] == '\t') {
        if (capacity > SIZE_MAX - layout->tab_size) goto large;
        capacity += layout->tab_size;
        line.ntabs++;
    }
    if (capacity == SIZE_MAX) goto large;
    buffer = ds_alloc_uninit(arena, capacity + 1);
    if (!buffer) return 0;
    for (i = 0; i < raw.n; i += k) {
        uint32_t c = text_utf8(raw.p + i, raw.n - i, &k);
        if (c == '\t') {
            size_t spaces = layout->tab_size - column % layout->tab_size;
            memset(buffer + used, ' ', spaces); used += spaces; column += spaces;
        } else if (horizontal(c)) {
            buffer[used++] = ' '; column++;
        } else {
            memcpy(buffer + used, raw.p + i, k); used += k; column++;
            right = column;
            leading = 0;
        }
        if (leading) line.indent = column;
    }
    buffer[used] = 0;
    line.text = (Str){buffer, used};
    line.raw = raw;
    line.source = layout->source;
    line.width = right;
    line.flags = flags;
    line.markers = markers;
    line.styles = styles;
    line.kind = markers.n && !ds_strip(line.text, 1).n ? TL_MARKERS : TL_TEXT;
    line.rule = rule_line(line.text);
    return add_line(layout, line);
large:
    snprintf(arena->error, sizeof(arena->error), "text line too large");
    return 0;
}
static int event(TextLayout *layout, int kind) {
    TextLine line = {0}; line.kind = kind; line.source = layout->source;
    return add_line(layout, line);
}
static int scan_line(TextLayout *layout, Str raw) {
    Arena *arena = &layout->document->arena;
    Vec markers = {0}, styles = {0};
    size_t i = 0, used = 0, column = 0;
    unsigned flags = layout->flags;
    char *buffer;
    if (!layout->partial) return finish_line(layout, raw, markers, styles, 0);
    if (!layout->comment && !memchr(raw.p, '<', raw.n)) return finish_line(layout, raw, markers, styles, flags);
    buffer = ds_alloc_uninit(arena, raw.n + 1);
    if (!buffer) return 0;
    while (i < raw.n) {
        size_t end, start, q, k, literal_end = i + 1;
        int closing = 0, kind = -1;
        unsigned style = 0;
        char quote = 0;
        Str name;
        if (layout->comment || (raw.n - i >= 4 && !memcmp(raw.p + i, "<!--", 4))) {
            if (!layout->comment) { layout->comment = 1; i += 4; }
            while (i < raw.n && !(raw.n - i >= 3 && !memcmp(raw.p + i, "-->", 3))) i++;
            if (i < raw.n) { layout->comment = 0; i += 3; }
            continue;
        }
        {
            size_t run = ds_text_span(raw.p + i, raw.n - i, 1);
            if (run) {
                memcpy(buffer + used, raw.p + i, run);
                used += run; i += run; column += run;
                continue;
            }
        }
        if (raw.p[i] != '<') goto literal;
        end = i + 1;
        if (end < raw.n && raw.p[end] == '/') { closing = 1; end++; }
        start = end;
        while (end < raw.n && ((raw.p[end] >= 'A' && raw.p[end] <= 'Z') ||
               (raw.p[end] >= 'a' && raw.p[end] <= 'z') || (raw.p[end] >= '0' && raw.p[end] <= '9') ||
               raw.p[end] == '-' || raw.p[end] == '_' || raw.p[end] == ':')) end++;
        name = (Str){raw.p + start, end - start};
        q = end;
        while (end < raw.n) {
            char c = raw.p[end];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '\'' || c == '"') quote = c;
            else if (c == '>' || c == '<') break;
            end++;
        }
        if (end == raw.n || raw.p[end] != '>' || !name.n ||
            (q < end && raw.p[q] != ' ' && raw.p[q] != '\t' && raw.p[q] != '/')) goto literal;
        literal_end = end + 1;
        if (tag_is(name, "TABLE")) kind = closing ? TL_END_TABLE : TL_TABLE;
        else if (tag_is(name, "PAGE")) kind = TL_PAGE;
        else if (tag_is(name, "BR") || tag_is(name, "P")) kind = TL_PAGE;
        else if (tag_is(name, "B")) style = BOLD;
        else if (tag_is(name, "I")) style = ITALIC;
        else if (tag_is(name, "U")) style = UNDERLINE;
        else if (tag_is(name, "S") || tag_is(name, "C")) {
            if (!closing) {
                size_t *position = ds_push(arena, &markers, sizeof(size_t));
                if (!position) return 0;
                *position = column;
            }
            /* Keep source columns while removing the marker itself. */
            memset(buffer + used, ' ', end + 1 - i);
            used += end + 1 - i; column += end + 1 - i;
            i = end + 1; continue;
        } else if (!tag_is(name, "CAPTION")) goto literal;
        if (kind >= 0) {
            if ((used || markers.n) && !finish_line(layout, (Str){buffer, used}, markers, styles, flags)) return 0;
            if (!event(layout, kind)) return 0;
            /* A new slice must not overwrite text retained by finish_line. */
            buffer += used; used = column = 0; markers = (Vec){0}; styles = (Vec){0}; flags = layout->flags;
        }
        if (style) {
            if (closing) layout->flags &= ~style;
            else layout->flags |= style;
            {
                TextStyle *change = ds_push(arena, &styles, sizeof(TextStyle));
                if (!change) return 0;
                *change = (TextStyle){column, layout->flags};
            }
        }
        i = end + 1; continue;
literal:
        while (i < literal_end) {
            uint32_t c = text_utf8(raw.p + i, raw.n - i, &k);
            memcpy(buffer + used, raw.p + i, k); used += k; i += k;
            column += c == '\t' ? layout->tab_size - column % layout->tab_size : 1;
        }
    }
    return finish_line(layout, (Str){buffer, used}, markers, styles, flags);
}
static int scan(TextLayout *layout, Str input) {
    size_t start = 0, i;
    if (input.n >= 3 && !memcmp(input.p, "\xef\xbb\xbf", 3)) start = 3;
    i = start;
    while (i < input.n) {
        i += ds_text_line_end(input.p + i, input.n - i);
        if (i == input.n) break;
        char c = input.p[i];
        if (!scan_line(layout, (Str){input.p + start, i - start})) return 0;
        if (c == '\f' && !event(layout, TL_PAGE)) return 0;
        if (c == '\r' && i + 1 < input.n && input.p[i + 1] == '\n') i++;
        layout->source++; start = ++i;
    }
    return scan_line(layout, (Str){input.p + start, input.n - start});
}
static void analyze_line(TextLine *line) {
    Str text = line->text;
    size_t i = 0, count = 0;
    int active = 0, digit = 0;
    if (line->analyzed) return;
    while (i < text.n) {
        if (text.p[i] == ' ') {
            size_t spaces = ds_text_spaces(text.p + i, text.n - i);
            line->numbers += digit; digit = 0;
            i += spaces;
            if (spaces >= 2) active = 0;
        } else {
            if (!active) { count++; active = 1; }
            if (text.p[i] >= '0' && text.p[i] <= '9') digit = line->has_digit = 1;
            i++;
        }
    }
    line->numbers += digit;
    line->nfields = count; line->analyzed = 1;
}
static size_t fields(TextLine *line, int *numeric) {
    analyze_line(line);
    if (numeric) *numeric = line->has_digit;
    return line->nfields;
}
static size_t line_numbers(TextLine *line) {
    analyze_line(line);
    return line->numbers;
}
static size_t numeric_tokens(Str text) {
    size_t i, count = 0;
    int digit = 0;
    for (i = 0; i <= text.n; i++) {
        if (i == text.n || text.p[i] == ' ') { count += digit; digit = 0; }
        else if (text.p[i] >= '0' && text.p[i] <= '9') digit = 1;
    }
    return count;
}
static int cuts_word(TextLine *line, size_t cut) {
    size_t byte = 0, column = 0, k;
    if (line->text.n == line->width) return cut && cut < line->text.n && line->text.p[cut] != ' ' && line->text.p[cut - 1] != ' ';
    while (byte < line->text.n && column < cut) {
        text_utf8(line->text.p + byte, line->text.n - byte, &k); byte += k; column++;
    }
    return byte && byte < line->text.n && line->text.p[byte] != ' ' && line->text.p[byte - 1] != ' ';
}
/* Casing is evidence for short headings, not for every line of an uppercase
 * paragraph. Ignore enumeration and punctuation when checking title case. */
static unsigned casing(Document *d, Str text) {
    size_t i = 0, words = 0;
    (void)d;
    text = ds_strip(text, 1);
    if (!text.n || text.n > 200) return 0;
    if (ds_upper(text)) return ALL_CAPS;
    /* A leading lowercase stop word is a continuation, not a title. */
    if (text.p[0] >= 'a' && text.p[0] <= 'z') return 0;
    while (i < text.n) {
        size_t start = i;
        unsigned char c = (unsigned char)text.p[i];
        if (c >= 128) return 0;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) { i++; continue; }
        while (i < text.n && ((text.p[i] >= 'A' && text.p[i] <= 'Z') || (text.p[i] >= 'a' && text.p[i] <= 'z'))) i++;
        if (!(c >= 'A' && c <= 'Z') && !ds_stop_word((Str){text.p + start, i - start})) return 0;
        words++;
    }
    return words ? PROPER_CASE : 0;
}
static Run *emit(TextLayout *layout, TextLine *line, size_t block, Str text, unsigned flags) {
    Document *d = layout->document;
    Run *run;
    if (layout->stream) {
        if (layout->pending_active && !ds_flow_run(d, &layout->pending)) return NULL;
        layout->pending = (Run){0}; layout->pending_active = 1;
        run = &layout->pending;
    } else run = doc2struct_add_run(d);
    if (!run) return NULL;
    run->block = block; run->line = line->source; run->text = text;
    run->rowspan = run->colspan = 1;
    run->flags = flags;
    run->has_indent = 1; run->indent = (double)line->indent * 8;
    return run;
}
static int styled_line(TextLayout *layout, TextLine *line, size_t block, unsigned inferred) {
    TextStyle *styles = line->styles.p;
    Str trimmed = ds_strip(line->text, 1);
    size_t i, byte = 0, column = 0, first = (size_t)(trimmed.p - line->text.p), last = first + trimmed.n;
    unsigned flags = line->flags;
    for (i = 0; i <= line->styles.n; i++) {
        size_t start = byte, limit = i < line->styles.n ? styles[i].column : SIZE_MAX, k;
        while (byte < line->text.n && column < limit) {
            text_utf8(line->text.p + byte, line->text.n - byte, &k); byte += k; column++;
        }
        if (start < first) start = first;
        if (byte > start && start < last) {
            Str text = {line->text.p + start, (byte < last ? byte : last) - start};
            if (!emit(layout, line, block, text, flags | inferred)) return 0;
        }
        if (i < line->styles.n) flags = styles[i].flags;
    }
    return 1;
}
static int plain(TextLayout *layout, size_t start, size_t end, size_t *block) {
    Document *d = layout->document;
    TextLine *lines = layout->lines.p;
    size_t i;
    for (i = start; i < end;) {
        TextLine *line = lines + i;
        Str text;
        unsigned flags, style;
        size_t j, k, marker, length;
        int underline = 0, wide = 0;
        if (line->kind != TL_TEXT) { i++; continue; }
        text = ds_strip(line->text, 1);
        if (!text.n) { i++; continue; }
        for (j = 0; j + 3 < text.n; j++)
            if (!memcmp(text.p + j, "    ", 4)) wide = 1;
        style = line->rule || wide ? 0 : casing(d, text);
        flags = line->flags | style;
        if (i + 1 < end && lines[i + 1].rule && !line->rule &&
            lines[i + 1].indent == line->indent && text.n <= 160) {
            size_t rule_width = ds_strip(lines[i + 1].text, 1).n;
            underline = rule_width >= 3 && rule_width <= text.n + 4 && rule_width * 2 >= text.n;
            if (underline) flags |= BOLD | UNDERLINE;
        }
        j = i + 1; length = text.n;
        /* Recognize hard wraps before assigning casing. In particular a
         * capitalized continuation must not become a new section. */
        while (!line->rule && !wide && !underline && !line->styles.n && !ds_list_marker(text, &marker) && j < end) {
            TextLine *next = lines + j, *previous = lines + j - 1;
            Str tail = ds_strip(previous->text, 1), value = ds_strip(next->text, 1);
            unsigned next_style;
            if (next->kind != TL_TEXT || next->rule || !value.n ||
                next->source != previous->source + 1 || ds_list_marker(value, &marker) ||
                next->styles.n || next->flags != line->flags) break;
            for (k = 0; k + 3 < value.n; k++)
                if (!memcmp(value.p + k, "    ", 4)) break;
            if (k + 3 < value.n) break;
            next_style = casing(d, value);
            if (tail.n < 45 || strchr(".!?:;", tail.p[tail.n - 1]) ||
                (style && next_style != style) || (!style && next->indent > line->indent + 4)) break;
            length += value.n + 1;
            j++;
        }
        if (j > i + 1 && (!style || length > 200)) flags = line->flags;
        for (k = i; k < j; k++) {
            Str value = ds_strip(lines[k].text, 1);
            if (lines[k].styles.n) {
                if (!styled_line(layout, lines + k, *block, flags & ~line->flags)) return 0;
                continue;
            }
            if (k > i) value = ds_cat(&d->arena, ds_str(" "), value);
            if (!value.p || !emit(layout, lines + k, *block, value, flags)) return 0;
        }
        (*block)++;
        i = j + underline;
    }
    return !d->arena.error[0];
}
typedef struct { Str text; size_t left, right; int number, letters, fraction, currency; } GridWord;
typedef struct { Vec words; } GridLine;
typedef struct { TextLayout *layout; Vec rows; size_t width; } TextGrid;
static int grid_currency(Str text) {
    return ds_is(text, "$") || ds_is(text, "\xc2\xa3") || ds_is(text, "\xe2\x82\xac") || ds_is(text, "\xc2\xa5");
}

static int grid_words(TextGrid *grid, size_t start, size_t end) {
    Document *d = grid->layout->document;
    TextLine *lines = grid->layout->lines.p;
    size_t i;
    for (i = start; i < end; i++) {
        TextLine *line = lines + i;
        GridLine *row;
        size_t byte = 0, column = 0, k;
        if (line->kind != TL_TEXT || line->rule || !ds_strip(line->text, 1).n) continue;
        row = ds_push(&d->arena, &grid->rows, sizeof(GridLine));
        if (!row) return 0;
        while (byte < line->text.n) {
            size_t first, left;
            GridWord *word;
            if (line->text.p[byte] == ' ') { byte++; column++; continue; }
            first = byte; left = column;
            while (byte < line->text.n && line->text.p[byte] != ' ') {
                text_utf8(line->text.p + byte, line->text.n - byte, &k); byte += k; column++;
            }
            word = ds_push(&d->arena, &row->words, sizeof(GridWord));
            if (!word) return 0;
            *word = (GridWord){{line->text.p + first, byte - first}, left, column, 0};
            word->number = numeric_tokens(word->text) != 0;
            word->fraction = memchr(word->text.p, '/', word->text.n) != NULL;
            word->currency = grid_currency(word->text);
            if (!word->number) {
                size_t q;
                word->letters = ds_upper(word->text);
                for (q = 0; !word->letters && q < word->text.n; q++)
                    if ((word->text.p[q] >= 'a' && word->text.p[q] <= 'z') ||
                        (word->text.p[q] >= 'A' && word->text.p[q] <= 'Z')) word->letters = 1;
            }
        }
    }
    return 1;
}
static size_t grid_word_start(GridLine *row, size_t column, int boundary) {
    GridWord *words = row->words.p;
    size_t first = 0, end = row->words.n;
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        int before = boundary ? words[middle].right <= column : words[middle].left < column;
        if (before) first = middle + 1;
        else end = middle;
    }
    return first;
}
static double grid_boundary(TextGrid *grid, size_t cut) {
    GridLine *rows = grid->rows.p;
    size_t i, j;
    double cost = 0;
    for (i = 0; i < grid->rows.n; i++) {
        GridWord *words = rows[i].words.p;
        j = grid_word_start(rows + i, cut, 1);
        if (j < rows[i].words.n) {
            GridWord *word = words + j;
            if (word->left < cut && cut < word->right) cost += 1000;
            /* One space inside a textual phrase is weak evidence for a cell
             * boundary. This keeps names such as "Class A" together. */
            if (j && words[j - 1].right <= cut && cut <= word->left &&
                word->left - words[j - 1].right == 1 && !word->number && !words[j - 1].number)
                cost += 60;
            if (j && words[j - 1].right <= cut && cut <= word->left &&
                words[j - 1].currency && word->number) cost += 100;
        }
    }
    return cost;
}
static double grid_cell(TextGrid *grid, size_t left, size_t right) {
    GridLine *rows = grid->rows.p;
    size_t i, j;
    double cost = 0;
    for (i = 0; i < grid->rows.n; i++) {
        GridWord *words = rows[i].words.p, *previous = NULL;
        size_t numbers = 0;
        int letters = 0;
        for (j = grid_word_start(rows + i, left, 0); j < rows[i].words.n; j++) {
            GridWord *word = words + j;
            if (word->right > right) break;
            if (previous && word->left - previous->right >= 2 &&
                !(previous->currency && word->number)) cost += 40;
            /* Currency symbols and a mixed fraction belong with their number. */
            if (word->number && !(previous && previous->number && word->fraction)) numbers++;
            letters |= word->letters;
            previous = word;
        }
        if (numbers > 1 && !letters) cost += (double)(numbers - 1) * 35;
    }
    return cost;
}
/* Optimize the complete ordered grid. The marker positions are soft priors;
 * cell contents and all body rows decide which neighboring gutters fit. */
static int grid_fit(TextGrid *grid, Vec anchors, Vec *out) {
    enum { WINDOW = 10, CANDIDATES = 2 * WINDOW + 1 };
    Document *d = grid->layout->document;
    size_t n = anchors.n, *hints = anchors.p, i, j, k, best = 0;
    size_t *positions, *back;
    double previous[CANDIDATES], current[CANDIDATES], best_cost = 1e30;
    if (n < 2 || n > 128) return 1;
    positions = ds_alloc(&d->arena, n * CANDIDATES * sizeof(size_t));
    back = ds_alloc(&d->arena, n * CANDIDATES * sizeof(size_t));
    if (!positions || !back) return 0;
    for (j = 0; j < CANDIDATES; j++) previous[j] = j ? 1e30 : 0;
    for (i = 1; i < n; i++) {
        for (j = 0; j < CANDIDATES; j++) {
            size_t cut;
            double boundary;
            current[j] = 1e30;
            if (hints[i] + j < WINDOW) continue;
            cut = hints[i] + j - WINDOW;
            if (!cut || cut >= grid->width) continue;
            positions[i * CANDIDATES + j] = cut;
            boundary = grid_boundary(grid, cut) + (double)(cut > hints[i] ? cut - hints[i] : hints[i] - cut);
            for (k = 0; k < CANDIDATES; k++) {
                size_t left = positions[(i - 1) * CANDIDATES + k];
                double cost;
                if (previous[k] >= 1e30 || cut <= left + 1) continue;
                cost = previous[k] + boundary + grid_cell(grid, left, cut);
                if (cost < current[j]) { current[j] = cost; back[i * CANDIDATES + j] = k; }
            }
        }
        memcpy(previous, current, sizeof(previous));
    }
    for (j = 0; j < CANDIDATES; j++) {
        size_t left = positions[(n - 1) * CANDIDATES + j];
        double cost = previous[j] + grid_cell(grid, left, grid->width);
        if (cost < best_cost) { best_cost = cost; best = j; }
    }
    if (best_cost >= 1e30) return 1;
    out->p = ds_alloc(&d->arena, n * sizeof(size_t));
    if (!out->p) return 0;
    out->n = out->cap = n;
    for (i = n - 1; i; i--) {
        ((size_t *)out->p)[i] = positions[i * CANDIDATES + best];
        best = back[i * CANDIDATES + best];
    }
    return 1;
}
static int marker_grid(TextLayout *layout, size_t start, size_t end, size_t width, Vec hints, Vec *out) {
    TextGrid grid = {layout, {0}, width};
    GridLine *rows;
    size_t i, j, pass;
    if (!grid_words(&grid, start, end)) return 0;
    if (!grid.rows.n || grid.rows.n > 512 || width > 4096) return 1;
    /* Zero data cost at the exact markers is a proven minimum of the same
     * nonnegative objective used below, including its distance-to-marker term.
     * Removal/insertion cannot improve it either. Keep all ambiguous cases on
     * the full fitter, including phrasing, currency and missing subcolumns. */
    if (hints.n >= 2 && hints.n <= 128) {
        size_t *cuts = hints.p, left = 0;
        for (j = 1; j < hints.n; j++) {
            if (cuts[j] <= left + 1 || cuts[j] >= width || grid_boundary(&grid, cuts[j]) != 0 ||
                grid_cell(&grid, left, cuts[j]) != 0) break;
            left = cuts[j];
        }
        if (j == hints.n && grid_cell(&grid, left, width) == 0) {
            size_t *result = ds_alloc(&layout->document->arena, hints.n * sizeof(size_t));
            if (!result) return 0;
            memcpy(result + 1, cuts + 1, (hints.n - 1) * sizeof(size_t));
            out->p = result; out->n = out->cap = hints.n;
            return 1;
        }
    }
    if (!grid_fit(&grid, hints, out) || !out->n) return !layout->document->arena.error[0];
    rows = grid.rows.p;
    /* A marker may also describe a formatting column that has no independent
     * field in the body. Keeping it must not force cuts through prose labels. */
    for (;;) {
        size_t *cuts = out->p, best_col = 0;
        double best_gain = 25;
        for (j = 1; j < out->n; j++) {
            size_t left = cuts[j - 1], cut = cuts[j], right = j + 1 < out->n ? cuts[j + 1] : width;
            double gain = grid_boundary(&grid, cut) + grid_cell(&grid, left, cut) + grid_cell(&grid, cut, right) -
                          grid_cell(&grid, left, right);
            if (gain > best_gain) { best_gain = gain; best_col = j; }
        }
        if (!best_col) break;
        memmove(cuts + best_col, cuts + best_col + 1, (out->n - best_col - 1) * sizeof(size_t));
        out->n--;
        if (out->n == 1) { out->n = 0; return 1; }
    }
    /* Some old generators omit a marker for a populated subcolumn. Add a
     * boundary only when splitting a repeated-space field improves the fit. */
    for (pass = 0; pass < 4 && out->n < 128; pass++) {
        size_t *cuts = out->p, best_cut = 0, best_col = 0;
        unsigned char *tried = ds_alloc(&layout->document->arena, width);
        double costs[128];
        double best_gain = 12;
        if (!tried) return 0;
        for (j = 0; j < out->n; j++) costs[j] = grid_cell(&grid, cuts[j], j + 1 < out->n ? cuts[j + 1] : width);
        for (i = 0; i < grid.rows.n; i++) {
            GridWord *words = rows[i].words.p;
            for (j = 1; j < rows[i].words.n; j++) {
                size_t cut, col = 0, right;
                double gain;
                if (words[j].left - words[j - 1].right < 2) continue;
                cut = (words[j].left + words[j - 1].right) / 2;
                if (tried[cut]) continue;
                tried[cut] = 1;
                while (col + 1 < out->n && cuts[col + 1] <= cut) col++;
                right = col + 1 < out->n ? cuts[col + 1] : width;
                if (cut <= cuts[col] + 1 || cut + 1 >= right || costs[col] <= best_gain) continue;
                gain = costs[col] - grid_cell(&grid, cuts[col], cut) -
                       grid_cell(&grid, cut, right) - grid_boundary(&grid, cut);
                if (gain > best_gain) { best_gain = gain; best_cut = cut; best_col = col + 1; }
            }
        }
        if (!best_cut) break;
        {
            size_t *added = ds_alloc(&layout->document->arena, (out->n + 1) * sizeof(size_t));
            if (!added) return 0;
            memcpy(added, cuts, best_col * sizeof(size_t)); added[best_col] = best_cut;
            memcpy(added + best_col + 1, cuts + best_col, (out->n - best_col) * sizeof(size_t));
            out->p = added; out->n++; out->cap = out->n;
        }
    }
    return 1;
}
/* Find gutters that stay blank across multiple rows. Partial SGML additionally
 * supplies a body boundary and soft column priors to the shared grid fitter. */
static int columns(TextLayout *layout, size_t start, size_t end, Vec *out, int marked) {
    Document *d = layout->document;
    TextLine *lines = layout->lines.p;
    size_t i, j, width = 0, rows = 0, numbers = 0, quantitative = 0, data_start = start, *position;
    unsigned char *occupied;
    Vec hints = {0};
    for (i = start; i < end; i++) {
        TextLine *line = lines + i;
        int numeric;
        if (line->markers.n >= 2 && line->markers.n >= hints.n) { hints = line->markers; data_start = i + 1; }
        if (line->kind != TL_TEXT || line->rule || !ds_strip(line->text, 1).n) continue;
        if (fields(line, &numeric) >= 2) {
            rows++; numbers += numeric;
            if (line_numbers(line) >= 2) quantitative++;
        }
        if (line->width > width) width = line->width;
    }
    if (!width || width > 65536) return 1;
    if (marked && hints.n >= 2 && hints.n <= 129) {
        if (!marker_grid(layout, data_start, end, width, hints, out)) return 0;
        if (out->n) return 1;
    }
    if (rows < (marked ? 2u : 3u) || (!marked && !numbers && rows < 4)) return 1;
    occupied = ds_alloc(&d->arena, width);
    if (!occupied) return 0;
    for (i = start; i < end; i++) {
        TextLine *line = lines + i;
        size_t byte = 0, column = 0, k;
        if (line->kind != TL_TEXT || line->rule || fields(line, NULL) < 2 ||
            (quantitative >= 3 && line_numbers(line) < 2)) continue;
        while (byte < line->text.n) {
            uint32_t c = text_utf8(line->text.p + byte, line->text.n - byte, &k);
            if (c != ' ') occupied[column] = 1;
            byte += k; column++;
        }
    }
    position = ds_push(&d->arena, out, sizeof(size_t)); if (!position) return 0; *position = 0;
    i = 0;
    while (i < width && !occupied[i]) i++;
    while (i < width) {
        if (occupied[i]) { i++; continue; }
        j = i;
        while (i < width && !occupied[i]) i++;
        if (i < width && (i - j >= 2 || (quantitative >= 3 && quantitative * 2 >= rows))) {
            position = ds_push(&d->arena, out, sizeof(size_t)); if (!position) return 0; *position = i;
            if (out->n > 128) { out->n = 0; return 1; }
        }
    }
    if (out->n < 2) out->n = 0;
    else {
        size_t kept = 1, *cuts = out->p;
        for (j = 1; j < out->n; j++) {
            size_t support = 0;
            for (i = start; i < end; i++)
                if (lines[i].kind == TL_TEXT && !lines[i].rule &&
                    lines[i].indent < cuts[j] && lines[i].width > cuts[j]) support++;
            if (support >= 2) cuts[kept++] = cuts[j];
        }
        out->n = kept >= 2 ? kept : 0;
    }
    return 1;
}
typedef struct { TextLine *line; Str text; } HeaderPiece;
typedef struct { size_t col, span, source, depth; Vec pieces; } TextHeader;

static int header_piece(Document *d, TextHeader *cell, TextLine *line, Str word) {
    HeaderPiece *pieces = cell->pieces.p, *piece;
    if (cell->pieces.n && pieces[cell->pieces.n - 1].line == line) {
        HeaderPiece *last = pieces + cell->pieces.n - 1;
        const char *p = last->text.p + last->text.n;
        while (p < word.p && *p == ' ') p++;
        if (p == word.p) { last->text.n = (size_t)(word.p + word.n - last->text.p); return 1; }
    }
    piece = ds_push(&d->arena, &cell->pieces, sizeof(HeaderPiece));
    if (!piece) return 0;
    *piece = (HeaderPiece){line, word};
    return 1;
}
/* Reconstruct tall headers above an explicit body marker. Underlines describe
 * parent groups; individual labels collect vertically in their leaf columns.
 * Pieces retain their source text/line instead of guessing missing words or
 * silently rewriting hyphenated labels. No standardizer-specific schema is used. */
static int table_header(TextLayout *layout, size_t start, size_t end, size_t block, Vec cols, size_t *rows) {
    Document *d = layout->document;
    TextLine *lines = layout->lines.p;
    Vec cells = {0};
    size_t i, j, n = cols.n, *cuts = cols.p, count = 0, width = 0, groups, depth = 0;
    TextHeader *all;
    for (i = start; i < end; i++) {
        if (lines[i].width > width) width = lines[i].width;
        if (lines[i].kind == TL_TEXT && !lines[i].rule && ds_strip(lines[i].text, 1).n) count++;
    }
    if (count < 4 || count > 64) return 1;
    if (width <= cuts[n - 1]) width = cuts[n - 1] + 1;
    for (i = start; i < end; i++) {
        TextLine *line = lines + i;
        size_t byte = 0, column = 0, k;
        if (line->kind != TL_TEXT || line->rule) continue;
        while (byte < line->text.n) {
            size_t left = column, first = byte, a = n, b = 0;
            if (line->text.p[byte] != '-') { text_utf8(line->text.p + byte, line->text.n - byte, &k); byte += k; column++; continue; }
            while (byte < line->text.n && line->text.p[byte] == '-') { byte++; column++; }
            if (byte - first < 3) continue;
            for (j = 0; j < n; j++) {
                size_t right = j + 1 < n ? cuts[j + 1] : width;
                size_t lo = left > cuts[j] ? left : cuts[j], hi = column < right ? column : right;
                if (hi > lo && (hi - lo) * 2 >= right - cuts[j]) { if (a == n) a = j; b = j + 1; }
            }
            if (a == n || b - a < 2) continue;
            all = cells.p;
            for (j = 0; j < cells.n; j++) if (all[j].col == a && all[j].span == b - a) break;
            if (j == cells.n) {
                TextHeader *cell = ds_push(&d->arena, &cells, sizeof(TextHeader));
                if (!cell) return 0;
                cell->col = a; cell->span = b - a; cell->source = line->source;
            }
        }
    }
    groups = cells.n;
    for (i = 0; i < groups; i++) {
        all = cells.p;
        for (j = 0; j < groups; j++) if (i != j && all[j].col <= all[i].col &&
            all[j].col + all[j].span >= all[i].col + all[i].span && all[j].span > all[i].span) all[i].depth++;
        if (all[i].depth + 1 > depth) depth = all[i].depth + 1;
    }
    for (j = 0; j < n; j++) {
        TextHeader *cell = ds_push(&d->arena, &cells, sizeof(TextHeader));
        if (!cell) return 0;
        cell->col = j; cell->span = 1; cell->depth = depth;
    }
    all = cells.p;
    for (i = start; i < end; i++) {
        TextLine *line = lines + i;
        size_t byte = 0, column = 0, k;
        if (line->kind != TL_TEXT || line->rule) continue;
        while (byte < line->text.n) {
            size_t first, left, selected = 0, overlap = 0, center;
            Str word;
            if (line->text.p[byte] == ' ') { byte++; column++; continue; }
            first = byte; left = column;
            while (byte < line->text.n && line->text.p[byte] != ' ') {
                text_utf8(line->text.p + byte, line->text.n - byte, &k); byte += k; column++;
            }
            word = (Str){line->text.p + first, byte - first};
            if (rule_line(word)) continue;
            center = left + (column - left) / 2;
            for (j = 0; j < n; j++) {
                size_t right = j + 1 < n ? cuts[j + 1] : width;
                size_t lo = left > cuts[j] ? left : cuts[j], hi = column < right ? column : right;
                if (hi > lo && hi - lo > overlap) { overlap = hi - lo; selected = groups + j; }
            }
            for (j = 0; j < groups; j++) {
                size_t right = all[j].col + all[j].span < n ? cuts[all[j].col + all[j].span] : width;
                if (line->source < all[j].source && center >= cuts[all[j].col] && center < right &&
                    (selected >= groups || all[j].span < all[selected].span)) selected = j;
            }
            if (!header_piece(d, all + selected, line, word)) return 0;
        }
    }
    for (i = 0; i <= depth; i++) {
        int emitted = 0;
        for (j = 0; j < cells.n; j++) if (all[j].depth == i && all[j].pieces.n) {
            HeaderPiece *pieces = all[j].pieces.p;
            size_t q;
            for (q = 0; q < all[j].pieces.n; q++) {
                Str text = pieces[q].text;
                Run *run;
                if (q && pieces[q - 1].line == pieces[q].line) text = ds_cat(&d->arena, ds_str(" "), text);
                if (!text.p || !(run = emit(layout, pieces[q].line, block, text, pieces[q].line->flags | BOLD))) return 0;
                run->in_table = 1; run->row = *rows; run->col = all[j].col; run->colspan = all[j].span;
            }
            emitted = 1;
        }
        if (emitted) (*rows)++;
    }
    return 1;
}
static int table(TextLayout *layout, size_t start, size_t end, size_t *block, Vec *cols) {
    Document *d = layout->document;
    TextLine *lines = layout->lines.p;
    size_t i, first = start, row = 0, data_start = end, *cuts = cols->p;
    int marked = 0;
    for (i = start; i < end; i++) if (lines[i].kind == TL_MARKERS) {
        data_start = i + 1; marked = 1;
    }
    if (!marked) for (i = start; i < end; i++)
        if (lines[i].kind == TL_TEXT && !lines[i].rule && line_numbers(lines + i)) { data_start = i; break; }
    if (marked) {
        if (!table_header(layout, start, data_start, *block, *cols, &row)) return 0;
        if (row) first = data_start;
    }
    for (i = first; i < end; i++) {
        TextLine *line = lines + i;
        size_t col, byte = 0, column = 0;
        if (line->kind != TL_TEXT || line->rule || !ds_strip(line->text, 1).n) continue;
        if (i >= data_start) {
            for (col = 1; col < cols->n; col++) if (cuts_word(line, cuts[col])) break;
            if (col < cols->n) {
                if (fields(line, NULL) == 1) {
                    Run *run;
                    size_t target = 0;
                    /* A standalone label can exceed its column's display
                     * width without implying several populated data cells. */
                    while (target + 1 < cols->n && cuts[target + 1] <= line->indent) target++;
                    run = emit(layout, line, *block, ds_strip(line->text, 1), line->flags | casing(d, ds_strip(line->text, 1)));
                    if (!run) return 0;
                    run->in_table = 1; run->row = row++; run->col = target;
                    continue;
                }
                /* A body row that contradicts the grid stays readable source
                 * text. A fabricated colspan would conceal an invalid fit. */
                (*block)++;
                if (!emit(layout, line, (*block)++, ds_strip(line->text, 1), line->flags)) return 0;
                row = 0;
                continue;
            }
        }
        for (col = 0; col < cols->n;) {
            size_t first = byte, k, span = 1;
            Str text;
            Run *run;
            while (byte < line->text.n && (col + 1 == cols->n || column < cuts[col + 1])) {
                text_utf8(line->text.p + byte, line->text.n - byte, &k); byte += k; column++;
            }
            /* A caption crossing column boundaries is a spanning cell. Never
             * cut through a word just because a lower row has a column there. */
            while (i < data_start && col + span < cols->n && byte && byte < line->text.n &&
                   line->text.p[byte] != ' ' && line->text.p[byte - 1] != ' ') {
                span++;
                while (byte < line->text.n && (col + span == cols->n || column < cuts[col + span])) {
                    text_utf8(line->text.p + byte, line->text.n - byte, &k); byte += k; column++;
                }
            }
            text = ds_strip((Str){line->text.p + first, byte - first}, 1);
            run = emit(layout, line, *block, text, line->flags | casing(d, text));
            if (!run) return 0;
            run->in_table = 1; run->row = row; run->col = col; run->colspan = span;
            col += span;
        }
        row++;
    }
    (*block)++;
    return 1;
}
typedef struct { size_t end, count, tail, first, second; int records; } Delimited;
/* Numeric identifier + a fixed-width alphanumeric key + arbitrary payload.
 * This is a layout convention, without any knowledge of NSAR field codes. */
static int record(Str text, size_t *first, size_t *second) {
    size_t i = 0, start;
    int digit = 0;
    while (i < text.n && text.p[i] >= '0' && text.p[i] <= '9') i++;
    if (i < 2 || i > 8 || i == text.n || text.p[i] != ' ') return 0;
    *first = i + 1;
    while (i < text.n && text.p[i] == ' ') i++;
    start = i;
    while (i < text.n && ((text.p[i] >= '0' && text.p[i] <= '9') ||
           (text.p[i] >= 'A' && text.p[i] <= 'Z') || (text.p[i] >= 'a' && text.p[i] <= 'z'))) {
        digit |= text.p[i] >= '0' && text.p[i] <= '9'; i++;
    }
    if (!digit || i - start < 4 || i - start > 16 || i == text.n || text.p[i] != ' ') return 0;
    *second = i + 1;
    return 1;
}
static Delimited delimited(TextLayout *layout, size_t start, size_t end) {
    TextLine *lines = layout->lines.p;
    Delimited result = {0};
    size_t i, count = lines[start].ntabs, first = 0, second = 0;
    if (lines[start].kind != TL_TEXT || lines[start].rule) return result;
    if (count < 2 || count > 127 || !lines[start].raw.n || lines[start].raw.p[0] == '\t') {
        if (!record(lines[start].text, &first, &second)) return result;
        result.records = 1;
    }
    for (i = start; i < end && lines[i].kind == TL_TEXT && !lines[i].rule; i++) {
        if (result.records) {
            size_t a, b;
            if (!record(lines[i].text, &a, &b) || a != first || b != second) break;
        } else if (lines[i].ntabs != count || !lines[i].raw.n || lines[i].raw.p[0] == '\t') break;
    }
    if (i - start < 3) return (Delimited){0};
    result.end = i; result.count = result.records ? 3 : count + 1;
    result.first = first; result.second = second; result.tail = 1;
    if (!result.records) {
        size_t expected = 0;
        for (i = start; i < result.end; i++) {
            size_t pos = lines[i].raw.n, n;
            TextLine tail = {0};
            while (pos && lines[i].raw.p[pos - 1] != '\t') pos--;
            tail.text = (Str){lines[i].raw.p + pos, lines[i].raw.n - pos};
            n = fields(&tail, NULL);
            if (!expected) expected = n;
            if (n != expected || n > 16) { expected = 1; break; }
        }
        if (expected) result.tail = expected;
    }
    return result;
}
static int emit_delimited(TextLayout *layout, size_t start, Delimited spec, size_t *block) {
    Document *d = layout->document;
    TextLine *lines = layout->lines.p;
    size_t i;
    for (i = start; i < spec.end; i++) {
        TextLine *line = lines + i;
        Str source = spec.records ? line->text : line->raw;
        size_t col, byte = 0;
        for (col = 0; col < spec.count + spec.tail - 1; col++) {
            size_t first = byte, last;
            Run *run;
            Str value;
            if (spec.records) {
                byte = col == 0 ? spec.first : col == 1 ? spec.second : source.n;
                last = byte;
            } else if (col + 1 < spec.count) {
                while (byte < source.n && source.p[byte] != '\t') byte++;
                last = byte;
                if (byte < source.n) byte++;
            } else if (spec.tail > 1) {
                while (byte < source.n && !(source.p[byte] == ' ' && byte + 1 < source.n && source.p[byte + 1] == ' ')) byte++;
                last = byte;
                while (byte < source.n && source.p[byte] == ' ') byte++;
            } else { last = byte = source.n; }
            value = ds_copy(&d->arena, ds_strip((Str){source.p + first, last - first}, 1));
            if (!value.p) return 0;
            run = emit(layout, line, *block, value, line->flags | casing(d, value));
            if (!run) return 0;
            run->in_table = 1; run->row = i - start; run->col = col;
        }
    }
    (*block)++;
    return 1;
}
static int marked_segment(TextLayout *layout, size_t start, size_t end, size_t *block) {
    TextLine *lines = layout->lines.p;
    size_t i, pending = start, numeric_rows = 0;
    int has_markers = 0, seen_markers = 0;
    for (i = start; i < end; i++) if (lines[i].markers.n >= 2) has_markers = 1;
    for (i = start; i <= end; i++) {
        int numeric = 0, boundary = i == end;
        Vec cols = {0};
        if (i < end && lines[i].kind == TL_MARKERS) { seen_markers = 1; numeric_rows = 0; }
        if (i < end && lines[i].kind == TL_TEXT && !lines[i].rule) {
            Str text = ds_strip(lines[i].text, 1);
            size_t count = fields(lines + i, &numeric);
            if (numeric && count >= 2) numeric_rows++;
            else if (numeric_rows >= 3 && (!has_markers || seen_markers) && text.n && text.n <= 100 && count == 1 &&
                     casing(layout->document, text) && i > pending && i + 1 < end &&
                     (lines[i - 1].rule || !ds_strip(lines[i - 1].text, 1).n) &&
                     (lines[i + 1].rule || !ds_strip(lines[i + 1].text, 1).n)) boundary = 1;
        }
        if (!boundary) continue;
        if (!columns(layout, pending, i, &cols, 1)) return 0;
        if (cols.n) {
            if (!table(layout, pending, i, block, &cols)) return 0;
        } else if (!plain(layout, pending, i, block)) return 0;
        pending = i; numeric_rows = 0; seen_markers = 0;
    }
    return 1;
}
static int marked_table(TextLayout *layout, size_t start, size_t end, size_t *block) {
    size_t i = start, pending = start;
    while (i < end) {
        Delimited spec = delimited(layout, i, end);
        if (!spec.end) { i++; continue; }
        if (!marked_segment(layout, pending, i, block) || !emit_delimited(layout, i, spec, block)) return 0;
        i = pending = spec.end;
    }
    return marked_segment(layout, pending, end, block);
}
static int layout_lines(TextLayout *layout) {
    TextLine *lines = layout->lines.p;
    size_t i = 0, pending = 0, block = 0;
    while (i < layout->lines.n) {
        size_t end;
        Vec cols = {0};
        Delimited spec = delimited(layout, i, layout->lines.n);
        int orphan = lines[i].kind == TL_MARKERS && lines[i].markers.n >= 2;
        int marked = lines[i].kind == TL_TABLE || orphan;
        if (spec.end) {
            if (!plain(layout, pending, i, &block) || !emit_delimited(layout, i, spec, &block)) return 0;
            i = pending = spec.end;
            continue;
        }
        if (marked) {
            size_t depth = 1;
            end = i + 1;
            while (end < layout->lines.n) {
                if (orphan && lines[end].kind != TL_TEXT) break;
                if (lines[end].kind == TL_TABLE) depth++;
                else if (lines[end].kind == TL_END_TABLE && !--depth) break;
                end++;
            }
        } else if (lines[i].kind == TL_TEXT && !lines[i].rule && fields(lines + i, NULL) >= 2) {
            end = i + 1;
            while (end < layout->lines.n && lines[end].kind == TL_TEXT) {
                Str value = ds_strip(lines[end].text, 1);
                size_t look = end, digit;
                if (delimited(layout, end, layout->lines.n).end) break;
                if (lines[end].rule || fields(lines + end, NULL) >= 2) { end++; continue; }
                /* Blank rows and single-column labels may separate table
                 * sections, but must lead back to aligned rows promptly. */
                while (look < layout->lines.n && look < end + 3 && lines[look].kind == TL_TEXT &&
                       !ds_strip(lines[look].text, 1).n) look++;
                if (!value.n && look < layout->lines.n && look < end + 3 && lines[look].kind == TL_TEXT) {
                    if (delimited(layout, look, layout->lines.n).end) break;
                    value = ds_strip(lines[look].text, 1);
                    for (digit = 0; digit < value.n && value.p[digit] >= '0' && value.p[digit] <= '9'; digit++);
                    if (fields(lines + look, NULL) >= 2 || (value.n >= 4 && digit == value.n)) { end = look + 1; continue; }
                } else if (value.n) {
                    look = end + 1;
                    while (look < layout->lines.n && look < end + 4 && lines[look].kind == TL_TEXT &&
                           !ds_strip(lines[look].text, 1).n) look++;
                    if (look < layout->lines.n && look < end + 4 && lines[look].kind == TL_TEXT &&
                        fields(lines + look, NULL) >= 2 && lines[end].indent >= lines[i].indent) { end++; continue; }
                }
                break;
            }
            if (!columns(layout, i, end, &cols, 0)) return 0;
        } else { i++; continue; }
        if (marked || cols.n) {
            if (!plain(layout, pending, i, &block)) return 0;
            if (marked) {
                if (!marked_table(layout, i + !orphan, end, &block)) return 0;
            } else if (cols.n) {
                if (!table(layout, i + marked, end, &block, &cols)) return 0;
            } else if (!plain(layout, i + marked, end, &block)) return 0;
            i = end + (marked && !orphan && end < layout->lines.n);
            pending = i;
        } else i = end;
    }
    return plain(layout, pending, layout->lines.n, &block);
}
static Document *text_document(ParserContext *parser, Str input, const Rules *rules, size_t tab_size,
                               int partial, doc2struct_error *error) {
    Document *d;
    TextLayout layout = {0};
    if (error) error->message[0] = 0;
    if ((!input.p && input.n) || !tab_size || tab_size > 256 || (parser && parser->closed)) {
        if (error) snprintf(error->message, sizeof(error->message), "invalid input, parser or tab_size (expected 1..256)");
        return NULL;
    }
    if (!input.p) input = ds_str("");
    d = doc2struct_document_new(error);
    if (!d) return NULL;
    if (parser) d->arena.pool = &parser->pool;
    layout.document = d; layout.partial = partial; layout.tab_size = tab_size;
    layout.stream = rules != NULL;
    if (rules) {
        d->retain_table_cells = rules->unwrap_layout;
        d->collect_hrefs = rules->collect_hrefs;
    d->preserve_styles = rules->preserve_styles;
    d->join_wrapped_lines = rules->join_wrapped_lines;
    }
    input = ds_input(&d->arena, input);
    /* One owned backing buffer lets ordinary ASCII lines be borrowed views.
     * Instruction-only callers can still release their input immediately. */
    if (input.p) input = ds_copy(&d->arena, input);
    if (!input.p || !scan(&layout, input) || (rules && !ds_flow_begin(d)) || !layout_lines(&layout)) goto failed;
    d->no_fonts = 1;
    if (!rules || ((!layout.pending_active || ds_flow_run(d, &layout.pending)) && ds_flow_finish(d) && ds_standardize(d, rules))) return d;
failed:
    if (error) snprintf(error->message, sizeof(error->message), "%s", d->arena.error[0] ? d->arena.error : "text layout failed");
    doc2struct_document_free(d);
    return NULL;
}
Document *doc2struct_text_instructions(ParserContext *parser, Str input, const Rules *rules, doc2struct_error *error) {
    if (!rules) { if (error) snprintf(error->message, sizeof(error->message), "rules are required"); return NULL; }
    return text_document(parser, input, NULL, rules->text_tab_size, 0, error);
}
Document *doc2struct_partialsgml_instructions(ParserContext *parser, Str input, const Rules *rules, doc2struct_error *error) {
    if (!rules) { if (error) snprintf(error->message, sizeof(error->message), "rules are required"); return NULL; }
    return text_document(parser, input, NULL, rules->partialsgml_tab_size, 1, error);
}
Document *doc2struct_parse_text(ParserContext *parser, Str input, const Rules *rules, doc2struct_error *error) {
    if (!rules) { if (error) snprintf(error->message, sizeof(error->message), "rules are required"); return NULL; }
    return text_document(parser, input, rules, rules->text_tab_size, 0, error);
}
Document *doc2struct_parse_partialsgml(ParserContext *parser, Str input, const Rules *rules, doc2struct_error *error) {
    if (!rules) { if (error) snprintf(error->message, sizeof(error->message), "rules are required"); return NULL; }
    return text_document(parser, input, rules, rules->partialsgml_tab_size, 1, error);
}
