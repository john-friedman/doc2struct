#include "tags.h"
#include "doc2struct.h"
#include <string.h>
#include <ctype.h>
#include "scan.h"
/* Locate the next tag and remember entities in the preceding text. The flag
 * accumulates across literal '<' characters until a real token is emitted. */
static size_t text_end(const char *p, size_t n, int *entity) {
    size_t i = 0;
#ifdef DS_SSE2
    const __m128i lt = _mm_set1_epi8('<'), amp = _mm_set1_epi8('&');
    for (; n - i >= 16; i += 16) {
        __m128i data = _mm_loadu_si128((const __m128i *)(p + i));
        unsigned ends = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(data, lt));
        unsigned amps = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(data, amp));
        if (ends) {
            unsigned bit = ds_first_bit(ends);
            *entity |= (amps & ((1u << bit) - 1)) != 0;
            return i + bit;
        }
        *entity |= amps != 0;
    }
#elif defined(DS_NEON)
    const uint8x16_t lt = vdupq_n_u8('<'), amp = vdupq_n_u8('&');
    for (; n - i >= 16; i += 16) {
        uint8x16_t data = vld1q_u8((const uint8_t *)p + i);
        uint64_t ends = ds_neon_mask(vceqq_u8(data, lt));
        uint64_t amps = ds_neon_mask(vceqq_u8(data, amp));
        if (ends) {
            unsigned offset = ds_neon_first(ends);
            /* Entities after the first '<' belong to the next token. */
            uint64_t before = (UINT64_C(1) << (4 * offset)) - 1;
            *entity |= (amps & before) != 0;
            return i + offset;
        }
        *entity |= amps != 0;
    }
#endif
    for (; i < n && p[i] != '<'; i++)
        *entity |= p[i] == '&';
    return i;
}

/* Cache hits require complete equality. Overlapping loads cover short tags
 * without reading outside either slice; other CPUs use the library fallback. */
static int tag_equal(const char *a, const char *b, size_t n) {
#ifdef DS_SSE2
    if (n >= 16 && n <= 32) {
        __m128i first = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)a),
                                      _mm_loadu_si128((const __m128i *)b));
        __m128i last = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(a + n - 16)),
                                     _mm_loadu_si128((const __m128i *)(b + n - 16)));
        return _mm_movemask_epi8(_mm_and_si128(first, last)) == 65535;
    }
    if (n > 32 && n <= 64) {
        __m128i x0 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)a),
                                   _mm_loadu_si128((const __m128i *)b));
        __m128i x1 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(a + 16)),
                                   _mm_loadu_si128((const __m128i *)(b + 16)));
        __m128i x2 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(a + n - 32)),
                                   _mm_loadu_si128((const __m128i *)(b + n - 32)));
        __m128i x3 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(a + n - 16)),
                                   _mm_loadu_si128((const __m128i *)(b + n - 16)));
        return _mm_movemask_epi8(
                   _mm_and_si128(_mm_and_si128(x0, x1), _mm_and_si128(x2, x3))) == 65535;
    }
    if (n >= 8 && n < 16) {
        uint64_t a0, b0, a1, b1;
        memcpy(&a0, a, 8);
        memcpy(&b0, b, 8);
        memcpy(&a1, a + n - 8, 8);
        memcpy(&b1, b + n - 8, 8);
        return ((a0 ^ b0) | (a1 ^ b1)) == 0;
    }
#endif
    return ds_bytes_equal(a, b, n);
}

static int space_at(Str s, size_t i, size_t *k) {
    unsigned char c = (unsigned char)s.p[i];
    if (c < 128) {
        *k = 1;
        return c == ' ' || (c >= 9 && c <= 13) || (c >= 28 && c <= 31);
    }
    return ds_space(ds_utf8(s.p + i, s.n - i, k));
}
static Str intern_tag(Document *d, Str tag) {
    Str lower, *cached;
    if (ds_upper_or_nonascii(tag.p, tag.n) == tag.n)
        return tag;
    cached = ds_map_get(&d->tags, tag);
    if (cached)
        return *cached;
    lower = ds_lower(&d->arena, tag);
    if (!lower.p)
        return (Str){0};
    cached = ds_alloc(&d->arena, sizeof(Str));
    if (!cached)
        return (Str){0};
    *cached = lower;
    if (!ds_map_put(&d->arena, &d->tags, tag, cached))
        return (Str){0};
    return lower;
}
int ds_void(Str t) {
    static const char *names[] = {"area", "base", "br",    "col",    "embed", "hr",  "img", "input",
                                  "link", "meta", "param", "source", "track", "wbr", NULL};
    size_t i;
    for (i = 0; names[i]; i++)
        if (ds_is(t, names[i]))
            return 1;
    return 0;
}
static int ascii_equal(Str s, const char *name) {
    size_t i;
    for (i = 0; i < s.n; i++)
        if (((unsigned char)s.p[i] | 32) != (unsigned char)name[i])
            return 0;
    return 1;
}
static int wanted(Str s) {
    switch (s.n) {
    case 3:
        if (ascii_equal(s, "src"))
            return A_SRC;
        if (ascii_equal(s, "alt"))
            return A_ALT;
        break;
    case 4:
        if (ascii_equal(s, "href"))
            return A_HREF;
        break;
    case 5:
        if (ascii_equal(s, "style"))
            return A_STYLE;
        break;
    case 6:
        if (ascii_equal(s, "hidden"))
            return A_HIDDEN;
        break;
    case 7:
        if (ascii_equal(s, "colspan"))
            return A_COLSPAN;
        if (ascii_equal(s, "rowspan"))
            return A_ROWSPAN;
        break;
    }
    return -1;
}
Token *ds_new_token(Document *d) {
    TokenBuffer *buffer = &d->tokens;
    if (!buffer->last || buffer->last->used == DS_TOKEN_CHUNK_SIZE) {
        TokenChunk *chunk = ds_alloc_uninit(&d->arena, sizeof(TokenChunk));
        if (!chunk)
            return NULL;
        chunk->next = NULL;
        chunk->used = 0;
        if (buffer->last)
            buffer->last->next = chunk;
        else
            buffer->first = chunk;
        buffer->last = chunk;
    }
    buffer->n++;
    return buffer->last->data + buffer->last->used++;
}

int ds_store_token(Document *d, int kind, Str value, const Attrs *attrs) {
    Token *token = ds_new_token(d);
    size_t i;
    if (!token)
        return 0;
    token->kind = kind;
    token->value = value;
    token->attrs = NULL;
    if (attrs) {
        for (i = 0; i < A_COUNT; i++)
            if (attrs->value[i].p)
                break;
        if (i < A_COUNT) {
            token->attrs = ds_alloc_uninit(&d->arena, sizeof(Attrs));
            if (!token->attrs)
                return 0;
            *token->attrs = *attrs;
        }
    }
    return 1;
}
static int attrs(Document *d, Str rest, Attrs *out) {
    size_t i = 0, k, start, end;
    memset(out, 0, sizeof(*out));
    while (i < rest.n) {
        Str name, value;
        int index;
        while (i < rest.n) {
            unsigned char c = (unsigned char)rest.p[i];
            if (!space_at(rest, i, &k) && !strchr("=/>\"'", c))
                break;
            i += k;
        }
        start = i;
        while (i < rest.n) {
            unsigned char c = (unsigned char)rest.p[i];
            if (space_at(rest, i, &k) || strchr("=/>\"'", c))
                break;
            i += k;
        }
        if (i == start)
            break;
        name = (Str){rest.p + start, i - start};
        index = wanted(name);
        while (i < rest.n && space_at(rest, i, &k))
            i += k;
        value = ds_str("");
        if (i < rest.n && rest.p[i] == '=') {
            i++;
            while (i < rest.n && space_at(rest, i, &k))
                i += k;
            start = i;
            if (i < rest.n && (rest.p[i] == '\'' || rest.p[i] == '\"')) {
                char quote = rest.p[i++];
                start = i;
                {
                    const char *close = memchr(rest.p + i, quote, rest.n - i);
                    i = close ? (size_t)(close - rest.p) : rest.n;
                }
                end = i;
                if (i < rest.n)
                    i++;
                else
                    end = end > start ? end - 1 : end;
            } else {
                while (i < rest.n && rest.p[i] != '>' && !space_at(rest, i, &k))
                    i += k;
                end = i;
            }
            value = (Str){rest.p + start, end - start};
        }
        if (index >= 0)
            out->value[index] = ds_unescape(&d->arena, value);
        if (d->arena.error[0])
            return 0;
    }
    return 1;
}

/* Parse normal start tags once. Unusual quote placement uses the general path
 * below so malformed input keeps the same tokenization behavior. Attribute
 * values remain input slices unless entity expansion requires a new buffer. */
static int start_attrs(Document *d, Str html, size_t i, size_t *end, Attrs *out) {
    size_t k, start, name_end, q;
    unsigned decode = 0;
    memset(out, 0, sizeof(*out));
    while (i < html.n) {
        unsigned char c = (unsigned char)html.p[i];
        Str value = {"", 0};
        int index, has_entity = 0;
        if (c == '>') {
            *end = i;
            for (q = 0; q < A_COUNT; q++)
                if (decode & (1u << q))
                    out->value[q] = ds_unescape(&d->arena, out->value[q]);
            return d->arena.error[0] ? -2 : 1;
        }
        if (space_at(html, i, &k)) {
            i += k;
            continue;
        }
        if (c == '/') {
            i++;
            continue;
        }
        if (c == '=' || c == '"' || c == '\'' || !c)
            return -1;
        /* Inline styles dominate filing markup. Recognize this common spelling
         * without a character-at-a-time name scan; value handling is shared. */
        if (html.n - i >= 6 && !memcmp(html.p + i, "style=", 6)) {
            index = A_STYLE;
            i += 6;
            goto read_value;
        }
        start = i;
        while (i < html.n) {
            c = (unsigned char)html.p[i];
            if (space_at(html, i, &k) || c == '=' || c == '/' || c == '>' || c == '"' ||
                c == '\'' || !c)
                break;
            i += k;
        }
        name_end = i;
        index = wanted((Str){html.p + start, name_end - start});
        while (i < html.n && space_at(html, i, &k))
            i += k;
        if (i < html.n && html.p[i] == '=') {
            i++;
        read_value:
            while (i < html.n && space_at(html, i, &k))
                i += k;
            start = i;
            if (i < html.n && (html.p[i] == '"' || html.p[i] == '\'')) {
                const char *close;
                char quote = html.p[i++];
                start = i;
                if (index >= 0) {
                    i += ds_quote_or_amp(html.p + i, html.n - i, quote);
                    if (i == html.n)
                        return 0;
                    has_entity = html.p[i] == '&';
                }
                close =
                    has_entity || index < 0 ? memchr(html.p + i, quote, html.n - i) : html.p + i;
                if (!close)
                    return 0;
                i = (size_t)(close - html.p);
                value = (Str){html.p + start, i - start};
                i++;
            } else {
                while (i < html.n && html.p[i] != '>' && !space_at(html, i, &k)) {
                    if (html.p[i] == '"' || html.p[i] == '\'')
                        return -1;
                    has_entity |= html.p[i] == '&';
                    i += k;
                }
                value = (Str){html.p + start, i - start};
            }
        }
        if (index >= 0) {
            out->value[index] = value;
            decode = (decode & ~(1u << index)) | (has_entity ? 1u << index : 0);
        }
    }
    return 0;
}

/* The parsing path can reject suppressed text before entity expansion. The
 * standalone token API continues to expose every token and decoded entity. */
static int send_text(Document *d, TokenSink sink, void *ctx, Str text, int entity) {
    if (d->flow) {
        if (d->skip_text)
            return 1;
        if (d->skip_blank) {
            if (ds_blank_text(text.p, text.n))
                return 1;
            if (text.n == 6 && !memcmp(text.p, "&#160;", 6))
                return 1;
        }
    }
    text = entity ? ds_unescape(&d->arena, text) : text;
    if (d->flow && sink == ds_normalize_token) return ds_html_text(ctx, text);
    return sink(ctx, TEXT, text, NULL);
}

enum { TEMPLATE_SLOTS = 4096, TEMPLATE_PREFIX = 512, TEMPLATE_MAX = 4096 };
typedef struct {
    Str raw, tag;
    Attrs attrs;
    int selfclosed;
    unsigned props;
} TagTemplate;

static size_t template_index(const char *text) {
    uint64_t first = 0, middle = 0, last = 0, hash;
    size_t n = TEMPLATE_PREFIX;
    const char *end = NULL;
#ifdef DS_SSE2
    const __m128i mark = _mm_set1_epi8('>');
    for (size_t offset = 0; offset < TEMPLATE_PREFIX; offset += 16) {
        unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(
            _mm_loadu_si128((const __m128i *)(text + offset)), mark));
        if (mask) { end = text + offset + ds_first_bit(mask); break; }
    }
#else
    end = memchr(text, '>', n);
#endif
    /* This is only a fingerprint. A '>' inside a quoted value is harmless:
     * every hit must still match the complete original tag, including quotes. */
    if (end)
        n = (size_t)(end - text) + 1;
    /* Sample bounded words rather than hashing every byte on every cache hit.
     * Differences outside these words can collide; full equality below is
     * still required before a template is reused. */
    if (n >= 8) {
        memcpy(&first, text, 8);
        memcpy(&last, text + n - 8, 8);
        memcpy(&middle, text + (n - 8) / 2, 8);
    } else {
        size_t i;
        for (i = 0; i < n; i++)
            first |= (uint64_t)(unsigned char)text[i] << (8 * i);
    }
    hash = (first ^ n) * 0x9e3779b185ebca87ULL;
    hash = (hash ^ last) * 0xc2b2ae3d27d4eb4fULL;
    hash = (hash ^ middle) * 0x9e3779b185ebca87ULL;
    hash ^= hash >> 32;
    return (size_t)hash & (TEMPLATE_SLOTS - 1);
}

static int send_html_text(Document *d, void *ctx, Str text, int entity) {
    if (d->flow) {
        if (d->skip_text)
            return 1;
        if (d->skip_blank) {
            if (ds_blank_text(text.p, text.n))
                return 1;
            if (text.n == 6 && !memcmp(text.p, "&#160;", 6))
                return 1;
        }
    }
    text = entity ? ds_unescape(&d->arena, text) : text;
    return ds_html_text(ctx, text);
}

#define DS_TOKENIZER_STORAGE static
#define DS_TOKENIZER_NAME ds_html_tokenize
#define DS_TOKENIZER_DIRECT 1
#define DS_TOKENIZER_SEND_TEXT(d, sink, ctx, ...) send_html_text(d, ctx, __VA_ARGS__)
#include "tokenizer_loop.h"
#undef DS_TOKENIZER_STORAGE
#undef DS_TOKENIZER_NAME
#undef DS_TOKENIZER_DIRECT
#undef DS_TOKENIZER_SEND_TEXT

#define DS_TOKENIZER_STORAGE
#define DS_TOKENIZER_NAME ds_tokenize
#define DS_TOKENIZER_DIRECT (d->flow && sink == ds_normalize_token)
#define DS_TOKENIZER_SEND_TEXT(d, sink, ctx, ...) send_text(d, sink, ctx, __VA_ARGS__)
#include "tokenizer_loop.h"
#undef DS_TOKENIZER_STORAGE
#undef DS_TOKENIZER_NAME
#undef DS_TOKENIZER_DIRECT
#undef DS_TOKENIZER_SEND_TEXT
