/* Included by pdf.c. Geometry stays in page points until instruction export. */
typedef struct { size_t glyph; double baseline; } DecorationGlyph;
static int decoration_by_baseline(const void *a,const void *b) {
    const DecorationGlyph *x=a,*y=b;
    return (x->baseline>y->baseline)-(x->baseline<y->baseline);
}
static size_t decoration_lower(const DecorationGlyph *refs,size_t count,double value) {
    size_t first=0;
    while (first<count) {
        size_t middle=first+(count-first)/2;
        if (refs[middle].baseline<value) first=middle+1; else count=middle;
    }
    return first;
}
static unsigned decoration_match(const doc2struct_pdf_rule *line,const doc2struct_pdf_glyph *g) {
    int vertical=line->bottom-line->top>line->right-line->left;
    double position=vertical?(line->left+line->right)*.5:(line->top+line->bottom)*.5;
    double offset=vertical?(position-g->x)*(-g->dy):(position-g->y)*g->dx;
    double low=vertical?g->top:g->left,high=vertical?g->bottom:g->right;
    double start=vertical?line->top:line->left,end=vertical?line->bottom:line->right;
    if ((vertical?fabs(g->dy):fabs(g->dx))<.98 || g->font<=0 ||
        line->thickness>fmax(.8,.14*g->font) || high<=low ||
        fmin(end,high)-fmax(start,low)<.6*(high-low)) return 0;
    if (offset>=-.04*g->font && offset<=.30*g->font) return UNDERLINE;
    if (offset>=-.66*g->font && offset<=-.16*g->font) return STRIKETHROUGH;
    return 0;
}
static int pdf_decorations(Arena *arena,FPDF_PAGE page,doc2struct_pdf_page *view,
                           doc2struct_pdf_glyph *glyphs) {
    DecorationGlyph *refs;
    size_t i,j,axis;
    double maxfont=0;
    Transform matrix;
    int annotations;
    if (!view->count) return 1;
    if (view->count>SIZE_MAX/sizeof(*refs)) return 0;
    refs=ds_alloc_uninit(arena,view->count*sizeof(*refs));
    if (!refs) return 0;
    for (i=0;i<view->count;i++) maxfont=fmax(maxfont,glyphs[i].font);
    for (axis=0;axis<2;axis++) {
        for (i=0;i<view->count;i++) refs[i]=(DecorationGlyph){i,axis?glyphs[i].x:glyphs[i].y};
        qsort(refs,view->count,sizeof(*refs),decoration_by_baseline);
        for (i=0;i<view->rule_count;i++) {
            const doc2struct_pdf_rule *line=view->rules+i;
            int vertical=line->bottom-line->top>line->right-line->left;
            double position=axis?(line->left+line->right)*.5:(line->top+line->bottom)*.5;
            double length=vertical?line->bottom-line->top:line->right-line->left;
            double covered=0;
            size_t first,end;
            if ((size_t)vertical!=axis || line->structural || line->thickness>fmax(.8,.14*maxfont)) continue;
            first=decoration_lower(refs,view->count,position-maxfont*.8);
            end=decoration_lower(refs,view->count,position+maxfont*.8);
            for (j=first;j<end;j++) {
                const doc2struct_pdf_glyph *g=glyphs+refs[j].glyph;
                if (decoration_match(line,g))
                    covered+=vertical?fmin(line->bottom,g->bottom)-fmax(line->top,g->top):
                                      fmin(line->right,g->right)-fmax(line->left,g->left);
            }
            /* Borders extending through mostly empty cells are weak evidence
             * for a text decoration. Short word underlines remain eligible. */
            if (length>40 && covered<.35*length) continue;
            for (j=first;j<end;j++) {
                doc2struct_pdf_glyph *g=glyphs+refs[j].glyph;
                g->flags|=decoration_match(line,g);
            }
        }
    }
    /* Explicit markup annotations have semantic types and affected quadrilaterals. */
    for (i=0;i<view->count;i++) refs[i]=(DecorationGlyph){i,glyphs[i].y};
    qsort(refs,view->count,sizeof(*refs),decoration_by_baseline);
    if (!transform(page,view->width,view->height,&matrix)) return 0;
    annotations=p_FPDFPage_GetAnnotCount(page);
    for (int a=0;a<annotations;a++) {
        FPDF_ANNOTATION annotation=p_FPDFPage_GetAnnot(page,a);
        int type;
        size_t quads;
        unsigned flag;
        if (!annotation) continue;
        type=p_FPDFAnnot_GetSubtype(annotation);
        flag=type==10?UNDERLINE:type==12?STRIKETHROUGH:0;
        quads=flag?p_FPDFAnnot_CountAttachmentPoints(annotation):0;
        if (quads>8192) quads=8192;
        for (i=0;i<quads;i++) {
            FS_QUADPOINTSF q;
            double xx[4],yy[4],left,right,top,bottom;
            size_t first,end;
            if (!p_FPDFAnnot_GetAttachmentPoints(annotation,i,&q)) continue;
            point(matrix,q.x1,q.y1,xx,yy); point(matrix,q.x2,q.y2,xx+1,yy+1);
            point(matrix,q.x3,q.y3,xx+2,yy+2); point(matrix,q.x4,q.y4,xx+3,yy+3);
            left=right=xx[0]; top=bottom=yy[0];
            for (j=1;j<4;j++) { left=fmin(left,xx[j]); right=fmax(right,xx[j]); top=fmin(top,yy[j]); bottom=fmax(bottom,yy[j]); }
            first=decoration_lower(refs,view->count,top-maxfont*.2);
            end=decoration_lower(refs,view->count,bottom+maxfont);
            for (j=first;j<end;j++) {
                doc2struct_pdf_glyph *g=glyphs+refs[j].glyph;
                double x=(g->left+g->right)*.5,y=(g->top+g->bottom)*.5;
                if (x>=left-.1 && x<=right+.1 && y>=top-.1 && y<=bottom+.1) g->flags|=flag;
            }
        }
        p_FPDFPage_CloseAnnot(annotation);
    }
    return 1;
}
static int pdf_prepare_glyphs(Arena *arena,FPDF_PAGE page,doc2struct_pdf_page *view,
                              doc2struct_pdf_glyph *glyphs,const PdfOptions *options) {
    size_t i,used=0,previous=SIZE_MAX;
    if (!options) return 1;
    if (options->clip_to_page) {
        for (i=0;i<view->count;i++) {
            doc2struct_pdf_glyph g=glyphs[i];
            if (g.right<-.05 || g.left>view->width+.05 || g.bottom<-.05 || g.top>view->height+.05) continue;
            if (previous!=SIZE_MAX && i!=previous+1) g.space_before=1;
            glyphs[used++]=g; previous=i;
        }
        view->count=used;
    }
    return !options->detect_decorations || pdf_decorations(arena,page,view,glyphs);
}
