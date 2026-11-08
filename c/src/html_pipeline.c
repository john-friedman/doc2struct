/* Keep the HTML stages together so the tokenizer can call the small text,
 * start and end handlers directly, without changing normalization rules. */
#if defined(_MSC_VER)
#define DS_HTML_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define DS_HTML_INLINE inline __attribute__((always_inline))
#else
#define DS_HTML_INLINE inline
#endif
#include "flow.c"
#include "normalizer.c"
#include "tokenizer.c"
#include "parser.c"
#undef DS_HTML_INLINE
