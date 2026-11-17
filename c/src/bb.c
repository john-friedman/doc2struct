#include "doc2struct.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

/* OCR spans stay intact. Page-local sorts and bounded row lookahead recover
 * layout without creating synthetic glyphs or comparing every pair of boxes. */
#define BB_COLUMNS 64
#define BB_LOOKAHEAD 8
#define BB_BINS 128

typedef doc2struct_bb_page BBPage;
typedef struct {
    Str text;
    double left, top, right, bottom;
    unsigned flags;
    size_t order;
} BBWord;

typedef struct {
    size_t first, end, characters, letters, digits, dashes, currency, percent, other;
    double left, right, top, bottom;
} Cell;

typedef struct {
    size_t first, end;
    double top, bottom, height, font;
} PhysicalRow;

typedef struct {
    size_t count;
    double left[BB_COLUMNS], right[BB_COLUMNS], boundary[BB_COLUMNS+1], height;
    unsigned numeric[BB_COLUMNS];
    int has_numbers;
} Grid;

typedef struct { double height; size_t row; } FontSample;

typedef struct {
    Document *document;
    Arena scratch;
    BBWord *words;
    Cell *cells;
    PhysicalRow *rows;
    size_t block, line;
    int stream, soft_break;
    double width;
} Layout;

static int compare_y(const void *a,const void *b) {
    const BBWord *x=a,*y=b;
    double cy=x->top+x->bottom,dy=y->top+y->bottom;
    if (cy!=dy) return cy<dy?-1:1;
    if (x->left!=y->left) return x->left<y->left?-1:1;
    return (x->order>y->order)-(x->order<y->order);
}
static int compare_x(const void *a,const void *b) {
    const BBWord *x=a,*y=b;
    if (x->left!=y->left) return x->left<y->left?-1:1;
    return (x->order>y->order)-(x->order<y->order);
}
static int compare_double(const void *a,const void *b) {
    double x=*(const double *)a,y=*(const double *)b;
    return (x>y)-(x<y);
}
static int compare_font(const void *a,const void *b) {
    double x=((const FontSample *)a)->height,y=((const FontSample *)b)->height;
    return (x>y)-(x<y);
}
static int numeric(Cell c) { return !c.other && (c.digits || c.dashes); }
static int prefix(Cell c) { return !c.other && c.currency && !c.digits && !c.dashes && !c.percent; }
static int suffix(Cell c) { return !c.other && c.percent && !c.digits && !c.dashes && !c.currency; }

static Cell word_cell(const BBWord *w,size_t index) {
    Cell c={0};
    size_t i=0,step;
    c.first=index; c.end=index+1;
    c.left=w->left; c.right=w->right; c.top=w->top; c.bottom=w->bottom;
    while (i<w->text.n) {
        uint32_t cp=ds_utf8(w->text.p+i,w->text.n-i,&step);
        if (!step) break;
        i+=step;
        if (ds_space(cp)) continue;
        c.characters++;
        if (cp>='0' && cp<='9') c.digits++;
        else if (cp=='-' || cp==0x2013 || cp==0x2014 || cp==0x2212) c.dashes++;
        else if (cp=='$' || cp==0xa3 || cp==0x20ac || cp==0xa5) c.currency++;
        else if (cp=='%') c.percent++;
        else if (cp!=',' && cp!='.' && cp!='(' && cp!=')' && cp!='+' && cp!='*') {
            c.other++;
            if ((cp>='A' && cp<='Z') || (cp>='a' && cp<='z') || cp>=128) c.letters++;
        }
    }
    return c;
}

static int build_rows(Layout *layout,BBPage page,Vec *out) {
    Vec cells={0},rows={0};
    size_t i,j,n=0;
    double scale=page.dpi>0?96/page.dpi:1056/page.height;
    BBWord *words;
    double *heights;
    FontSample *fonts;
    if (!isfinite(scale) || scale<=0 || !isfinite(page.width*scale) ||
        page.count>SIZE_MAX/sizeof(BBWord) || page.count>SIZE_MAX/sizeof(double)) return 0;
    layout->width=page.width*scale;
    words=ds_alloc_uninit(&layout->scratch,page.count*sizeof(BBWord));
    heights=ds_alloc_uninit(&layout->scratch,page.count*sizeof(double));
    if (!words || !heights) return 0;
    for (i=0;i<page.count;i++) {
        doc2struct_bb_word input=page.words[i];
        BBWord w={input.text,input.left,input.top,input.right,input.bottom,input.flags,i};
        w.text=ds_strip(w.text,1);
        if (!w.text.n) continue;
        w.left*=scale; w.right*=scale; w.top*=scale; w.bottom*=scale;
        if (!isfinite(w.bottom-w.top) || w.bottom<=w.top || w.right<=w.left) return 0;
        words[n++]=w;
    }
    if (n>1) qsort(words,n,sizeof(*words),compare_y);
    for (i=0;i<n;) {
        size_t start=i++,samples=0,first=cells.n;
        double top=words[start].top,bottom=words[start].bottom;
        double anchor=(top+bottom)*.5,h=bottom-top;
        PhysicalRow row={0},*stored;
        while (i<n) {
            double wh=words[i].bottom-words[i].top;
            double overlap=fmin(bottom,words[i].bottom)-fmax(top,words[i].top);
            if (overlap<.5*fmin(h,wh) ||
                fabs((words[i].top+words[i].bottom)*.5-anchor)>.45*fmax(h,wh)) break;
            i++;
        }
        if (i-start>1) qsort(words+start,i-start,sizeof(*words),compare_x);
        row.top=top; row.bottom=bottom;
        for (j=start;j<i;j++) {
            Cell c=word_cell(words+j,j);
            row.top=fmin(row.top,c.top); row.bottom=fmax(row.bottom,c.bottom);
            if (c.letters>=3) heights[samples++]=words[j].bottom-words[j].top;
        }
        if (!samples) for (j=start;j<i;j++) heights[samples++]=words[j].bottom-words[j].top;
        qsort(heights,samples,sizeof(double),compare_double);
        row.height=heights[samples/2];
        for (j=start;j<i;j++) {
            Cell current=word_cell(words+j,j);
            Cell *last=cells.n>first?(Cell *)cells.p+cells.n-1:NULL;
            double distance=last?current.left-last->right:1e30;
            int merge=last &&
                ((distance<.9*row.height &&
                  !(numeric(*last) && numeric(current) && distance>.25*row.height) && !prefix(current)) ||
                 (prefix(*last) && numeric(current) && distance<2*row.height) ||
                 (numeric(*last) && suffix(current) && distance<row.height));
            if (merge) {
                last->end=current.end;
                last->left=fmin(last->left,current.left); last->right=fmax(last->right,current.right);
                last->top=fmin(last->top,current.top); last->bottom=fmax(last->bottom,current.bottom);
                last->characters+=current.characters; last->letters+=current.letters;
                last->digits+=current.digits; last->dashes+=current.dashes;
                last->currency+=current.currency; last->percent+=current.percent; last->other+=current.other;
            } else {
                Cell *part=ds_push(&layout->scratch,&cells,sizeof(Cell));
                if (!part) return 0;
                *part=current;
            }
        }
        row.first=first; row.end=cells.n;
        stored=ds_push(&layout->scratch,&rows,sizeof(PhysicalRow));
        if (!stored) return 0;
        *stored=row;
    }
    /* Box measurements vary even within a font. Cluster heights, then assign
     * one exact font value per cluster for the existing font-mode classifier. */
    if (rows.n>SIZE_MAX/sizeof(FontSample)) return 0;
    fonts=ds_alloc_uninit(&layout->scratch,rows.n*sizeof(FontSample));
    if (!fonts) return 0;
    for (i=0;i<rows.n;i++) fonts[i]=(FontSample){((PhysicalRow *)rows.p)[i].height,i};
    if (rows.n>1) qsort(fonts,rows.n,sizeof(FontSample),compare_font);
    for (i=0;i<rows.n;) {
        size_t end=i+1;
        double font;
        while (end<rows.n && fonts[end].height<=1.18*fonts[i].height) end++;
        font=fmax(.5,round(fonts[i+(end-i)/2].height*1.25*2)/2);
        for (j=i;j<end;j++) ((PhysicalRow *)rows.p)[fonts[j].row].font=font;
        i=end;
    }
    layout->words=words; layout->cells=cells.p; layout->rows=rows.p;
    *out=rows;
    return 1;
}

static int word_space(Str before,Str after) {
    if (!before.n || !after.n) return 0;
    if (after.n==1 && strchr(",.;:!?%)]}",after.p[0])) return 0;
    if (before.n==1 && strchr("([$",before.p[0])) return 0;
    return 1;
}

static int emit_words(Layout *layout,size_t first,size_t end,double font,double indent,
                      int table,size_t row,size_t col,size_t span,int leading) {
    Document *d=layout->document;
    size_t i=first,j;
    while (i<end) {
        size_t stop=i+1,count=0;
        Str *parts,text;
        Run run={0},*stored;
        while (stop<end && layout->words[stop].flags==layout->words[i].flags) stop++;
        if (stop-i>SIZE_MAX/2/sizeof(Str)) return 0;
        parts=ds_alloc_uninit(&layout->scratch,(stop-i)*2*sizeof(Str));
        if (!parts) return 0;
        for (j=i;j<stop;j++) {
            if ((j>first || leading) &&
                (j==first || word_space(layout->words[j-1].text,layout->words[j].text))) parts[count++]=ds_str(" ");
            parts[count++]=layout->words[j].text;
        }
        text=count==1?ds_copy(&d->arena,parts[0]):ds_join(&d->arena,parts,count,ds_str(""));
        if (!text.p) return 0;
        run.block=layout->block; run.line=layout->line; run.text=text;
        run.font=font; run.has_font=1; run.indent=fmax(0,indent); run.has_indent=1;
        run.flags=layout->words[i].flags;
        if (ds_upper(text)) run.flags|=ALL_CAPS;
        else if (ds_proper(text)) run.flags|=PROPER_CASE;
        run.soft_break=layout->soft_break;
        run.in_table=table; run.logical_cell=table;
        run.row=row; run.col=col; run.rowspan=1; run.colspan=span;
        if (layout->stream) {
            if (!ds_flow_run(d,&run)) return 0;
        } else {
            stored=doc2struct_add_run(d);
            if (!stored) return 0;
            *stored=run;
        }
        i=stop;
    }
    return 1;
}

static size_t column(const Grid *grid,Cell c,double *distance) {
    size_t i,best=0;
    *distance=1e30;
    for (i=0;i<grid->count;i++) {
        double delta=numeric(c) && grid->numeric[i]?fabs(c.right-grid->right[i]):
            fmin(fabs(c.left-grid->left[i]),fabs(c.right-grid->right[i]));
        if (delta<*distance) { *distance=delta; best=i; }
    }
    return best;
}

static int make_grid(Layout *layout,PhysicalRow row,Grid *grid) {
    size_t i,n=0;
    *grid=(Grid){0}; grid->height=row.height;
    for (i=row.first;i<row.end;i++) {
        Cell c=layout->cells[i];
        if (prefix(c) || suffix(c)) continue;
        if (n==BB_COLUMNS) return 0;
        grid->left[n]=c.left; grid->right[n]=c.right; grid->numeric[n]=numeric(c);
        if (n && numeric(c)) grid->has_numbers=1;
        n++;
    }
    if (n<2 || (!grid->has_numbers && n<3)) return 0;
    grid->count=n; grid->boundary[0]=grid->left[0]-row.height;
    for (i=1;i<n;i++) grid->boundary[i]=(grid->right[i-1]+grid->left[i])*.5;
    grid->boundary[n]=grid->right[n-1]+row.height;
    if (!grid->has_numbers) {
        size_t long_cells=0;
        for (i=0;i<n;i++) long_cells+=grid->right[i]-grid->left[i]>16*row.height;
        if (long_cells>=2) return 0;
    }
    return 1;
}

static int fits(Layout *layout,const Grid *grid,PhysicalRow row) {
    size_t i;
    if (row.height<.6*grid->height || row.height>1.6*grid->height) return 0;
    for (i=row.first;i<row.end;i++) {
        Cell c=layout->cells[i];
        if (c.left<grid->boundary[0]-.5*grid->height ||
            c.right>grid->boundary[grid->count]+.5*grid->height) return 0;
    }
    return 1;
}

static int supports(Layout *layout,const Grid *grid,PhysicalRow row) {
    size_t i,matches=0,data=0;
    uint64_t occupied=0;
    if (row.end-row.first<2 || !fits(layout,grid,row)) return 0;
    for (i=row.first;i<row.end;i++) {
        Cell c=layout->cells[i];
        double distance;
        size_t col=column(grid,c,&distance);
        if (distance>.75*fmax(grid->height,row.height) || prefix(c) || suffix(c)) continue;
        if (!(occupied & (UINT64_C(1)<<col))) {
            occupied|=UINT64_C(1)<<col; matches++;
            data+=col && numeric(c);
        }
    }
    return matches>=2 && (grid->has_numbers?data>0:matches>=3);
}

static int nearby(PhysicalRow before,PhysicalRow after) {
    return after.top-before.bottom<2.5*fmax(before.height,after.height);
}

static int find_table(Layout *layout,size_t first,size_t count,Grid *grid,size_t *body,size_t *end) {
    PhysicalRow *rows=layout->rows;
    size_t q,j,stop=first+BB_LOOKAHEAD<count?first+BB_LOOKAHEAD:count,best=SIZE_MAX,quality=0;
    if (rows[first].end-rows[first].first<2) return 0;
    for (q=first;q<stop;q++) {
        Grid candidate;
        size_t votes=0,score;
        if (q>first && !nearby(rows[q-1],rows[q])) break;
        if (!make_grid(layout,rows[q],&candidate)) continue;
        for (j=first;j<count && j<q+BB_LOOKAHEAD;j++) {
            if (j>first && !nearby(rows[j-1],rows[j])) break;
            if (!fits(layout,&candidate,rows[j])) break;
            votes+=supports(layout,&candidate,rows[j]);
        }
        if (votes<(candidate.has_numbers?2u:3u)) continue;
        score=votes*4+candidate.count;
        if (score>quality) { quality=score; best=q; *grid=candidate; }
    }
    if (best==SIZE_MAX) return 0;
    for (j=first;j<best;j++) if (!fits(layout,grid,rows[j])) return 0;
    *body=best;
    for (j=first;j<=best;j++) if (supports(layout,grid,rows[j])) { *body=j; break; }
    *end=best+1;
    for (j=best+1;j<count && nearby(rows[j-1],rows[j]);j++) {
        size_t k;
        if (!fits(layout,grid,rows[j])) break;
        if (supports(layout,grid,rows[j])) { *end=j+1; continue; }
        for (k=j+1;k<count && k<=j+2;k++) {
            if (!nearby(rows[k-1],rows[k]) || !fits(layout,grid,rows[k])) break;
            if (supports(layout,grid,rows[k])) break;
        }
        if (k<count && k<=j+2 && nearby(rows[k-1],rows[k]) && fits(layout,grid,rows[k]) &&
            supports(layout,grid,rows[k])) { *end=j+1; continue; }
        /* One short final label continuation may belong to the preceding cell. */
        if (rows[j].end-rows[j].first==1 && layout->cells[rows[j].first].letters &&
            layout->cells[rows[j].first].right<grid->boundary[1] &&
            rows[j].top-rows[j-1].bottom<.6*rows[j].height) *end=j+1;
        break;
    }
    return *end-first>=2;
}

static int emit_table(Layout *layout,size_t first,size_t body,size_t end,const Grid *grid) {
    size_t i,j,row=0;
    layout->soft_break=0;
    for (i=first;i<end;i++) {
        PhysicalRow physical=layout->rows[i];
        size_t previous_col=SIZE_MAX;
        int previous_prefix=0;
        int continuation=i>body && physical.end-physical.first==1 &&
            layout->cells[physical.first].letters &&
            layout->cells[physical.first].right<grid->boundary[1] &&
            physical.top-layout->rows[i-1].bottom<.6*physical.height;
        if (i>first && !continuation) row++;
        for (j=physical.first;j<physical.end;j++) {
            Cell c=layout->cells[j];
            double distance,center=(c.left+c.right)*.5;
            size_t col=column(grid,c,&distance),span=1,last;
            if (prefix(c) && j+1<physical.end && numeric(layout->cells[j+1]))
                col=column(grid,layout->cells[j+1],&distance);
            else if (suffix(c) && j>physical.first && numeric(layout->cells[j-1]))
                col=column(grid,layout->cells[j-1],&distance);
            else if (distance>grid->height) {
                col=0;
                while (col+1<grid->count && center>grid->boundary[col+1]) col++;
            }
            if (i<body && !numeric(c)) {
                last=col;
                while (col && c.left<=grid->left[col-1] && c.right>=grid->right[col]) col--;
                while (last+1<grid->count && c.left<=grid->left[last] && c.right>=grid->right[last+1]) last++;
                span=last-col+1;
            }
            if (!emit_words(layout,c.first,c.end,physical.font,c.left,1,row,col,span,
                            previous_col==col && !previous_prefix && !suffix(c))) return 0;
            previous_col=col;
            previous_prefix=prefix(c);
        }
        layout->line++;
    }
    layout->block++;
    return 1;
}

static int selected(Cell cell,double left,double right) {
    double center=(cell.left+cell.right)*.5;
    return center>=left && center<right;
}

static double gutter(Layout *layout,size_t first,size_t end,double left,double right) {
    long difference[BB_BINS+1]={0},coverage[BB_BINS];
    size_t i,j,nrows=0,start=0,best=0,begin=0;
    double height=0,width=right-left,cut;
    long active=0,allowed;
    if (end-first<3 || width<=0) return 0;
    for (i=first;i<end;i++) {
        int found=0;
        PhysicalRow row=layout->rows[i];
        for (j=row.first;j<row.end;j++) if (selected(layout->cells[j],left,right)) {
            Cell c=layout->cells[j];
            int a=(int)floor(fmax(0,fmin(BB_BINS-1,(c.left-left)/width*BB_BINS)));
            int b=(int)ceil(fmax(0,fmin(BB_BINS,(c.right-left)/width*BB_BINS)));
            difference[a]++; difference[b]--; found=1;
        }
        if (found) { nrows++; height+=row.height; }
    }
    if (nrows<3) return 0;
    height/=nrows; allowed=(long)(nrows/5); if (allowed>2) allowed=2;
    for (i=0;i<BB_BINS;i++) { active+=difference[i]; coverage[i]=active; }
    for (i=16;i<BB_BINS-16;i++) {
        if (coverage[i]<=allowed) {
            if (!start) start=i;
            if (i-start+1>best) { best=i-start+1; begin=start; }
        } else start=0;
    }
    if (best*width/BB_BINS<1.5*height) return 0;
    cut=left+(begin+best*.5)*width/BB_BINS;
    {
        size_t lrows=0,rrows=0,lletters=0,rletters=0;
        for (i=first;i<end;i++) {
            int l=0,r=0;
            for (j=layout->rows[i].first;j<layout->rows[i].end;j++) {
                Cell c=layout->cells[j];
                if (!selected(c,left,right)) continue;
                if (c.right<=cut) { l=1; lletters+=c.letters; }
                if (c.left>=cut) { r=1; rletters+=c.letters; }
            }
            lrows+=l; rrows+=r;
        }
        if (lrows<3 || rrows<3 || lletters<30 || rletters<30) return 0;
    }
    return cut;
}

static int plain_row(Layout *layout,size_t index,double left,double right,size_t *previous) {
    PhysicalRow row=layout->rows[index];
    size_t j,first=SIZE_MAX,last=0;
    double row_left=0;
    for (j=row.first;j<row.end;j++) if (selected(layout->cells[j],left,right)) {
        Cell c=layout->cells[j];
        if (first==SIZE_MAX) { first=c.first; row_left=c.left; }
        last=c.end;
    }
    if (first==SIZE_MAX) return 1;
    layout->soft_break=0;
    if (*previous!=SIZE_MAX) {
        PhysicalRow before=layout->rows[*previous];
        double old_left=0,old_right=0;
        int found=0;
        for (j=before.first;j<before.end;j++) if (selected(layout->cells[j],left,right)) {
            if (!found) old_left=layout->cells[j].left;
            old_right=layout->cells[j].right; found=1;
        }
        layout->soft_break=found && row.font==before.font &&
            row.top-before.bottom<.9*row.height && row.top>before.top &&
            fabs(row_left-old_left)<.6*row.height && old_right-old_left>.55*(right-left);
    }
    if (!emit_words(layout,first,last,row.font,row_left-left,0,0,0,1,0)) return 0;
    layout->block++; layout->line++; *previous=index;
    return 1;
}

/* Spanning rows divide a column region into bands: headings are emitted first,
 * then each column downwards, then the next spanning heading/footer. */
static int plain_region(Layout *layout,size_t first,size_t end,double left,double right,int depth) {
    double cut=depth<3?gutter(layout,first,end,left,right):0;
    size_t i,j,start=first,previous=SIZE_MAX;
    if (cut>left && cut<right) {
        for (i=first;i<end;i++) {
            int spans=0;
            for (j=layout->rows[i].first;j<layout->rows[i].end;j++) {
                Cell c=layout->cells[j];
                if (selected(c,left,right) && c.left<cut && c.right>cut) { spans=1; break; }
            }
            if (!spans) continue;
            if (!plain_region(layout,start,i,left,cut,depth+1) ||
                !plain_region(layout,start,i,cut,right,depth+1) ||
                !plain_row(layout,i,left,right,&previous)) return 0;
            start=i+1; previous=SIZE_MAX;
        }
        return plain_region(layout,start,end,left,cut,depth+1) &&
               plain_region(layout,start,end,cut,right,depth+1);
    }
    for (i=first;i<end;i++) if (!plain_row(layout,i,left,right,&previous)) return 0;
    return 1;
}

static int layout_page(Layout *layout,BBPage page) {
    Vec rows={0};
    size_t i,start=0;
    if (!build_rows(layout,page,&rows)) return 0;
    for (i=0;i<rows.n;) {
        Grid grid;
        size_t body,end;
        if (!find_table(layout,i,rows.n,&grid,&body,&end)) { i++; continue; }
        if (!plain_region(layout,start,i,0,layout->width,0) ||
            !emit_table(layout,i,body,end,&grid)) return 0;
        i=start=end;
    }
    return plain_region(layout,start,rows.n,0,layout->width,0);
}

static Document *parse_bb(ParserContext *parser,const BBPage *pages,size_t count,
                          const Rules *rules,int instructions,doc2struct_error *error) {
    Document *d;
    Layout layout={0};
    ArenaPool pool={0};
    size_t i;
    int ok=0;
    if (!rules || (count && !pages) || (parser && parser->closed)) {
        if (error) snprintf(error->message,sizeof(error->message),"rules, valid pages and an open parser are required");
        return NULL;
    }
    for(i=0;i<count;i++) {
        const BBPage *page=pages+i;size_t j;
        if(!isfinite(page->width) || !isfinite(page->height) || !isfinite(page->dpi) ||
           page->width<=0 || page->height<=0 || page->dpi<0 || (page->count && !page->words)) {
            if(error)snprintf(error->message,sizeof(error->message),"invalid bounding-box page");
            return NULL;
        }
        for(j=0;j<page->count;j++) {
            const doc2struct_bb_word *word=page->words+j;
            if((word->text.n && !word->text.p) || (word->flags & ~DOC2STRUCT_TEXT_STYLES) ||
               !isfinite(word->left) || !isfinite(word->top) || !isfinite(word->right) || !isfinite(word->bottom) ||
               word->left<0 || word->top<0 || word->right<=word->left || word->bottom<=word->top ||
               word->right>page->width || word->bottom>page->height) {
                if(error)snprintf(error->message,sizeof(error->message),"invalid bounding-box word");
                return NULL;
            }
        }
    }
    d=doc2struct_document_new(error);
    if (!d) return NULL;
    if (parser) d->arena.pool=&parser->pool;
    pool.limit=DS_DEFAULT_CACHE_SIZE;
    layout.document=d; layout.stream=!instructions; layout.scratch.pool=&pool;
    d->retain_table_cells=rules->unwrap_layout; d->collect_hrefs=rules->collect_hrefs;
    d->keep_single_row_tables=!rules->single_row_to_text;
    d->preserve_styles=rules->preserve_styles; d->join_wrapped_lines=rules->join_wrapped_lines;
    if (!instructions && !ds_flow_begin(d)) goto done;
    for (i=0;i<count;i++) {
        if (!layout_page(&layout,pages[i])) goto done;
        ds_arena_free(&layout.scratch); layout.scratch.pool=&pool;
    }
    ok=instructions || (ds_flow_finish(d) && ds_standardize(d,rules));
done:
    if (!ok && error) snprintf(error->message,sizeof(error->message),"%s",
        d->arena.error[0]?d->arena.error:layout.scratch.error[0]?layout.scratch.error:"bounding-box layout failed");
    ds_arena_free(&layout.scratch); ds_pool_clear(&pool);
    if (!ok) { doc2struct_document_free(d); return NULL; }
    return d;
}

Document *doc2struct_parse_bb(ParserContext *parser,const doc2struct_bb_page *pages,size_t count,
                              const Rules *rules,doc2struct_error *error) {
    return parse_bb(parser,pages,count,rules,0,error);
}

Document *doc2struct_bb_instructions(ParserContext *parser,const doc2struct_bb_page *pages,size_t count,
                                     const Rules *rules,doc2struct_error *error) {
    return parse_bb(parser,pages,count,rules,1,error);
}
