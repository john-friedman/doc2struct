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

/* These masks are temporary register data, shared by higher-level analyses.
 * NEON uses four bits per byte; SSE2 and scalar code use one. */
#ifdef DS_NEON
#define DS_MASK_STEP 4
#else
#define DS_MASK_STEP 1
#endif
typedef struct {
    uint64_t upper, lower, digit, space, plain_space, nonascii;
} AsciiMasks;

static inline unsigned ds_mask_first(uint64_t mask) {
#ifdef DS_SSE2
    return ds_first_bit((unsigned)mask);
#elif defined(DS_NEON)
    return ds_neon_first(mask);
#else
    unsigned i = 0;
    while (!(mask & 1)) { mask >>= 1; i++; }
    return i;
#endif
}

/* The caller supplies exactly sixteen readable bytes. ASCII whitespace here
 * matches ds_space(), including the four information-separator controls. */
static inline AsciiMasks ds_ascii_masks(const char *p) {
    AsciiMasks masks;
#ifdef DS_SSE2
    __m128i data = _mm_loadu_si128((const __m128i *)p);
    __m128i upper = _mm_and_si128(_mm_cmpgt_epi8(data, _mm_set1_epi8('A' - 1)),
                                  _mm_cmpgt_epi8(_mm_set1_epi8('Z' + 1), data));
    __m128i lower = _mm_and_si128(_mm_cmpgt_epi8(data, _mm_set1_epi8('a' - 1)),
                                  _mm_cmpgt_epi8(_mm_set1_epi8('z' + 1), data));
    __m128i digit = _mm_and_si128(_mm_cmpgt_epi8(data, _mm_set1_epi8('0' - 1)),
                                  _mm_cmpgt_epi8(_mm_set1_epi8('9' + 1), data));
    __m128i plain = _mm_cmpeq_epi8(data, _mm_set1_epi8(' '));
    __m128i controls = _mm_or_si128(
        _mm_and_si128(_mm_cmpgt_epi8(data, _mm_set1_epi8(8)),
                       _mm_cmpgt_epi8(_mm_set1_epi8(14), data)),
        _mm_and_si128(_mm_cmpgt_epi8(data, _mm_set1_epi8(27)),
                       _mm_cmpgt_epi8(_mm_set1_epi8(32), data)));
    masks.upper = (unsigned)_mm_movemask_epi8(upper);
    masks.lower = (unsigned)_mm_movemask_epi8(lower);
    masks.digit = (unsigned)_mm_movemask_epi8(digit);
    masks.space = (unsigned)_mm_movemask_epi8(_mm_or_si128(plain, controls));
    masks.plain_space = (unsigned)_mm_movemask_epi8(plain);
    masks.nonascii = (unsigned)_mm_movemask_epi8(data);
#elif defined(DS_NEON)
    uint8x16_t data = vld1q_u8((const uint8_t *)p);
    uint8x16_t plain = vceqq_u8(data, vdupq_n_u8(' '));
    uint8x16_t controls = vorrq_u8(
        vandq_u8(vcgeq_u8(data, vdupq_n_u8(9)), vcleq_u8(data, vdupq_n_u8(13))),
        vandq_u8(vcgeq_u8(data, vdupq_n_u8(28)), vcleq_u8(data, vdupq_n_u8(31))));
    masks.upper = ds_neon_mask(ds_neon_upper(data));
    masks.lower = ds_neon_mask(vandq_u8(vcgeq_u8(data, vdupq_n_u8('a')),
                                       vcleq_u8(data, vdupq_n_u8('z'))));
    masks.digit = ds_neon_mask(vandq_u8(vcgeq_u8(data, vdupq_n_u8('0')),
                                       vcleq_u8(data, vdupq_n_u8('9'))));
    masks.space = ds_neon_mask(vorrq_u8(plain, controls));
    masks.plain_space = ds_neon_mask(plain);
    masks.nonascii = ds_neon_mask(vcgeq_u8(data, vdupq_n_u8(128)));
#else
    size_t i;
    masks = (AsciiMasks){0};
    for (i = 0; i < 16; i++) {
        unsigned char c = (unsigned char)p[i];
        uint64_t bit = UINT64_C(1) << i;
        if (c >= 'A' && c <= 'Z') masks.upper |= bit;
        if (c >= 'a' && c <= 'z') masks.lower |= bit;
        if (c >= '0' && c <= '9') masks.digit |= bit;
        if (c == ' ' || (c >= 9 && c <= 13) || (c >= 28 && c <= 31)) masks.space |= bit;
        if (c == ' ') masks.plain_space |= bit;
        if (c >= 128) masks.nonascii |= bit;
    }
#endif
    return masks;
}

static inline int ds_ascii_space(unsigned char c) {
    return c == ' ' || (c >= 9 && c <= 13) || (c >= 28 && c <= 31);
}

static inline size_t ds_rule_span(const char *p, size_t n) {
    size_t i = 0;
    for (; n - i >= 16; i += 16) {
        AsciiMasks masks = ds_ascii_masks(p + i);
        uint64_t stop = masks.space | masks.nonascii;
        if (stop) return i + ds_mask_first(stop);
    }
    for (; i < n; i++)
        if ((unsigned char)p[i] >= 128 || ds_ascii_space((unsigned char)p[i])) break;
    return i;
}

/* Table predicates operate on dense fact bytes rather than revisiting strings. */
static inline int ds_flags_any(const unsigned char *p, size_t n, unsigned char bit) {
    size_t i = 0;
    if (n && (p[0] & bit)) return 1;
#ifdef DS_SSE2
    __m128i wanted = _mm_set1_epi8((char)bit), zero = _mm_setzero_si128();
    for (; n - i >= 16; i += 16) {
        __m128i flags = _mm_and_si128(_mm_loadu_si128((const __m128i *)(p + i)), wanted);
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(flags, zero)) != 0xffff) return 1;
    }
#elif defined(DS_NEON)
    uint8x16_t wanted = vdupq_n_u8(bit);
    for (; n - i >= 16; i += 16) {
        uint8x16_t flags = vandq_u8(vld1q_u8(p + i), wanted);
        if (ds_neon_mask(vcgtq_u8(flags, vdupq_n_u8(0)))) return 1;
    }
#endif
    for (; i < n; i++) if (p[i] & bit) return 1;
    return 0;
}

static inline size_t ds_flags_columns(unsigned char *keep, const unsigned char *p,
                                     size_t n, unsigned char bit) {
    size_t i = 0, added = 0;
#ifdef DS_SSE2
    __m128i wanted = _mm_set1_epi8((char)bit), zero = _mm_setzero_si128();
    for (; n - i >= 16; i += 16) {
        __m128i previous = _mm_loadu_si128((const __m128i *)(keep + i));
        if (!_mm_movemask_epi8(_mm_cmpeq_epi8(previous, zero))) continue;
        __m128i flags = _mm_and_si128(_mm_loadu_si128((const __m128i *)(p + i)), wanted);
        __m128i present = _mm_andnot_si128(_mm_cmpeq_epi8(flags, zero), _mm_set1_epi8(1));
        __m128i counts = _mm_sad_epu8(_mm_andnot_si128(previous, present), zero);
        added += (unsigned)_mm_cvtsi128_si32(counts) +
                 (unsigned)_mm_cvtsi128_si32(_mm_srli_si128(counts, 8));
        _mm_storeu_si128((__m128i *)(keep + i), _mm_or_si128(previous, present));
    }
#elif defined(DS_NEON)
    uint8x16_t wanted = vdupq_n_u8(bit);
    for (; n - i >= 16; i += 16) {
        uint8x16_t previous = vld1q_u8(keep + i);
        if (vminvq_u8(previous)) continue;
        uint8x16_t flags = vandq_u8(vld1q_u8(p + i), wanted);
        uint8x16_t present = vandq_u8(vcgtq_u8(flags, vdupq_n_u8(0)), vdupq_n_u8(1));
        added += vaddvq_u8(vbicq_u8(present, previous));
        vst1q_u8(keep + i, vorrq_u8(previous, present));
    }
#endif
    for (; i < n; i++)
        if (!keep[i] && (p[i] & bit)) { keep[i] = 1; added++; }
    return added;
}

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
