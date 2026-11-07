#include "doc2struct.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

typedef union {
    long double number;
    void *pointer;
    uint64_t integer;
} Alignment;
struct Block {
    Block *next;
    size_t used, cap;
    Alignment alignment;
    unsigned char data[];
};
static Block *pool_take(ArenaPool *pool, size_t capacity) {
    Block **link, **best = NULL, *block;
    if (!pool || !pool->blocks)
        return NULL;
    for (link = &pool->blocks; *link; link = &(*link)->next)
        if ((*link)->cap >= capacity && (!best || (*link)->cap < (*best)->cap)) {
            best = link;
            if ((*link)->cap == capacity)
                break;
        }
    if (!best)
        return NULL;
    block = *best;
    *best = block->next;
    pool->bytes -= sizeof(Block) + block->cap;
    return block;
}
void *ds_alloc_uninit(Arena *a, size_t n) {
    Block *b = a->blocks;
    size_t align = sizeof(Alignment), cap;
    if (a->error[0])
        return NULL;
    if (n > SIZE_MAX - align - sizeof(Block))
        goto oom;
    n = (n + align - 1) / align * align;
    if (!b || n > b->cap - b->used) {
        cap = n > 65536 ? n : 65536;
        b = pool_take(a->pool, cap);
        if (b)
            cap = b->cap;
        else
            b = malloc(sizeof(Block) + cap);
        if (!b)
            goto oom;
        b->next = a->blocks;
        b->used = 0;
        b->cap = cap;
        a->blocks = b;
    }
    {
        void *p = b->data + b->used;
        b->used += n;
        return p;
    }
oom:
    snprintf(a->error, sizeof(a->error), "out of memory");
    return NULL;
}
void *ds_alloc(Arena *a, size_t n) {
    void *p = ds_alloc_uninit(a, n);
    if (p)
        memset(p, 0, n);
    return p;
}
void ds_arena_free(Arena *a) {
    Block *b = a->blocks;
    while (b) {
        Block *next = b->next;
        size_t bytes = sizeof(Block) + b->cap;
        if (a->pool && bytes <= a->pool->limit && a->pool->bytes <= a->pool->limit - bytes) {
            b->next = a->pool->blocks;
            a->pool->blocks = b;
            a->pool->bytes += bytes;
        } else
            free(b);
        b = next;
    }
    a->blocks = NULL;
}
void ds_pool_clear(ArenaPool *pool) {
    Block *block = pool->blocks;
    while (block) {
        Block *next = block->next;
        free(block);
        block = next;
    }
    pool->blocks = NULL;
    pool->bytes = 0;
}
void *ds_push(Arena *a, Vec *v, size_t width) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 16;
        void *p;
        if (cap < v->cap || cap > SIZE_MAX / width) {
            snprintf(a->error, sizeof(a->error), "array too large");
            return NULL;
        }
        p = ds_alloc_uninit(a, cap * width);
        if (!p)
            return NULL;
        if (v->n)
            memcpy(p, v->p, v->n * width);
        memset((char *)p + v->n * width, 0, (cap - v->n) * width);
        v->p = p;
        v->cap = cap;
    }
    return (char *)v->p + v->n++ * width;
}
static uint64_t hash(Str s) {
    uint64_t h = 0x9e3779b97f4a7c15ULL ^ (uint64_t)s.n, word;
    size_t i = 0;
    /* Compare complete keys after hashing; process unaligned words without
     * imposing alignment or aliasing requirements on input slices. */
    for (; s.n - i >= 8; i += 8) {
        memcpy(&word, s.p + i, 8);
        h = (h ^ word) * 0x9e3779b185ebca87ULL;
        h ^= h >> 29;
    }
    word = 0;
    if (i < s.n)
        memcpy(&word, s.p + i, s.n - i);
    h = (h ^ word) * 0xc2b2ae3d27d4eb4fULL;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h ? h : 1;
}
void *ds_map_get(Map *m, Str k) {
    size_t i;
    uint64_t h;
    if (!m->cap)
        return NULL;
    h = hash(k);
    i = (size_t)h & (m->cap - 1);
    while (m->entries[i].hash) {
        if (m->entries[i].hash == h && ds_eq(m->entries[i].key, k))
            return m->entries[i].value;
        i = (i + 1) & (m->cap - 1);
    }
    return NULL;
}
int ds_map_put(Arena *a, Map *m, Str k, void *value) {
    size_t i;
    uint64_t h = hash(k);
    if (!m->cap || (m->n + 1) * 10 > m->cap * 7) {
        size_t cap = m->cap ? m->cap * 2 : 32, j;
        Entry *entries;
        if (cap < m->cap || cap > SIZE_MAX / sizeof(Entry)) {
            snprintf(a->error, sizeof(a->error), "map too large");
            return 0;
        }
        entries = ds_alloc(a, cap * sizeof(Entry));
        if (!entries)
            return 0;
        for (j = 0; j < m->cap; j++)
            if (m->entries[j].hash) {
                i = (size_t)m->entries[j].hash & (cap - 1);
                while (entries[i].hash)
                    i = (i + 1) & (cap - 1);
                entries[i] = m->entries[j];
            }
        m->entries = entries;
        m->cap = cap;
    }
    i = (size_t)h & (m->cap - 1);
    while (m->entries[i].hash && !ds_eq(m->entries[i].key, k))
        i = (i + 1) & (m->cap - 1);
    if (!m->entries[i].hash)
        m->n++;
    m->entries[i] = (Entry){k, value, h};
    return 1;
}
Str ds_copy(Arena *a, Str s) {
    char *p = ds_alloc_uninit(a, s.n + 1);
    if (!p)
        return (Str){0};
    if (s.n)
        memcpy(p, s.p, s.n);
    p[s.n] = 0;
    return (Str){p, s.n};
}
Str ds_cat(Arena *a, Str x, Str y) {
    char *p;
    if (!x.n)
        return y.p ? y : ds_str("");
    if (!y.n)
        return x;
    if (x.n > SIZE_MAX - y.n - 1) {
        snprintf(a->error, sizeof(a->error), "text too large");
        return (Str){0};
    }
    p = ds_alloc_uninit(a, x.n + y.n + 1);
    if (!p)
        return (Str){0};
    if (x.n)
        memcpy(p, x.p, x.n);
    if (y.n)
        memcpy(p + x.n, y.p, y.n);
    p[x.n + y.n] = 0;
    return (Str){p, x.n + y.n};
}
Str ds_join(Arena *a, Str *parts, size_t n, Str sep) {
    size_t i, total = 0, pos = 0;
    char *p;
    if (!n)
        return ds_str("");
    if (n == 1)
        return parts[0].p ? parts[0] : ds_str("");
    for (i = 0; i < n; i++) {
        if (parts[i].n > SIZE_MAX - total - sep.n - 1) {
            snprintf(a->error, sizeof(a->error), "text too large");
            return (Str){0};
        }
        total += parts[i].n + (i ? sep.n : 0);
    }
    p = ds_alloc_uninit(a, total + 1);
    if (!p)
        return (Str){0};
    for (i = 0; i < n; i++) {
        if (i && sep.n) {
            memcpy(p + pos, sep.p, sep.n);
            pos += sep.n;
        }
        if (parts[i].n) {
            memcpy(p + pos, parts[i].p, parts[i].n);
            pos += parts[i].n;
        }
    }
    p[total] = 0;
    return (Str){p, total};
}
int ds_keep_href(Document *d, Vec **values, Str href) {
    Str *slot;
    if (!d->collect_hrefs || !href.p)
        return 1;
    if (!*values) {
        *values = ds_alloc(&d->arena, sizeof(Vec));
        if (!*values)
            return 0;
    }
    slot = ds_push(&d->arena, *values, sizeof(Str));
    if (!slot)
        return 0;
    *slot = href;
    return 1;
}
int ds_append_hrefs(Document *d, Vec **target, const Vec *source) {
    size_t i;
    if (!d->collect_hrefs || !source || *target == source)
        return 1;
    for (i = 0; i < source->n; i++)
        if (!ds_keep_href(d, target, ((Str *)source->p)[i]))
            return 0;
    return 1;
}
void ds_document_free(Document *d) {
    if (d->match)
        pcre2_match_data_free(d->match);
    ds_arena_free(&d->arena);
    memset(d, 0, sizeof(*d));
}
void ds_rules_free(Rules *r) {
    Regex *x = r->regexes;
    while (x) {
        pcre2_code_free(x->code);
        x = x->next;
    }
    ds_arena_free(&r->arena);
}
