/* Opt-in page regions. Projection occupancy is bounded independently of page
 * length. A spanning title stays above the columns it spans. Glyphs retain
 * their original page coordinates throughout recursive partitioning. */
typedef struct { double top,bottom; } RegionBand;
static int region_numeric_body(const doc2struct_pdf_page *page) {
    size_t i,numbers=0,digits=0;
    for (i=0;i<page->count;) {
        size_t first=i++,j,ndigits=0,value=0;
        int numeric=1;
        while (i<page->count && !page->glyphs[i].space_before &&
               same_line(page->glyphs+i-1,page->glyphs+i) &&
               gap(page->glyphs+i-1,page->glyphs+i)<.4*page->glyphs[i].font)i++;
        for (j=first;j<i;j++) {
            uint32_t cp=page->glyphs[j].unicode;
            if (cp>='0' && cp<='9') { ndigits++;digits++;if(ndigits<=4)value=value*10+cp-'0'; }
            else if (cp!=',' && cp!='.' && cp!='-' && cp!='(' && cp!=')' && cp!='%' && cp!='$' && cp!=0xa3 && cp!=0x20ac)numeric=0;
        }
        /* Years inside prose are weak evidence; repeated observation numbers
         * or numeric values are strong evidence of a table body. */
        if (numeric && ndigits && !(ndigits==4 && value>=1800 && value<=2099))numbers++;
    }
    return numbers>=16 || digits>.08*page->count;
}
static unsigned region_bits(uint64_t value) {
    unsigned n=0;
    while (value) { value&=value-1;n++; }
    return n;
}
static int region_columns_allowed(const doc2struct_pdf_page *page,double cut,double font,int numeric,
                                  double left,double right,double top,double bottom) {
    size_t i,own_left=0,own_right=0,crossing=0,prefixes=0,sentences[2]={0,0};
    double cross_top=1e30,cross_bottom=-1e30,last_prefix=-1e30;
    for(i=0;i<page->rule_count;i++) {
        const doc2struct_pdf_rule *r=page->rules+i;
        if(r->right-r->left<8*font || fabs(r->bottom-r->top)>font*.2 || r->thickness>font*.25)continue;
        if(r->left<cut-font && r->right>cut+font) {
            crossing++;cross_top=fmin(cross_top,r->top);cross_bottom=fmax(cross_bottom,r->bottom);
        }
        if(r->right<=cut+font && r->left<cut-8*font)own_left++;
        if(r->left>=cut-font && r->right>cut+8*font)own_right++;
    }
    /* Independent ruled grids may share baselines, but a grid that actually
     * crosses the gutter is one table. Page/header rules alone are insufficient. */
    if(crossing>=3 && cross_bottom-cross_top>.4*(bottom-top))return 0;
    if(own_left>=3 && own_right>=3)return 1;
    if(!numeric)return 1;
    {
        unsigned char labels[256]={0},values[256]={0};
        size_t paired=0,value_rows=0;
        for(i=0;i<page->count;i++) {
            const Glyph *g=page->glyphs+i;uint32_t cp=g->unicode;
            unsigned y=(unsigned)fmin(255,fmax(0,(g->y-top)/(bottom-top)*255));
            if(g->right<cut && ((cp>='A'&&cp<='Z')||(cp>='a'&&cp<='z')||(cp>='0'&&cp<='9')))labels[y]=1;
            if(g->left>cut && cp>='0'&&cp<='9')values[y]=1;
        }
        for(i=0;i<256;i++)if(values[i]) {
            value_rows++;paired+=labels[i] || (i && labels[i-1]) || (i+1<256 && labels[i+1]);
        }
        /* Repeated label/value baselines define rows across this gutter, even
         * when introductory paragraphs above the table look like prose. */
        if(paired>=8 && paired>=.6*value_rows)return 0;
    }
    for(i=0;i<page->count;) {
        size_t first=i++,j,letters=0,digits=0;uint32_t last=0;
        while(i<page->count && !page->glyphs[i].space_before &&
              same_line(page->glyphs+i-1,page->glyphs+i) && gap(page->glyphs+i-1,page->glyphs+i)<.4*page->glyphs[i].font)i++;
        for(j=first;j<i;j++) {
            uint32_t cp=page->glyphs[j].unicode;
            letters+=(cp>='A'&&cp<='Z')||(cp>='a'&&cp<='z');digits+=cp>='0'&&cp<='9';last=cp;
        }
        if(digits && !letters && page->glyphs[first].left<left+fmax(3*font,.06*(right-left)) &&
           fabs(page->glyphs[first].y-last_prefix)>.5*font){prefixes++;last_prefix=page->glyphs[first].y;}
        if(letters>=2 && (last=='.'||last=='?'||last=='!'||last==';'))
            sentences[page->glyphs[first].left>cut]++;
    }
    /* Numbered observation/name columns are row relationships. A prose body
     * beside a numeric table, however, should retain its own reading region. */
    if(prefixes>=6)return 0;
    return sentences[0]>=3 || sentences[1]>=3;
}
static double region_gutter(const doc2struct_pdf_page *page,RegionBand *bands,size_t *nbands) {
    uint64_t occupied[256][4]={{0}},rows[4]={0};
    unsigned coverage[256]={0},total=0,limit;
    double left=1e30,right=-1e30,top=1e30,bottom=-1e30,font=0,best_width=0,cut=0;
    size_t i,start=0;
    int numeric=region_numeric_body(page);
    *nbands=0;
    if (page->count<160) return 0;
    for (i=0;i<page->count;i++) {
        const Glyph *g=page->glyphs+i;
        left=fmin(left,g->left);right=fmax(right,g->right);
        top=fmin(top,g->y);bottom=fmax(bottom,g->y);font+=g->font;
    }
    font/=page->count;
    if (right-left<12*font || bottom-top<8*font) return 0;
    for (i=0;i<page->count;i++) {
        const Glyph *g=page->glyphs+i;
        int a=(int)floor((g->left-left)/(right-left)*255),b=(int)ceil((g->right-left)/(right-left)*255),x;
        unsigned y=(unsigned)fmin(255,fmax(0,(g->y-top)/(bottom-top)*255));
        if (a<0) a=0;if (b>255) b=255;
        rows[y/64]|=UINT64_C(1)<<(y%64);
        for (x=a;x<=b;x++) occupied[x][y/64]|=UINT64_C(1)<<(y%64);
    }
    for (i=0;i<4;i++) total+=region_bits(rows[i]);
    if (total<8) return 0;
    limit=total/6;
    for (i=0;i<256;i++) {
        size_t k;
        for (k=0;k<4;k++) coverage[i]+=region_bits(occupied[i][k]);
    }
    for (i=25;i<=230;i++) {
        if (i<230 && coverage[i]<=limit) { if (!start) start=i;continue; }
        if (start) {
            double width=(i-start)*(right-left)/255,candidate=left+(start+i)*.5*(right-left)/255;
            size_t letters_left=0,letters_right=0,k;
            if (width>=fmax(.65*font,.012*(right-left)) && width>best_width) {
                for (k=0;k<page->count;k++) {
                    const Glyph *g=page->glyphs+k;
                    if (!((g->unicode>='A' && g->unicode<='Z') || (g->unicode>='a' && g->unicode<='z'))) continue;
                    if (g->right<candidate) letters_left++;else if (g->left>candidate) letters_right++;
                }
                /* Label/value gutters and numeric table columns do not have
                 * substantial alphabetic bodies on both sides. */
                if (letters_left>=80 && letters_right>=80 &&
                    region_columns_allowed(page,candidate,font,numeric,left,right,top,bottom)) { cut=candidate;best_width=width; }
            }
            start=0;
        }
    }
    if (!cut) return 0;
    {
        size_t x=(size_t)fmin(255,fmax(0,(cut-left)/(right-left)*255)),y;
        for (y=0;y<256;y++) if (occupied[x][y/64]&(UINT64_C(1)<<(y%64))) {
            double baseline=top+y*(bottom-top)/255;
            RegionBand band={baseline-.75*font,baseline+.75*font};
            if (*nbands && band.top<=bands[*nbands-1].bottom) bands[*nbands-1].bottom=band.bottom;
            else if (*nbands<32) bands[(*nbands)++]=band;
            else return 0;
        }
    }
    return cut;
}
static int layout_regions(PdfLayout *,const doc2struct_pdf_page *,unsigned);
static int region_part(PdfLayout *layout,const doc2struct_pdf_page *page,
                        double left,double top,double right,double bottom,unsigned depth) {
    doc2struct_pdf_page part=*page;
    Glyph *glyphs=NULL;
    doc2struct_pdf_rule *rules=NULL;
    size_t i;
    int ok;
    double old_width=layout->region_width,minx=1e30,maxx=-1e30;
    part.count=part.rule_count=0;
    if (page->count>SIZE_MAX/sizeof(*glyphs) || page->rule_count>SIZE_MAX/sizeof(*rules)) return 0;
    glyphs=malloc(page->count*sizeof(*glyphs));
    if (!glyphs && page->count) return 0;
    for (i=0;i<page->count;i++) {
        const Glyph *g=page->glyphs+i;
        double x=(g->left+g->right)*.5;
        if (x>=left && x<right && g->y>=top && g->y<bottom) {
            glyphs[part.count++]=*g;minx=fmin(minx,g->left);maxx=fmax(maxx,g->right);
        }
    }
    if (!part.count) { free(glyphs);return 1; }
    if (page->rule_count) {
        rules=malloc(page->rule_count*sizeof(*rules));
        if (!rules) { free(glyphs);return 0; }
        for (i=0;i<page->rule_count;i++) {
            doc2struct_pdf_rule rule=page->rules[i];
            if (rule.right<left || rule.left>=right || rule.bottom<top || rule.top>=bottom) continue;
            rule.left=fmax(left,rule.left);rule.right=fmin(right,rule.right);
            rule.top=fmax(top,rule.top);rule.bottom=fmin(bottom,rule.bottom);
            rules[part.rule_count++]=rule;
        }
    }
    part.glyphs=glyphs;part.rules=rules;
    layout->region_width=maxx-minx;
    ok=layout_regions(layout,&part,depth);
    layout->region_width=old_width;
    free(rules);free(glyphs);
    return ok;
}
#include "pdf_ruled_grid.h"
static int layout_regions(PdfLayout *layout,const doc2struct_pdf_page *page,unsigned depth) {
    RegionBand bands[32];
    size_t count=0,i;
    double cut,top=-1e30;
    if(depth<6){int grid=layout_ruled_page(layout,page,depth);if(grid)return grid>0;}
    cut=depth<8?region_gutter(page,bands,&count):0;
    if (!cut) return layout_experimental(layout,page,region_numeric_body(page)?0:2);
    for (i=0;i<=count;i++) {
        double bottom=i<count?bands[i].top:1e30;
        if (!region_part(layout,page,-1e30,top,cut,bottom,depth+1) ||
            !region_part(layout,page,cut,top,1e30,bottom,depth+1)) return 0;
        if (i<count) {
            if (!region_part(layout,page,-1e30,bands[i].top,1e30,bands[i].bottom,depth+1)) return 0;
            top=bands[i].bottom;
        }
    }
    return 1;
}
