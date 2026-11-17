#ifndef DS_PDF_H
#define DS_PDF_H
#include "doc2struct.h"
typedef struct {
    Document *document;
    Arena scratch;
    ArenaPool pool;
    size_t block, line;
    int stream, soft_break, logical_cells;
    const Rules *rules;
    double origin_x;
    double region_width;
} PdfLayout;
typedef struct {
    size_t first, end;
    double left, right, y, font;
    int horizontal;
} PdfFragment;
int ds_pdf_emit(PdfLayout *, const doc2struct_pdf_page *, PdfFragment,
                int table, size_t row, size_t col, size_t rowspan, size_t colspan, int leading_space);
int ds_pdf_layout(void *context, const doc2struct_pdf_page *page);
int ds_pdf_layout_experimental(void *context, const doc2struct_pdf_page *page);
#endif
