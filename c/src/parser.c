#include "doc2struct.h"

/* Input and rules stay owned by the caller and must outlive the document.
 * The document owns transformed strings and result records. */
int ds_parse(Document *document, Str input, const Rules *rules) {
    void *normalizer;
    input = ds_input(&document->arena, input);
    document->retain_table_cells = rules->unwrap_layout;
    document->collect_hrefs = rules->collect_hrefs;
    if (!input.p || !ds_flow_begin(document))
        return 0;
    normalizer = ds_normalizer(document, rules->html);
    return normalizer && ds_html_tokenize(document, input, ds_normalize_token, normalizer) &&
           ds_normalize_finish(normalizer) && ds_standardize(document, rules);
}

void ds_parser_init(ParserContext *context, size_t cache_size) {
    *context = (ParserContext){0};
    context->pool.limit = cache_size;
}

int ds_parser_parse(ParserContext *context, Document *document, Str input, const Rules *rules) {
    document->arena.pool = &context->pool;
    return ds_parse(document, input, rules);
}

void ds_parser_clear(ParserContext *context) { ds_pool_clear(&context->pool); }
