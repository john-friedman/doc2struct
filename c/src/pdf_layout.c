#include "pdf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef doc2struct_pdf_glyph Glyph;
typedef PdfFragment Fragment;
typedef struct { size_t first, end; double y, font; } PdfRow;

static int by_y(const void *a, const void *b) {
    const Fragment *x=a, *y=b;
    if(x->y != y->y) return x->y < y->y ? -1 : 1;
    if(x->left != y->left) return x->left < y->left ? -1 : 1;
    return x->first < y->first ? -1 : x->first != y->first;
}
static int by_x(const void *a, const void *b) {
    const Fragment *x=a, *y=b;
    if(x->left != y->left) return x->left < y->left ? -1 : 1;
    return x->first < y->first ? -1 : x->first != y->first;
}
static double gap(const Glyph *a, const Glyph *b) {
    if(a->dx > .9) return b->left-a->right;
    if(a->dx < -.9) return a->left-b->right;
    if(a->dy > .9) return b->top-a->bottom;
    return a->top-b->bottom;
}
static int same_line(const Glyph *a, const Glyph *b) {
    double dx=b->x-a->x, dy=b->y-a->y;
    return a->dx*b->dx+a->dy*b->dy > .98 &&
           fabs(dx*a->dy-dy*a->dx) <= .3*fmin(a->font,b->font) &&
           dx*a->dx+dy*a->dy > -.5*a->font;
}
static size_t utf8(char *out, uint32_t cp) {
    if(cp < 128) { out[0]=(char)cp; return 1; }
    if(cp < 2048) { out[0]=(char)(192|(cp>>6)); out[1]=(char)(128|(cp&63)); return 2; }
    if(cp < 65536) {
        out[0]=(char)(224|(cp>>12)); out[1]=(char)(128|((cp>>6)&63));
        out[2]=(char)(128|(cp&63)); return 3;
    }
    out[0]=(char)(240|(cp>>18)); out[1]=(char)(128|((cp>>12)&63));
    out[2]=(char)(128|((cp>>6)&63)); out[3]=(char)(128|(cp&63)); return 4;
}

/* A fragment owns a contiguous slice of the page's glyph array. No per-character
 * allocations: text is encoded once, directly into the result arena. */
int ds_pdf_emit(PdfLayout *layout, const doc2struct_pdf_page *page, PdfFragment f,
                int table, size_t row, size_t col, size_t rowspan, size_t colspan, int leading_space) {
    Document *d=layout->document;
    const Glyph *g=page->glyphs;
    size_t i=f.first;
    while(i<f.end) {
        size_t start=i, end=i+1, used=0, j;
        Run run={0}, *stored;
        char *text;
        while(end<f.end && g[end].flags==g[start].flags && fabs(g[end].font-g[start].font)<.1) end++;
        if(end-start>(SIZE_MAX-2)/5) return 0;
        text=ds_alloc_uninit(&d->arena,(end-start)*5+2);
        if(!text) return 0;
        if(start==f.first && leading_space) text[used++]=' ';
        for(j=start;j<end;j++) {
            if(j>f.first && (g[j].space_before || gap(g+j-1,g+j)>.3*fmin(g[j-1].font,g[j].font))) text[used++]=' ';
            used+=utf8(text+used,g[j].unicode);
        }
        text[used]=0;
        run.text=(Str){text,used}; run.block=layout->block; run.line=layout->line;
        run.flags=g[start].flags;
        run.soft_break=layout->soft_break;
        if(ds_upper(run.text)) run.flags|=ALL_CAPS;
        if(ds_proper(run.text)) run.flags|=PROPER_CASE;
        run.font=g[start].font*4/3; run.has_font=1;
        run.indent=fmax(0,f.left)*4/3; run.has_indent=1;
        run.in_table=table; run.row=row; run.col=col; run.rowspan=rowspan; run.colspan=colspan;
        run.logical_cell=table && layout->logical_cells;
        if(layout->rules && (layout->rules->pdf.region_layout || layout->rules->pdf.clip_to_page || layout->rules->pdf.detect_decorations)) {
            run.has_bbox=1; run.page=page->index;
            run.bbox[0]=g[start].left; run.bbox[1]=g[start].top;
            run.bbox[2]=g[start].right; run.bbox[3]=g[start].bottom;
            for(j=start+1;j<end;j++) {
                run.bbox[0]=fmin(run.bbox[0],g[j].left); run.bbox[1]=fmin(run.bbox[1],g[j].top);
                run.bbox[2]=fmax(run.bbox[2],g[j].right); run.bbox[3]=fmax(run.bbox[3],g[j].bottom);
            }
            run.bbox[0]+=layout->origin_x;run.bbox[2]+=layout->origin_x;
            for(j=0;j<4;j++) run.bbox[j]*=4.0/3;
        }
        if(layout->stream) {
            if(!ds_flow_run(d,&run)) return 0;
        } else {
            stored=ds_push(&d->arena,&d->runs,sizeof(Run));
            if(!stored) return 0;
            *stored=run;
        }
        i=end;
    }
    return 1;
}
static int emit(PdfLayout *layout, const doc2struct_pdf_page *page, Fragment f,
                int table, size_t row, size_t col, int leading_space) {
    return ds_pdf_emit(layout,page,f,table,row,col,1,1,leading_space);
}

static int numeric(const Glyph *glyphs, Fragment f) {
    size_t i, digits=0, other=0;
    for(i=f.first;i<f.end;i++) {
        uint32_t cp=glyphs[i].unicode;
        if(cp>='0' && cp<='9') digits++;
        else if(cp!='$' && cp!=',' && cp!='.' && cp!='(' && cp!=')' &&
                cp!='-' && cp!='%' && cp!=0x2014) other++;
    }
    return digits>0 && other==0;
}
static int aligned(const Fragment *f, PdfRow a, PdfRow b) {
    size_t i, n=a.end-a.first;
    if(n<2 || n!=b.end-b.first) return 0;
    for(i=0;i<n;i++) {
        const Fragment *x=f+a.first+i, *y=f+b.first+i;
        if(!x->horizontal || !y->horizontal ||
           fmin(fabs(x->left-y->left),fabs(x->right-y->right))>fmax(a.font,b.font)) return 0;
    }
    return 1;
}

int ds_pdf_layout(void *context, const doc2struct_pdf_page *page) {
    PdfLayout *layout=context;
    Arena *scratch=&layout->scratch;
    const Glyph *g=page->glyphs;
    Vec fragments={0}, rows={0};
    Fragment *f;
    PdfRow *r;
    size_t i=0, j, k, previous=SIZE_MAX;
    int ok=0;
    while(i<page->count) {
        size_t start=i++;
        Fragment *part=ds_push(scratch,&fragments,sizeof(*part));
        if(!part) goto done;
        *part=(Fragment){start,i,g[start].left,g[start].right,g[start].y,g[start].font,
                         g[start].dx>.98 && fabs(g[start].dy)<.1};
        while(i<page->count && same_line(g+i-1,g+i) &&
              gap(g+i-1,g+i)<1.5*fmax(g[i-1].font,g[i].font)) {
            part->left=fmin(part->left,g[i].left); part->right=fmax(part->right,g[i].right);
            part->end=++i;
        }
    }
    f=fragments.p;
    if(fragments.n>1) qsort(f,fragments.n,sizeof(*f),by_y);
    for(i=0;i<fragments.n;) {
        size_t start=i++;
        PdfRow *row=ds_push(scratch,&rows,sizeof(*row));
        if(!row) goto done;
        while(i<fragments.n && f[start].horizontal && f[i].horizontal &&
              fabs(f[i].y-f[start].y)<.35*fmin(f[start].font,f[i].font)) i++;
        *row=(PdfRow){start,i,f[start].y,f[start].font};
        if(i-start>1) qsort(f+start,i-start,sizeof(*f),by_x);
        /* Adjacent objects (including style changes) are one cell, not columns.
         * Leave their fragments separate so glyph order and styling survive. */
    }
    r=rows.p;
    for(i=0;i<rows.n;) {
        size_t end=i+1, columns=r[i].end-r[i].first;
        int table=0, separated=columns>=2;
        for(j=r[i].first+1;j<r[i].end;j++)
            if(f[j].left-f[j-1].right<1.5*r[i].font) separated=0;
        if(separated) {
            while(end<rows.n && aligned(f,r[i],r[end]) &&
                  r[end].y-r[end-1].y<2.2*fmax(r[end].font,r[end-1].font)) end++;
            /* Require repeated aligned rows and numeric evidence. This avoids
             * interpreting two newspaper-style text columns as a table. */
            if(end-i>=2) {
                for(j=i;j<end;j++) for(k=1;k<columns;k++)
                    if(numeric(g,f[r[j].first+k])) table=1;
            }
        }
        layout->soft_break=0;
        if(layout->document->join_wrapped_lines&&!table&&!separated&&previous!=SIZE_MAX) {
            Fragment a=f[r[previous].first],b=f[r[i].first];
            double right=f[r[previous].end-1].right;
            layout->soft_break=fabs(r[i].font-r[previous].font)<.5 &&
                r[i].y-r[previous].y>0 && r[i].y-r[previous].y<=1.6*r[i].font &&
                fabs(a.left-b.left)<=.5*r[i].font && right-a.left>.45*(layout->region_width>0?layout->region_width:page->width);
        }
        if(table) {
            previous=SIZE_MAX;
            for(j=i;j<end;j++) {
                for(k=0;k<columns;k++)
                    if(!emit(layout,page,f[r[j].first+k],1,j-i,k,0)) goto done;
                layout->line++;
            }
            layout->block++;
            i=end;
        } else if(separated && columns==2 && end-i>=3) {
            previous=SIZE_MAX;
            /* A sustained gutter without numeric cells: read each column down. */
            for(k=0;k<columns;k++) for(j=i;j<end;j++) {
                if(!emit(layout,page,f[r[j].first+k],0,0,0,0)) goto done;
                layout->block++; layout->line++;
            }
            i=end;
        } else {
            for(j=r[i].first;j<r[i].end;j++) {
                int space=j>r[i].first && (g[f[j].first].space_before || f[j].left-f[j-1].right>.3*r[i].font);
                if(!emit(layout,page,f[j],0,0,0,space)) goto done;
            }
            previous=i;
            layout->block++; layout->line++; i++;
        }
    }
    ok=1;
done:
    if(!ok && !layout->document->arena.error[0])
        snprintf(layout->document->arena.error,sizeof(layout->document->arena.error),"%s",
                 scratch->error[0]?scratch->error:"PDF layout failed");
    ds_arena_free(scratch);
    scratch->pool=&layout->pool;
    return ok;
}
