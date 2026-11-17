#include "doc2struct.h"
#include <string.h>
#include <stdio.h>

const char *ds_table_steps[] = {"validate_structure",
                                "merge_formatting_chars",
                                "remove_empty_rows",
                                "remove_empty_columns",
                                "remove_subset_rows_bottom_to_top",
                                "remove_subset_rows_top_to_bottom",
                                "remove_subset_columns_left_to_right",
                                "remove_subset_columns_right_to_left",
                                "merge_duplicate_header_rows_down",
                                NULL};
unsigned ds_style_bit(Str s) {
    static const char *names[] = {"bold",     "italic",      "underline", "centered",
                                  "all_caps", "proper_case", "fake_table", "strikethrough"};
    unsigned i;
    for (i = 0; i < 8; i++)
        if (ds_is(s, names[i]))
            return 1u << i;
    return 0;
}
Regex *ds_regex(Rules *r, Str pattern, int caseless) {
    Regex *rx;
    int error;
    PCRE2_SIZE offset;
    uint32_t options = PCRE2_UTF | PCRE2_UCP;
    char *buf;
    size_t i = 0, j = 0;
    int in_class = 0;
    if (caseless)
        options |= PCRE2_CASELESS;
    if (pattern.n > (SIZE_MAX - 1) / 3) {
        snprintf(r->arena.error, sizeof(r->arena.error), "regex too large");
        return NULL;
    }
    buf = ds_alloc(&r->arena, pattern.n * 3 + 1);
    if (!buf)
        return NULL;
    while (i < pattern.n) {
        if (!in_class && i + 4 <= pattern.n && !memcmp(pattern.p + i, "(?u)", 4)) {
            i += 4;
            continue;
        }
        if (!in_class && i + 4 <= pattern.n && !memcmp(pattern.p + i, "(?a)", 4)) {
            options &= ~PCRE2_UCP;
            i += 4;
            continue;
        }
        if (!in_class && i + 4 <= pattern.n && !memcmp(pattern.p + i, "(?u:", 4)) {
            memcpy(buf + j, "(?:", 3);
            j += 3;
            i += 4;
            continue;
        }
        if (pattern.p[i] == '\\' && i + 1 < pattern.n) {
            char c = pattern.p[i + 1];
            if (c == 'Z') {
                buf[j++] = '\\';
                buf[j++] = 'z';
                i += 2;
                continue;
            }
            if (c == 'u' || c == 'U') {
                size_t count = c == 'u' ? 4 : 8, k;
                if (i + 2 + count <= pattern.n) {
                    buf[j++] = '\\';
                    buf[j++] = 'x';
                    buf[j++] = '{';
                    for (k = 0; k < count; k++)
                        buf[j++] = pattern.p[i + 2 + k];
                    buf[j++] = '}';
                    i += 2 + count;
                    continue;
                }
            }
            buf[j++] = pattern.p[i++];
            buf[j++] = pattern.p[i++];
            continue;
        }
        if (pattern.p[i] == '[')
            in_class = 1;
        else if (pattern.p[i] == ']')
            in_class = 0;
        buf[j++] = pattern.p[i++];
    }
    rx = ds_alloc(&r->arena, sizeof(*rx));
    if (!rx)
        return NULL;
    rx->code = pcre2_compile((PCRE2_SPTR)buf, j, options, &error, &offset, NULL);
    if (!rx->code) {
        PCRE2_UCHAR msg[160];
        pcre2_get_error_message(error, msg, sizeof(msg));
        snprintf(r->arena.error, sizeof(r->arena.error), "native regex at byte %zu: %s",
                 (size_t)offset, (char *)msg);
        return NULL;
    }
    pcre2_pattern_info(rx->code, PCRE2_INFO_CAPTURECOUNT, &rx->groups);
    /* A conservative reject-only guard for an anchored mandatory first atom.
     * Unsupported constructs always use PCRE2; successful matches still use
     * PCRE2 so captures, Unicode behavior, and error handling stay unchanged. */
    rx->caseless = caseless != 0;
    if (pattern.n > 1 && pattern.p[0] == '^') {
        size_t pos, atom_end = 2;
        unsigned char first = (unsigned char)pattern.p[1];
        int supported = 1;
        for (pos = 1; pos < pattern.n; pos++) {
            if (pattern.p[pos] == '|') {
                supported = 0;
                break;
            }
            if (pos + 2 < pattern.n && pattern.p[pos] == '(' && pattern.p[pos + 1] == '?' &&
                !strchr("=!<:#>P", pattern.p[pos + 2])) {
                supported = 0;
                break;
            }
        }
        if (first == '\\' && pattern.n > 2 && pattern.p[2] == 'd') {
            atom_end = 3;
            if (supported && (atom_end == pattern.n || !strchr("?*{", pattern.p[atom_end])))
                rx->first_digit = 1;
        } else if (supported && ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
                                 (first >= '0' && first <= '9') || first == '_' || first == '-')) {
            if (atom_end == pattern.n || !strchr("?*{", pattern.p[atom_end]))
                rx->first_ascii = (unsigned)first + 1;
        }
    }
    rx->next = r->regexes;
    r->regexes = rx;
    return rx;
}
int ds_match(Document *d, Regex *rx, Str text, int search) {
    int rc;
    size_t need;
    if (!rx)
        return 0;
    if (!search && (rx->first_ascii || rx->first_digit)) {
        unsigned char first;
        if (!text.n)
            return 0;
        first = (unsigned char)text.p[0];
        if (first < 128) {
            if (rx->first_digit && (first < '0' || first > '9'))
                return 0;
            if (rx->first_ascii) {
                unsigned expected = rx->first_ascii - 1;
                if (rx->caseless ? ((first | 32u) != (expected | 32u)) : first != expected)
                    return 0;
            }
        }
    }
    need = (size_t)rx->groups + 1;
    if (need > d->match_pairs) {
        pcre2_match_data *next = pcre2_match_data_create((uint32_t)need, NULL);
        if (!next) {
            snprintf(d->arena.error, sizeof(d->arena.error), "out of memory");
            return 0;
        }
        if (d->match)
            pcre2_match_data_free(d->match);
        d->match = next;
        d->match_pairs = need;
    }
    rc = pcre2_match(rx->code, (PCRE2_SPTR)(text.p ? text.p : ""), text.n, 0,
                     PCRE2_NO_UTF_CHECK | (search ? 0 : PCRE2_ANCHORED), d->match, NULL);
    if (rc >= 0)
        return 1;
    if (rc != PCRE2_ERROR_NOMATCH) {
        PCRE2_UCHAR msg[160];
        pcre2_get_error_message(rc, msg, sizeof(msg));
        snprintf(d->arena.error, sizeof(d->arena.error), "native regex match: %s", (char *)msg);
    }
    return 0;
}
Str ds_group(Document *d, Str text, unsigned group) {
    PCRE2_SIZE *o = pcre2_get_ovector_pointer(d->match);
    if (group >= pcre2_get_ovector_count(d->match) || o[group * 2] == PCRE2_UNSET)
        return (Str){0};
    return (Str){text.p + o[group * 2], o[group * 2 + 1] - o[group * 2]};
}

/* Expand only the first match's replacement, without retaining unmatched
 * subject text. Reuse ds_match's captures; unset optional groups are empty. */
Str ds_column_replace(Document *d, const Column *column, Str input) {
    size_t capacity;
    PCRE2_SIZE used;
    char *buffer;
    int rc;
    uint32_t options = PCRE2_NO_UTF_CHECK | PCRE2_SUBSTITUTE_MATCHED |
        PCRE2_SUBSTITUTE_EXTENDED | PCRE2_SUBSTITUTE_UNSET_EMPTY |
        PCRE2_SUBSTITUTE_REPLACEMENT_ONLY | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;
    if (!memchr(column->value.p, '\\', column->value.n) &&
        !memchr(column->value.p, '$', column->value.n)) return column->value;
    if (column->value.n == SIZE_MAX || input.n > SIZE_MAX - column->value.n - 1) {
        snprintf(d->arena.error, sizeof(d->arena.error), "column replacement too large");
        return (Str){0};
    }
    capacity = input.n + column->value.n + 1;
    buffer = ds_alloc_uninit(&d->arena, capacity);
    if (!buffer) return (Str){0};
    used = capacity;
    rc = pcre2_substitute(column->rx->code, (PCRE2_SPTR)input.p, input.n, 0,
        options, d->match, NULL, (PCRE2_SPTR)column->value.p, column->value.n,
        (PCRE2_UCHAR *)buffer, &used);
    if (rc == PCRE2_ERROR_NOMEMORY && used > capacity) {
        capacity = used;
        buffer = ds_alloc_uninit(&d->arena, capacity);
        if (!buffer) return (Str){0};
        rc = pcre2_substitute(column->rx->code, (PCRE2_SPTR)input.p, input.n, 0,
            options, d->match, NULL, (PCRE2_SPTR)column->value.p, column->value.n,
            (PCRE2_UCHAR *)buffer, &used);
    }
    if (rc < 0) {
        PCRE2_UCHAR message[160];
        pcre2_get_error_message(rc, message, sizeof(message));
        snprintf(d->arena.error, sizeof(d->arena.error), "column replacement: %s", (char *)message);
        return (Str){0};
    }
    return (Str){buffer, used};
}
