#include <doc2struct/doc2struct.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef doc2struct_string Str;
typedef doc2struct_row Row;
typedef struct {
    char *data;
    size_t size, capacity;
    const char *error;
} Writer;

static int is(Str s, const char *text) {
    size_t n = strlen(text);
    return s.n == n && (!n || (s.p && !memcmp(s.p, text, n)));
}
static int equal(Str a, Str b) {
    return a.n == b.n && (!a.n || (a.p && b.p && !memcmp(a.p, b.p, a.n)));
}
static int reserve(Writer *w, size_t n) {
    size_t needed, capacity;
    char *data;
    if (w->error) return 0;
    if (n > SIZE_MAX - w->size - 1) {
        w->error = "Markdown output too large";
        return 0;
    }
    needed = w->size + n + 1;
    if (needed <= w->capacity) return 1;
    capacity = w->capacity ? w->capacity : 4096;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
        capacity *= 2;
    }
    data = realloc(w->data, capacity);
    if (!data) { w->error = "out of memory"; return 0; }
    w->data = data;
    w->capacity = capacity;
    return 1;
}
static int bytes(Writer *w, const char *p, size_t n) {
    if (!p && n) { w->error = "invalid Markdown input string"; return 0; }
    if (!reserve(w, n)) return 0;
    if (n) memcpy(w->data + w->size, p, n);
    w->size += n;
    return 1;
}
static int literal(Writer *w, const char *s) { return bytes(w, s, strlen(s)); }
static int repeat(Writer *w, char c, size_t n) {
    if (!reserve(w, n)) return 0;
    memset(w->data + w->size, c, n);
    w->size += n;
    return 1;
}
static int block(Writer *w) {
    size_t end = w->size;
    while (end && w->data[end - 1] == '\n') end--;
    w->size = end;
    return !end || literal(w, "\n\n");
}
static int space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
static size_t spacing(Str s, size_t i) {
    const unsigned char *p = (const unsigned char *)s.p + i;
    size_t n = s.n - i;
    if (space(p[0])) return 1;
    if (n >= 2 && p[0] == 0xc2 && p[1] == 0xa0) return 2;
    if (n >= 3 && p[0] == 0xe2 && p[1] == 0x80 &&
        ((p[2] >= 0x80 && p[2] <= 0x8a) || p[2] == 0xaf)) return 3;
    if (n >= 3 && p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x80) return 3;
    return 0;
}
/* Source text is literal, not Markdown. Collapse layout whitespace and escape
 * markup, HTML/entities and accidental list markers without changing numbers. */
static int text(Writer *w, Str s) {
    size_t i = 0, start;
    int pending = 0, digits = 1;
    if (!s.p && s.n) { w->error = "invalid Markdown input string"; return 0; }
    while (i < s.n && spacing(s, i)) i += spacing(s, i);
    start = i;
    for (; i < s.n; i++) {
        unsigned char c = (unsigned char)s.p[i];
        int escape;
        size_t whitespace = spacing(s, i);
        if (whitespace) { pending = 1; i += whitespace - 1; continue; }
        if (pending) {
            if (!literal(w, " ")) return 0;
            pending = 0;
            digits = 0;
        }
        escape = strchr("\\`*_{}[]<>|&#~", c) != NULL ||
                 (i == start && (c == '-' || c == '+' || c == '=')) ||
                 (digits && i > start && (c == '.' || c == ')') &&
                  (i + 1 == s.n || spacing(s, i + 1)));
        if (escape && !literal(w, "\\")) return 0;
        if (!bytes(w, s.p + i, 1)) return 0;
        if (c < '0' || c > '9') digits = 0;
    }
    return 1;
}
static int paragraph(Writer *w, Str s) {
    return !s.n || (block(w) && text(w, s));
}
static int matrix_valid(Writer *w, const doc2struct_matrix *m) {
    if ((m->nr && m->nc && !m->cells) || (m->nc && m->nr > SIZE_MAX / m->nc)) {
        w->error = "invalid Markdown input matrix";
        return 0;
    }
    return 1;
}
static int table(Writer *w, const Row *r, Str heading) {
    size_t i, j;
    const doc2struct_matrix *m = &r->matrix;
    if (!matrix_valid(w, m)) return 0;
    if (r->text.n && !equal(r->text, heading)) {
        if (!block(w) || !literal(w, "**") || !text(w, r->text) || !literal(w, "**")) return 0;
    }
    if (!paragraph(w, r->preamble)) return 0;
    if (m->nr && m->nc) {
        if (!block(w) || !literal(w, "|")) return 0;
        for (j = 0; j < m->nc; j++) if (!literal(w, "  |")) return 0;
        if (!literal(w, "\n|")) return 0;
        for (j = 0; j < m->nc; j++) if (!literal(w, " --- |")) return 0;
        if (!literal(w, "\n")) return 0;
        for (i = 0; i < m->nr; i++) {
            if (!literal(w, "|")) return 0;
            for (j = 0; j < m->nc; j++)
                if (!literal(w, " ") || !text(w, m->cells[i * m->nc + j]) || !literal(w, " |")) return 0;
            if (!literal(w, "\n")) return 0;
        }
    }
    if (r->nnotes && !r->notes) { w->error = "invalid Markdown input footnotes"; return 0; }
    for (i = 0; i < r->nnotes; i++) {
        if (!block(w) || !text(w, r->notes[i].id)) return 0;
        if (r->notes[i].id.n && r->notes[i].text.n && !literal(w, " ")) return 0;
        if (!text(w, r->notes[i].text)) return 0;
    }
    return paragraph(w, r->postamble);
}
static int depth(Writer *w, Str s, size_t *value) {
    size_t i, n = 0;
    if (!s.p || !s.n) goto invalid;
    for (i = 0; i < s.n; i++) {
        unsigned char c = (unsigned char)s.p[i];
        if (c < '0' || c > '9' || n > (SIZE_MAX / 4 - (c - '0')) / 10) goto invalid;
        n = n * 10 + (c - '0');
    }
    *value = n;
    return 1;
invalid:
    w->error = "list depth must be a nonnegative integer string";
    return 0;
}
static size_t bullet(Str s, size_t start) {
    static const char *const markers[] = {
        "-", "*", "+", "\xc2\xb7", "\xe2\x80\xa2", "\xe2\x97\xa6",
        "\xe2\x97\x8f", "\xe2\x96\xaa", "\xe2\x96\xa0", "\xe2\x97\x8b",
        "\xe2\x80\xa3", "\xe2\x81\x83", "\xe2\x80\x93"
    };
    size_t j;
    for (j = 0; j < sizeof(markers) / sizeof(*markers); j++) {
        size_t n = strlen(markers[j]);
        if (n <= s.n - start && !memcmp(s.p + start, markers[j], n) &&
            (start + n == s.n || spacing(s, start + n))) return n;
    }
    return 0;
}
/* Keep Markdown-compatible numbers. Other legal labels, such as (a), stay
 * literal inside bullets; remove existing bullet markers to avoid doubling. */
static int list_item(Writer *w, Str s, size_t *width) {
    size_t i = 0, start, end;
    while (i < s.n && spacing(s, i)) i += spacing(s, i);
    start = i;
    while (i < s.n && s.p[i] >= '0' && s.p[i] <= '9') i++;
    if (i > start && i - start <= 9 && i + 1 < s.n &&
        s.p[i] == '.' && spacing(s, i + 1)) {
        end = i + 1;
        *width = end - start + 1;
        if (!bytes(w, s.p + start, end - start) || !literal(w, " ")) return 0;
        i = end;
    } else {
        i = start;
        *width = 2;
        if (!literal(w, "- ")) return 0;
        if (i < s.n) i += bullet(s, i);
    }
    return text(w, (Str){s.p ? s.p + i : NULL, s.n - i});
}
static int list(Writer *w, const Row *r) {
    typedef struct { size_t depth, indent, width; } Parent;
    size_t i, relative, indent, width, nparents = 0;
    Parent *parents = NULL;
    const doc2struct_matrix *m = &r->matrix;
    if (!matrix_valid(w, m)) return 0;
    if (m->nr && m->nc != 2) { w->error = "list matrix rows must contain depth and text"; return 0; }
    if (!paragraph(w, r->text) || !block(w)) return 0;
    if (m->nr > SIZE_MAX / sizeof(*parents)) { w->error = "Markdown list too large"; return 0; }
    if (m->nr && !(parents = malloc(m->nr * sizeof(*parents)))) {
        w->error = "out of memory";
        return 0;
    }
    for (i = 0; i < m->nr; i++) {
        Str item = m->cells[i * 2 + 1];
        if ((!item.p && item.n) || !depth(w, m->cells[i * 2], &relative)) {
            if (!w->error) w->error = "invalid Markdown input list item";
            goto failed;
        }
        while (nparents && parents[nparents - 1].depth >= relative) nparents--;
        indent = relative * 4;
        if (nparents) {
            Parent *p = parents + nparents - 1;
            size_t gap = (relative - p->depth - 1) * 4;
            if (p->indent > SIZE_MAX - p->width || gap > SIZE_MAX - p->indent - p->width) {
                w->error = "Markdown list indentation too large";
                goto failed;
            }
            indent = p->indent + p->width + gap;
        }
        if (!repeat(w, ' ', indent) || !list_item(w, item, &width) || !literal(w, "\n")) goto failed;
        parents[nparents++] = (Parent){relative, indent, width};
    }
    free(parents);
    return 1;
failed:
    free(parents);
    return 0;
}

char *doc2struct_convert_standardized_text_to_markdown(const Row *rows, size_t count,
        int redundant, size_t *output_size, doc2struct_error *error) {
    Writer w = {0};
    Str heading = {0};
    size_t i;
    if (output_size) *output_size = 0;
    if (error) error->message[0] = 0;
    if ((!rows && count) || redundant < DOC2STRUCT_REDUNDANT_BLOCKQUOTE ||
        redundant > DOC2STRUCT_REDUNDANT_OMIT) {
        w.error = "invalid Markdown rows or redundant mode";
        goto failed;
    }
    for (i = 0; i < count; i++) {
        const Row *r = rows + i;
        if (r->removed) continue;
        if (is(r->type, "title")) {
            if (i == 0 && r->id == 0 && r->level == 0 && is(r->text, "introduction")) continue;
            heading = r->text;
            if (!r->text.n) continue;
            if (!block(&w)) goto failed;
            if (r->level < 6) {
                if (!repeat(&w, '#', r->level + 1) || !literal(&w, " ") || !text(&w, r->text)) goto failed;
            } else if (!literal(&w, "**") || !text(&w, r->text) || !literal(&w, "**")) goto failed;
        } else if (is(r->type, "table")) {
            if (!table(&w, r, heading)) goto failed;
        } else if (is(r->type, "list")) {
            if (!list(&w, r)) goto failed;
        } else if (is(r->type, "redundant")) {
            if (redundant == DOC2STRUCT_REDUNDANT_OMIT || !r->text.n) continue;
            if (!block(&w)) goto failed;
            if (redundant == DOC2STRUCT_REDUNDANT_BLOCKQUOTE && !literal(&w, "> ")) goto failed;
            if (!text(&w, r->text)) goto failed;
        } else if (is(r->type, "image")) {
            if (!block(&w) || !literal(&w, "\\[Image")) goto failed;
            if (r->text.n && (!literal(&w, ": ") || !text(&w, r->text))) goto failed;
            if (!literal(&w, "\\]")) goto failed;
        } else if (!paragraph(&w, r->text)) goto failed;
    }
    while (w.size && w.data[w.size - 1] == '\n') w.size--;
    if (w.size && !literal(&w, "\n")) goto failed;
    if (!reserve(&w, 0)) goto failed;
    w.data[w.size] = 0;
    if (output_size) *output_size = w.size;
    return w.data;
failed:
    if (error) snprintf(error->message, sizeof(error->message), "%s", w.error ? w.error : "Markdown conversion failed");
    free(w.data);
    return NULL;
}
