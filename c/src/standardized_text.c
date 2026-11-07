#include "doc2struct.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int image(const Item *i) { return i->run.src.p != NULL; }
static int empty(const Item *i) { return !image(i) && !ds_strip(i->run.text, 1).n; }
static int same(const Item *a, const Item *b) {
    return a->run.flags == b->run.flags && a->run.has_font == b->run.has_font &&
           (!a->run.has_font || a->run.font == b->run.font);
}
static int append_item(Document *d, Vec *v, Item item) {
    Item *p = ds_push(&d->arena, v, sizeof(Item));
    if (!p)
        return 0;
    *p = item;
    return 1;
}
static int append_line(Document *d, Vec *v, Line line) {
    Line *p = ds_push(&d->arena, v, sizeof(Line));
    if (!p)
        return 0;
    *p = line;
    return 1;
}
typedef struct {
    Run *run;
    size_t index;
} Ref;
typedef struct {
    Str text;
    Run *first, *font, *indent, *href, *image;
    size_t row, col, rs, cs, index;
    unsigned flags;
    Vec *hrefs;
} Cell;
static int compare_ref(const void *a, const void *b) {
    const Ref *x = a, *y = b;
    if (x->run->row != y->run->row)
        return x->run->row < y->run->row ? -1 : 1;
    if (x->run->col != y->run->col)
        return x->run->col < y->run->col ? -1 : 1;
    if (x->run->line != y->run->line)
        return x->run->line < y->run->line ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}
static int compare_cell(const void *a, const void *b) {
    const Cell *x = a, *y = b;
    return x->index < y->index ? -1 : x->index > y->index;
}
static Run *cell_run(Run *runs, Ref *refs, size_t i) { return refs ? refs[i].run : runs + i; }
static Str cell_text(Document *d, Run *runs, Ref *refs, size_t i, size_t end) {
    size_t k, total = 0, pos = 0, parts = 0, last_line = 0;
    Str only = ds_str("");
    char *out;
    for (k = i; k < end; k++) {
        Run *r = cell_run(runs, refs, k);
        size_t add = r->text.n;
        if (k > i && r->line != last_line) {
            if (total == SIZE_MAX)
                goto large;
            total++;
            parts++;
        }
        if (r->src.p) {
            if (add > SIZE_MAX - r->src.n - 14)
                goto large;
            add += r->src.n + 14;
            parts += 5;
        } else if (add) {
            parts++;
            only = r->text;
        }
        if (add > SIZE_MAX - total - 1)
            goto large;
        total += add;
        last_line = r->line;
    }
    if (!total)
        return ds_str("");
    if (parts == 1)
        return ds_strip(only, 1);
    out = ds_alloc_uninit(&d->arena, total + 1);
    if (!out)
        return (Str){0};
    for (k = i; k < end; k++) {
        Run *r = cell_run(runs, refs, k);
        if (k > i && r->line != cell_run(runs, refs, k - 1)->line)
            out[pos++] = '\n';
        if (r->src.p) {
            memcpy(out + pos, "[ALT: ", 6);
            pos += 6;
        }
        if (r->text.n) {
            memcpy(out + pos, r->text.p, r->text.n);
            pos += r->text.n;
        }
        if (r->src.p) {
            memcpy(out + pos, ". SRC: ", 7);
            pos += 7;
            if (r->src.n) {
                memcpy(out + pos, r->src.p, r->src.n);
                pos += r->src.n;
            }
            out[pos++] = ']';
        }
    }
    out[pos] = 0;
    return ds_strip((Str){out, pos}, 1);
large:
    snprintf(d->arena.error, sizeof(d->arena.error), "table cell text too large");
    return (Str){0};
}
static int table_line(Document *d, Run *runs, size_t n, Line *out) {
    Ref *refs = NULL;
    Cell *cells, **grid;
    size_t ncells = 0, i, j, k, maxr = 0, maxc = 0, meaningful = 0, lastrow = 0;
    for (i = 1; i < n; i++) {
        Run *a = runs + i - 1, *b = runs + i;
        if (a->row > b->row ||
            (a->row == b->row && (a->col > b->col || (a->col == b->col && a->line > b->line))))
            break;
    }
    if (i < n) {
        refs = ds_alloc_uninit(&d->arena, n * sizeof(Ref));
        if (!refs)
            return 0;
        for (i = 0; i < n; i++)
            refs[i] = (Ref){runs + i, i};
        qsort(refs, n, sizeof(Ref), compare_ref);
    }
    for (i = 0; i < n; i = j) {
        Run *r = cell_run(runs, refs, i);
        for (j = i + 1; j < n; j++) {
            Run *x = cell_run(runs, refs, j);
            if (x->row != r->row || x->col != r->col)
                break;
        }
        ncells++;
    }
    cells = ds_alloc(&d->arena, ncells * sizeof(Cell));
    if (!cells)
        return 0;
    for (i = 0, k = 0; i < n; i = j, k++) {
        Run *first = cell_run(runs, refs, i);
        Cell *c = cells + k;
        c->first = first;
        c->row = first->row;
        c->col = first->col;
        c->rs = first->rowspan;
        c->cs = first->colspan;
        c->index = refs ? refs[i].index : i;
        for (j = i; j < n; j++) {
            Run *r = cell_run(runs, refs, j);
            size_t index = refs ? refs[j].index : j;
            if (r->row != c->row || r->col != c->col)
                break;
            if (index < c->index) {
                c->index = index;
                c->rs = r->rowspan;
                c->cs = r->colspan;
            }
            c->flags |= r->flags;
            if (r->has_font)
                c->font = r;
            if (r->has_indent)
                c->indent = r;
            if (r->href.p)
                c->href = r;
            if (r->src.p)
                c->image = r;
            if (!ds_keep_href(d, &c->hrefs, r->href))
                return 0;
        }
        c->text = cell_text(d, runs, refs, i, j);
        if (!c->text.p)
            return 0;
        if (c->row > SIZE_MAX - c->rs || c->col > SIZE_MAX - c->cs)
            goto large;
        if (c->row + c->rs > maxr)
            maxr = c->row + c->rs;
        if (c->col + c->cs > maxc)
            maxc = c->col + c->cs;
    }
    if (maxc && maxr > SIZE_MAX / maxc / sizeof(Str))
        goto large;
    grid = ds_alloc(&d->arena, maxr * maxc * sizeof(Cell *));
    if (!grid)
        return 0;
    for (i = 1; i < ncells; i++)
        if (cells[i - 1].index > cells[i].index)
            break;
    if (i < ncells)
        qsort(cells, ncells, sizeof(Cell), compare_cell);
    for (i = 0; i < ncells; i++)
        if (ds_strip(cells[i].text, 1).n)
            break;
    if (i < ncells) {
        size_t length;
        Str text = ds_strip(cells[i].text, 1);
        if (d->retain_table_cells || (ds_list_marker(text, &length) && length == text.n)) {
            Item *items = ds_alloc(&d->arena, ncells * sizeof(Item));
            if (!items)
                return 0;
            for (i = 0; i < ncells; i++) {
                Cell *c = cells + i;
                items[i].run = *c->first;
                items[i].run.text = c->text;
                items[i].run.src = c->image ? c->image->src : (Str){0};
                items[i].run.flags = c->flags;
                items[i].run.href = c->href ? c->href->href : (Str){0};
                items[i].hrefs = c->hrefs;
                if (c->font) {
                    items[i].run.font = c->font->font;
                    items[i].run.has_font = 1;
                }
                if (c->indent) {
                    items[i].run.indent = c->indent->indent;
                    items[i].run.has_indent = 1;
                }
            }
            if (!ds_list_table(d, out, items, ncells))
                return 0;
            if (d->retain_table_cells) {
                out->table_cells = items;
                out->ntable_cells = ncells;
            }
        }
    }
    for (i = 0; i < ncells; i++) {
        Cell *c = cells + i;
        size_t y, x;
        for (y = 0; y < c->rs; y++)
            for (x = 0; x < c->cs; x++)
                grid[(c->row + y) * maxc + c->col + x] = c;
    }
    for (i = 0; i < maxr; i++) {
        for (j = 0; j < maxc; j++)
            if (grid[i * maxc + j] && grid[i * maxc + j]->text.n)
                break;
        if (j < maxc) {
            meaningful++;
            lastrow = i;
        }
    }
    if (meaningful == 1) {
        size_t count = 0;
        Item *items;
        for (j = 0; j < maxc; j++)
            if (grid[lastrow * maxc + j] && grid[lastrow * maxc + j]->text.n)
                count++;
        items = ds_alloc(&d->arena, count * sizeof(Item));
        if (!items)
            return 0;
        for (j = 0, k = 0; j < maxc; j++) {
            Cell *c = grid[lastrow * maxc + j];
            Item *item;
            if (!c || !c->text.n)
                continue;
            item = items + k++;
            item->fake_table = 1;
            item->run = *c->first;
            item->run.text = c->text;
            item->run.src = (Str){0};
            item->run.flags = c->flags;
            item->run.has_font = c->font != NULL;
            item->run.has_indent = c->indent != NULL;
            item->run.href = c->href ? c->href->href : (Str){0};
            item->hrefs = c->hrefs;
            if (c->font) {
                item->run.font = c->font->font;
            }
            if (c->indent) {
                item->run.indent = c->indent->indent;
            }
        }
        out->items = items;
        out->nitems = count;
    } else if (meaningful) {
        Str *texts = ds_alloc_uninit(&d->arena, maxr * maxc * sizeof(Str));
        if (!texts)
            return 0;
        for (i = 0; i < maxr * maxc; i++)
            texts[i] = grid[i] ? grid[i]->text : ds_str("");
        out->matrix = (Matrix){texts, maxr, maxc};
    }
    return !d->arena.error[0];
large:
    snprintf(d->arena.error, sizeof(d->arena.error), "table dimensions overflow");
    return 0;
}
static int group(Document *d, Vec *lines) {
    Run *runs = d->runs.p;
    size_t i, j, k;
    for (i = 0; i < d->runs.n; i = j) {
        Line line = {0};
        Vec items = {0};
        for (j = i; j < d->runs.n && runs[j].block == runs[i].block; j++)
            ;
        if (runs[i].in_table) {
            if (!table_line(d, runs + i, j - i, &line))
                return 0;
            if ((line.nitems || line.matrix.nr) && !append_line(d, lines, line))
                return 0;
            continue;
        }
        for (k = i; k < j; k++) {
            Item cur = {runs[k], 0};
            Item *all = items.p;
            size_t q;
            if (!items.n && empty(&cur))
                continue;
            if (!ds_keep_href(d, &cur.hrefs, cur.run.href))
                return 0;
            if (!items.n) {
                if (!append_item(d, &items, cur))
                    return 0;
                continue;
            }
            if (image(&cur) || image(all + items.n - 1)) {
                if (!append_item(d, &items, cur))
                    return 0;
            } else if (empty(&cur) || same(&cur, all + items.n - 1)) {
                if (!ds_append_hrefs(d, &all[items.n - 1].hrefs, cur.hrefs))
                    return 0;
                all[items.n - 1].run.text =
                    ds_cat(&d->arena, all[items.n - 1].run.text, cur.run.text);
            } else {
                int merged = 0;
                for (q = items.n; q; q--)
                    if (!image(all + q - 1) && same(&cur, all + q - 1)) {
                        Vec parts = {0};
                        Str *p;
                        size_t x;
                        for (x = q - 1; x < items.n; x++)
                            if (!image(all + x)) {
                                p = ds_push(&d->arena, &parts, sizeof(Str));
                                if (!p)
                                    return 0;
                                *p = all[x].run.text;
                                if (x >= q && !ds_append_hrefs(d, &all[q - 1].hrefs, all[x].hrefs))
                                    return 0;
                            }
                        p = ds_push(&d->arena, &parts, sizeof(Str));
                        if (!p)
                            return 0;
                        *p = cur.run.text;
                        if (!ds_append_hrefs(d, &all[q - 1].hrefs, cur.hrefs))
                            return 0;
                        all[q - 1].run.text = ds_join(&d->arena, parts.p, parts.n, ds_str(""));
                        items.n = q;
                        merged = 1;
                        break;
                    }
                if (!merged && !append_item(d, &items, cur))
                    return 0;
            }
        }
        if (items.n == 1 && !image(items.p))
            ((Item *)items.p)->run.text = ds_strip(((Item *)items.p)->run.text, 1);
        for (k = 0; k < items.n; k++)
            if (!empty((Item *)items.p + k))
                break;
        if (k < items.n) {
            line.items = items.p;
            line.nitems = items.n;
            if (!append_line(d, lines, line))
                return 0;
        }
    }
    return !d->arena.error[0];
}
static Str line_text(Document *, Item *, size_t);
static Str display_text(Document *d, Line *line) {
    TextView *view = line->text_view;
    if (view && view->items == line->items && view->nitems == line->nitems)
        return view->display;
    return line_text(d, line->items, line->nitems);
}
static Str clean_lower(Document *d, Item *item, Str *cached) {
    if (cached && cached->p) return *cached;
    {
        Str text = ds_lower(&d->arena, ds_strip(item->run.text, 1));
        if (cached) *cached = text;
        return text;
    }
}
static double font_stats(Document *, Vec *, int *);
static int semantic_heading(const Run *run) {
    return run->tag.n == 2 && run->tag.p[0] == 'h' &&
           run->tag.p[1] >= '1' && run->tag.p[1] <= '6';
}
static int clean(Document *d, const Rules *r, Vec *lines) {
    Map counts = {0}, labels = {0};
    Vec out = {0};
    Line *all = lines->p;
    Str *lower = NULL;
    size_t *offsets = NULL;
    size_t i, j, k, distinct_labels = 0;
    int dominant = 0;
    double common = r->page_label ? font_stats(d, lines, &dominant) : 0;
    /* The original slices are immutable throughout cleanup. Reuse lowercase
     * views between page-label detection, repetition counting and removal. */
    if (lines->n && (r->repeat_threshold || r->page_label)) {
        size_t count = 0;
        offsets = ds_alloc_uninit(&d->arena, lines->n * sizeof(size_t));
        if (!offsets) return 0;
        for (i = 0; i < lines->n; i++) {
            offsets[i] = count;
            if (all[i].nitems > SIZE_MAX - count) return 0;
            count += all[i].nitems;
        }
        if (count > SIZE_MAX / sizeof(Str)) return 0;
        lower = ds_alloc(&d->arena, count * sizeof(Str));
        if (!lower) return 0;
    }
    for (i = 0; r->page_label && i < lines->n; i++) {
        Line *line = all + i;
        Str text;
        if (line->matrix.nr || line->listitem || !line->nitems || image(line->items))
            continue;
        text = line->nitems == 1 ? clean_lower(d, line->items, lower ? lower + offsets[i] : NULL) :
               ds_lower(&d->arena, ds_strip(line_text(d, line->items, line->nitems), 1));
        if (ds_match(d, r->page_label, text, 0) && !ds_map_get(&labels, text)) {
            if (!ds_map_put(&d->arena, &labels, text, line->items))
                return 0;
            distinct_labels++;
        }
    }
    for (i = 0; r->repeat_threshold && i < lines->n; i++) {
        if (all[i].matrix.nr || all[i].listitem)
            continue;
        for (j = 0; j < all[i].nitems; j++) {
            Item *it = all[i].items + j;
            Str text = clean_lower(d, it, lower ? lower + offsets[i] + j : NULL);
            size_t *count;
            if (!text.n || image(it) || all[i].listitem)
                continue;
            count = ds_map_get(&counts, text);
            if (!count) {
                count = ds_alloc(&d->arena, sizeof(size_t));
                if (!count || !ds_map_put(&d->arena, &counts, text, count))
                    return 0;
            }
            (*count)++;
        }
    }
    for (i = 0; i < lines->n; i++) {
        Line line = all[i];
        size_t start = 0;
        int previous = -1, page_label = 0;
        if (line.matrix.nr || line.listitem) {
            if (!append_line(d, &out, line))
                return 0;
            continue;
        }
        if (r->page_label && line.nitems && !image(line.items) &&
            !semantic_heading(&line.items[0].run)) {
            Run *first = &line.items[0].run;
            int big = dominant && first->has_font && first->font > 1.2 * common;
            int small = dominant && first->has_font && first->font < common;
            if (!big && (small || distinct_labels >= 3)) {
                Str text = line.nitems == 1 ? clean_lower(d, line.items, lower ? lower + offsets[i] : NULL) :
                           ds_lower(&d->arena, ds_strip(line_text(d, line.items, line.nitems), 1));
                page_label = ds_match(d, r->page_label, text, 0);
            }
        }
        for (j = 0; j < line.nitems; j++) {
            Item it = line.items[j];
            Str text;
            size_t *count;
            int redundant = 0;
            if (!image(&it)) {
                text = clean_lower(d, &it, lower ? lower + offsets[i] + j : NULL);
                count = ds_map_get(&counts, text);
                redundant = page_label || (count && r->repeat_threshold &&
                    (r->repeat_threshold < 0 || *count >= (size_t)r->repeat_threshold));
                for (k = 0; !redundant && k < r->nremove; k++)
                    if ((!r->remove[k].needs_href || it.run.href.p) &&
                        ds_match(d, r->remove[k].rx, text, 0))
                        redundant = 1;
            }
            /* Preserve both sides of a mixed line, in source order. A matched
             * piece must not discard its neighbors or promote them to headings. */
            if (previous >= 0 && redundant != previous) {
                Line part = line;
                part.items += start;
                part.nitems = j - start;
                part.redundant = previous;
                if (!append_line(d, &out, part))
                    return 0;
                start = j;
            }
            previous = redundant;
        }
        if (line.nitems) {
            line.items += start;
            line.nitems -= start;
            line.redundant = previous;
            if (!append_line(d, &out, line))
                return 0;
        }
    }
    *lines = out;
    return !d->arena.error[0];
}
static double font_stats(Document *d, Vec *lines, int *dominant) {
    Map counts = {0};
    size_t i, best = 0, eligible = lines->n;
    double common = 0;
    Line *all = lines->p;
    *dominant = 0;
    if (d->no_fonts) return 0;
    for (i = 0; i < lines->n; i++)
        if (all[i].redundant)
            eligible--;
    for (i = 0; i < lines->n; i++)
        if (all[i].nitems && !all[i].redundant) {
            Item *f = all[i].items;
            size_t *count;
            Str key;
            if (image(f) || !f->run.has_font)
                continue;
            key = (Str){(const char *)&f->run.font, sizeof(double)};
            count = ds_map_get(&counts, key);
            if (!count) {
                count = ds_alloc(&d->arena, sizeof(size_t));
                if (!count || !ds_map_put(&d->arena, &counts, key, count))
                    return 0;
            }
            (*count)++;
        }
    /* Iterate in source order to preserve Python's first-seen tie breaking. */
    for (i = 0; i < lines->n; i++)
        if (all[i].nitems && !all[i].redundant) {
            Item *f = all[i].items;
            size_t *count;
            if (image(f) || !f->run.has_font)
                continue;
            count = ds_map_get(&counts, (Str){(char *)&f->run.font, sizeof(double)});
            if (count && *count > best) {
                best = *count;
                common = f->run.font;
            }
        }
    *dominant = best > eligible * .5;
    return common;
}
enum { CASING_KNOWN = 128 };
static unsigned attrs(Item *i) {
    unsigned flags = i->run.flags;
    if (!i->casing) {
        i->casing = CASING_KNOWN; /* Cache negative results as well. */
        if (ds_upper(i->run.text)) i->casing |= ALL_CAPS;
        else if (ds_proper(i->run.text)) i->casing |= PROPER_CASE;
    }
    flags |= i->casing & (ALL_CAPS | PROPER_CASE);
    if (i->fake_table)
        flags |= FAKE_TABLE;
    return flags;
}
static int phone_line(Document *d, Line *line) {
    Str first = line->nitems ? ds_strip(line->items[0].run.text, 0) : (Str){0};
    return first.n && first.p[0] == '(' && ds_phone(display_text(d, line));
}
/* Reuse the regex compiler's conservative mandatory-first-atom guard before
 * allocating normalized copies of ordinary body paragraphs. */
static int header_candidate(const Rules *r, Line *line) {
    size_t i, j;
    Str first = {0};
    unsigned char c;
    for (i = 0; i < line->nitems; i++) {
        if (image(line->items + i))
            continue;
        first = ds_strip(line->items[i].run.text, 0);
        if (first.n)
            break;
    }
    if (!first.n)
        return 0;
    c = (unsigned char)first.p[0];
    if (c >= 128)
        return r->nheaders != 0;
    /* Header rules always receive lowercased logical text. */
    if (c >= 'A' && c <= 'Z')
        c |= 32;
    for (j = 0; j < r->nheaders; j++) {
        Regex *rx = r->headers[j].rx;
        if (rx->first_digit && (c < '0' || c > '9'))
            continue;
        if (rx->first_ascii) {
            unsigned char expected = (unsigned char)(rx->first_ascii - 1);
            if (rx->caseless) {
                if (expected >= 'A' && expected <= 'Z')
                    expected |= 32;
                if (c >= 'A' && c <= 'Z')
                    c |= 32;
            }
            if (c != expected)
                continue;
        }
        return 1;
    }
    return 0;
}
/* Rules see the complete logical line, with HTML whitespace normalized.
 * Display text retains its original whitespace and capitalization. */
static Str rule_text(Document *d, Line *line) {
    TextView *view = line->text_view;
    /* Cleanup and heading splitting create slices of the original items.
     * Share immutable views only while the complete slice still matches. */
    if (!view || view->items != line->items || view->nitems != line->nitems) {
        view = ds_alloc(&d->arena, sizeof(*view));
        if (!view) return (Str){0};
        view->items = line->items;
        view->nitems = line->nitems;
        view->display = line_text(d, line->items, line->nitems);
        line->text_view = view;
    }
    if (!view->rule.p) view->rule = ds_rule_text(&d->arena, view->display);
    return view->rule;
}
static int header_match(Document *d, const Rules *r, Line *line, Str text) {
    size_t j;
    unsigned flags = line->nitems ? attrs(line->items) : 0;
    for (j = 0; j < r->nheaders; j++) {
        Header *h = r->headers + j;
        /* Explicit requirements override the shared any-style policy; an
         * explicit empty array permits unstyled structural headings. */
        int styled = h->has_requires ? (h->requires & flags) == h->requires :
                     (!r->header_styles || (r->header_styles & flags));
        if (styled && ds_match(d, h->rx, text, 0))
            return (int)j;
    }
    return -1;
}
static int item_label(Str text) {
    size_t i = 4, count = 0;
    if (text.n < 5 || memcmp(text.p, "item", 4))
        return 0;
    while (i < text.n) {
        size_t start;
        while (i < text.n && (text.p[i] == ' ' || text.p[i] == ','))
            i++;
        start = i;
        while (i < text.n && text.p[i] >= '0' && text.p[i] <= '9')
            i++;
        if (start == i || i - start > 3)
            return 0;
        if (i < text.n && text.p[i] == '.')
            i++;
        if (i < text.n && text.p[i] >= 'a' && text.p[i] <= 'z')
            i++;
        if (i < text.n && text.p[i] == '.')
            i++;
        if (i < text.n && text.p[i] != ' ' && text.p[i] != ',')
            return 0;
        count++;
    }
    return count != 0;
}
static int split(Document *d, const Rules *r, Vec *lines) {
    Vec out = {0};
    Line *all = lines->p;
    int dominant;
    double common = font_stats(d, lines, &dominant);
    size_t i;
    for (i = 0; i < lines->n; i++) {
        Line line = all[i];
        if (line.redundant) {
            if (!append_line(d, &out, line))
                return 0;
            continue;
        }
        if (!line.layout_text && line.nitems && !image(line.items) && header_candidate(r, &line) &&
            header_match(d, r, &line, rule_text(d, &line)) >= 0) {
            /* Precise explicit section rules take precedence over inferred
             * enumeration. Lists still precede heuristic heading prediction. */
            line.listitem = 0;
            if (!append_line(d, &out, line))
                return 0;
            continue;
        }
        if (!line.layout_text && !line.listitem && line.nitems > 1 && !image(line.items) && !phone_line(d, &line) &&
            ((attrs(line.items) & ~FAKE_TABLE) ||
             (dominant && line.items[0].run.has_font && line.items[0].run.font > common * 1.2))) {
            Line first = line;
            first.nitems = 1;
            if (!append_line(d, &out, first))
                return 0;
            line.items++;
            line.nitems--;
        }
        if (!append_line(d, &out, line))
            return 0;
    }
    *lines = out;
    return !d->arena.error[0];
}
/* Require evidence of multiple sentences before demoting long italic text.
 * A single full-sentence risk heading must retain its heading status. */
static int italic_prose(Document *d, Line *line, int big) {
    Item *first = line->nitems ? line->items : NULL;
    Str text;
    size_t i, words = 0;
    int in_word = 0;
    if (!first || big || !(first->run.flags & ITALIC) ||
        (first->run.flags & (BOLD | CENTERED | UNDERLINE)) || semantic_heading(&first->run))
        return 0;
    text = ds_strip(display_text(d, line), 1);
    for (i = 0; i < text.n; i++) {
        int space = text.p[i] == ' ' || text.p[i] == '\n' || text.p[i] == '\t';
        if (!space && !in_word)
            words++;
        in_word = !space;
    }
    if (words < 28 || text.n < 160)
        return 0;
    for (i = 1; i + 2 < text.n; i++)
        if ((text.p[i] == '.' || text.p[i] == '?' || text.p[i] == '!') &&
            text.p[i + 1] == ' ' && text.p[i + 2] >= 'A' && text.p[i + 2] <= 'Z') {
            size_t start = i;
            while (start && text.p[start - 1] >= 'a' && text.p[start - 1] <= 'z')
                start--;
            if (i - start >= 3 && (!start || text.p[start - 1] == ' '))
                return 1;
        }
    return 0;
}
static int classify(Document *d, const Rules *r, Vec *lines) {
    Line *all = lines->p;
    int dominant, predicted = 0, section = -1, attr_levels[128], next = 0;
    double common = font_stats(d, lines, &dominant);
    size_t i, j;
    for (i = 0; i < 128; i++)
        attr_levels[i] = -1;
    if (r->nheaders) {
        predicted = r->headers[0].level + 1;
        for (i = 1; i < r->nheaders; i++)
            if (r->headers[i].level >= predicted)
                predicted = r->headers[i].level + 1;
    }
    for (i = 0; i < lines->n; i++) {
        Line *ln = all + i;
        Item *first = ln->nitems ? ln->items : NULL;
        ln->level = -1;
        ln->klass = ds_str("text");
        if (ln->redundant) {
            ln->klass = ds_str("redundant");
            continue;
        }
        if (ln->listitem) {
            ln->klass = ds_str("listitem");
            continue;
        }
        if (first && !image(first)) {
            int big = dominant && first->run.has_font && first->run.font > 1.2 * common;
            int matched;
            Str text;
            ln->attrs = attrs(first);
            matched = -1;
            if (!ln->layout_text && header_candidate(r, ln)) {
                text = rule_text(d, ln);
                matched = header_match(d, r, ln, text);
            }
            if (matched >= 0) {
                Header *h = r->headers + matched;
                Vec groups = {0};
                unsigned g;
                Str *p = ds_push(&d->arena, &groups, sizeof(Str));
                if (!p)
                    return 0;
                *p = h->name;
                for (g = 1; g <= h->rx->groups; g++) {
                    Str part = ds_group(d, text, g);
                    if (part.p) {
                        p = ds_push(&d->arena, &groups, sizeof(Str));
                        if (!p)
                            return 0;
                        *p = part;
                    }
                }
                ln->title = ds_join(&d->arena, groups.p, groups.n, ds_str(""));
                ln->level = h->level;
                ln->klass = h->name;
            } else if (dominant && first->run.has_font && first->run.font < common) {
                ln->level = -2;
                ln->klass = ds_str("textsmall");
                ln->attrs = 0;
            } else if (!ln->layout_text && (big || (ln->attrs & ~FAKE_TABLE)) &&
                       !phone_line(d, ln) && !italic_prose(d, ln, big)) {
                ln->level = predicted;
                ln->klass = ds_str("predicted header");
                ln->predicted = 1;
            }
        }
    }
    /* A continued explicit heading belongs to the active section. Suppress it
     * before it can reset predicted-heading levels or the document stack. */
    {
        Vec active = {0};
        size_t kept = 0;
        for (i = 0; i < lines->n; i++) {
            Line ln = all[i];
            if (!ln.predicted && ln.level >= 0 && ln.title.n) {
                Line *parents = active.p, *previous = NULL, *slot;
                Str text = rule_text(d, &ln);
                int continued = 0, bare = 1;
                size_t q, used = 0;
                for (q = 0; q + 9 <= text.n; q++)
                    if ((!q || text.p[q - 1] < 'a' || text.p[q - 1] > 'z') &&
                        !memcmp(text.p + q, "continued", 9) &&
                        ds_strip((Str){text.p + q + 9, text.n - q - 9}, 0).n <= 2)
                        continued = 1;
                for (q = 0; q < text.n; q++)
                    if (text.p[q] != ' ' && text.p[q] != '.') {
                        if (used >= ln.title.n || text.p[q] != ln.title.p[used])
                            bare = 0;
                        used++;
                    }
                bare = bare && used == ln.title.n;
                bare = bare || item_label(text);
                if (bare && dominant && ln.items[0].run.has_font &&
                    ln.items[0].run.font < common) {
                    int upcoming = 0;
                    for (q = i + 1; q < lines->n && q < i + 5; q++)
                        if (!all[q].predicted && ds_eq(all[q].title, ln.title) &&
                            ds_eq(all[q].klass, ln.klass)) {
                            upcoming = 1;
                            break;
                        }
                    if (upcoming) {
                        ln.redundant = 1;
                        ln.level = -1;
                        ln.klass = ds_str("redundant");
                        ln.title = (Str){0};
                        all[kept++] = ln;
                        continue;
                    }
                }
                for (q = active.n; q; q--)
                    if (parents[q - 1].level == ln.level) {
                        previous = parents + q - 1;
                        break;
                    }
                if (previous && ds_eq(previous->title, ln.title) &&
                    ds_eq(previous->klass, ln.klass) &&
                    (continued || (bare && dominant && ln.items[0].run.has_font &&
                                   ln.items[0].run.font < common))) {
                    ln.redundant = 1;
                    ln.level = -1;
                    ln.klass = ds_str("redundant");
                    ln.title = (Str){0};
                    all[kept++] = ln;
                    continue;
                }
                while (active.n && ((Line *)active.p)[active.n - 1].level >= ln.level)
                    active.n--;
                slot = ds_push(&d->arena, &active, sizeof(Line));
                if (!slot)
                    return 0;
                *slot = ln;
            }
            all[kept++] = ln;
        }
        lines->n = kept;
    }
    for (i = 0; i < lines->n; i++) {
        Line *ln = all + i;
        if (ln->listitem)
            continue;
        if (!ln->predicted && ln->level >= 0) {
            if (ln->level <= section) {
                for (j = 0; j < 128; j++)
                    attr_levels[j] = -1;
                next = 0;
            }
            section = ln->level;
        } else if (ln->predicted) {
            unsigned key = ln->attrs & (127 & ~FAKE_TABLE);
            if (attr_levels[key] < 0)
                attr_levels[key] = predicted + next++;
            ln->level = attr_levels[key];
        }
    }
    return !d->arena.error[0];
}
static Str line_text(Document *d, Item *items, size_t n) {
    size_t i, count = 0;
    Str *parts;
    int fake = n && items[0].fake_table;
    for (i = 0; i < n; i++)
        if (!image(items + i))
            count++;
    if (!count)
        return ds_str("");
    if (count == 1) {
        for (i = 0; i < n; i++)
            if (!image(items + i))
                return fake ? ds_strip(items[i].run.text, 0) : items[i].run.text;
    }
    parts = ds_alloc_uninit(&d->arena, count * sizeof(Str));
    if (!parts)
        return (Str){0};
    for (i = 0, count = 0; i < n; i++)
        if (!image(items + i))
            parts[count++] = fake ? ds_strip(items[i].run.text, 0) : items[i].run.text;
    return ds_join(&d->arena, parts, count, fake ? ds_str(" ") : ds_str(""));
}
static int layout_number(Str text) {
    size_t i, k;
    int digit = 0;
    for (i = 0; i < text.n; i += k) {
        uint32_t c = ds_utf8(text.p + i, text.n - i, &k);
        if (c >= '0' && c <= '9') {
            digit = 1;
            continue;
        }
        if (!ds_space(c) && c != '$' && c != '%' && c != '.' && c != ',' &&
            c != '(' && c != ')' && c != '+' && c != '-' && c != 0x2013 && c != 0x2014)
            return 0;
    }
    return digit;
}
/* Optional, rule-driven layout-table recovery. Work with original cells, not
 * span-expanded strings, so content is emitted once and styles remain usable.
 * Explicit section rows plus substantial prose are required; numeric tables
 * and tables of contents retain their matrix representation. */
static int layout_tables(Document *d, const Rules *r, Vec *lines) {
    Vec out = {0};
    Line *all = lines->p;
    size_t i;
    if (!r->unwrap_layout || !r->nheaders)
        return 1;
    for (i = 0; i < lines->n; i++) {
        Line source = all[i];
        Vec proposed = {0};
        size_t c, j, nonempty = 0, numbers = 0, headers = 0, prose = 0;
        int ordered = 1;
        if (source.matrix.nr && source.ntable_cells) {
            for (c = 0; c < source.ntable_cells; c = j) {
                Line row = {0};
                Vec items = {0};
                Str text;
                size_t k, row_numbers = 0;
                for (j = c + 1; j < source.ntable_cells &&
                     source.table_cells[j].run.row == source.table_cells[c].run.row; j++)
                    ;
                if (c && source.table_cells[c].run.row < source.table_cells[c - 1].run.row)
                    ordered = 0;
                for (k = c; k < j; k++) {
                    Item item = source.table_cells[k];
                    item.run.text = ds_strip(item.run.text, 1);
                    item.casing = 0;
                    if (!item.run.text.n)
                        continue;
                    nonempty++;
                    row_numbers += layout_number(item.run.text);
                    item.fake_table = 1;
                    item.run.tag = ds_str("td");
                    item.run.src = (Str){0}; /* cell_text already preserves embedded ALT/SRC */
                    item.run.in_table = 0;
                    if (!append_item(d, &items, item))
                        return 0;
                }
                if (!items.n)
                    continue;
                numbers += row_numbers;
                row.items = items.p;
                row.nitems = items.n;
                text = rule_text(d, &row);
                row.layout_text = 1;
                if (!row_numbers && text.n > 240 && row.nitems > 1) {
                    Line marker = row;
                    Str marker_text, suffix;
                    marker.nitems = 1;
                    marker_text = rule_text(d, &marker);
                    if (header_candidate(r, &marker) &&
                        header_match(d, r, &marker, marker_text) >= 0) {
                        Str matched = ds_group(d, marker_text, 0);
                        suffix = ds_strip((Str){marker_text.p + matched.n,
                                                marker_text.n - matched.n}, 0);
                        if (!suffix.n || ds_is(suffix, ".") || ds_is(suffix, ":")) {
                            marker.layout_text = 0;
                            if (!append_line(d, &proposed, marker))
                                return 0;
                            headers++;
                            row.items++;
                            row.nitems--;
                            prose++;
                            if (!append_line(d, &proposed, row))
                                return 0;
                            continue;
                        }
                    }
                }
                if (!row_numbers && text.n <= 240 && header_candidate(r, &row) &&
                    header_match(d, r, &row, text) >= 0) {
                    headers++;
                    row.layout_text = 0;
                } else if (text.n >= 200)
                    prose++;
                if (!append_line(d, &proposed, row))
                    return 0;
            }
        }
        if (ordered && headers && prose &&
            (!numbers || numbers < nonempty / 4 + (nonempty % 4 != 0))) {
            Line *rows = proposed.p;
            for (c = 0; c < proposed.n; c++)
                if (!append_line(d, &out, rows[c]))
                    return 0;
        } else if (!append_line(d, &out, source))
            return 0;
    }
    *lines = out;
    return !d->arena.error[0];
}
static Row *add_row(Document *d, size_t level, Str type, Str klass) {
    Row *r = ds_push(&d->arena, &d->rows, sizeof(Row));
    if (!r)
        return NULL;
    r->level = level;
    r->type = type;
    r->klass = klass;
    r->cleaned = -1;
    return r;
}
static int build(Document *d, Vec *lines) {
    Vec stack = {0};
    Vec indents = {0};
    int *p;
    size_t i, j;
    Row *r = add_row(d, 0, ds_str("title"), ds_str("introduction"));
    if (!r)
        return 0;
    r->text = ds_str("introduction");
    p = ds_push(&d->arena, &stack, sizeof(int));
    if (!p)
        return 0;
    *p = -1;
    p = ds_push(&d->arena, &stack, sizeof(int));
    if (!p)
        return 0;
    *p = 0;
    for (i = 0; i < lines->n; i++) {
        Line *ln = (Line *)lines->p + i;
        size_t content_level = stack.n - 1;
        Str text;
        if (ln->listitem) {
            double *indent;
            while (indents.n > 1 &&
                   ln->list_indent < ((double *)indents.p)[indents.n - 1] - 2)
                indents.n--;
            if (!indents.n || ln->list_indent > ((double *)indents.p)[indents.n - 1] + 2) {
                indent = ds_push(&d->arena, &indents, sizeof(double));
                if (!indent)
                    return 0;
                *indent = ln->list_indent;
            } else if (indents.n == 1)
                ((double *)indents.p)[0] = ln->list_indent;
            content_level += indents.n - 1;
        } else
            indents.n = 0;
        if (ln->matrix.nr) {
            r = add_row(d, stack.n - 1, ds_str("table"), ds_str("table"));
            if (!r)
                return 0;
            r->matrix = ln->matrix;
            continue;
        }
        if (ln->level >= 0) {
            while (stack.n > 1 && ((int *)stack.p)[stack.n - 1] >= ln->level)
                stack.n--;
            r = add_row(d, stack.n - 1, ds_str("title"), ln->klass);
            if (!r)
                return 0;
            r->text = display_text(d, ln);
            r->title = ln->title.n ? ln->title : (Str){0};
            r->items = ln->items;
            r->nitems = ln->nitems;
            p = ds_push(&d->arena, &stack, sizeof(int));
            if (!p)
                return 0;
            *p = ln->level;
            continue;
        }
        text = ds_strip(display_text(d, ln), 1);
        if (text.n) {
            Vec items = {0};
            for (j = 0; j < ln->nitems; j++)
                if (image(ln->items + j))
                    break;
            if (j == ln->nitems) {
                items.p = ln->items;
                items.n = ln->nitems;
            } else
                for (j = 0; j < ln->nitems; j++)
                    if (!image(ln->items + j) && !append_item(d, &items, ln->items[j]))
                        return 0;
            r = add_row(d, content_level, ln->klass, ln->klass);
            if (!r)
                return 0;
            r->text = text;
            r->items = items.p;
            r->nitems = items.n;
        }
        for (j = 0; j < ln->nitems; j++)
            if (image(ln->items + j)) {
                r = add_row(d, content_level, ds_str("image"), ds_str("image"));
                if (!r)
                    return 0;
                r->text = ln->items[j].run.text;
                r->items = ln->items + j;
                r->nitems = 1;
            }
    }
    return !d->arena.error[0];
}
static int same_title_style(const Row *a, const Row *b) {
    const Run *x, *y;
    if (!a->nitems || !b->nitems)
        return 0;
    x = &a->items[0].run;
    y = &b->items[0].run;
    return same(a->items, b->items) && x->has_indent == y->has_indent &&
           (!x->has_indent || x->indent == y->indent);
}
enum { MERGE_TEXT, MERGE_TITLES };
/* Titles use their first source run's formatting, as heading detection does.
 * Merge them before table-title inheritance; compact prose after annotations.
 * Every merge retains source items and links for custom columns. */
static int merge_rows(Document *d, int kind) {
    Row *rows = d->rows.p;
    size_t i, j, k, out = 0;
    for (i = 0; i < d->rows.n; i = j) {
        Row block = rows[i];
        j = i + 1;
        int title = kind == MERGE_TITLES;
        if (title ? ds_is(block.type, "title") :
                    ds_is(block.type, "text") || ds_is(block.type, "textsmall")) {
            while (j < d->rows.n && ds_eq(rows[j].type, block.type) &&
                   rows[j].level == block.level &&
                   (!title || same_title_style(&block, rows + j)))
                j++;
            if (j > i + 1) {
                Vec parts = {0}, items = {0};
                size_t row;
                for (row = i; row < j; row++) {
                    Str *part = ds_push(&d->arena, &parts, sizeof(Str));
                    if (!part)
                        return 0;
                    *part = rows[row].text;
                    for (k = 0; k < rows[row].nitems; k++) {
                        Item *item = ds_push(&d->arena, &items, sizeof(Item));
                        if (!item)
                            return 0;
                        *item = rows[row].items[k];
                    }
                }
                block.text = ds_join(&d->arena, parts.p, parts.n, ds_str(" "));
                block.items = items.p;
                block.nitems = items.n;
            }
        }
        block.id = out;
        rows[out++] = block;
    }
    d->rows.n = out;
    return !d->arena.error[0];
}
int ds_standardize(Document *d, const Rules *r) {
    Vec lines = d->flow ? d->lines : (Vec){0};
    d->retain_table_cells = r->unwrap_layout;
    d->collect_hrefs = r->collect_hrefs;
    return (d->flow || group(d, &lines)) && layout_tables(d, r, &lines) && ds_lists(d, &lines) &&
           clean(d, r, &lines) && split(d, r, &lines) &&
           classify(d, r, &lines) && build(d, &lines) && ds_list_continuations(d, r) && ds_list_blocks(d) &&
           merge_rows(d, MERGE_TITLES) && ds_tables(d, r) && merge_rows(d, MERGE_TEXT);
}
