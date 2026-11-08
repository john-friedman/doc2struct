/* Intentionally included twice by tokenizer.c. Keep one lexer implementation
 * while specializing its normalizer dispatch for the HTML parsing pipeline. */
DS_TOKENIZER_STORAGE int DS_TOKENIZER_NAME(Document *d, Str html, TokenSink sink, void *ctx) {
    size_t i = 0, pos = 0, n = html.n;
    int text_entity = 0;
    int direct = DS_TOKENIZER_DIRECT;
    TagTemplate *templates =
        d->flow && n >= 65536 ? ds_alloc(&d->arena, TEMPLATE_SLOTS * sizeof(TagTemplate)) : NULL;
    if (d->arena.error[0])
        return 0;
    while (i < n) {
        size_t start = i, j, k, name_start, name_end, rest_start, end;
        int closing = 0, selfclosed = 0, parsed_attrs = 0;
        Str tag;
        Attrs at;
        const Attrs *dispatch_attrs = &at;
        TagTemplate *cached = NULL;
        unsigned props = 0;
        if (i == pos)
            text_entity = 0;
        if (html.p[i] != '<') {
            i += text_end(html.p + i, n - i, &text_entity);
            if (i == n)
                break;
            start = i;
        }
        if (i + 4 <= n && !memcmp(html.p + i, "<!--", 4)) {
            j = i + 4;
            while (j + 3 <= n && memcmp(html.p + j, "-->", 3))
                j++;
            end = j + 3 <= n ? j + 3 : n;
            if (start > pos && !DS_TOKENIZER_SEND_TEXT(d, sink, ctx, (Str){html.p + pos, start - pos}, text_entity))
                return 0;
            pos = i = end;
            continue;
        }
        if (i + 1 < n && (html.p[i + 1] == '!' || html.p[i + 1] == '?')) {
            j = i + 2;
            while (j < n && html.p[j] != '>')
                j++;
            end = j < n ? j + 1 : n;
            if (start > pos && !DS_TOKENIZER_SEND_TEXT(d, sink, ctx, (Str){html.p + pos, start - pos}, text_entity))
                return 0;
            pos = i = end;
            continue;
        }
        j = i + 1;
        if (j < n && html.p[j] == '/') {
            closing = 1;
            j++;
        }
        if (j >= n ||
            !((html.p[j] >= 'a' && html.p[j] <= 'z') || (html.p[j] >= 'A' && html.p[j] <= 'Z'))) {
            i++;
            continue;
        }
        /* Ordinary lowercase end tags have no attributes or quoted content.
         * Keep the general path for every other spelling and malformed form. */
        if (closing) {
            size_t q = j;
            while (q < n && html.p[q] >= 'a' && html.p[q] <= 'z')
                q++;
            if (q < n && html.p[q] == '>') {
                if (start > pos && !DS_TOKENIZER_SEND_TEXT(d, sink, ctx, (Str){html.p + pos, start - pos}, text_entity))
                    return 0;
                if (!(direct ? ds_html_end(ctx, (Str){html.p + j, q - j}) : sink(ctx, END, (Str){html.p + j, q - j}, NULL)))
                    return 0;
                pos = i = q + 1;
                continue;
            }
        }
        name_start = j;
        if (!closing && templates && n - j >= TEMPLATE_PREFIX) {
            cached = templates + template_index(html.p + j);
            if (cached->raw.p && cached->raw.n <= n - j &&
                tag_equal(html.p + j, cached->raw.p, cached->raw.n)) {
                tag = cached->tag;
                props = cached->props;
                dispatch_attrs = &cached->attrs;
                selfclosed = cached->selfclosed;
                end = j + cached->raw.n;
                if (start > pos && !DS_TOKENIZER_SEND_TEXT(d, sink, ctx, (Str){html.p + pos, start - pos}, text_entity))
                    return 0;
                pos = end;
                goto dispatch_start;
            }
        }
        j++;
        while (j < n && html.p[j] != '/' && html.p[j] != '>' && !space_at(html, j, &k))
            j += k;
        name_end = j;
        rest_start = j;
        if (!closing) {
            int status = start_attrs(d, html, j, &j, &at);
            if (status == -2)
                return 0;
            if (!status) {
                i++;
                continue;
            }
            parsed_attrs = status == 1;
        }
        while (!parsed_attrs && j < n) {
            j += ds_tag_mark(html.p + j, n - j);
            if (j == n)
                break;
            char c = html.p[j];
            if (c == '\'' || c == '\"') {
                char quote = c;
                j++;
                {
                    const char *close = memchr(html.p + j, quote, n - j);
                    j = close ? (size_t)(close - html.p) : n;
                }
                if (j < n)
                    j++;
                else
                    break;
            } else if (c == '>')
                break;
            else
                j++;
        }
        if (j >= n) {
            i++;
            continue;
        }
        end = j + 1;
        if (start > pos && !DS_TOKENIZER_SEND_TEXT(d, sink, ctx, (Str){html.p + pos, start - pos}, text_entity))
            return 0;
        tag = intern_tag(d, (Str){html.p + name_start, name_end - name_start});
        if (!tag.p)
            return 0;
        pos = end;
        if (closing) {
            if (!(direct ? ds_html_end(ctx, tag) : sink(ctx, END, tag, NULL)))
                return 0;
            i = end;
            continue;
        }
        {
            Str rest = {html.p + rest_start, j - rest_start};
            size_t tail = j;
            while (tail > rest_start) {
                unsigned char c = (unsigned char)html.p[tail - 1];
                if (c >= 128) {
                    Str trim = ds_strip(rest, 0);
                    selfclosed = trim.n && trim.p[trim.n - 1] == '/';
                    break;
                }
                if (!(c == ' ' || (c >= 9 && c <= 13) || (c >= 28 && c <= 31))) {
                    selfclosed = c == '/';
                    break;
                }
                tail--;
            }
            if (!parsed_attrs && !attrs(d, rest, &at))
                return 0;
        }
        props = ds_tag_flags(tag);
        if (cached && end - name_start <= TEMPLATE_MAX) {
            if (at.value[A_STYLE].n) {
                at.prepared_style = ds_style(d, at.value[A_STYLE]);
                if (!at.prepared_style)
                    return 0;
            }
            cached->raw = (Str){html.p + name_start, end - name_start};
            cached->tag = tag;
            cached->attrs = at;
            cached->selfclosed = selfclosed;
            cached->props = props;
        }
    dispatch_start:
        if (!(direct ? ds_html_start(ctx, tag, dispatch_attrs, props) : sink(ctx, START, tag, dispatch_attrs)))
            return 0;
        if (!selfclosed && (ds_is(tag, "script") || ds_is(tag, "style") || ds_is(tag, "title"))) {
            size_t q = end;
            int found = 0;
            while (q + 2 + tag.n <= n) {
                if (html.p[q] == '<' && html.p[q + 1] == '/') {
                    size_t x;
                    for (x = 0; x < tag.n; x++)
                        if (tolower((unsigned char)html.p[q + 2 + x]) != (unsigned char)tag.p[x])
                            break;
                    if (x == tag.n) {
                        size_t z = q + 2 + tag.n;
                        while (z < n && space_at(html, z, &k))
                            z += k;
                        if (z < n && html.p[z] == '>') {
                            pos = z + 1;
                            found = 1;
                            break;
                        }
                    }
                }
                q++;
            }
            if (!found)
                pos = n;
            if (!(direct ? ds_html_end(ctx, tag) : sink(ctx, END, tag, NULL)))
                return 0;
        } else if (selfclosed && !ds_void(tag)) {
            if (!(direct ? ds_html_end(ctx, tag) : sink(ctx, END, tag, NULL)))
                return 0;
        }
        i = pos;
    }
    if (pos < n && !DS_TOKENIZER_SEND_TEXT(d, sink, ctx, (Str){html.p + pos, n - pos}, text_entity))
        return 0;
    return !d->arena.error[0];
}
