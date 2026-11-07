#include "doc2struct.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "unicode_data.h"
#include "scan.h"

uint32_t ds_utf8(const char *p, size_t n, size_t *step) {
    const unsigned char *s = (const unsigned char *)p;
    uint32_t c;
    size_t k, i;
    if (!n) {
        *step = 0;
        return 0;
    }
    c = s[0];
    if (c < 128) {
        *step = 1;
        return c;
    }
    k = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!k || k > n) {
        *step = 1;
        return 0xfffd;
    }
    c &= (1u << (7 - k)) - 1;
    for (i = 1; i < k; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            *step = 1;
            return 0xfffd;
        }
        c = (c << 6) | (s[i] & 63);
    }
    if ((k == 3 && c < 0x800) || (k == 4 && c < 0x10000) || c > 0x10ffff ||
        (c >= 0xd800 && c <= 0xdfff)) {
        *step = 1;
        return 0xfffd;
    }
    *step = k;
    return c;
}
static size_t encode(char *p, uint32_t c) {
    if (c < 0x80) {
        p[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        p[0] = (char)(0xc0 | (c >> 6));
        p[1] = (char)(0x80 | (c & 63));
        return 2;
    }
    if (c < 0x10000) {
        p[0] = (char)(0xe0 | (c >> 12));
        p[1] = (char)(0x80 | ((c >> 6) & 63));
        p[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    p[0] = (char)(0xf0 | (c >> 18));
    p[1] = (char)(0x80 | ((c >> 12) & 63));
    p[2] = (char)(0x80 | ((c >> 6) & 63));
    p[3] = (char)(0x80 | (c & 63));
    return 4;
}
static const UnicodeEntry *entry(uint32_t cp) {
    size_t l = 0, r = sizeof(unicode_entries) / sizeof(*unicode_entries);
    while (l < r) {
        size_t m = l + (r - l) / 2;
        if (unicode_entries[m].cp < cp)
            l = m + 1;
        else
            r = m;
    }
    return l < sizeof(unicode_entries) / sizeof(*unicode_entries) && unicode_entries[l].cp == cp
               ? unicode_entries + l
               : NULL;
}
static unsigned uflags(uint32_t cp) {
    const UnicodeEntry *e;
    if (cp < 128)
        return (cp == ' ' || (cp >= 9 && cp <= 13) || (cp >= 28 && cp <= 31) ? 1 : 0) |
               (cp >= 'A' && cp <= 'Z' ? 2 : 0) | (cp >= 'a' && cp <= 'z' ? 4 : 0);
    e = entry(cp);
    return e ? e->flags : 0;
}
int ds_space(uint32_t cp) { return (uflags(cp) & 1) != 0; }
static void phone_spaces(Str text, size_t *i) {
    size_t k;
    while (*i < text.n && ds_space(ds_utf8(text.p + *i, text.n - *i, &k)))
        *i += k;
}
int ds_phone(Str text) {
    size_t i = 0, start, area, group, last, k;
    uint32_t separator;
    phone_spaces(text, &i);
    if (i == text.n || text.p[i++] != '(')
        return 0;
    if (i < text.n && text.p[i] == '+')
        i++;
    start = i;
    while (i < text.n && text.p[i] >= '0' && text.p[i] <= '9')
        i++;
    area = i - start;
    if (area < 2 || area > 4 || i == text.n || text.p[i++] != ')')
        return 0;
    phone_spaces(text, &i);
    start = i;
    while (i < text.n && text.p[i] >= '0' && text.p[i] <= '9')
        i++;
    group = i - start;
    if (group < 2 || group > 4 || i == text.n)
        return 0;
    separator = ds_utf8(text.p + i, text.n - i, &k);
    if (separator != '-' && separator != '.' && separator != 0x2011 &&
        separator != 0x2013 && !ds_space(separator))
        return 0;
    i += k;
    phone_spaces(text, &i);
    start = i;
    while (i < text.n && text.p[i] >= '0' && text.p[i] <= '9')
        i++;
    last = i - start;
    return last >= 3 && last <= 4 && area + group + last >= 9;
}
static int empty_char(uint32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xa0 || c == 0x200b;
}
Str ds_strip(Str s, int custom) {
    size_t i = 0, j = 0, k;
    uint32_t c;
    while (i < s.n) {
        unsigned char byte = (unsigned char)s.p[i];
        if (byte < 128) {
            int space =
                custom ? (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r')
                       : (byte == ' ' || (byte >= 9 && byte <= 13) || (byte >= 28 && byte <= 31));
            if (!space)
                break;
            i++;
            continue;
        }
        c = ds_utf8(s.p + i, s.n - i, &k);
        if (!(custom ? empty_char(c) : ds_space(c)))
            break;
        i += k;
    }
    j = s.n;
    while (j > i) {
        unsigned char byte = (unsigned char)s.p[j - 1];
        if (byte < 128) {
            int space =
                custom ? (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r')
                       : (byte == ' ' || (byte >= 9 && byte <= 13) || (byte >= 28 && byte <= 31));
            if (!space)
                break;
            j--;
            continue;
        }
        size_t start = j - 1;
        while (start > i && ((unsigned char)s.p[start] & 0xc0) == 0x80)
            start--;
        c = ds_utf8(s.p + start, j - start, &k);
        if (!(custom ? empty_char(c) : ds_space(c)))
            break;
        j = start;
    }
    return (Str){s.p ? s.p + i : NULL, j - i};
}
Str ds_input(Arena *a, Str s) {
    size_t prefix = ds_nonascii(s.p, s.n);
    size_t i = prefix, k, j;
    uint32_t cp;
    char *p;
    static const uint32_t cp1252[] = {
        0x20ac, 0xfffd, 0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021, 0x2c6,  0x2030, 0x160,
        0x2039, 0x152,  0xfffd, 0x17d,  0xfffd, 0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
        0x2013, 0x2014, 0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0xfffd, 0x17e,  0x178};
    while (i < s.n) {
        cp = ds_utf8(s.p + i, s.n - i, &k);
        if (cp == 0xfffd && k == 1)
            break;
        i += k;
        i += ds_nonascii(s.p + i, s.n - i);
    }
    if (i == s.n)
        return s;
    if (s.n > (SIZE_MAX - 1) / 3) {
        snprintf(a->error, sizeof(a->error), "input too large");
        return (Str){0};
    }
    p = ds_alloc_uninit(a, s.n * 3 + 1);
    if (!p)
        return (Str){0};
    /* ASCII is unchanged by either encoding. Convert everything after it,
     * including any valid UTF-8 sequences preceding the invalid byte. */
    if (prefix)
        memcpy(p, s.p, prefix);
    for (i = prefix, j = prefix; i < s.n; i++) {
        cp = (unsigned char)s.p[i];
        if (cp >= 128 && cp < 160)
            cp = cp1252[cp - 128];
        j += encode(p + j, cp);
    }
    p[j] = 0;
    return (Str){p, j};
}
int ds_upper(Str s) {
    size_t i = 0, k;
    int found = 0;
    while (i < s.n) {
        if (s.n - i >= 16) {
            AsciiMasks masks = ds_ascii_masks(s.p + i);
            if (masks.lower) return 0;
            found |= masks.upper != 0;
            if (!masks.nonascii) {
                i += 16;
                continue;
            }
            i += ds_mask_first(masks.nonascii);
        }
        unsigned f = uflags(ds_utf8(s.p + i, s.n - i, &k));
        if (f & 4)
            return 0;
        if (f & 2)
            found = 1;
        i += k;
    }
    return found;
}
Str ds_lower(Arena *a, Str s) {
    size_t i, k, j = 0, first;
    char *p;
    int preceding = 0, changed = 0;
    first = ds_upper_or_nonascii(s.p, s.n);
    if (first == s.n)
        return s;
    if (s.n > (SIZE_MAX - 1) / 3) {
        snprintf(a->error, sizeof(a->error), "text too large");
        return (Str){0};
    }
    if (ds_nonascii(s.p + first, s.n - first) == s.n - first) {
        p = ds_alloc_uninit(a, s.n + 1);
        if (!p)
            return (Str){0};
        ds_ascii_lower(p, s.p, s.n);
        p[s.n] = 0;
        return (Str){p, s.n};
    }
    p = ds_alloc_uninit(a, s.n * 3 + 1);
    if (!p)
        return (Str){0};
    for (i = 0; i < s.n; i += k) {
        if ((unsigned char)s.p[i] < 128) {
            size_t run = ds_nonascii(s.p + i, s.n - i);
            unsigned char last = (unsigned char)s.p[i + run - 1];
            changed |= ds_ascii_lower(p + j, s.p + i, run);
            preceding = (last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z');
            j += run;
            k = run;
            continue;
        }
        uint32_t cp = ds_utf8(s.p + i, s.n - i, &k);
        const UnicodeEntry *e = cp >= 128 ? entry(cp) : NULL;
        unsigned f = uflags(cp);
        const char *lower = e ? e->lower : NULL;
        if (cp == 0x3a3 && preceding) {
            size_t x = i + k, step;
            int following = 0;
            while (x < s.n) {
                uint32_t next = ds_utf8(s.p + x, s.n - x, &step);
                unsigned nf = uflags(next);
                if (nf & 8) {
                    x += step;
                    continue;
                }
                following = (nf & 6) != 0;
                break;
            }
            lower = following ? "\317\203" : "\317\202";
        }
        if (cp >= 'A' && cp <= 'Z') {
            p[j++] = (char)(cp + 32);
            changed = 1;
        } else if (lower) {
            size_t len = strlen(lower);
            memcpy(p + j, lower, len);
            j += len;
            changed = 1;
        } else {
            memcpy(p + j, s.p + i, k);
            j += k;
        }
        if (!(f & 8))
            preceding = (f & 6) != 0;
    }
    p[j] = 0;
    return changed ? (Str){p, j} : s;
}
enum { TEXT_ASCII = 1, TEXT_UPPER = 2, TEXT_NORMAL_SPACE = 4 };

/* Summarize the exact logical text. Whitespace runs may cross SIMD blocks. */
static unsigned rule_facts(Str s) {
    unsigned facts = TEXT_ASCII | TEXT_NORMAL_SPACE;
    size_t i = 0;
    int previous_space = 0;
    for (; s.n - i >= 16; i += 16) {
        AsciiMasks masks = ds_ascii_masks(s.p + i);
        if (masks.nonascii) return 0;
        if (masks.upper) facts |= TEXT_UPPER;
        if (masks.space != masks.plain_space ||
            (masks.plain_space & (masks.plain_space << DS_MASK_STEP)) ||
            ((!i || previous_space) && (masks.plain_space & 1)))
            facts &= ~TEXT_NORMAL_SPACE;
        previous_space = (masks.plain_space & (UINT64_C(1) << (15 * DS_MASK_STEP))) != 0;
    }
    for (; i < s.n; i++) {
        unsigned char c = (unsigned char)s.p[i];
        if (c >= 128) return 0;
        if (c >= 'A' && c <= 'Z') facts |= TEXT_UPPER;
        if (ds_ascii_space(c)) {
            if (c != ' ' || !i || previous_space) facts &= ~TEXT_NORMAL_SPACE;
            previous_space = 1;
        } else previous_space = 0;
    }
    if (previous_space) facts &= ~TEXT_NORMAL_SPACE;
    return facts;
}

/* Unicode case mappings can depend on the whole line (for example final
 * sigma), so mixed text keeps the existing full-string lowercase operation. */
static Str unicode_rule_text(Arena *a, Str source) {
    char *buffer;
    size_t i, k, used = 0;
    int space = 0;
    source = ds_lower(a, source);
    if (!source.p) return source;
    buffer = ds_alloc_uninit(a, source.n + 1);
    if (!buffer) return (Str){0};
    for (i = 0; i < source.n; i += k) {
        uint32_t c = ds_utf8(source.p + i, source.n - i, &k);
        if (ds_space(c) || c == 0x200b) {
            space = used != 0;
            continue;
        }
        if (space) buffer[used++] = ' ';
        memcpy(buffer + used, source.p + i, k);
        used += k;
        space = 0;
    }
    buffer[used] = 0;
    return (Str){buffer, used};
}

Str ds_rule_text(Arena *a, Str source) {
    unsigned facts;
    char *buffer;
    size_t i = 0, used = 0;
    int space = 0;
    if (!source.p) return source;
    facts = rule_facts(source);
    if (!(facts & TEXT_ASCII)) return unicode_rule_text(a, source);
    if ((facts & TEXT_NORMAL_SPACE) && !(facts & TEXT_UPPER)) return source;
    if (source.n == SIZE_MAX) {
        snprintf(a->error, sizeof(a->error), "text too large");
        return (Str){0};
    }
    buffer = ds_alloc_uninit(a, source.n + 1);
    if (!buffer) return (Str){0};
    if (facts & TEXT_NORMAL_SPACE) {
        ds_ascii_lower(buffer, source.p, source.n);
        used = source.n;
    } else while (i < source.n) {
        size_t run = ds_rule_span(source.p + i, source.n - i);
        if (run) {
            if (space) buffer[used++] = ' ';
            if (facts & TEXT_UPPER) ds_ascii_lower(buffer + used, source.p + i, run);
            else memcpy(buffer + used, source.p + i, run);
            used += run;
            i += run;
            space = 0;
        } else {
            do { i++; } while (i < source.n && ds_ascii_space((unsigned char)source.p[i]));
            space = used != 0;
        }
    }
    buffer[used] = 0;
    return (Str){buffer, used};
}

int ds_stop_word(Str word) {
    size_t j;
    static const char *stops[] = {"and", "or",  "of",   "the", "in", "on", "at",
                                  "to",  "for", "with", "by",  "a",  "an", NULL};
    for (j = 0; stops[j]; j++) {
        size_t n = strlen(stops[j]), q;
        if (n != word.n) continue;
        for (q = 0; q < n; q++)
            if (tolower((unsigned char)word.p[q]) != stops[j][q]) break;
        if (q == n) return 1;
    }
    return 0;
}
int ds_proper(Str s) {
    size_t i = 0, k, start;
    int words = 0;
    while (i < s.n) {
        start = i;
        while (i < s.n && ((s.p[i] >= 'A' && s.p[i] <= 'Z') || (s.p[i] >= 'a' && s.p[i] <= 'z'))) i++;
        if (i == start) return 0;
        if (!(s.p[start] >= 'A' && s.p[start] <= 'Z') && !ds_stop_word((Str){s.p + start, i - start}))
            return 0;
        words++;
        if (i == s.n)
            break;
        if (!ds_space(ds_utf8(s.p + i, s.n - i, &k)))
            return 0;
        do {
            i += k;
            if (i == s.n)
                return 0;
        } while (ds_space(ds_utf8(s.p + i, s.n - i, &k)));
    }
    return words > 0;
}
static const Entity *longest_entity(const char *p, size_t n, size_t *used) {
    size_t i = 1, node;
    int found = -1;
    unsigned char byte;
    if (!n || (byte = (unsigned char)p[0]) >= 128 || !entity_root[byte])
        return NULL;
    node = entity_root[byte] - 1;
    for (;;) {
        const EntityNode *current = entity_nodes + node;
        size_t j;
        if (current->value >= 0) {
            found = current->value;
            *used = i;
        }
        if (!current->count || i == n || i == DS_ENTITY_MAX_NAME)
            break;
        byte = (unsigned char)p[i];
        for (j = 0; j < current->count; j++)
            if (entity_edges[current->first + j].byte == byte)
                break;
        if (j == current->count)
            break;
        node = entity_edges[current->first + j].target;
        i++;
    }
    return found >= 0 ? entities + found : NULL;
}
Str ds_unescape(Arena *a, Str s) {
    char *out;
    const char *first;
    size_t i, j, capacity;
    /* No HTML character reference can fit in fewer than three bytes. */
    if (s.n < 3)
        return s;
    first = memchr(s.p, '&', s.n);
    if (!first)
        return s;
    /* Numeric references never grow. The generated ratio also bounds every
     * named entity, including the rare expansions such as &nGt;. */
    if (s.n > (SIZE_MAX - DS_ENTITY_EXPANSION_DENOMINATOR) / DS_ENTITY_EXPANSION_NUMERATOR) {
        snprintf(a->error, sizeof(a->error), "text too large");
        return (Str){0};
    }
    capacity = (s.n * DS_ENTITY_EXPANSION_NUMERATOR + DS_ENTITY_EXPANSION_DENOMINATOR - 1) /
                   DS_ENTITY_EXPANSION_DENOMINATOR +
               1;
    out = ds_alloc_uninit(a, capacity);
    if (!out)
        return (Str){0};
    i = j = (size_t)(first - s.p);
    if (i)
        memcpy(out, s.p, i);
    while (i < s.n) {
        if (s.p[i] != '&') {
            const char *next = memchr(s.p + i, '&', s.n - i);
            size_t count = next ? (size_t)(next - s.p - i) : s.n - i;
            memcpy(out + j, s.p + i, count);
            i += count;
            j += count;
            continue;
        }
        if (i + 2 < s.n && s.p[i + 1] == '#') {
            size_t q = i + 2, start;
            uint32_t cp = 0;
            int base = 10, overflow = 0;
            if (s.p[q] == 'x' || s.p[q] == 'X') {
                base = 16;
                q++;
            }
            start = q;
            while (q < s.n) {
                unsigned char c = (unsigned char)s.p[q];
                int v = c >= '0' && c <= '9'   ? c - '0'
                        : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                               : 99;
                if (v >= base)
                    break;
                if (cp > 0x110000)
                    overflow = 1;
                else
                    cp = cp * (unsigned)base + (unsigned)v;
                q++;
            }
            if (q > start) {
                Str raw;
                size_t z;
                if (q < s.n && s.p[q] == ';')
                    q++;
                if (overflow || !cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                    cp = 0xfffd;
                else if (cp == 129 || cp == 141 || cp == 143 || cp == 144 || cp == 157) {
                    j += encode(out + j, cp);
                    i = q;
                    continue;
                } else if (cp >= 128 && cp <= 159) {
                    char b = (char)cp;
                    raw = ds_input(a, (Str){&b, 1});
                    if (!raw.p)
                        return (Str){0};
                    for (z = 0; z < raw.n; z++)
                        out[j++] = raw.p[z];
                    i = q;
                    continue;
                } else if ((cp >= 1 && cp <= 8) || cp == 11 || (cp >= 14 && cp <= 31) ||
                           cp == 127 || (cp >= 0xfdd0 && cp <= 0xfdef) || (cp & 0xffff) == 0xfffe ||
                           (cp & 0xffff) == 0xffff) {
                    i = q;
                    continue;
                }
                j += encode(out + j, cp);
                i = q;
                continue;
            }
        }
        {
            size_t limit = 0;
            const Entity *e = longest_entity(s.p + i + 1, s.n - i - 1, &limit);
            if (e) {
                size_t n = e->value_length;
                memcpy(out + j, e->value, n);
                j += n;
                i += limit + 1;
                continue;
            }
        }
        out[j++] = s.p[i++];
    }
    out[j] = 0;
    return (Str){out, j};
}
