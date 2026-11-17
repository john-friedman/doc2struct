/* Connected painted rules provide stronger cell boundaries than text alignment.
 * All working data is page-local; output remains ordinary instruction runs. */
typedef struct { double position,start,end;size_t parent; } GridRule;
typedef struct { double left,top,right,bottom;size_t root,nh,nv; } RuledBox;
typedef struct { size_t parent,first_row,last_row,first_col,last_col,area,glyphs; } RuledCell;
typedef struct { Glyph glyph;size_t cell; } GridGlyph;
static int grid_rule_order(const void *a,const void *b) {
    const GridRule *x=a,*y=b;
    double xp=round(x->position*2),yp=round(y->position*2);
    if(xp!=yp)return xp<yp?-1:1;
    if(x->start!=y->start)return x->start<y->start?-1:1;
    return (x->end>y->end)-(x->end<y->end);
}
static size_t grid_rule_root(GridRule *rules,size_t i) {
    size_t root=i;
    while(rules[root].parent!=root)root=rules[root].parent;
    while(rules[i].parent!=i){size_t next=rules[i].parent;rules[i].parent=root;i=next;}
    return root;
}
static void grid_rule_join(GridRule *rules,size_t a,size_t b) {
    a=grid_rule_root(rules,a);b=grid_rule_root(rules,b);if(a!=b)rules[b].parent=a;
}
static size_t grid_merge_rules(GridRule *rules,size_t n,double tolerance) {
    size_t i,kept=0;
    if(n>1)qsort(rules,n,sizeof(*rules),grid_rule_order);
    for(i=0;i<n;i++) {
        if(kept && fabs(rules[i].position-rules[kept-1].position)<=tolerance &&
           rules[i].start<=rules[kept-1].end+tolerance) {
            rules[kept-1].end=fmax(rules[kept-1].end,rules[i].end);
        } else rules[kept++]=rules[i];
    }
    return kept;
}
static int ruled_box_order(const void *a,const void *b) {
    const RuledBox *x=a,*y=b;
    if(x->top!=y->top)return x->top<y->top?-1:1;
    return (x->left>y->left)-(x->left<y->left);
}
static int ruled_box_x(const void *a,const void *b) {
    const RuledBox *x=a,*y=b;
    return (x->left>y->left)-(x->left<y->left);
}
static size_t grid_cell_root(RuledCell *cells,size_t i) {
    size_t root=i;
    while(cells[root].parent!=root)root=cells[root].parent;
    while(cells[i].parent!=i){size_t next=cells[i].parent;cells[i].parent=root;i=next;}
    return root;
}
static void grid_cell_join(RuledCell *cells,size_t a,size_t b) {
    a=grid_cell_root(cells,a);b=grid_cell_root(cells,b);
    if(a!=b){if(a>b){size_t t=a;a=b;b=t;}cells[b].parent=a;}
}
static size_t grid_interval(const double *values,size_t n,double value) {
    size_t first=0,end=n;
    while(first<end){size_t middle=first+(end-first)/2;if(values[middle]<=value)first=middle+1;else end=middle;}
    if(!first)return 0;
    return first>=n?n-2:first-1;
}
static size_t grid_coordinates(double *values,size_t n,double tolerance) {
    size_t i,kept=0;
    if(n>1)qsort(values,n,sizeof(*values),by_number);
    for(i=0;i<n;i++)if(!kept || values[i]-values[kept-1]>tolerance)values[kept++]=values[i];
    return kept;
}
static int grid_glyph_order(const void *a,const void *b) {
    const GridGlyph *x=a,*y=b;
    if(x->cell!=y->cell)return x->cell<y->cell?-1:1;
    if(x->glyph.y!=y->glyph.y)return x->glyph.y<y->glyph.y?-1:1;
    return (x->glyph.left>y->glyph.left)-(x->glyph.left<y->glyph.left);
}
static int grid_glyph_x(const void *a,const void *b) {
    const GridGlyph *x=a,*y=b;
    return (x->glyph.left>y->glyph.left)-(x->glyph.left<y->glyph.left);
}
static size_t grid_coordinate(const double *values,size_t n,double value,double tolerance) {
    size_t i=grid_interval(values,n,value);
    if(i+1<n && fabs(values[i+1]-value)<fabs(values[i]-value))i++;
    return fabs(values[i]-value)<=tolerance?i:SIZE_MAX;
}
static int emit_ruled_box(PdfLayout *layout,const doc2struct_pdf_page *page,GridRule *rules,size_t nh,size_t nv,
                           RuledBox box,double font,double tolerance) {
    Arena arena={0};Vec horizontal={0},vertical={0};
    GridRule *h,*v;double *xs,*ys;size_t nx=0,ny=0,nr,nc,i,j,k,used=0;
    RuledCell *cells;GridGlyph *glyphs;Glyph *ordered;unsigned char *occupied,*vertical_edges,*horizontal_edges;size_t *columns;
    doc2struct_pdf_page view=*page;int ok=0;
    xs=ds_alloc_uninit(&arena,(nv+2)*sizeof(*xs));ys=ds_alloc_uninit(&arena,(nh+page->count+2)*sizeof(*ys));
    if(!xs||!ys)goto done;
    for(i=0;i<nh+nv;i++)if(grid_rule_root(rules,i)==box.root) {
        GridRule *out=ds_push(&arena,i<nh?&horizontal:&vertical,sizeof(*out));if(!out)goto done;*out=rules[i];
        if(i<nh)ys[ny++]=rules[i].position;else xs[nx++]=rules[i].position;
    }
    h=horizontal.p;v=vertical.p;nx=grid_coordinates(xs,nx,tolerance);ny=grid_coordinates(ys,ny,tolerance);
    if(nx>=3 && nx<=MAX_COLUMNS+1 && ny>=2) {
        GridGlyph *lines=ds_alloc_uninit(&arena,page->count*sizeof(*lines));
        size_t n=0;double previous_value=-1e30;
        if(!lines)goto done;
        for(i=0;i<page->count;i++) {
            Glyph g=page->glyphs[i];double x=(g.left+g.right)*.5;
            if(x>=box.left && x<box.right && g.y>=box.top && g.y<box.bottom)lines[n++]=(GridGlyph){g,0};
        }
        if(n>1)qsort(lines,n,sizeof(*lines),grid_glyph_order);
        for(i=0;i<n;) {
            size_t start=i++,values=0;uint64_t columns_with_values=0;double y=lines[start].glyph.y;
            while(i<n && lines[i].glyph.y-y<font*.45)i++;
            for(j=start;j<i;j++)if(lines[j].glyph.unicode>='0' && lines[j].glyph.unicode<='9') {
                size_t col=grid_interval(xs,nx,(lines[j].glyph.left+lines[j].glyph.right)*.5);
                if(col)columns_with_values|=UINT64_C(1)<<col;
            }
            for(;columns_with_values;columns_with_values>>=1)values+=columns_with_values&1;
            if(values>=2) {
                if(previous_value>-1e20 && y-previous_value<4*font) {
                    int boundary=0;
                    for(j=0;j<ny;j++)if(ys[j]>previous_value+font*.2 && ys[j]<y-font*.2){boundary=1;break;}
                    if(!boundary) {
                        GridRule *line=ds_push(&arena,&horizontal,sizeof(*line));if(!line)goto done;
                        *line=(GridRule){(previous_value+y)*.5,box.left,box.right,0};ys[ny++]=line->position;
                    }
                }
                previous_value=y;
            }
        }
        h=horizontal.p;ny=grid_coordinates(ys,ny,tolerance);
    }
    if(nx<3||nx>MAX_COLUMNS+1||ny<4||ny>1025)goto fallback;
    nc=nx-1;nr=ny-1;
    cells=ds_alloc(&arena,nr*nc*sizeof(*cells));glyphs=ds_alloc_uninit(&arena,page->count*sizeof(*glyphs));
    ordered=ds_alloc_uninit(&arena,page->count*sizeof(*ordered));occupied=ds_alloc(&arena,nc);columns=ds_alloc(&arena,(nc+1)*sizeof(*columns));
    vertical_edges=ds_alloc(&arena,nr*nx);horizontal_edges=ds_alloc(&arena,ny*nc);
    if(!cells||!glyphs||!ordered||!occupied||!columns||!vertical_edges||!horizontal_edges)goto done;
    for(i=0;i<vertical.n;i++) {
        size_t col=grid_coordinate(xs,nx,v[i].position,tolerance);if(col==SIZE_MAX)continue;
        for(j=0;j<nr;j++){double mid=(ys[j]+ys[j+1])*.5;if(mid>=v[i].start-tolerance && mid<=v[i].end+tolerance)vertical_edges[j*nx+col]=1;}
    }
    for(i=0;i<horizontal.n;i++) {
        size_t row=grid_coordinate(ys,ny,h[i].position,tolerance);if(row==SIZE_MAX)continue;
        for(j=0;j<nc;j++){double mid=(xs[j]+xs[j+1])*.5;if(mid>=h[i].start-tolerance && mid<=h[i].end+tolerance)horizontal_edges[row*nc+j]=1;}
    }
    for(i=0;i<nr*nc;i++)cells[i].parent=i;
    for(i=0;i<nr;i++)for(j=0;j<nc;j++) {
        if(j+1<nc && !vertical_edges[i*nx+j+1])grid_cell_join(cells,i*nc+j,i*nc+j+1);
        if(i+1<nr && !horizontal_edges[(i+1)*nc+j])grid_cell_join(cells,i*nc+j,(i+1)*nc+j);
    }
    for(i=0;i<nr*nc;i++){cells[i].first_row=cells[i].first_col=SIZE_MAX;}
    for(i=0;i<nr;i++)for(j=0;j<nc;j++) {
        RuledCell *cell=cells+grid_cell_root(cells,i*nc+j);cell->area++;
        if(i<cell->first_row)cell->first_row=i;if(i>cell->last_row)cell->last_row=i;
        if(j<cell->first_col)cell->first_col=j;if(j>cell->last_col)cell->last_col=j;
    }
    for(i=0;i<page->count;i++) {
        const Glyph *g=page->glyphs+i;double x=(g->left+g->right)*.5,y=(g->top+g->bottom)*.5;size_t cell,col;
        if(x<box.left || x>=box.right || g->y<box.top || g->y>=box.bottom)continue;
        if(g->dx<.98 || fabs(g->dy)>.1)goto fallback;
        col=grid_interval(xs,nx,x);cell=grid_cell_root(cells,grid_interval(ys,ny,y)*nc+col);
        glyphs[used++]=(GridGlyph){*g,cell};cells[cell].glyphs++;occupied[col]=1;
    }
    for(i=0;i<nr*nc;i++)if(cells[i].glyphs &&
        cells[i].area!=(cells[i].last_row-cells[i].first_row+1)*(cells[i].last_col-cells[i].first_col+1))goto fallback;
    for(i=0;i<nc;i++)columns[i+1]=columns[i]+(occupied[i] || xs[i+1]-xs[i]>=1.2*font);
    if(used>1)qsort(glyphs,used,sizeof(*glyphs),grid_glyph_order);
    {
        size_t collisions=0;
        for(i=0;i<used;) {
            size_t start=i++,tokens=0,letters=0;double y=glyphs[start].glyph.y,last=-1e30;
            while(i<used && glyphs[i].cell==glyphs[start].cell && glyphs[i].glyph.y-y<font*.45)i++;
            if(i-start>1)qsort(glyphs+start,i-start,sizeof(*glyphs),grid_glyph_x);
            for(j=start;j<i;j++) {
                const Glyph *g=&glyphs[j].glyph;uint32_t cp=g->unicode;
                letters+=(cp>='A'&&cp<='Z')||(cp>='a'&&cp<='z');
                if(cp>='0'&&cp<='9') {
                    if(g->left-last>font)tokens++;
                    last=g->right;
                } else if(cp=='%'||cp==')'||cp==','||cp=='.')last=g->right;
            }
            collisions+=!letters && tokens>=2;
        }
        /* Decorative borders may enclose several real value columns. Use the
         * text alignment path when their repeated anchors contradict the grid. */
        if(collisions>=4)goto fallback;
    }
    view.glyphs=ordered;view.count=used;
    layout->logical_cells=1;layout->soft_break=0;
    for(i=0;i<used;) {
        size_t first=i,end=i+1;RuledCell cell=cells[glyphs[i].cell];
        while(end<used && glyphs[end].cell==glyphs[first].cell)end++;
        while(i<end) {
            size_t start=i++;double baseline=glyphs[start].glyph.y,rowfont=glyphs[start].glyph.font,left=1e30,right=-1e30;
            while(i<end && fabs(glyphs[i].glyph.y-baseline)<.5*fmax(rowfont,glyphs[i].glyph.font)) {
                if(glyphs[i].glyph.font>rowfont){rowfont=glyphs[i].glyph.font;baseline=glyphs[i].glyph.y;}i++;
            }
            if(i-start>1)qsort(glyphs+start,i-start,sizeof(*glyphs),grid_glyph_x);
            for(k=start;k<i;k++){ordered[k]=glyphs[k].glyph;left=fmin(left,ordered[k].left);right=fmax(right,ordered[k].right);}
            if(!ds_pdf_emit(layout,&view,(Fragment){start,i,left,right,baseline,rowfont,1},1,
                cell.first_row,columns[cell.first_col],cell.last_row-cell.first_row+1,
                columns[cell.last_col+1]-columns[cell.first_col],0))goto done;
            layout->line++;
        }
    }
    layout->logical_cells=0;layout->block++;ok=1;goto done;
fallback:
    {
        Glyph *part=ds_alloc_uninit(&arena,page->count*sizeof(*part));size_t n=0;
        if(!part)goto done;
        for(i=0;i<page->count;i++) {
            const Glyph *g=page->glyphs+i;double x=(g->left+g->right)*.5;
            if(x>=box.left && x<box.right && g->y>=box.top && g->y<box.bottom)part[n++]=*g;
        }
        view.glyphs=part;view.count=n;ok=layout_experimental(layout,&view,2);
    }
done:
    if(arena.error[0] && !layout->document->arena.error[0])snprintf(layout->document->arena.error,256,"%s",arena.error);
    ds_arena_free(&arena);return ok;
}

/* Returns 0 to retain alignment inference, 1 after emitting all page content,
 * and -1 for an allocation/extraction failure. */
static int layout_ruled_page(PdfLayout *layout,const doc2struct_pdf_page *page,unsigned depth) {
    Arena arena={0};Vec horizontal={0},vertical={0},boxes={0};GridRule *rules;RuledBox *box;
    size_t i,j,nh,nv,kept=0;double font=0,tolerance,previous=-1e30;int result=0;
    if(page->count<40 || page->rule_count<6 || page->count>SIZE_MAX/sizeof(GridGlyph)-4098)return 0;
    for(i=0;i<page->count;i++)font+=page->glyphs[i].font;
    font/=page->count;tolerance=fmax(.5,font*.05);
    for(i=0;i<page->rule_count;i++) {
        const doc2struct_pdf_rule *r=page->rules+i;GridRule *out;double w=r->right-r->left,h=r->bottom-r->top;
        if(r->structural || r->thickness>fmax(2,font*.25))continue;
        if(h<tolerance && w>2*font) {
            out=ds_push(&arena,&horizontal,sizeof(*out));if(!out)goto error;
            *out=(GridRule){(r->top+r->bottom)*.5,r->left,r->right,0};
        } else if(w<tolerance && h>font) {
            out=ds_push(&arena,&vertical,sizeof(*out));if(!out)goto error;
            *out=(GridRule){(r->left+r->right)*.5,r->top,r->bottom,0};
        }
    }
    nh=grid_merge_rules(horizontal.p,horizontal.n,tolerance);nv=grid_merge_rules(vertical.p,vertical.n,tolerance);
    if(nh<3||nv<3||nh>4096||nv>2048)goto done;
    rules=ds_alloc_uninit(&arena,(nh+nv)*sizeof(*rules));if(!rules)goto error;
    memcpy(rules,horizontal.p,nh*sizeof(*rules));memcpy(rules+nh,vertical.p,nv*sizeof(*rules));
    for(i=0;i<nh+nv;i++)rules[i].parent=i;
    for(i=0;i<nh;i++)for(j=nh;j<nh+nv;j++)
        if(rules[j].position>=rules[i].start-tolerance && rules[j].position<=rules[i].end+tolerance &&
           rules[i].position>=rules[j].start-tolerance && rules[i].position<=rules[j].end+tolerance)grid_rule_join(rules,i,j);
    for(i=0;i<nh+nv;i++)if(grid_rule_root(rules,i)==i) {
        RuledBox *out=ds_push(&arena,&boxes,sizeof(*out));if(!out)goto error;
        *out=(RuledBox){1e30,1e30,-1e30,-1e30,i,0,0};
    }
    box=boxes.p;
    for(i=0;i<nh+nv;i++)for(j=0;j<boxes.n;j++)if(grid_rule_root(rules,i)==box[j].root) {
        if(i<nh){box[j].nh++;box[j].left=fmin(box[j].left,rules[i].start);box[j].right=fmax(box[j].right,rules[i].end);box[j].top=fmin(box[j].top,rules[i].position);box[j].bottom=fmax(box[j].bottom,rules[i].position);}
        else {box[j].nv++;box[j].left=fmin(box[j].left,rules[i].position);box[j].right=fmax(box[j].right,rules[i].position);box[j].top=fmin(box[j].top,rules[i].start);box[j].bottom=fmax(box[j].bottom,rules[i].end);}break;
    }
    if(boxes.n>256)goto done;
    if(boxes.n>1)qsort(box,boxes.n,sizeof(*box),ruled_box_order);
    /* Foot rows can have a small printed gap yet share the same column frame. */
    for(i=0;i<boxes.n;i++)if(box[i].nh && box[i].nv>=3) {
        double body_bottom=box[i].bottom;
        for(j=i+1;j<boxes.n;j++)if(box[j].nh && box[j].nv>=2 &&
            box[j].left>=box[i].left-tolerance*2 && box[j].right<=box[i].right+tolerance*2 &&
            box[j].top>=body_bottom-tolerance && box[j].top-box[i].bottom<2.5*font) {
            size_t k;int left_edge=0,right_edge=0;
            for(k=nh;k<nh+nv;k++)if(grid_rule_root(rules,k)==grid_rule_root(rules,box[i].root)) {
                left_edge|=fabs(rules[k].position-box[j].left)<tolerance*2;
                right_edge|=fabs(rules[k].position-box[j].right)<tolerance*2;
            }
            if(!left_edge||!right_edge)continue;
            grid_rule_join(rules,box[i].root,box[j].root);box[i].root=grid_rule_root(rules,box[i].root);
            box[i].bottom=fmax(box[i].bottom,box[j].bottom);box[i].nh+=box[j].nh;box[i].nv+=box[j].nv;box[j].nh=0;
        }
    }
    for(i=0;i<boxes.n;i++)if(box[i].nh>=3 && box[i].nv>=3 && box[i].bottom-box[i].top>=3*font) {
        size_t letters=0,digits=0;double first_text=1e30,last_text=-1e30;int separated=0;
        for(j=0;j<page->count;j++) {
            const Glyph *g=page->glyphs+j;double x=(g->left+g->right)*.5;
            if(x>=box[i].left && x<box[i].right && g->y>=box[i].top && g->y<box[i].bottom) {
                digits+=g->unicode>='0' && g->unicode<='9';letters+=(g->unicode>='A'&&g->unicode<='Z')||(g->unicode>='a'&&g->unicode<='z');
                first_text=fmin(first_text,x);last_text=fmax(last_text,x);
            }
        }
        /* A frame around a single text column does not define a whole table.
         * At least one internal border must separate populated text regions. */
        for(j=nh;j<nh+nv;j++)if(grid_rule_root(rules,j)==box[i].root &&
            rules[j].position>first_text+font && rules[j].position<last_text-font)separated=1;
        if(digits>=16 && letters>=8 && separated)box[kept++]=box[i];
    }
    if(!kept)goto done;
    boxes.n=kept;
    /* Overlapping drawing frames are ambiguous. Keep the existing inference. */
    for(i=0;i<kept;i++)for(j=i+1;j<kept;j++)if(box[i].left<box[j].right && box[j].left<box[i].right &&
        box[i].top<box[j].bottom && box[j].top<box[i].bottom)goto done;
    for(i=0;i<kept;) {
        size_t end=i+1,k;double bottom=box[i].bottom;
        while(end<kept && box[end].top<bottom){bottom=fmax(bottom,box[end].bottom);end++;}
        for(j=i;j<end;j++)for(k=j+1;k<end;k++)
            if(box[j].left<box[k].right && box[k].left<box[j].right)goto done;
        i=end;
    }
    for(i=0;i<kept;) {
        size_t end=i+1;double top=box[i].top,bottom=box[i].bottom,left=-1e30;
        while(end<kept && box[end].top<bottom){bottom=fmax(bottom,box[end].bottom);end++;}
        qsort(box+i,end-i,sizeof(*box),ruled_box_x);
        if(!region_part(layout,page,-1e30,previous,1e30,top,depth+1))goto error;
        for(j=i;j<end;j++) {
            if(!region_part(layout,page,left,top,box[j].left,bottom,depth+1) ||
               !region_part(layout,page,box[j].left,top,box[j].right,box[j].top,depth+1) ||
               !emit_ruled_box(layout,page,rules,nh,nv,box[j],font,tolerance) ||
               !region_part(layout,page,box[j].left,box[j].bottom,box[j].right,bottom,depth+1))goto error;
            left=box[j].right;
        }
        if(!region_part(layout,page,left,top,1e30,bottom,depth+1))goto error;
        previous=bottom;i=end;
    }
    if(!region_part(layout,page,-1e30,previous,1e30,1e30,depth+1))goto error;
    result=1;goto done;
error:
    if(arena.error[0] && !layout->document->arena.error[0])snprintf(layout->document->arena.error,256,"%s",arena.error);
    result=-1;
done:
    ds_arena_free(&arena);return result;
}
