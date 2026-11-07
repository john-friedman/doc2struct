#include "doc2struct.h"
#include "scan.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    Str text;
    size_t next;
} Part;

typedef struct {
    Str tag;
    size_t block, first_line;
    int in_table;
    size_t row, col, rs, cs, line, records, parts, head, tail, bytes;
    Str text, href, image_src;
    double font, indent;
    unsigned flags;
    int has_font, has_indent;
    Vec *hrefs;
} FlowCell;

typedef struct {
    Document *document;
    Vec items, cells, parts;
    FlowCell **grid;
    size_t grid_cap;
    size_t block;
    size_t max_row, max_col, last_row, last_col;
    int active, table, has_coordinate, unordered;
} Flow;

static int image(const Item *item) { return item->run.src.p != NULL; }
static int empty(const Item *item) {
    return !image(item) && ds_blank_text(item->run.text.p, item->run.text.n);
}
static int same(const Item *a, const Item *b) {
    return a->run.flags == b->run.flags && a->run.has_font == b->run.has_font &&
           (!a->run.has_font || a->run.font == b->run.font);
}
typedef struct {
    Item item;
    size_t capacity;
} FlowItem;

static int add_item(Flow *flow, Item value) {
    FlowItem *item = ds_push(&flow->document->arena, &flow->items, sizeof(FlowItem));
    if (!item)
        return 0;
    *item = (FlowItem){value, 0};
    return ds_keep_href(flow->document, &item->item.hrefs, value.run.href);
}
static int append_text(Flow *flow, FlowItem *item, Str text) {
    Str old = item->item.run.text;
    size_t needed, capacity;
    char *buffer;
    if (!text.n)
        return 1;
    if (!old.n) {
        item->item.run.text = text;
        item->capacity = 0;
        return 1;
    }
    if (old.n > SIZE_MAX - text.n - 1) {
        snprintf(flow->document->arena.error, 256, "text too large");
        return 0;
    }
    needed = old.n + text.n + 1;
    if (needed > item->capacity) {
        capacity = needed < 64 ? 64 : needed;
        if (item->capacity <= SIZE_MAX / 2 && item->capacity * 2 > capacity)
            capacity = item->capacity * 2;
        buffer = ds_alloc_uninit(&flow->document->arena, capacity);
        if (!buffer)
            return 0;
        memcpy(buffer, old.p, old.n);
        item->item.run.text.p = buffer;
        item->capacity = capacity;
    } else
        buffer = (char *)old.p;
    memcpy(buffer + old.n, text.p, text.n);
    buffer[needed - 1] = 0;
    item->item.run.text.n = needed - 1;
    return 1;
}

/* This is the native line model, assembled while normalization emits text.
 * No document-wide array of instruction runs is constructed on this path. */
static int plain_run(Flow *flow, const Run *run) {
    Item current = {*run, 0};
    FlowItem *items = flow->items.p;
    size_t i, j;
    if (!flow->items.n)
        return empty(&current) || add_item(flow, current);
    if (image(&current) || image(&items[flow->items.n - 1].item))
        return add_item(flow, current);
    if (empty(&current) || same(&current, &items[flow->items.n - 1].item))
        return ds_keep_href(flow->document, &items[flow->items.n - 1].item.hrefs, run->href) &&
               append_text(flow, items + flow->items.n - 1, run->text);
    for (i = flow->items.n; i; i--) {
        if (!image(&items[i - 1].item) && same(&current, &items[i - 1].item)) {
            for (j = i; j < flow->items.n; j++)
                if (!image(&items[j].item) &&
                    (!ds_append_hrefs(flow->document, &items[i - 1].item.hrefs, items[j].item.hrefs) ||
                     !append_text(flow, items + i - 1, items[j].item.run.text)))
                    return 0;
            if (!ds_keep_href(flow->document, &items[i - 1].item.hrefs, run->href) ||
                !append_text(flow, items + i - 1, run->text))
                return 0;
            flow->items.n = i;
            return 1;
        }
    }
    return add_item(flow, current);
}

static int cell_part(Flow *flow, FlowCell *cell, Str text) {
    Document *d = flow->document;
    Part *part;
    size_t index;
    if (!text.n)
        return 1;
    if (text.n > SIZE_MAX - cell->bytes - 1) {
        snprintf(d->arena.error, sizeof(d->arena.error), "table cell text too large");
        return 0;
    }
    cell->bytes += text.n;
    if (!cell->parts) {
        cell->text = text;
        cell->parts = 1;
        return 1;
    }
    if (cell->parts == 1) {
        index = flow->parts.n;
        part = ds_push(&d->arena, &flow->parts, sizeof(Part));
        if (!part)
            return 0;
        *part = (Part){cell->text, SIZE_MAX};
        cell->head = cell->tail = index;
    }
    index = flow->parts.n;
    part = ds_push(&d->arena, &flow->parts, sizeof(Part));
    if (!part)
        return 0;
    *part = (Part){text, SIZE_MAX};
    ((Part *)flow->parts.p)[cell->tail].next = index;
    cell->tail = index;
    cell->parts++;
    return 1;
}

static int table_run(Flow *flow, const Run *run) {
    Document *d = flow->document;
    FlowCell *cells = flow->cells.p, *cell = NULL;
    size_t i;
    int new_coordinate = !flow->has_coordinate || run->row > flow->last_row ||
                         (run->row == flow->last_row && run->col > flow->last_col);
    if (run->row > SIZE_MAX - run->rowspan || run->col > SIZE_MAX - run->colspan) {
        snprintf(d->arena.error, sizeof(d->arena.error), "table dimensions overflow");
        return 0;
    }
    if (run->row + run->rowspan > flow->max_row)
        flow->max_row = run->row + run->rowspan;
    if (run->col + run->colspan > flow->max_col)
        flow->max_col = run->col + run->colspan;
    flow->last_row = run->row;
    flow->last_col = run->col;
    flow->has_coordinate = 1;
    /* The normalizer allocates a new cell only at an unoccupied coordinate.
     * An empty 1x1 cell therefore contributes bounds but cannot overwrite a
     * previous rowspan. Larger empty spans still need explicit placement. */
    if (new_coordinate && run->in_table && !run->text.n && !run->src.p && run->rowspan == 1 &&
        run->colspan == 1)
        return 1;
    if (flow->cells.n) {
        cell = cells + flow->cells.n - 1;
        if (cell->row != run->row || cell->col != run->col) {
            if (run->row < cell->row || (run->row == cell->row && run->col < cell->col)) {
                for (i = 0; i < flow->cells.n; i++)
                    if (cells[i].row == run->row && cells[i].col == run->col)
                        break;
                cell = i < flow->cells.n ? cells + i : NULL;
            } else
                cell = NULL;
        }
    }
    if (!cell) {
        if (flow->cells.n && (run->row < cells[flow->cells.n - 1].row ||
            (run->row == cells[flow->cells.n - 1].row && run->col < cells[flow->cells.n - 1].col))) flow->unordered = 1;
        cell = ds_push(&d->arena, &flow->cells, sizeof(FlowCell));
        if (!cell)
            return 0;
        *cell = (FlowCell){0};
        cell->tag = run->tag;
        cell->block = run->block;
        cell->first_line = run->line;
        cell->in_table = run->in_table;
        cell->row = run->row;
        cell->col = run->col;
        cell->rs = run->rowspan;
        cell->cs = run->colspan;
    }
    cell->flags |= run->flags;
    if (run->has_font) {
        cell->has_font = 1;
        cell->font = run->font;
    }
    if (run->has_indent) {
        cell->has_indent = 1;
        cell->indent = run->indent;
    }
    if (run->href.p)
        cell->href = run->href;
    if (!ds_keep_href(d, &cell->hrefs, run->href))
        return 0;
    if (cell->records && cell->line != run->line && !cell_part(flow, cell, ds_str("\n")))
        return 0;
    cell->line = run->line;
    cell->records++;
    if (run->src.p) {
        cell->image_src = run->src;
        Str parts[] = {ds_str("[ALT: "), run->text, ds_str(". SRC: "), run->src, ds_str("]")};
        return cell_part(flow, cell, ds_join(&d->arena, parts, 5, ds_str("")));
    }
    return cell_part(flow, cell, run->text);
}

static Str cell_text(Flow *flow, FlowCell *cell) {
    char *text;
    size_t index, position = 0;
    Part *parts = flow->parts.p;
    if (!cell->parts)
        return ds_str("");
    if (cell->parts == 1)
        return ds_strip(cell->text, 1);
    text = ds_alloc_uninit(&flow->document->arena, cell->bytes + 1);
    if (!text)
        return (Str){0};
    for (index = cell->head; index != SIZE_MAX; index = parts[index].next) {
        memcpy(text + position, parts[index].text.p, parts[index].text.n);
        position += parts[index].text.n;
    }
    text[position] = 0;
    return ds_strip((Str){text, position}, 1);
}

static int compare_cell(const void *a, const void *b) {
    const FlowCell *x = a, *y = b;
    if (x->row != y->row) return x->row < y->row ? -1 : 1;
    return (x->col > y->col) - (x->col < y->col);
}
static Run cell_run(const FlowCell *cell) {
    Run run = {0};
    run.block = cell->block; run.line = cell->first_line; run.tag = cell->tag;
    run.in_table = cell->in_table;
    run.row = cell->row; run.col = cell->col;
    run.rowspan = cell->rs; run.colspan = cell->cs;
    run.text = cell->text; run.href = cell->href; run.flags = cell->flags;
    run.font = cell->font; run.indent = cell->indent;
    run.has_font = cell->has_font; run.has_indent = cell->has_indent;
    return run;
}
static int table_finish(Flow *flow, Line *line) {
    Document *d = flow->document;
    FlowCell *cells = flow->cells.p, **grid;
    size_t i, j, y, x, nr = flow->max_row, nc = flow->max_col, meaningful = 0, last = 0;
    if (!flow->cells.n)
        return 1;
    if (flow->unordered) qsort(cells, flow->cells.n, sizeof(FlowCell), compare_cell);
    for (i = 0; i < flow->cells.n; i++) {
        FlowCell *cell = cells + i;
        cell->text = cell_text(flow, cell);
        if (!cell->text.p)
            return 0;
        if (cell->row > SIZE_MAX - cell->rs || cell->col > SIZE_MAX - cell->cs)
            goto large;
        if (cell->row + cell->rs > nr)
            nr = cell->row + cell->rs;
        if (cell->col + cell->cs > nc)
            nc = cell->col + cell->cs;
    }
    if (nc && nr > SIZE_MAX / nc / sizeof(Str))
        goto large;
    /* This grid is scratch storage. Output items and matrices own their arrays. */
    size_t grid_size = nr * nc;
    if (!flow->grid || grid_size > flow->grid_cap) {
        grid = ds_alloc_uninit(&d->arena, grid_size * sizeof(*grid));
        if (!grid)
            return 0;
        flow->grid = grid;
        flow->grid_cap = grid_size;
    }
    grid = flow->grid;
    memset(grid, 0, grid_size * sizeof(*grid));
    for (i = 0; i < flow->cells.n; i++) {
        FlowCell *cell = cells + i;
        for (y = 0; y < cell->rs; y++)
            for (x = 0; x < cell->cs; x++)
                grid[(cell->row + y) * nc + cell->col + x] = cell;
    }
    for (i = 0; i < flow->cells.n; i++)
        if (ds_strip(cells[i].text, 1).n)
            break;
    if (i < flow->cells.n) {
        size_t length;
        Str text = ds_strip(cells[i].text, 1);
        if (d->retain_table_cells || (ds_list_marker(text, &length) && length == text.n)) {
            Item *items = ds_alloc(&d->arena, flow->cells.n * sizeof(Item));
            if (!items)
                return 0;
            for (i = 0; i < flow->cells.n; i++) {
                FlowCell *cell = cells + i;
                Run *run = &items[i].run;
                *run = cell_run(cell);
                run->src = cell->image_src;
                items[i].hrefs = cell->hrefs;
            }
            if (!ds_list_table(d, line, items, flow->cells.n))
                return 0;
            if (d->retain_table_cells) {
                line->table_cells = items;
                line->ntable_cells = flow->cells.n;
            }
        }
    }
    for (i = 0; i < nr; i++) {
        for (j = 0; j < nc; j++)
            if (grid[i * nc + j] && grid[i * nc + j]->text.n)
                break;
        if (j < nc) {
            meaningful++;
            last = i;
        }
    }
    if (meaningful == 1) {
        size_t count = 0;
        for (j = 0; j < nc; j++)
            if (grid[last * nc + j] && grid[last * nc + j]->text.n)
                count++;
        line->items = ds_alloc(&d->arena, count * sizeof(Item));
        if (!line->items)
            return 0;
        line->nitems = count;
        for (j = 0, i = 0; j < nc; j++) {
            FlowCell *cell = grid[last * nc + j];
            Item *item;
            if (!cell || !cell->text.n)
                continue;
            item = line->items + i++;
            item->run = cell_run(cell);
            item->fake_table = 1;
            item->hrefs = cell->hrefs;
        }
    } else if (meaningful) {
        Str *texts = ds_alloc_uninit(&d->arena, nr * nc * sizeof(Str));
        if (!texts)
            return 0;
        for (i = 0; i < nr * nc; i++)
            texts[i] = grid[i] ? grid[i]->text : ds_str("");
        line->matrix = (Matrix){texts, nr, nc};
    }
    return !d->arena.error[0];
large:
    snprintf(d->arena.error, sizeof(d->arena.error), "table dimensions overflow");
    return 0;
}

static int finish_block(Flow *flow) {
    Document *d = flow->document;
    Line line = {0}, *out;
    FlowItem *items = flow->items.p;
    size_t i;
    if (!flow->active)
        return 1;
    if (flow->table) {
        if (!table_finish(flow, &line))
            return 0;
    } else {
        if (flow->items.n == 1 && !image(&items[0].item))
            items[0].item.run.text = ds_strip(items[0].item.run.text, 1);
        for (i = 0; i < flow->items.n; i++)
            if (!empty(&items[i].item))
                break;
        if (i < flow->items.n) {
            line.items = ds_alloc_uninit(&d->arena, flow->items.n * sizeof(Item));
            if (!line.items)
                return 0;
            for (i = 0; i < flow->items.n; i++)
                line.items[i] = items[i].item;
            line.nitems = flow->items.n;
        }
    }
    if (line.nitems || line.matrix.nr) {
        out = ds_push(&d->arena, &d->lines, sizeof(Line));
        if (!out)
            return 0;
        *out = line;
    }
    flow->items.n = flow->cells.n = flow->parts.n = 0;
    flow->max_row = flow->max_col = 0;
    flow->has_coordinate = flow->unordered = 0;
    flow->active = 0;
    return !d->arena.error[0];
}

int ds_flow_begin(Document *d) {
    Flow *flow = ds_alloc(&d->arena, sizeof(Flow));
    if (!flow)
        return 0;
    flow->document = d;
    d->flow = flow;
    return 1;
}
int ds_flow_run(Document *d, const Run *run) {
    Flow *flow = d->flow;
    if (flow->active && flow->block != run->block && !finish_block(flow))
        return 0;
    if (!flow->active) {
        flow->active = 1;
        flow->block = run->block;
        flow->table = run->in_table;
    }
    return flow->table ? table_run(flow, run) : plain_run(flow, run);
}
int ds_flow_finish(Document *d) { return finish_block(d->flow); }
