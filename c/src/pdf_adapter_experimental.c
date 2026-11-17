#include "pdf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Page-local text and vector-rule geometry. A fragment retains its glyph slice;
 * cells group fragments independently of PDF text objects and style changes.
 * Candidate search has bounded lookahead and never compares all page pairs. */
typedef doc2struct_pdf_glyph Glyph;
typedef PdfFragment Fragment;
typedef struct {
    size_t first, end;
    double left, right, font, value_right;
    unsigned numeric, prefix, suffix, leader;
    uint32_t last;
    size_t characters;
    unsigned bold, unit;
} Cell;
typedef struct {
    size_t first, end;
    double y, font;
} PhysicalRow;
#define MAX_COLUMNS 64
#define LOOKAHEAD 8
typedef struct {
    size_t count;
    double left[MAX_COLUMNS], right[MAX_COLUMNS], boundary[MAX_COLUMNS + 1];
    unsigned numeric_column[MAX_COLUMNS];
    double font;
    int numeric, numeric_first, sidebar, tiered_header;
} Grid;
typedef struct { size_t col, span; } Placement;
typedef struct { size_t first, body, end; Grid grid; const double *bands; size_t nbands; } TableRegion;
typedef struct { size_t cell, row, line; Placement placement; size_t rowspan; } PlacedCell;

static int by_y(const void *a, const void *b) {
    const Fragment *x=a, *y=b;
    if (x->y != y->y) return x->y < y->y ? -1 : 1;
    if (x->left != y->left) return x->left < y->left ? -1 : 1;
    return (x->first > y->first) - (x->first < y->first);
}
static int by_x(const void *a, const void *b) {
    const Fragment *x=a, *y=b;
    if (x->left != y->left) return x->left < y->left ? -1 : 1;
    return (x->first > y->first) - (x->first < y->first);
}
static double gap(const Glyph *a, const Glyph *b) {
    if (a->dx > .9) return b->left-a->right;
    if (a->dx < -.9) return a->left-b->right;
    if (a->dy > .9) return b->top-a->bottom;
    return a->top-b->bottom;
}
static int same_line(const Glyph *a, const Glyph *b) {
    double dx=b->x-a->x, dy=b->y-a->y;
    return a->dx*b->dx+a->dy*b->dy > .98 &&
           fabs(dx*a->dy-dy*a->dx) <= .3*fmin(a->font,b->font) &&
           dx*a->dx+dy*a->dy > -.5*a->font;
}
static Cell fragment_cell(const Glyph *g, const Fragment *f, size_t index) {
    Cell c={index,index+1,f->left,f->right,f->font,f->right,0,0,0,0};
    size_t i, digits=0, dashes=0, marks=0, other=0, prefixes=0, suffixes=0;
    for (i=f->first;i<f->end;i++) {
        uint32_t cp=g[i].unicode;
        if (cp>='0' && cp<='9') digits++;
        else if (digits && i+1==f->end && (cp=='T' || cp=='B' || cp=='M' || cp=='K' || cp=='#')) {}
        else if (cp=='-' || cp==0x2013 || cp==0x2014) dashes++;
        else if (cp=='$' || cp==0xa3 || cp==0x20ac || cp==0xa5) prefixes++;
        else if (cp=='%') suffixes++;
        else if (cp=='*') marks++;
        else if (cp!=',' && cp!='.' && cp!='(' && cp!=')' && cp!='+' && cp!='*') other++;
    }
    c.numeric=!other && (digits || dashes || marks);
    c.prefix=prefixes && !other && !digits && !dashes && !suffixes;
    c.suffix=suffixes && !other && !digits && !dashes && !prefixes;
    c.leader=f->end-f->first>=3;
    for (i=f->first;i<f->end && c.leader;i++)
        if (g[i].unicode!='.' && g[i].unicode!='_' && g[i].unicode!=0x2026) c.leader=0;
    c.last=g[f->end-1].unicode; c.characters=f->end-f->first; c.bold=(g[f->first].flags&BOLD)!=0;
    if(c.characters<=8) {
        size_t letters=0;int valid=1,compound=0;
        for(i=f->first;i<f->end;i++) {
            uint32_t cp=g[i].unicode;
            if((cp>='A'&&cp<='Z')||(cp>='a'&&cp<='z'))letters++;
            else if((cp>='0'&&cp<='9')||cp=='/'||cp==0xb0||(cp>=0x2080&&cp<=0x2089))compound=1;
            else if(cp!=0xfffd)valid=0;
        }
        c.unit=valid && letters && (letters<=3 || compound) && ((g[f->first].unicode>='A'&&g[f->first].unicode<='Z')||(g[f->first].unicode>='a'&&g[f->first].unicode<='z'));
    }
    return c;
}
static size_t numeric_count(const Cell *c, PhysicalRow r) {
    size_t i, n=0;
    for (i=r.first+1;i<r.end;i++) n+=c[i].numeric!=0;
    return n;
}
static void make_grid(Grid *grid, const Cell *c, PhysicalRow row) {
    size_t i, used=1;
    grid->font=row.font;
    grid->sidebar=0;
    grid->tiered_header=0;
    grid->numeric=numeric_count(c,row)!=0;
    grid->numeric_first=c[row.first].numeric!=0;
    grid->left[0]=c[row.first].left;
    grid->right[0]=c[row.first].numeric?c[row.first].value_right:c[row.first].right;
    grid->numeric_column[0]=c[row.first].numeric;
    for (i=row.first+1;i<row.end;i++) {
        /* Fragmented labels (including dot leaders) do not create columns.
         * A dense numeric body row supplies one label and its value anchors. */
        if (c[i].prefix || c[i].suffix || c[i].leader) {
            if (used==1 && c[i].leader) grid->right[0]=c[i].right;
            continue;
        }
        grid->left[used]=c[i].left;
        grid->right[used]=c[i].numeric?c[i].value_right:c[i].right;
        grid->numeric_column[used]=c[i].numeric;
        used++;
    }
    grid->count=used;
    grid->boundary[0]=grid->left[0]-row.font;
    for (i=1;i<grid->count;i++)
        grid->boundary[i]=!grid->numeric_column[i-1] &&
            (grid->numeric_column[i] || !grid->numeric || grid->right[i]-grid->left[i]<5*row.font)?
            grid->left[i]-.5*row.font:(grid->right[i-1]+grid->left[i])*.5;
    grid->boundary[grid->count]=grid->right[grid->count-1]+row.font;
}
static size_t anchor(const Grid *grid, Cell cell, double *distance) {
    size_t col, best=0;
    *distance=1e30;
    for (col=0;col<grid->count;col++) {
        double d=cell.numeric && col ? fabs(cell.value_right-grid->right[col]) :
                 fmin(fabs(cell.left-grid->left[col]),fabs(cell.right-grid->right[col]));
        if (!grid->numeric)
            d=fmin(d,fabs((cell.left+cell.right-grid->left[col]-grid->right[col])*.5));
        if (d<*distance) { *distance=d; best=col; }
    }
    return best;
}
static int supports(const Grid *grid, const Cell *c, PhysicalRow row) {
    size_t i, matches=0, data=0;
    uint64_t occupied=0;
    if (row.end-row.first<2 || row.font<.5*grid->font || row.font>2.5*grid->font) return 0;
    for (i=row.first;i<row.end;i++) {
        double distance;
        size_t col=anchor(grid,c[i],&distance);
        if (distance>.7*fmax(grid->font,row.font)) continue;
        if (!(occupied & (UINT64_C(1)<<col))) {
            occupied|=UINT64_C(1)<<col;
            matches++;
            if (col && c[i].numeric) data++;
        }
    }
    return matches>=2 && (grid->numeric ? data>0 : grid->count>=3);
}
static int nearby(PhysicalRow a, PhysicalRow b) {
    return b.y-a.y>0 && b.y-a.y<=4.5*fmax(a.font,b.font);
}
static int inside(const Grid *grid, const Cell *c, PhysicalRow row, int header) {
    size_t i;
    if (row.font<.6*grid->font || row.font>1.45*grid->font) return 0;
    for (i=row.first;i<row.end;i++) {
        double distance;
        size_t col=anchor(grid,c[i],&distance);
        if (c[i].left<grid->boundary[0]-.5*grid->font ||
            c[i].right>grid->boundary[grid->count]+(header && row.end-row.first>=3?4:.5)*grid->font) return 0;
        if (header && row.end-row.first<=2 &&
            c[i].right-c[i].left>.7*(grid->right[grid->count-1]-grid->left[0])) return 0;
        if (header && row.end-row.first==1 &&
            (c[i].last==':' || (c[i].characters>35 && (c[i].last=='.' || c[i].last==';')))) return 0;
        if (!header && distance>grid->font &&
            c[i].right-c[i].left>grid->right[0]-grid->left[0]+grid->font) {
            size_t slot=0;
            while (slot+1<grid->count && c[i].left>=grid->boundary[slot+1]) slot++;
            if (c[i].right>grid->boundary[slot+1]+.5*grid->font) return 0;
        }
        if (!header && row.end-row.first==1 && !c[i].numeric && grid->numeric_column[col]) return 0;
    }
    return 1;
}

/* The body supplies the column anchors. Headers may be sparse or centered and
 * are recovered only after at least two independent body rows support a grid. */
static int find_table(const Cell *c, const PhysicalRow *r, size_t n, size_t start,
                      Grid *grid, size_t *first_body, size_t *end,int regions) {
    size_t q, j, stop=start+LOOKAHEAD<n?start+LOOKAHEAD:n, best=SIZE_MAX, quality=0;
    Grid candidate;
    if (r[start].end-r[start].first<2) return 0;
    for (q=start;q<stop;q++) {
        size_t columns=r[q].end-r[q].first, votes=0, score;
        if (q>start && !nearby(r[q-1],r[q])) break;
        if (columns<2 || columns>MAX_COLUMNS) continue;
        make_grid(&candidate,c,r[q]);
        columns=candidate.count;
        if (!candidate.numeric && candidate.numeric_first && q!=start) continue;
        if (!candidate.numeric && columns<3) continue;
        /* Long parallel prose columns are not evidence of a text table. */
        if (!candidate.numeric) {
            size_t long_cells=0;
            for (j=r[q].first;j<r[q].end;j++)
                long_cells+=c[j].right-c[j].left>18*r[q].font;
            if (long_cells>=2) continue;
        }
        for (j=start;j<n && j<q+LOOKAHEAD;j++) {
            if (j>start && !nearby(r[j-1],r[j])) break;
            if(regions && !candidate.numeric && columns>=4) {
                size_t k,matches=0;uint64_t occupied=0;
                for(k=r[j].first;k<r[j].end;k++) {
                    double distance;size_t col=anchor(&candidate,c[k],&distance);
                    if(distance<=.5*candidate.font && !(occupied&(UINT64_C(1)<<col))){occupied|=UINT64_C(1)<<col;matches++;}
                }
                votes+=matches>=3 && matches*2>=columns;
            } else votes+=supports(&candidate,c,r[j]);
        }
        if (votes<(columns==2?3u:2u)) continue;
        score=numeric_count(c,r[q])*4+columns;
        if (score>quality || (score==quality && best!=SIZE_MAX &&
            candidate.left[0]<grid->left[0]-grid->font &&
            candidate.right[0]-candidate.left[0]>1.25*(grid->right[0]-grid->left[0]))) {
            quality=score; best=q; *grid=candidate;
        }
    }
    if (best==SIZE_MAX) return 0;
    for (j=start;j<best;j++)
        if (!inside(grid,c,r[j],1)) return 0;
    *first_body=best;
    for (j=start;j<=best;j++)
        if (supports(grid,c,r[j]) && fabs(c[r[j].first].left-grid->left[0])<grid->font) {
            *first_body=j; break;
        }
    while (*first_body>start) {
        PhysicalRow before=r[*first_body-1];
        Cell head=c[before.first];
        if (before.end-before.first!=1 || !head.bold || fabs(head.left-grid->left[0])>grid->font ||
            before.font<.9*grid->font || before.font>1.2*grid->font) break;
        (*first_body)--;
    }
    *end=best+1;
    for (j=best+1;j<n && nearby(r[j-1],r[j]);j++) {
        size_t k;
        if (supports(grid,c,r[j])) { *end=j+1; continue; }
        if (!inside(grid,c,r[j],0)) break;
        /* Retain wrapped labels and section rows only when another body row
         * follows soon. A short final continuation is handled separately. */
        for (k=j+1;k<n && k<=j+3 && nearby(r[k-1],r[k]);k++)
            if (supports(grid,c,r[k])) break;
        if (k<n && k<=j+3 && supports(grid,c,r[k])) { *end=j+1; continue; }
        if (r[j].end-r[j].first==1 && c[r[j].first].left<grid->boundary[1] &&
            r[j].y-r[j-1].y<1.25*r[j].font) *end=j+1;
        break;
    }
    return *end-*first_body>=2;
}
static Placement place(const Grid *grid, Cell c, int header) {
    Placement p={0,1};
    double distance, center=(c.left+c.right)*.5;
    size_t col=anchor(grid,c,&distance), last;
    if (distance<=.7*fmax(grid->font,c.font) && !header) { p.col=col; return p; }
    while (p.col+1<grid->count && center>grid->boundary[p.col+1]) p.col++;
    if (!header) return p;
    last=p.col;
    /* Crossing a gutter is not sufficient: centered header text often extends
     * into it. Require coverage of neighboring value anchors for a span. */
    while (p.col && c.left<grid->left[p.col-1] && c.right>grid->right[p.col]) p.col--;
    while (last+1<grid->count && c.left<grid->left[last] && c.right>grid->right[last+1]) last++;
    p.span=last-p.col+1;
    return p;
}
static Placement header_place(const doc2struct_pdf_page *page,const Grid *grid,
                               const Cell *cells,PhysicalRow row,Cell cell) {
    Placement result=place(grid,cell,1);
    size_t i,k;
    double nearest=1e30;
    if(grid->tiered_header && !cell.numeric) {
        const Cell *groups[8];size_t count=0,target=SIZE_MAX,n=grid->count-1,a,b,t;
        double cost[9][MAX_COLUMNS+1],prefix[MAX_COLUMNS+1]={0};unsigned char previous[9][MAX_COLUMNS+1];
        for(i=row.first;i<row.end;i++)if(!cells[i].numeric && (cells[i].left+cells[i].right)*.5>grid->boundary[1]) {
            if(count==8){count=0;break;}if(cells[i].first==cell.first)target=count;groups[count++]=cells+i;
        }
        if(count>=2 && count<n && target!=SIZE_MAX) {
            for(i=1;i<=n;i++)prefix[i]=prefix[i-1]+(grid->left[i]+grid->right[i])*.5;
            for(a=0;a<=count;a++)for(b=0;b<=n;b++)cost[a][b]=1e30;
            cost[0][0]=0;
            for(a=1;a<=count;a++)for(b=a;b<=n;b++)for(t=a-1;t<b;t++) {
                const Cell *head=groups[a-1];double center=(prefix[b]-prefix[t])/(b-t),offset=center-(head->left+head->right)*.5;
                double next=cost[a-1][t]+offset*offset;
                if(head->left<grid->left[t+1]-2*grid->font || head->right>grid->right[b]+2*grid->font)continue;
                if(next<cost[a][b]){cost[a][b]=next;previous[a][b]=(unsigned char)t;}
            }
            if(cost[count][n]<count*grid->font*grid->font) {
                b=n;
                for(a=count;a>0;a--){t=previous[a][b];if(a-1==target)return (Placement){t+1,b-t};b=t;}
            }
        }
    }
    if (!cell.numeric || cell.characters!=4) return result;
    for (i=0;i<page->rule_count;i++) {
        const doc2struct_pdf_rule *rule=page->rules+i;
        size_t first=SIZE_MAX,last=0,headings=0;
        double dy=rule->top-row.y;
        if (dy<0 || dy>row.font || dy>nearest || cell.left<rule->left-row.font || cell.right>rule->right+row.font ||
            rule->right-rule->left>.7*(grid->right[grid->count-1]-grid->left[0])) continue;
        for (k=row.first;k<row.end;k++)
            if ((cells[k].left+cells[k].right)*.5>=rule->left && (cells[k].left+cells[k].right)*.5<=rule->right) headings++;
        if (headings!=1) continue;
        for (k=0;k<grid->count;k++)
            if (grid->right[k]>=rule->left-.5*row.font && grid->right[k]<=rule->right+.5*row.font) {
                if (first==SIZE_MAX) first=k;
                last=k;
            }
        if (first==SIZE_MAX || first==last) continue;
        result=(Placement){first,last-first+1}; nearest=dy;
    }
    return result;
}
static int emit_cell(PdfLayout *layout, const doc2struct_pdf_page *page, const Fragment *f,
                     Cell c, int table, size_t row, Placement p, int leading_space) {
    size_t i;
    for (i=c.first;i<c.end;i++) {
        int space=i==c.first?leading_space:
            page->glyphs[f[i].first].space_before || f[i].left-f[i-1].right>.3*c.font;
        if (!ds_pdf_emit(layout,page,f[i],table,row,p.col,1,p.span,space)) return 0;
    }
    return 1;
}
static int by_coordinate(const void *a, const void *b) {
    const PlacedCell *x=a, *y=b;
    if (x->row!=y->row) return x->row<y->row?-1:1;
    if (x->placement.col!=y->placement.col) return x->placement.col<y->placement.col?-1:1;
    if (x->line!=y->line) return x->line<y->line?-1:1;
    return (x->cell>y->cell)-(x->cell<y->cell);
}
static const Cell *column_cell(const Grid *grid,const Cell *c,PhysicalRow row,size_t col) {
    size_t i;
    for (i=row.first;i<row.end;i++) if (place(grid,c[i],0).col==col) return c+i;
    return NULL;
}
static size_t band_index(const double *bands,size_t count,double y) {
    size_t first=0;
    while (first<count) {
        size_t middle=first+(count-first)/2;
        if (bands[middle]<y) first=middle+1; else count=middle;
    }
    return first;
}
static int emit_table(PdfLayout *layout, const doc2struct_pdf_page *page,
                      const Fragment *f, const Cell *c, const PhysicalRow *r,
                      size_t first, size_t body, size_t end, const Grid *grid,
                      const double *bands,size_t nbands) {
    size_t i, j, row=0, count=0, capacity=r[end-1].end-r[first].first, base=layout->line;
    PlacedCell *placed;
    int previous_header_span=0,row_has_values=0;
    layout->logical_cells=1;
    if (capacity>SIZE_MAX/sizeof(*placed)) return 0;
    placed=ds_alloc_uninit(&layout->scratch,capacity*sizeof(*placed));
    if (!placed) return 0;
    layout->soft_break=0;
    for (i=first;i<end;i++) {
        int header=i<body, spans=0, continuation=0;
        int values=supports(grid,c,r[i]);
        if (header)
            for (j=r[i].first;j<r[i].end;j++) spans|=header_place(page,grid,c,r[i],c[j]).span>1;
        if (i>first) {
            int same_band=1;
            if (nbands) {
                size_t before=band_index(bands,nbands,r[i-1].y),current=band_index(bands,nbands,r[i].y);
                same_band=before==current;
            }
            if (header) {
                continuation=!spans && !previous_header_span;
                if(grid->tiered_header && i+1==body && r[i].end-r[i].first==1 &&
                   c[r[i].first].left<grid->boundary[1] && c[r[i].first].bold)continuation=0;
            } else if (i>body && r[i].y-r[i-1].y<1.7*fmax(r[i].font,r[i-1].font)) {
                size_t col=place(grid,c[r[i].first],0).col;
                const Cell *before=column_cell(grid,c,r[i-1],col),*current=c+r[i].first;
                double edge=grid->boundary[col+1],width=edge-grid->left[col];
                uint32_t last=page->glyphs[f[c[r[i-1].end-1].end-1].end-1].unicode;
                if (before && !grid->numeric_column[col]) {
                    if (values && !row_has_values && before->right>edge-.3*width && last!=':') continuation=1;
                    else if (!values && before->right>edge-.3*width && current->left>=before->left-.5*grid->font &&
                        !(page->glyphs[f[current->first].first].flags&BOLD) && last!=':') continuation=1;
                }
                if (!grid->numeric && !row_has_values && col>0 &&
                    place(grid,c[r[i-1].first],0).col==0) continuation=1;
                if (!grid->numeric && !values && col==0 && row_has_values &&
                    place(grid,c[r[i-1].first],0).col>0) continuation=1;
                if (grid->numeric_first && col>0 && row_has_values) continuation=1;
                if(grid->tiered_header && values && !row_has_values && before && before->bold &&
                   r[i-1].end-r[i-1].first==1 && before->characters<50)continuation=0;
            }
            if (nbands && !grid->numeric && same_band) continuation=1;
            if (grid->sidebar && !values) continuation=1;
            if (!same_band) continuation=0;
            if (!continuation) { row++; row_has_values=0; }
        }
        row_has_values|=values;
        for (j=r[i].first;j<r[i].end;j++) {
            Placement p=header?header_place(page,grid,c,r[i],c[j]):place(grid,c[j],0);
            /* Accounting-style currency symbols can be far from the value.
             * Their neighboring value supplies the column, not the symbol's x. */
            if (!header && c[j].prefix && j+1<r[i].end && c[j+1].numeric)
                p=place(grid,c[j+1],0);
            if (!header && c[j].suffix && j>r[i].first && c[j-1].numeric)
                p=place(grid,c[j-1],0);
            placed[count++]=(PlacedCell){j,row,base+i-first,p,1};
        }
        previous_header_span=spans;
    }
    if (grid->sidebar && nbands) {
        typedef struct { size_t first,last; } BandRows;
        BandRows *bounds;
        if (nbands>=SIZE_MAX/sizeof(*bounds)) return 0;
        bounds=ds_alloc_uninit(&layout->scratch,(nbands+1)*sizeof(*bounds));
        if (!bounds) return 0;
        for (i=0;i<=nbands;i++) bounds[i]=(BandRows){SIZE_MAX,0};
        for (i=0;i<count;i++) if (placed[i].placement.col) {
            size_t band=band_index(bands,nbands,r[placed[i].line-base+first].y);
            if (placed[i].row<bounds[band].first) bounds[band].first=placed[i].row;
            if (placed[i].row>bounds[band].last) bounds[band].last=placed[i].row;
        }
        for (i=0;i<count;i++) if (!placed[i].placement.col) {
            BandRows span=bounds[band_index(bands,nbands,r[placed[i].line-base+first].y)];
            if (span.first!=SIZE_MAX) { placed[i].row=span.first; placed[i].rowspan=span.last-span.first+1; }
        }
    }
    /* A cell's styled/wrapped parts must stay together. The flow builder's
     * fast path consumes row/column order, independently of physical lines. */
    if (count>1) qsort(placed,count,sizeof(*placed),by_coordinate);
    for (i=0;i<count;i++) {
        PlacedCell p=placed[i];
        int space=0;
        if (i && placed[i-1].row==p.row && placed[i-1].placement.col==p.placement.col &&
            placed[i-1].line==p.line) {
            Cell a=c[placed[i-1].cell], b=c[p.cell];
            space=page->glyphs[f[b.first].first].space_before || b.left-a.right>.3*b.font;
        }
        layout->line=p.line;
        if (grid->sidebar && !p.placement.col) {
            Cell cell=c[p.cell];
            for (j=cell.first;j<cell.end;j++)
                if (!ds_pdf_emit(layout,page,f[j],1,p.row,0,p.rowspan,p.placement.span,
                    j==cell.first?space:page->glyphs[f[j].first].space_before)) return 0;
        } else if (!emit_cell(layout,page,f,c[p.cell],1,p.row,p.placement,space)) return 0;
    }
    layout->line=base+end-first;
    layout->logical_cells=0;
    layout->block++;
    return 1;
}

typedef struct { double left,right,y; } HorizontalRule;
static int by_rule(const void *a,const void *b) {
    const HorizontalRule *x=a,*y=b;
    double xl=round(x->left),yl=round(y->left),xr=round(x->right),yr=round(y->right);
    if (xl!=yl) return xl<yl?-1:1;
    if (xr!=yr) return xr<yr?-1:1;
    return (x->y>y->y)-(x->y<y->y);
}
static int by_number(const void *a,const void *b) {
    double x=*(const double *)a,y=*(const double *)b;
    return (x>y)-(x<y);
}
static int by_region(const void *a,const void *b) {
    const TableRegion *x=a,*y=b;
    if (x->first!=y->first) return x->first<y->first?-1:1;
    return x->end>y->end?-1:x->end!=y->end;
}
static int ruled_regions(Arena *scratch,const doc2struct_pdf_page *page,
                         const Cell *cells,const PhysicalRow *rows,size_t n,Vec *regions,double width) {
    Vec rules={0};
    HorizontalRule *h;
    size_t i,j,k;
    for (i=0;i<page->rule_count;i++) {
        const doc2struct_pdf_rule *rule=page->rules+i;
        HorizontalRule *stored;
        if (rule->right-rule->left<.35*width) continue;
        stored=ds_push(scratch,&rules,sizeof(*stored)); if (!stored) return 0;
        *stored=(HorizontalRule){rule->left,rule->right,(rule->top+rule->bottom)*.5};
    }
    h=rules.p;
    if (rules.n>1) qsort(h,rules.n,sizeof(*h),by_rule);
    for (i=0;i<rules.n;i=j) {
        double *ys,*gaps,median_gap;
        size_t count=0,first_group;
        for (j=i+1;j<rules.n && round(h[j].left)==round(h[i].left) && round(h[j].right)==round(h[i].right);j++);
        if (j-i<3) continue;
        ys=ds_alloc_uninit(scratch,(j-i)*sizeof(double)); gaps=ds_alloc_uninit(scratch,(j-i)*sizeof(double));
        if (!ys || !gaps) return 0;
        for (k=i;k<j;k++) if (!count || h[k].y-ys[count-1]>.6) ys[count++]=h[k].y;
        if (count<3) continue;
        for (k=1;k<count;k++) gaps[k-1]=ys[k]-ys[k-1];
        qsort(gaps,count-1,sizeof(double),by_number); median_gap=gaps[(count-1)/2];
        for (first_group=0;first_group<count;) {
            size_t last_group=first_group+1,first=0,end=0,q,best=SIZE_MAX,quality=0,body;
            Grid grid;
            while (last_group<count && ys[last_group]-ys[last_group-1]<=fmax(40,4*median_gap)) last_group++;
            if (last_group-first_group<3) { first_group=last_group; continue; }
            while (first<n && rows[first].y<ys[first_group]-.5) first++;
            end=first;
            while (end<n && rows[end].y<ys[last_group-1]+.5) end++;
            /* A bounded set of seed rows keeps long ruled tables linear in
             * their row count; each seed is checked against the whole region. */
            for (q=first;q<end && q-first<2*LOOKAHEAD;q++) {
                Grid candidate;
                size_t columns=rows[q].end-rows[q].first,votes=0,score,t;
                if (columns<2 || columns>MAX_COLUMNS) continue;
                if (cells[rows[q].first].left<h[i].left-3 || cells[rows[q].end-1].right>h[i].right+3) continue;
                make_grid(&candidate,cells,rows[q]);
                for (t=first;t<end;t++) votes+=supports(&candidate,cells,rows[t]);
                if (votes<2) continue;
                score=numeric_count(cells,rows[q])*8+candidate.count;
                if (score>quality) { quality=score; best=q; grid=candidate; }
            }
            if (best!=SIZE_MAX) {
                TableRegion *region;
                size_t original=first;
                size_t side_count=0,t,u;
                double side_left=1e30,side_right=0;
                for (t=first;t<end;t++) for (u=rows[t].first;u<rows[t].end;u++)
                    if (!cells[u].numeric && cells[u].left<grid.left[0]-2*grid.font && cells[u].right<grid.left[0]-grid.font) {
                        side_count++; side_left=fmin(side_left,cells[u].left); side_right=fmax(side_right,cells[u].right);
                    }
                if (side_count>=4 && grid.count<MAX_COLUMNS) {
                    size_t kept=1;
                    for (t=1;t<grid.count;t++) {
                        size_t votes=0;
                        for (q=first;q<end;q++) for (u=rows[q].first;u<rows[q].end;u++)
                            if (cells[u].numeric && fabs(cells[u].value_right-grid.right[t])<.7*grid.font) votes++;
                        if (votes<3) continue;
                        grid.left[kept]=grid.left[t]; grid.right[kept]=grid.right[t]; grid.numeric_column[kept++]=grid.numeric_column[t];
                    }
                    grid.count=kept;
                    for (t=grid.count;t>0;t--) { grid.left[t]=grid.left[t-1]; grid.right[t]=grid.right[t-1]; grid.numeric_column[t]=grid.numeric_column[t-1]; }
                    grid.count++; grid.left[0]=side_left; grid.right[0]=side_right; grid.numeric_column[0]=0;
                    grid.sidebar=1; grid.numeric=1;
                    for (t=1;t<grid.count;t++)
                        grid.boundary[t]=!grid.numeric_column[t-1] && grid.numeric_column[t]?grid.left[t]-.5*grid.font:(grid.right[t-1]+grid.left[t])*.5;
                    grid.boundary[grid.count]=grid.right[grid.count-1]+grid.font;
                }
                grid.left[0]=fmin(grid.left[0],h[i].left);
                grid.boundary[0]=grid.left[0]-grid.font;
                while (grid.numeric && first && (!grid.sidebar || rows[first-1].end-rows[first-1].first>=2) && nearby(rows[first-1],rows[first]) &&
                       (inside(&grid,cells,rows[first-1],1) ||
                        (rows[first-1].end-rows[first-1].first==1 && cells[rows[first-1].first].bold && cells[rows[first-1].first].last==':' &&
                         cells[rows[first-1].first].right-cells[rows[first-1].first].left<.7*(grid.right[grid.count-1]-grid.left[0]) &&
                         inside(&grid,cells,rows[first-1],0)))) first--;
                while (end<n && nearby(rows[end-1],rows[end]) && supports(&grid,cells,rows[end])) end++;
                body=best;
                for (q=first;q<=best;q++)
                    if (supports(&grid,cells,rows[q]) &&
                        (grid.sidebar?column_cell(&grid,cells,rows[q],1)!=NULL:fabs(cells[rows[q].first].left-grid.left[0])<grid.font)) { body=q; break; }
                while (body>first && rows[body-1].end-rows[body-1].first==1 &&
                       cells[rows[body-1].first].bold && fabs(cells[rows[body-1].first].left-grid.left[0])<grid.font &&
                       rows[body-1].font>=.9*grid.font && rows[body-1].font<=1.2*grid.font) body--;
                while (grid.numeric && !grid.sidebar && first && original-first<LOOKAHEAD && nearby(rows[first-1],rows[first]) &&
                       inside(&grid,cells,rows[first-1],1) && !supports(&grid,cells,rows[first-1])) first--;
                region=ds_push(scratch,regions,sizeof(*region)); if (!region) return 0;
                *region=(TableRegion){first,body,end,grid,ys+first_group,last_group-first_group};
            }
            first_group=last_group;
        }
    }
    if (regions->n>1) {
        TableRegion *r=regions->p;
        size_t kept=0;
        qsort(r,regions->n,sizeof(*r),by_region);
        for (i=0;i<regions->n;i++)
            if (!kept || r[i].first>=r[kept-1].end) r[kept++]=r[i];
        regions->n=kept;
    }
    return 1;
}

static double page_gutter(const doc2struct_pdf_page *page) {
    unsigned coverage[256]={0};
    size_t i,left_words=0,right_words=0,start=0,best=0,begin=0;
    double cut;
    if (page->count<200 || page->width<=0) return 0;
    for (i=0;i<page->count;i++) {
        const Glyph *g=page->glyphs+i;
        int a,b,k;
        if (g->y<page->height*.12 || g->y>page->height*.88) continue;
        a=(int)floor(g->left/page->width*256); b=(int)ceil(g->right/page->width*256);
        if (a<0) a=0; if (b>255) b=255;
        for (k=a;k<=b;k++) if (k>=0 && k<256) coverage[k]++;
    }
    for (i=51;i<205;i++) {
        if (!coverage[i]) {
            if (!start) start=i;
            if (i-start+1>best) { begin=start; best=i-start+1; }
        } else start=0;
    }
    if (best<10) return 0;
    cut=(begin+best*.5)*page->width/256;
    /* A label/value gutter is not a page column boundary. Require substantial
     * alphabetic text on both sides, including the right-hand content body. */
    for (i=0;i<page->count;i++) {
        const Glyph *g=page->glyphs+i;
        if (g->y<page->height*.12 || g->y>page->height*.88 ||
            !((g->unicode>='A' && g->unicode<='Z') || (g->unicode>='a' && g->unicode<='z'))) continue;
        if (g->x<cut) left_words++; else right_words++;
    }
    return left_words>100 && right_words>100?cut:0;
}

static int compatible_header(const Glyph *g,const Fragment *f,const Cell *c,const PhysicalRow *r,
                               const TableRegion *head,const TableRegion *body) {
    size_t i,j,years=0;
    if(head->grid.count<5 || head->grid.count!=body->grid.count || head->end-head->first>8 ||
       body->first<head->end || body->first-head->end>3 || body->end-body->body<3 ||
       r[body->first].y-r[head->end-1].y>6*body->grid.font)return 0;
    for(i=1;i<head->grid.count;i++)
        if(fabs((head->grid.left[i]+head->grid.right[i]-body->grid.left[i]-body->grid.right[i])*.5)>2.5*body->grid.font)return 0;
    for(i=head->first;i<body->first;i++) {
        if(i>=head->end && (r[i].end-r[i].first!=1 || c[r[i].first].left>body->grid.boundary[1] ||
           c[r[i].first].characters>40 || !c[r[i].first].bold))return 0;
        for(j=r[i].first;j<r[i].end;j++)if(c[j].numeric) {
            size_t k,t,n=0,value=0;
            if(c[j].characters<=2 && c[j].font<.8*body->grid.font)continue;
            for(k=c[j].first;k<c[j].end;k++)for(t=f[k].first;t<f[k].end;t++) {
                uint32_t cp=g[t].unicode;if(cp<'0'||cp>'9'||++n>4)return 0;value=value*10+cp-'0';
            }
            if(n!=4 || value<1800 || value>2099)return 0;years++;
        }
    }
    return years>=3;
}

static int layout_experimental(void *context, const doc2struct_pdf_page *page, int depth) {
    PdfLayout *layout=context;
    Arena *scratch=&layout->scratch;
    const Glyph *g=page->glyphs;
    Vec fragments={0}, cells={0}, rows={0}, regions={0}, forced={0};
    Fragment *f;
    Cell *c;
    PhysicalRow *r;
    size_t i=0, j, previous=SIZE_MAX, region=0;
    int ok=0;
    if (depth<2) {
        double cut=page_gutter(page);
        if (cut>0) {
            Glyph *partition;
            doc2struct_pdf_rule *partition_rules=NULL;
            doc2struct_pdf_page left=*page,right=*page;
            size_t count=0,a=0,b;
            if (page->count>SIZE_MAX/sizeof(*partition)) goto done;
            partition=malloc(page->count*sizeof(*partition));
            if (!partition) goto done;
            for (i=0;i<page->count;i++) count+=page->glyphs[i].x<cut;
            b=count;
            for (i=0;i<page->count;i++) {
                Glyph glyph=page->glyphs[i];
                if (glyph.x<cut) partition[a++]=glyph;
                else { glyph.x-=cut; glyph.left-=cut; glyph.right-=cut; partition[b++]=glyph; }
            }
            left.glyphs=partition; left.count=count; left.width=cut;
            right.glyphs=partition+count; right.count=page->count-count; right.width=page->width-cut;
            left.rule_count=right.rule_count=0;
            if (page->rule_count) {
                if (page->rule_count>SIZE_MAX/2/sizeof(*partition_rules)) { free(partition); goto done; }
                partition_rules=malloc(page->rule_count*2*sizeof(*partition_rules));
                if (!partition_rules) { free(partition); goto done; }
                left.rules=partition_rules; right.rules=partition_rules+page->rule_count;
                for (i=0;i<page->rule_count;i++) {
                    doc2struct_pdf_rule rule=page->rules[i];
                    if (rule.left<cut) { rule.right=fmin(rule.right,cut); partition_rules[left.rule_count++]=rule; }
                    rule=page->rules[i];
                    if (rule.right>cut) { rule.left=fmax(rule.left,cut)-cut; rule.right-=cut; partition_rules[page->rule_count+right.rule_count++]=rule; }
                }
            }
            ok=layout_experimental(context,&left,depth+1);
            if (ok) {
                layout->origin_x+=cut;
                ok=layout_experimental(context,&right,depth+1);
                layout->origin_x-=cut;
            }
            free(partition_rules); free(partition); return ok;
        }
    }
    while (i<page->count) {
        size_t start=i++;
        Fragment *part=ds_push(scratch,&fragments,sizeof(*part));
        if (!part) goto done;
        *part=(Fragment){start,i,g[start].left,g[start].right,g[start].y,g[start].font,
                         g[start].dx>.98 && fabs(g[start].dy)<.1};
        while (i<page->count && same_line(g+i-1,g+i) &&
               !g[i].space_before &&
               fabs(g[i].font-g[i-1].font)<.1 &&
               gap(g+i-1,g+i)<.55*fmax(g[i-1].font,g[i].font)) {
            part->left=fmin(part->left,g[i].left); part->right=fmax(part->right,g[i].right);
            part->end=++i;
        }
    }
    f=fragments.p;
    if (fragments.n>1) qsort(f,fragments.n,sizeof(*f),by_y);
    for (i=0;i<fragments.n;) {
        size_t start=i++, first=cells.n;
        PhysicalRow *row;
        double baseline=f[start].y, row_font=f[start].font;
        while (i<fragments.n && f[start].horizontal && f[i].horizontal &&
               fabs(f[i].y-baseline)<.55*((layout->rules && layout->rules->pdf.region_layout &&
                   fmax(row_font,f[i].font)>2*fmin(row_font,f[i].font))?fmin(row_font,f[i].font):fmax(row_font,f[i].font))) {
            if (f[i].font>row_font && (!(layout->rules && layout->rules->pdf.region_layout) || f[i].font<=2*row_font)) { row_font=f[i].font; baseline=f[i].y; }
            i++;
        }
        if (i-start>1) qsort(f+start,i-start,sizeof(*f),by_x);
        for (j=start;j<i;j++) {
            Cell current=fragment_cell(g,f+j,j), *last=cells.n>first?(Cell *)cells.p+cells.n-1:NULL;
            double distance=last?current.left-last->right:1e30;
            int annotation=current.font<.8*row_font &&
                           f[j].end-f[j].first<=6;
            int unit=last && last->numeric && current.unit && distance<.65*row_font &&
                layout->rules && layout->rules->pdf.region_layout;
            int merge=last && f[j].horizontal && f[j-1].horizontal &&
                ((distance<.65*row_font && !(last->numeric && current.numeric && distance>.25*row_font) &&
                  !current.prefix) ||
                 (last->prefix && current.numeric && distance<2*row_font) ||
                 (last->numeric && current.suffix && distance<row_font) ||
                 (annotation && distance<row_font));
            if (merge) {
                last->end=current.end; last->right=fmax(last->right,current.right);
                if (!annotation && !unit) last->value_right=current.value_right;
                last->last=current.last; last->characters+=current.characters; last->bold&=current.bold;
                if (!annotation) last->numeric=(last->numeric || last->prefix) && (current.numeric || current.suffix || unit);
                last->prefix=0; last->suffix=0;
            } else {
                Cell *stored=ds_push(scratch,&cells,sizeof(*stored));
                if (!stored) goto done;
                *stored=current;
            }
        }
        row=ds_push(scratch,&rows,sizeof(*row));
        if (!row) goto done;
        *row=(PhysicalRow){first,cells.n,baseline,row_font};
    }
    c=cells.p; r=rows.p;
    if (!ruled_regions(scratch,page,c,r,rows.n,&forced,layout->region_width>0?layout->region_width:page->width)) goto done;
    /* Detect complete regions before emitting text. This permits recovery of
     * wrapped headers above a body without increasing the body search window. */
    for (i=0;i<rows.n;i++) {
        Grid grid;
        size_t body,end;
        TableRegion *next=region<forced.n?(TableRegion *)forced.p+region:NULL;
        if (next && next->first==i) {
            TableRegion *table=ds_push(scratch,&regions,sizeof(*table));
            if (!table) goto done;
            *table=*next; i=next->end-1; region++; continue;
        }
        if (find_table(c,r,rows.n,i,&grid,&body,&end,layout->rules && layout->rules->pdf.region_layout)) {
            size_t first=i, lower=regions.n?((TableRegion *)regions.p)[regions.n-1].end:0;
            TableRegion *table;
            if (next && end>next->first) continue;
            while (first>lower && i-first<LOOKAHEAD && nearby(r[first-1],r[first]) &&
                   inside(&grid,c,r[first-1],1) && !supports(&grid,c,r[first-1])) {
                PhysicalRow before=r[first-1];
                Cell head=c[before.first];
                if (before.end-before.first==1 && head.left<grid.boundary[1] &&
                    g[f[head.first].first].unicode!='(') break;
                first--;
            }
            if (first<i) {
                uint64_t columns=0;
                size_t h,k,distinct=0;
                for (h=first;h<body;h++)
                    for (k=r[h].first;k<r[h].end;k++) columns|=UINT64_C(1)<<place(&grid,c[k],1).col;
                for (;columns;columns>>=1) distinct+=columns&1;
                /* A lone trailing word from an unrecovered header is weak
                 * evidence. Leave it as text rather than inventing a header. */
                if (distinct<2) first=i;
            }
            table=ds_push(scratch,&regions,sizeof(*table));
            if (!table) goto done;
            *table=(TableRegion){first,body,end,grid};
            i=end-1;
        }
    }
    if(layout->rules && layout->rules->pdf.region_layout && regions.n>1) {
        TableRegion *tables=regions.p;size_t kept=0;
        for(i=0;i<regions.n;i++) {
            if(kept && compatible_header(g,f,c,r,tables+kept-1,tables+i)) {
                size_t first=tables[kept-1].first;tables[kept-1]=tables[i];tables[kept-1].first=first;tables[kept-1].grid.tiered_header=1;
            } else tables[kept++]=tables[i];
        }
        regions.n=kept;
    }
    region=0;
    if (!regions.n) {
        ds_arena_free(scratch); scratch->pool=&layout->pool;
        return ds_pdf_layout(context,page);
    }
    for (i=0;i<rows.n;) {
        TableRegion *table=region<regions.n?(TableRegion *)regions.p+region:NULL;
        if (table && table->first==i) {
            if (!emit_table(layout,page,f,c,r,i,table->body,table->end,&table->grid,table->bands,table->nbands)) goto done;
            previous=SIZE_MAX; i=table->end; region++;
            continue;
        }
        layout->soft_break=0;
        if (layout->document->join_wrapped_lines && previous!=SIZE_MAX &&
            r[i].end-r[i].first==1 && r[previous].end-r[previous].first==1) {
            Cell a=c[r[previous].first], b=c[r[i].first];
            layout->soft_break=fabs(r[i].font-r[previous].font)<.5 &&
                r[i].y-r[previous].y>0 && r[i].y-r[previous].y<=1.6*r[i].font &&
                fabs(a.left-b.left)<=.5*r[i].font && a.right-a.left>.45*(layout->region_width>0?layout->region_width:page->width);
        }
        for (j=r[i].first;j<r[i].end;j++) {
            int space=j>r[i].first && (g[f[c[j].first].first].space_before || c[j].left-c[j-1].right>.3*r[i].font);
            if (!emit_cell(layout,page,f,c[j],0,0,(Placement){0,1},space)) goto done;
        }
        previous=i; layout->block++; layout->line++; i++;
    }
    ok=1;
done:
    if (!ok && !layout->document->arena.error[0])
        snprintf(layout->document->arena.error,sizeof(layout->document->arena.error),"%s",
                 scratch->error[0]?scratch->error:"experimental PDF layout failed");
    ds_arena_free(scratch); scratch->pool=&layout->pool;
    return ok;
}
#include "pdf_regions.h"
int ds_pdf_layout_experimental(void *context, const doc2struct_pdf_page *page) {
    PdfLayout *layout=context;
    if (layout->rules && layout->rules->pdf.region_layout) return layout_regions(layout,page,0);
    return layout_experimental(context,page,0);
}
