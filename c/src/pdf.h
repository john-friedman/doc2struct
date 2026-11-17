#ifndef DS_PDF_H
#define DS_PDF_H
#include "doc2struct.h"
typedef struct {
    Document *document;
    Arena scratch;
    ArenaPool pool;
    size_t block, line;
    int stream, soft_break;
} PdfLayout;
int ds_pdf_layout(void *context, const doc2struct_pdf_page *page);
#endif
