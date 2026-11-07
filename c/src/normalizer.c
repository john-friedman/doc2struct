#include "doc2struct.h"
#include "tags.h"
#include "scan.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

typedef struct {
    Str tag, href, block_tag;
    unsigned flags, props;
    double font, indent;
    double list_indent;
    size_t list_depth;
    int list_item;
    int has_font, has_indent, suppressed, hides, act, root;
    unsigned open_props;
} Frame;
typedef struct {
    Document *d;
    Options options;
    Vec stack;
    int in_body, hidden, line_open, table, cell, emitted;
    size_t table_depth, next_block, cur_block, line, table_block, row, col, cell_row, cell_col, rs,
        cs;
    size_t *occupied, occupied_cap, occupied_used;
    Str missing_ends[16];
    size_t missing_count;
} Normalizer;
static double rounded(double);
static int member(Str t, const char *names) {
    const char *p = names;
    while (*p) {
        const char *e = strchr(p, ' ');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (ds_eq(t, (Str){p, n}))
            return 1;
        if (!e)
            break;
        p = e + 1;
    }
    return 0;
}

static double px(Str s) {
    char buf[96];
    size_t i = 0, n;
    double value;
    Str unit;
    s = ds_strip(s, 1);
    if (!s.n)
        return 0;
    while (i < s.n &&
           ((s.p[i] >= '0' && s.p[i] <= '9') || s.p[i] == '.' || (i == 0 && s.p[i] == '-')))
        i++;
    n = i < sizeof(buf) - 1 ? i : sizeof(buf) - 1;
    memcpy(buf, s.p, n);
    buf[n] = 0;
    {
        char *end;
        value = strtod(buf, &end);
        if ((size_t)(end - buf) != n)
            value = 0;
    }
    unit = i < s.n ? ds_strip((Str){s.p + i, s.n - i}, 1) : ds_str("px");
    if (member(unit, "auto inherit initial"))
        return 0;
    if (ds_is(unit, "pt"))
        return value * 1.333;
    if (member(unit, "em rem pc"))
        return value * 16;
    if (ds_is(unit, "%"))
        return value * .16;
    if (member(unit, "ex ch"))
        return value * 8;
    if (ds_is(unit, "cm"))
        return value * 37.8;
    if (ds_is(unit, "mm"))
        return value * 3.78;
    if (ds_is(unit, "in"))
        return value * 96;
    return value;
}
static size_t words(Str s, Str *parts, size_t cap) {
    size_t i = 0, k, n = 0, start;
    while (i < s.n) {
        while (i < s.n && ds_space(ds_utf8(s.p + i, s.n - i, &k)))
            i += k;
        start = i;
        while (i < s.n && !ds_space(ds_utf8(s.p + i, s.n - i, &k)))
            i += k;
        if (i > start) {
            if (n < cap)
                parts[n] = (Str){s.p + start, i - start};
            n++;
        }
    }
    return n;
}
static double left_box(Str s) {
    Str p[5];
    size_t n = words(s, p, 5);
    return n == 4 ? px(p[3]) : n == 2 || n == 3 ? px(p[1]) : px(s);
}
Style *ds_style(Document *d, Str raw) {
    static const char *keys[] = {"font-weight", "font-style",  "text-decoration", "text-align",
                                 "font-size",   "text-indent", "padding",         "padding-left",
                                 "margin",      "margin-left", "display",         "visibility",
                                 "font"};
    Str values[13] = {{0}}, lower;
    size_t i = 0, j, k;
    Style *st = ds_map_get(&d->styles, raw);
    if (st)
        return st;
    st = ds_alloc(&d->arena, sizeof(*st));
    if (!st)
        return NULL;
    lower = ds_lower(&d->arena, raw);
    if (!lower.p)
        return NULL;
    while (i < lower.n) {
        size_t start = i, colon = SIZE_MAX;
        Str name, value;
        while (i < lower.n && lower.p[i] != ';') {
            if (colon == SIZE_MAX && lower.p[i] == ':')
                colon = i;
            i++;
        }
        if (colon != SIZE_MAX) {
            name = ds_strip((Str){lower.p + start, colon - start}, 1);
            value = ds_strip((Str){lower.p + colon + 1, i - colon - 1}, 1);
            for (j = 0; j < 13; j++)
                if (ds_is(name, keys[j])) {
                    values[j] = value;
                    break;
                }
        }
        if (i < lower.n)
            i++;
    }
    if (values[12].p) {
        Str parts[32];
        size_t n = words(values[12], parts, 32), q = 0;
        if (n >= 2) {
            if (member(parts[q], "italic oblique normal")) {
                if (ds_is(parts[q], "italic"))
                    values[1] = ds_str("italic");
                q++;
            }
            if (q < n && member(parts[q], "small-caps normal"))
                q++;
            if (q < n && q < 32 &&
                member(parts[q],
                       "bold 700 normal 400 100 200 300 500 600 800 900 lighter bolder")) {
                values[0] = parts[q];
                q++;
            }
            if (q < n && q < 32) {
                values[4] = parts[q];
                for (k = 0; k < values[4].n; k++)
                    if (values[4].p[k] == '/') {
                        values[4].n = k;
                        break;
                    }
            }
        }
    }
    if (member(values[0], "bold 700"))
        st->flags |= BOLD;
    if (ds_is(values[1], "italic"))
        st->flags |= ITALIC;
    if (ds_is(values[2], "underline"))
        st->flags |= UNDERLINE;
    if (ds_is(values[3], "center"))
        st->flags |= CENTERED;
    else if (member(values[3], "left right justify start end initial"))
        st->clear_flags |= CENTERED;
    st->has_font = values[4].p != NULL;
    st->font = px(values[4]);
    st->indent =
        px(values[5]) + left_box(values[6]) + px(values[7]) + left_box(values[8]) + px(values[9]);
    st->has_indent = st->indent != 0;
    st->font = rounded(st->font);
    st->indent = rounded(st->indent);
    st->display_none = ds_is(values[10], "none");
    st->vis_hidden = member(values[11], "hidden collapse");
    if (!ds_map_put(&d->arena, &d->styles, raw, st))
        return NULL;
    return st;
}
static double rounded(double v) {
    double x = floor(v), delta = v - x;
    if (delta > .5 || (delta == .5 && fmod(x, 2) != 0))
        x++;
    return x == 0 ? 0 : x;
}
static void break_line(Normalizer *n) {
    if (n->line_open) {
        n->line_open = 0;
        n->line++;
    }
    n->d->skip_blank = 1;
}
static int emit(Normalizer *n, Str text, Str src, int blank) {
    Run value = {0}, *r = &value;
    Frame *frames = n->stack.p, *f = n->stack.n ? frames + n->stack.n - 1 : NULL;
    if (!n->line_open) {
        n->line_open = 1;
        n->d->skip_blank = 0;
        if (!n->cell)
            n->cur_block = n->next_block++;
    }
    r->block = n->cell ? n->table_block : n->cur_block;
    r->line = n->line;
    r->text = text;
    r->src = src;
    r->rowspan = r->colspan = 1;
    if (n->cell) {
        r->in_table = 1;
        r->row = n->cell_row;
        r->col = n->cell_col;
        r->rowspan = n->rs;
        r->colspan = n->cs;
        n->emitted = 1;
    }
    if (!blank && f) {
        r->flags = f->flags;
        r->font = f->font;
        r->indent = f->indent;
        r->has_font = f->has_font;
        r->has_indent = f->has_indent;
        r->href = f->href;
        r->tag = f->block_tag;
        if (f->list_item && !n->cell) {
            r->tag = ds_str("li");
            r->indent = f->list_indent;
            r->has_indent = 1;
        }
    }
    if (n->d->flow)
        return ds_flow_run(n->d, r);
    r = ds_push(&n->d->arena, &n->d->runs, sizeof(Run));
    if (!r)
        return 0;
    *r = value;
    return 1;
}
static int pop(Normalizer *n) {
    Frame *f = (Frame *)n->stack.p + --n->stack.n;
    if (f->hides) {
        n->hidden--;
        n->d->skip_text = !n->in_body || n->hidden;
    }
    if (f->suppressed)
        return 1;
    if (f->props & T_LINE)
        break_line(n);
    else if (f->props & T_CELL) {
        if (f->act && n->table && n->cell) {
            if (!n->emitted && !emit(n, ds_str(""), (Str){0}, 1))
                return 0;
            n->cell = 0;
            break_line(n);
        }
    } else if (f->props & T_TR) {
        if (f->act && n->table) {
            n->row++;
            n->col = 0;
        }
    } else if (f->props & T_TABLE) {
        n->table_depth--;
        break_line(n);
        if (f->root) {
            n->table = 0;
            n->cell = 0;
        }
    }
    return 1;
}
static int pop_open(Normalizer *n, unsigned names, unsigned stop) {
    Frame *frames = n->stack.p;
    size_t i;
    /* Each frame includes its ancestors' properties. Reject absent targets
     * without walking the stack; possible matches still respect stop tags. */
    if (!n->stack.n || !(frames[n->stack.n - 1].open_props & names))
        return 1;
    for (i = n->stack.n; i; i--) {
        unsigned props = frames[i - 1].props;
        if (props & names) {
            while (n->stack.n >= i)
                if (!pop(n))
                    return 0;
            return 1;
        }
        if (props & stop)
            return 1;
    }
    return 1;
}
static int implied(Normalizer *n, unsigned props) {
    if ((props & T_P_CLOSE) && !pop_open(n, T_P, T_TABLE | T_CELL | T_BODY_HTML))
        return 0;
    if (props & T_LI)
        return pop_open(n, T_LI, T_LIST | T_TABLE);
    if (props & T_DTDD)
        return pop_open(n, T_DTDD, T_DL | T_TABLE);
    if (props & T_CELL)
        return pop_open(n, T_CELL, T_TR | T_TABLE);
    if (props & T_TR)
        return pop_open(n, T_CELL, T_TR | T_TABLE) && pop_open(n, T_TR, T_TABLE);
    if (props & T_SECTION)
        return pop_open(n, T_CELL, T_TR | T_TABLE) && pop_open(n, T_TR, T_TABLE) &&
               pop_open(n, T_SECTION, T_TABLE);
    return 1;
}
static size_t span(Str s, size_t cap) {
    char buf[64], *end;
    long long v;
    if (!s.p || !s.n)
        return 1;
    s = ds_strip(s, 0);
    if (s.n >= sizeof(buf))
        return s.p[0] == '-' ? 1 : cap;
    memcpy(buf, s.p, s.n);
    buf[s.n] = 0;
    v = strtoll(buf, &end, 10);
    if (end == buf || *end)
        return 1;
    if (v < 1)
        return 1;
    return (uint64_t)v > cap ? cap : (size_t)v;
}
void *ds_normalizer(Document *d, Options o) {
    Normalizer *n = ds_alloc(&d->arena, sizeof(*n));
    if (n) {
        n->d = d;
        n->options = o;
        d->skip_text = d->skip_blank = 1;
    }
    return n;
}
int ds_normalize_token(void *ctx, int kind, Str tag, const Attrs *a) {
    Normalizer *n = ctx;
    Document *d = n->d;
    Style *st = NULL;
    Frame *f;
    int hides;
    size_t i;
    unsigned props;
    if (d->arena.error[0])
        return 0;
    if (kind == TEXT) {
        if (!n->in_body || n->hidden || !tag.n ||
            (!n->line_open && ds_blank_text(tag.p, tag.n)))
            return 1;
        return emit(n, tag, (Str){0}, 0);
    }
    if (kind == END && n->stack.n) {
        Frame *top = (Frame *)n->stack.p + n->stack.n - 1;
        if (top->tag.n == tag.n && ds_bytes_equal(top->tag.p, tag.p, tag.n)) {
            if (top->props & T_BODY_HTML)
                return 1;
            return pop(n);
        }
    }
    props = ds_tag_flags(tag);
    if (kind == END) {
        unsigned stop;
        if (!n->in_body || (props & (T_VOID | T_BODY_HTML)))
            return 1;
        stop = (props & T_CELL)               ? T_TR | T_TABLE
               : (props & (T_TR | T_SECTION)) ? T_TABLE
               : (props & T_TABLE)            ? 0
                                              : T_TABLE | T_CELL;
        for (i = 0; i < n->missing_count; i++)
            if (ds_eq(n->missing_ends[i], tag))
                return 1;
        for (i = n->stack.n; i; i--) {
            Str t = ((Frame *)n->stack.p)[i - 1].tag;
            if (ds_eq(t, tag)) {
                while (n->stack.n >= i)
                    if (!pop(n))
                        return 0;
                return 1;
            }
            if (((Frame *)n->stack.p)[i - 1].props & stop)
                return 1;
        }
        /* Only a full-stack miss proves absence. A table barrier may hide a
         * matching ancestor. Pops preserve absence; every push clears it.
         * Keep small stacks on the direct path and bound cache lookup cost. */
        if (n->stack.n >= 128 &&
            n->missing_count < sizeof(n->missing_ends) / sizeof(*n->missing_ends))
            n->missing_ends[n->missing_count++] = tag;
        return 1;
    }
    if (!n->in_body) {
        if (props & T_HEAD)
            return 1;
        n->in_body = 1;
        d->skip_text = n->hidden != 0;
    }
    if (a->value[A_STYLE].n) {
        st = a->prepared_style ? a->prepared_style : ds_style(d, a->value[A_STYLE]);
        if (!st)
            return 0;
    }
    hides = (st && (st->display_none || (n->options.hide_visibility && st->vis_hidden))) ||
            (n->options.hide_hidden_attr && a->value[A_HIDDEN].p);
    if (props & T_VOID) {
        if (n->hidden || hides)
            return 1;
        if (props & T_BR)
            break_line(n);
        else if (props & T_IMG)
            return emit(n, a->value[A_ALT].p ? a->value[A_ALT] : ds_str(""),
                        a->value[A_SRC].p ? a->value[A_SRC] : ds_str(""), 0);
        return 1;
    }
    if (!implied(n, props))
        return 0;
    {
        f = ds_push(&d->arena, &n->stack, sizeof(Frame));
        if (!f)
            return 0;
        /* Read the parent after growth, which can relocate the stack. */
        if (n->stack.n > 1)
            *f = ((Frame *)n->stack.p)[n->stack.n - 2];
        else
            *f = (Frame){0};
        f->suppressed = f->hides = f->act = f->root = 0;
        f->tag = tag;
        f->props = props;
        f->open_props |= props;
        n->missing_count = 0;
        if (props & T_BLOCK)
            f->block_tag = tag;
    }
    if (n->hidden) {
        f->suppressed = 1;
        return 1;
    }
    if (hides) {
        f->suppressed = f->hides = 1;
        n->hidden++;
        d->skip_text = 1;
        return 1;
    }
    if (st) {
        f->flags = (f->flags & ~st->clear_flags) | st->flags;
        if (st->has_font) {
            f->font = st->font;
            f->has_font = 1;
        }
        if (st->has_indent) {
            f->indent = st->indent;
            f->has_indent = 1;
        }
    }
    if (props & T_BOLD)
        f->flags |= BOLD;
    else if (props & T_ITALIC)
        f->flags |= ITALIC;
    else if (props & T_UNDER)
        f->flags |= UNDERLINE;
    else if (props & T_A)
        f->href = a->value[A_HREF].p ? a->value[A_HREF] : ds_str("");
    if (props & T_LIST) {
        break_line(n);
        f->list_depth++;
        f->list_item = 0;
    } else if (props & T_LI) {
        break_line(n);
        f->list_item = 1;
        f->list_indent = f->indent + (f->list_depth > 1 ? (f->list_depth - 1) * 40 : 0);
    }
    if (props & T_TABLE) {
        n->table_depth++;
        break_line(n);
        if (n->table_depth == 1) {
            f->root = 1;
            n->table = 1;
            n->table_block = n->next_block++;
            n->row = n->col = 0;
            n->cell = 0;
            /* Only cell placement writes this prefix; the unused tail stays zero. */
            if (n->occupied_used)
                memset(n->occupied, 0, n->occupied_used * sizeof(*n->occupied));
            n->occupied_used = 0;
        }
    } else if (props & (T_TR | T_CELL)) {
        f->act = n->table && n->table_depth == 1;
        if (f->act && !(props & T_TR)) {
            size_t needed;
            break_line(n);
            while (n->col < n->occupied_cap && n->occupied[n->col] > n->row)
                n->col++;
            n->rs = span(a->value[A_ROWSPAN], n->options.max_span);
            n->cs = span(a->value[A_COLSPAN], n->options.max_span);
            if (n->col > SIZE_MAX - n->cs || n->row > SIZE_MAX - n->rs) {
                snprintf(d->arena.error, sizeof(d->arena.error), "table dimensions overflow");
                return 0;
            }
            needed = n->col + n->cs;
            if (needed > n->occupied_cap) {
                size_t cap = needed < 32 ? 32 : needed;
                size_t *p;
                if (n->occupied_cap <= SIZE_MAX / sizeof(*p) / 2 && n->occupied_cap * 2 > cap)
                    cap = n->occupied_cap * 2;
                if (cap > SIZE_MAX / sizeof(size_t)) {
                    snprintf(d->arena.error, sizeof(d->arena.error), "table too large");
                    return 0;
                }
                p = ds_alloc(&d->arena, cap * sizeof(size_t));
                if (!p)
                    return 0;
                if (n->occupied_used)
                    memcpy(p, n->occupied, n->occupied_used * sizeof(*p));
                n->occupied = p;
                n->occupied_cap = cap;
            }
            for (i = n->col; i < needed; i++)
                if (n->occupied[i] < n->row + n->rs)
                    n->occupied[i] = n->row + n->rs;
            if (needed > n->occupied_used)
                n->occupied_used = needed;
            n->cell = 1;
            n->cell_row = n->row;
            n->cell_col = n->col;
            n->emitted = 0;
            n->col = needed;
        }
    }
    return !d->arena.error[0];
}
int ds_normalize_finish(void *ctx) {
    Normalizer *n = ctx;
    while (n->stack.n)
        if (!pop(n))
            return 0;
    return n->d->flow ? ds_flow_finish(n->d) : !n->d->arena.error[0];
}
