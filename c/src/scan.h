#ifndef DOC2STRUCT_SCAN_H
#define DOC2STRUCT_SCAN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Constant-size copies compile to unaligned integer loads without aliasing
 * violations or reads outside either slice. Longer strings use libc. */
static inline int ds_bytes_equal(const char *a, const char *b, size_t n) {
    if (n > 8)
        return !memcmp(a, b, n);
    if (n >= 4) {
        uint32_t a0, b0, a1, b1;
        memcpy(&a0, a, 4);
        memcpy(&b0, b, 4);
        memcpy(&a1, a + n - 4, 4);
        memcpy(&b1, b + n - 4, 4);
        return ((a0 ^ b0) | (a1 ^ b1)) == 0;
    }
    if (n >= 2) {
        uint16_t x, y;
        memcpy(&x, a, 2);
        memcpy(&y, b, 2);
        return x == y && (n == 2 || a[2] == b[2]);
    }
    return !n || a[0] == b[0];
}

/* The normalizer's blank set is deliberately narrower than Unicode whitespace.
 * Testing emptiness needs only a forward scan, not a stripped output slice. */
static inline int ds_blank_text(const char *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)p[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            i++;
            continue;
        }
        if (c < 128)
            return 0;
        if (n - i >= 2 && c == 0xc2 && (unsigned char)p[i + 1] == 0xa0) {
            i += 2;
            continue;
        }
        if (n - i >= 3 && c == 0xe2 && (unsigned char)p[i + 1] == 0x80 &&
            (unsigned char)p[i + 2] == 0x8b) {
            i += 3;
            continue;
        }
        return 0;
    }
    return 1;
}

/* SSE2 and NEON are baseline features on their respective 64-bit targets.
 * Unsupported targets and explicitly scalar builds use the bytewise loops. */
#if !defined(DS_SCALAR_SCAN) && (defined(_M_X64) || defined(__SSE2__))
#include <emmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#define DS_SSE2 1
static inline unsigned ds_first_bit(unsigned mask) {
#if defined(_MSC_VER)
    unsigned long bit;
    _BitScanForward(&bit, mask);
    return (unsigned)bit;
#else
    return (unsigned)__builtin_ctz(mask);
#endif
}
#elif !defined(DS_SCALAR_SCAN) && \
    (defined(_M_ARM64) || (defined(__aarch64__) && defined(__ARM_NEON) && \
     defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
#include <arm_neon.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#define DS_NEON 1

/* Comparison bytes are 0x00 or 0xff. Narrow adjacent byte pairs into
 * four-bit lanes, ordered from the lowest address on little-endian ARM64. */
static inline uint64_t ds_neon_mask(uint8x16_t matches) {
    uint8x8_t packed = vshrn_n_u16(vreinterpretq_u16_u8(matches), 4);
    return vget_lane_u64(vreinterpret_u64_u8(packed), 0);
}
/* Only call for a nonzero mask: four bits represent each matching byte. */
static inline unsigned ds_neon_first(uint64_t mask) {
#if defined(_MSC_VER)
    unsigned long bit;
    _BitScanForward64(&bit, mask);
    return (unsigned)bit / 4;
#else
    return (unsigned)__builtin_ctzll(mask) / 4;
#endif
}
static inline uint8x16_t ds_neon_upper(uint8x16_t bytes) {
    return vandq_u8(vcgeq_u8(bytes, vdupq_n_u8('A')),
                   vcleq_u8(bytes, vdupq_n_u8('Z')));
}
#endif

/* Plain text/partial SGML scanners. Like the HTML scans, these never read
 * outside the supplied slice; scalar builds use exactly the same predicates. */
static inline size_t ds_text_line_end(const char *p, size_t n) {
    size_t i = 0;
#ifdef DS_SSE2
    const __m128i lf = _mm_set1_epi8('\n'), cr = _mm_set1_epi8('\r'), ff = _mm_set1_epi8('\f');
    for (; n - i >= 16; i += 16) {
        __m128i data = _mm_loadu_si128((const __m128i *)(p + i));
        unsigned mask = (unsigned)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(data, lf),
            _mm_or_si128(_mm_cmpeq_epi8(data, cr), _mm_cmpeq_epi8(data, ff))));
        if (mask) return i + ds_first_bit(mask);
    }
#elif defined(DS_NEON)
    const uint8x16_t lf = vdupq_n_u8('\n'), cr = vdupq_n_u8('\r'), ff = vdupq_n_u8('\f');
    for (; n - i >= 16; i += 16) {
        uint8x16_t data = vld1q_u8((const uint8_t *)p + i);
        uint8x16_t matches = vorrq_u8(vceqq_u8(data, lf),
                                     vorrq_u8(vceqq_u8(data, cr), vceqq_u8(data, ff)));
        uint64_t mask = ds_neon_mask(matches);
        if (mask) return i + ds_neon_first(mask);
    }
#endif
    for (; i < n; i++) if (p[i] == '\n' || p[i] == '\r' || p[i] == '\f') break;
    return i;
}
static inline size_t ds_text_span(const char *p, size_t n, int markup) {
    size_t i = 0;
#ifdef DS_SSE2
    const __m128i control_end = _mm_set1_epi8(31), zero = _mm_setzero_si128(), lt = _mm_set1_epi8('<');
    for (; n - i >= 16; i += 16) {
        __m128i data = _mm_loadu_si128((const __m128i *)(p + i));
        __m128i special = _mm_or_si128(data, _mm_cmpeq_epi8(_mm_subs_epu8(data, control_end), zero));
        if (markup) special = _mm_or_si128(special, _mm_cmpeq_epi8(data, lt));
        {
            unsigned mask = (unsigned)_mm_movemask_epi8(special);
            if (mask) return i + ds_first_bit(mask);
        }
    }
#elif defined(DS_NEON)
    const uint8x16_t control_end = vdupq_n_u8(32), nonascii = vdupq_n_u8(128), lt = vdupq_n_u8('<');
    for (; n - i >= 16; i += 16) {
        uint8x16_t data = vld1q_u8((const uint8_t *)p + i);
        uint8x16_t special = vorrq_u8(vcltq_u8(data, control_end), vcgeq_u8(data, nonascii));
        uint64_t mask;
        if (markup) special = vorrq_u8(special, vceqq_u8(data, lt));
        mask = ds_neon_mask(special);
        if (mask) return i + ds_neon_first(mask);
    }
#endif
    for (; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 32 || c >= 128 || (markup && c == '<')) break;
    }
    return i;
}
static inline size_t ds_text_spaces(const char *p, size_t n) {
    size_t i = 0;
#ifdef DS_SSE2
    const __m128i space = _mm_set1_epi8(' ');
    for (; n - i >= 16; i += 16) {
        unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i)), space));
        if (mask != 0xffffu) return i + ds_first_bit((~mask) & 0xffffu);
    }
#elif defined(DS_NEON)
    const uint8x16_t space = vdupq_n_u8(' ');
    for (; n - i >= 16; i += 16) {
        uint8x16_t data = vld1q_u8((const uint8_t *)p + i);
        uint64_t mask = ds_neon_mask(vmvnq_u8(vceqq_u8(data, space)));
        if (mask) return i + ds_neon_first(mask);
    }
#endif
    for (; i < n && p[i] == ' '; i++);
    return i;
}

/* Never read beyond the supplied slice: callers need no padding or alignment. */
static inline size_t ds_nonascii(const char *p, size_t n) {
    size_t i = 0;
#ifdef DS_SSE2
    for (; n - i >= 16; i += 16) {
        __m128i data = _mm_loadu_si128((const __m128i *)(p + i));
        unsigned mask = (unsigned)_mm_movemask_epi8(data);
        if (mask)
            return i + ds_first_bit(mask);
    }
#elif defined(DS_NEON)
    const uint8x16_t nonascii = vdupq_n_u8(128);
    for (; n - i >= 16; i += 16) {
        uint8x16_t data = vld1q_u8((const uint8_t *)p + i);
        uint64_t mask = ds_neon_mask(vcgeq_u8(data, nonascii));
        if (mask) return i + ds_neon_first(mask);
    }
#endif
    for (; i < n; i++)
        if ((unsigned char)p[i] >= 128)
            break;
    return i;
}

static inline size_t ds_upper_or_nonascii(const char *p, size_t n) {
    size_t i = 0;
#ifdef DS_SSE2
    const __m128i before_a = _mm_set1_epi8('A' - 1), after_z = _mm_set1_epi8('Z' + 1);
    for (; n - i >= 16; i += 16) {
        __m128i data = _mm_loadu_si128((const __m128i *)(p + i));
        __m128i upper =
            _mm_and_si128(_mm_cmpgt_epi8(data, before_a), _mm_cmpgt_epi8(after_z, data));
        unsigned mask = (unsigned)_mm_movemask_epi8(_mm_or_si128(data, upper));
        if (mask)
            return i + ds_first_bit(mask);
    }
#elif defined(DS_NEON)
    const uint8x16_t nonascii = vdupq_n_u8(128);
    for (; n - i >= 16; i += 16) {
        uint8x16_t data = vld1q_u8((const uint8_t *)p + i);
        uint8x16_t special = vorrq_u8(ds_neon_upper(data), vcgeq_u8(data, nonascii));
        uint64_t mask = ds_neon_mask(special);
        if (mask) return i + ds_neon_first(mask);
    }
#endif
    for (; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if ((c >= 'A' && c <= 'Z') || c >= 128)
            break;
    }
    return i;
}

/* Find the next quote or tag terminator, skipping ordinary attribute bytes. */
static inline size_t ds_tag_mark(const char *p, size_t n) {
    size_t i = 0;
#ifdef DS_SSE2
    const __m128i gt = _mm_set1_epi8('>'), dq = _mm_set1_epi8('"'), sq = _mm_set1_epi8('\'');
    for (; n - i >= 16; i += 16) {
        __m128i data = _mm_loadu_si128((const __m128i *)(p + i));
        __m128i matches =
            _mm_or_si128(_mm_cmpeq_epi8(data, gt),
                         _mm_or_si128(_mm_cmpeq_epi8(data, dq), _mm_cmpeq_epi8(data, sq)));
        unsigned mask = (unsigned)_mm_movemask_epi8(matches);
        if (mask)
            return i + ds_first_bit(mask);
    }
#elif defined(DS_NEON)
    const uint8x16_t gt = vdupq_n_u8('>'), dq = vdupq_n_u8('"'), sq = vdupq_n_u8('\'');
    for (; n - i >= 16; i += 16) {
        uint8x16_t data = vld1q_u8((const uint8_t *)p + i);
        uint8x16_t matches = vorrq_u8(vceqq_u8(data, gt),
                                     vorrq_u8(vceqq_u8(data, dq), vceqq_u8(data, sq)));
        uint64_t mask = ds_neon_mask(matches);
        if (mask) return i + ds_neon_first(mask);
    }
#endif
    for (; i < n; i++)
        if (p[i] == '>' || p[i] == '"' || p[i] == '\'')
            break;
    return i;
}

static inline size_t ds_quote_or_amp(const char *p, size_t n, char quote) {
    size_t i = 0;
#ifdef DS_SSE2
    const __m128i q = _mm_set1_epi8(quote), amp = _mm_set1_epi8('&');
    for (; n - i >= 16; i += 16) {
        __m128i data = _mm_loadu_si128((const __m128i *)(p + i));
        unsigned mask = (unsigned)_mm_movemask_epi8(
            _mm_or_si128(_mm_cmpeq_epi8(data, q), _mm_cmpeq_epi8(data, amp)));
        if (mask)
            return i + ds_first_bit(mask);
    }
#elif defined(DS_NEON)
    const uint8x16_t q = vdupq_n_u8((uint8_t)quote), amp = vdupq_n_u8('&');
    for (; n - i >= 16; i += 16) {
        uint8x16_t data = vld1q_u8((const uint8_t *)p + i);
        uint64_t mask = ds_neon_mask(vorrq_u8(vceqq_u8(data, q), vceqq_u8(data, amp)));
        if (mask) return i + ds_neon_first(mask);
    }
#endif
    for (; i < n; i++)
        if (p[i] == quote || p[i] == '&')
            break;
    return i;
}

static inline int ds_ascii_lower(char *out, const char *in, size_t n) {
    size_t i = 0;
    int changed = 0;
#ifdef DS_SSE2
    const __m128i before_a = _mm_set1_epi8('A' - 1), after_z = _mm_set1_epi8('Z' + 1);
    const __m128i delta = _mm_set1_epi8(32);
    for (; n - i >= 16; i += 16) {
        __m128i data = _mm_loadu_si128((const __m128i *)(in + i));
        __m128i upper =
            _mm_and_si128(_mm_cmpgt_epi8(data, before_a), _mm_cmpgt_epi8(after_z, data));
        changed |= _mm_movemask_epi8(upper);
        _mm_storeu_si128((__m128i *)(out + i), _mm_or_si128(data, _mm_and_si128(upper, delta)));
    }
#elif defined(DS_NEON)
    const uint8x16_t delta = vdupq_n_u8(32);
    uint8x16_t any_upper = vdupq_n_u8(0);
    for (; n - i >= 16; i += 16) {
        uint8x16_t data = vld1q_u8((const uint8_t *)in + i);
        uint8x16_t upper = ds_neon_upper(data);
        any_upper = vorrq_u8(any_upper, upper);
        vst1q_u8((uint8_t *)out + i, vorrq_u8(data, vandq_u8(upper, delta)));
    }
    changed = ds_neon_mask(any_upper) != 0;
#endif
    for (; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 'A' && c <= 'Z') {
            out[i] = (char)(c + 32);
            changed = 1;
        } else
            out[i] = (char)c;
    }
    return changed != 0;
}

static inline size_t ds_json_escape(const char *p, size_t n) {
    size_t i = 0;
#ifdef DS_SSE2
    const __m128i quote = _mm_set1_epi8('"'), slash = _mm_set1_epi8('\\');
    const __m128i control_end = _mm_set1_epi8(31), zero = _mm_setzero_si128();
    for (; n - i >= 16; i += 16) {
        __m128i data = _mm_loadu_si128((const __m128i *)(p + i));
        __m128i control = _mm_cmpeq_epi8(_mm_subs_epu8(data, control_end), zero);
        unsigned mask = (unsigned)_mm_movemask_epi8(_mm_or_si128(control,
            _mm_or_si128(_mm_cmpeq_epi8(data, quote), _mm_cmpeq_epi8(data, slash))));
        if (mask) return i + ds_first_bit(mask);
    }
#elif defined(DS_NEON)
    const uint8x16_t quote = vdupq_n_u8('"'), slash = vdupq_n_u8('\\'), control_end = vdupq_n_u8(32);
    for (; n - i >= 16; i += 16) {
        uint8x16_t data = vld1q_u8((const uint8_t *)p + i);
        uint8x16_t special = vorrq_u8(vcltq_u8(data, control_end),
                                     vorrq_u8(vceqq_u8(data, quote), vceqq_u8(data, slash)));
        uint64_t mask = ds_neon_mask(special);
        if (mask) return i + ds_neon_first(mask);
    }
#endif
    for (; i < n; i++)
        if ((unsigned char)p[i] < 32 || p[i] == '"' || p[i] == '\\') break;
    return i;
}

#endif
