/* Private implementation included by pdf.c so all PDFium calls use its loaded
 * ABI and engine lock. No page rendering, external fonts, or learned weights.
 * Broken CID encodings are compared with PDFium's built-in vector fonts once
 * per embedded font. Valid Unicode mappings never enter this path. */
#define SHAPE_W 20
#define SHAPE_H 24
#define SHAPE_WORDS 8
#define SHAPE_TEMPLATES 900
typedef struct {
    uint64_t bits[SHAPE_WORDS];
    double width,height,bottom,top,advance,left;
    size_t contours;
    uint32_t unicode;
} GlyphShape;
typedef struct { double ax,ay,bx,by; } GlyphEdge;
typedef struct {
    FPDF_FONT source, identity;
    uint32_t *unicode;
    double *quality;
    size_t count;
    GlyphShape *shapes;
    uint32_t *metadata;
    Map observed;
    size_t bad_codes,total_codes;
    int raw_codes;
} RecoveredFont;
typedef struct { Arena arena; Vec fonts; FPDF_DOCUMENT document; int policy; } FontRecovery;
static GlyphShape shape_templates[SHAPE_TEMPLATES];
static size_t shape_template_count;

static uint16_t font_u16(const unsigned char *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t font_u32(const unsigned char *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
#include "pdf_font_metadata.h"
static uint32_t font_cff_sid(const unsigned char *data,size_t size,size_t index,size_t sid) {
    size_t n,bytes,pos,a=0,b=0,k;
    if (sid<391) return cff_standard_unicode[sid];
    sid-=391;
    if (index>size || size-index<3) return 0;
    n=font_u16(data+index);bytes=data[index+2];pos=index+3;
    if (sid>=n || !bytes || bytes>4 || n+1>(size-pos)/bytes) return 0;
    for (k=0;k<bytes;k++) { a=(a<<8)|data[pos+sid*bytes+k]; b=(b<<8)|data[pos+(sid+1)*bytes+k]; }
    pos+=(n+1)*bytes;
    if (!a || b<a || b-1>size-pos) return 0;
    return font_named_unicode(data+pos+a-1,b-a);
}
static int font_index(const unsigned char *data,size_t size,size_t *offset,
                      const unsigned char **first,size_t *length,size_t *count) {
    size_t pos=*offset,n,bytes,i,start=0,finish=0,last=0;
    if (pos>size || size-pos<2) return 0;
    n=font_u16(data+pos); pos+=2; *count=n;
    if (!n) { *offset=pos; *first=NULL; *length=0; return 1; }
    if (pos==size || !(bytes=data[pos++]) || bytes>4 || n+1>(size-pos)/bytes) return 0;
    for (i=0;i<=n;i++) {
        size_t value=0,k;
        for (k=0;k<bytes;k++) value=(value<<8)|data[pos+i*bytes+k];
        if (!value || (i && value<last)) return 0;
        if (!i) start=value-1;
        if (i==1) finish=value-1;
        last=value;
    }
    pos+=(n+1)*bytes;
    if (last-1>size-pos || finish<start) return 0;
    *first=data+pos+start; *length=finish-start; *offset=pos+last-1;
    return 1;
}
static int font_cid_map(const unsigned char *data,size_t size,uint16_t *map,size_t *count,uint32_t *metadata) {
    size_t n,i,pos,charset=0,charstrings=0,glyphs=0,length=0,operands=0,strings=0;
    int cid_font=0;
    const unsigned char *dict;
    int stack[48];
    memset(map,0,65536*sizeof(*map)); *count=0;
    if (metadata && size>=12 && !memcmp(data,"OTTO",4)) {
        size_t cff_size=0;
        const unsigned char *cff=font_table(data,size,"CFF ",&cff_size);
        if (cff) return font_cid_map(cff,cff_size,map,count,metadata);
    }
    if (size>=12 && font_u32(data)==0x00010000) {
        n=font_u16(data+4);
        if (n>(size-12)/16) return 0;
        for (i=0;i<n;i++) {
            const unsigned char *entry=data+12+i*16;
            if (memcmp(entry,"maxp",4)) continue;
            pos=font_u32(entry+8);
            if (pos>size || size-pos<6) return 0;
            glyphs=font_u16(data+pos+4);
            if (!glyphs || glyphs>8192) return 0;
            for (i=0;i<glyphs;i++) map[i]=(uint16_t)i;
            if (metadata) { font_metadata_cmap(data,size,metadata,glyphs);font_metadata_post(data,size,metadata,glyphs); }
            *count=glyphs; return 1;
        }
        return 0;
    }
    if (size<4 || data[0]!=1 || data[2]<4 || data[2]>size) return 0;
    pos=data[2];
    if (!font_index(data,size,&pos,&dict,&length,&n) || !font_index(data,size,&pos,&dict,&length,&n) || !n) return 0;
    for (i=0;i<length;) {
        unsigned b=dict[i++]; int value;
        if (b<=21) {
            if (b==15 && operands) charset=(size_t)stack[operands-1];
            if (b==17 && operands) charstrings=(size_t)stack[operands-1];
            if (b==12) { if (i==length) return 0; if (dict[i]==30) cid_font=1; i++; }
            operands=0; continue;
        }
        if (b>=32 && b<=246) value=(int)b-139;
        else if (b>=247 && b<=250) { if (i==length) return 0; value=((int)b-247)*256+dict[i++]+108; }
        else if (b>=251 && b<=254) { if (i==length) return 0; value=-((int)b-251)*256-dict[i++]-108; }
        else if (b==28) { if (length-i<2) return 0; value=(int16_t)font_u16(dict+i); i+=2; }
        else if (b==29) { if (length-i<4) return 0; value=(int32_t)font_u32(dict+i); i+=4; }
        else if (b==30) {
            while (i<length && (dict[i]&15)!=15 && (dict[i]>>4)!=15) i++;
            if (i==length) return 0;
            i++; value=0;
        } else return 0;
        if (operands==48) return 0;
        stack[operands++]=value;
    }
    strings=pos;
    if ((!metadata && charset<=2) || charset>=size || charstrings>=size) return 0;
    pos=charstrings;
    if (!font_index(data,size,&pos,&dict,&length,&glyphs) || !glyphs || glyphs>8192) return 0;
    if (metadata && !cid_font && charset<=2) {
        for (i=0;i<glyphs;i++) {
            map[i]=(uint16_t)i;
            metadata[i]=charset==0 && i<229?cff_standard_unicode[i]:
                charset==1 && i<sizeof(cff_expert_unicode)/sizeof(*cff_expert_unicode)?cff_expert_unicode[i]:
                charset==2 && i<sizeof(cff_expert_subset_unicode)/sizeof(*cff_expert_subset_unicode)?cff_expert_subset_unicode[i]:0;
        }
        *count=glyphs;return 3;
    }
    pos=charset;
    {
        unsigned format=data[pos++]; size_t gid=1,maximum=0;
        while (gid<glyphs) {
            size_t cid,extra=0,k;
            if (size-pos<2) return 0;
            cid=font_u16(data+pos); pos+=2;
            if (format==1) { if (pos==size) return 0; extra=data[pos++]; }
            else if (format==2) { if (size-pos<2) return 0; extra=font_u16(data+pos); pos+=2; }
            else if (format) return 0;
            if (extra>=glyphs-gid || ((cid_font || !metadata) && cid+extra>=8192)) return 0;
            for (k=0;k<=extra;k++) {
                if (metadata && !cid_font) { map[gid]=(uint16_t)gid; metadata[gid]=font_cff_sid(data,size,strings,cid+k); }
                else map[cid+k]=(uint16_t)gid;
                gid++;
            }
            if (cid+extra>maximum) maximum=cid+extra;
        }
        *count=metadata && !cid_font?glyphs:maximum+1;
    }
    return metadata && !cid_font?3:2;
}
static int shape_edge(GlyphEdge *edges,size_t *n,double ax,double ay,double bx,double by) {
    if (*n==4096) return 0;
    edges[(*n)++]=(GlyphEdge){ax,ay,bx,by}; return 1;
}
static int font_shape(FPDF_FONT font,uint32_t cp,GlyphShape *out) {
    FPDF_GLYPHPATH path=p_FPDFFont_GetGlyphPath(font,cp,1000);
    GlyphEdge edges[4096];
    size_t count=0,contours=0,i,x,y;
    int n=p_FPDFGlyphPath_CountGlyphSegments(path),at=0;
    double px=0,py=0,sx=0,sy=0,left=1e30,right=-1e30,top=-1e30,bottom=1e30;
    float advance=0;
    if (n<=0 || n>2048) return 0;
    memset(out,0,sizeof(*out));
    while (at<n) {
        FPDF_PATHSEGMENT segment=p_FPDFGlyphPath_GetGlyphPathSegment(path,at++);
        float xx,yy;
        int kind=p_FPDFPathSegment_GetType(segment),close=p_FPDFPathSegment_GetClose(segment);
        if (!p_FPDFPathSegment_GetPoint(segment,&xx,&yy)) return 0;
        xx*=1000; yy*=1000;
        if (kind==2) { px=sx=xx; py=sy=yy; contours++; }
        else if (kind==0) {
            if (!shape_edge(edges,&count,px,py,xx,yy)) return 0;
            px=xx; py=yy;
        } else if (kind==1 && at+1<n) {
            float cx,cy,dx,dy;
            double ax=px,ay=py;
            int k;
            segment=p_FPDFGlyphPath_GetGlyphPathSegment(path,at++);
            if (!p_FPDFPathSegment_GetPoint(segment,&cx,&cy)) return 0;
            cx*=1000; cy*=1000;
            segment=p_FPDFGlyphPath_GetGlyphPathSegment(path,at++);
            if (!p_FPDFPathSegment_GetPoint(segment,&dx,&dy)) return 0;
            dx*=1000; dy*=1000;
            close=p_FPDFPathSegment_GetClose(segment);
            for (k=1;k<=12;k++) {
                double t=k/12.0,u=1-t,bx=u*u*u*ax+3*u*u*t*xx+3*u*t*t*cx+t*t*t*dx;
                double by=u*u*u*ay+3*u*u*t*yy+3*u*t*t*cy+t*t*t*dy;
                if (!shape_edge(edges,&count,px,py,bx,by)) return 0;
                px=bx; py=by;
            }
        } else return 0;
        if (close) {
            if (!shape_edge(edges,&count,px,py,sx,sy)) return 0;
            px=sx; py=sy;
        }
    }
    for (i=0;i<count;i++) {
        left=fmin(left,fmin(edges[i].ax,edges[i].bx)); right=fmax(right,fmax(edges[i].ax,edges[i].bx));
        bottom=fmin(bottom,fmin(edges[i].ay,edges[i].by)); top=fmax(top,fmax(edges[i].ay,edges[i].by));
    }
    if (!(right>left && top>bottom) || !isfinite(right-left+top-bottom)) return 0;
    out->width=right-left; out->height=top-bottom; out->bottom=bottom; out->top=top; out->contours=contours;out->left=left;
    if (p_FPDFFont_GetGlyphWidth(font,cp,1000,&advance)) out->advance=advance;
    for (y=0;y<SHAPE_H;y++) for (x=0;x<SHAPE_W;x++) {
        double xx=left+(x+.5)*out->width/SHAPE_W, yy=bottom+(y+.5)*out->height/SHAPE_H;
        int winding=0;
        for (i=0;i<count;i++) {
            GlyphEdge e=edges[i];
            if ((e.ay<=yy && e.by>yy) || (e.by<=yy && e.ay>yy))
                if (e.ax+(yy-e.ay)*(e.bx-e.ax)/(e.by-e.ay)>xx) winding+=e.by>e.ay?1:-1;
        }
        if (winding) { size_t bit=y*SHAPE_W+x; out->bits[bit/64]|=UINT64_C(1)<<(bit%64); }
    }
    return 1;
}
static size_t shape_popcount(uint64_t x) {
    x=x-((x>>1)&UINT64_C(0x5555555555555555));
    x=(x&UINT64_C(0x3333333333333333))+((x>>2)&UINT64_C(0x3333333333333333));
    return (size_t)((((x+(x>>4))&UINT64_C(0x0f0f0f0f0f0f0f0f))*UINT64_C(0x0101010101010101))>>56);
}
static int font_templates(void) {
    static const char *families[]={"Helvetica","Helvetica-Bold","Helvetica-Oblique","Helvetica-BoldOblique",
                                    "Times-Roman","Times-Bold","Times-Italic","Courier"};
    static const uint32_t extra[]={0x2013,0x2014,0x2018,0x2019,0x201c,0x201d,0xa3,0x20ac,0x2020,0x2022};
    FPDF_DOCUMENT doc;
    size_t f,i;
    if (shape_template_count) return 1;
    doc=p_FPDF_CreateNewDocument(); if (!doc) return 0;
    for (f=0;f<sizeof(families)/sizeof(*families);f++) {
        FPDF_FONT font=p_FPDFText_LoadStandardFont(doc,families[f]);
        if (!font) continue;
        for (i=0;i<94+sizeof(extra)/sizeof(*extra);i++) {
            uint32_t cp=i<94?(uint32_t)i+33:extra[i-94];
            GlyphShape shape;
            if (font_shape(font,cp,&shape) && shape_template_count<SHAPE_TEMPLATES) {
                shape.unicode=cp; shape_templates[shape_template_count++]=shape;
            }
        }
        p_FPDFFont_Close(font);
    }
    p_FPDF_CloseDocument(doc);
    return shape_template_count!=0;
}
static double shape_distance(const GlyphShape *a,const GlyphShape *b) {
    size_t i,difference=0;
    for (i=0;i<SHAPE_WORDS;i++) difference+=shape_popcount(a->bits[i]^b->bits[i]);
    return difference/(double)(SHAPE_W*SHAPE_H)+
        .35*(fabs(log(a->width/b->width))+fabs(log(a->height/b->height)))+
        .0005*(fabs(a->bottom-b->bottom)+fabs(a->top-b->top))+
        .0002*fabs(a->advance-b->advance)+.015*fabs((double)a->contours-b->contours);
}
static uint32_t font_match_shape(const GlyphShape *shape,double *quality) {
    uint32_t best=0xfffd;
    size_t i;
    *quality=1e30;
    for (i=0;i<shape_template_count;i++) {
        double distance=shape_distance(shape,shape_templates+i);
        if (distance<*quality) { *quality=distance; best=shape_templates[i].unicode; }
    }
    return *quality<=.4?best:0xfffd;
}
static uint32_t font_recognize(FPDF_FONT font,uint32_t cp,double *quality) {
    GlyphShape shape;
    if (!font_shape(font,cp,&shape)) { *quality=0; return ' '; }
    return font_match_shape(&shape,quality);
}
static uint32_t font_recognize_robust(FPDF_FONT font,uint32_t cp,double *quality) {
    GlyphShape shape;
    uint32_t best=0xfffd;
    double other=1e30;
    size_t i;
    if (!font_shape(font,cp,&shape)) { *quality=0;return ' '; }
    *quality=1e30;
    for(i=0;i<shape_template_count;i++) {
        double q=shape_distance(&shape,shape_templates+i);
        if(q<*quality){*quality=q;best=shape_templates[i].unicode;}
    }
    if(*quality<=.4)return best;
    if(*quality>.6)return 0xfffd;
    for(i=0;i<shape_template_count;i++)if(shape_templates[i].unicode!=best)
        other=fmin(other,shape_distance(&shape,shape_templates+i));
    return other-*quality>=.08?best:0xfffd;
}
static int font_sequence(uint32_t cp) {
    return cp>=33 && cp<=126;
}
static RecoveredFont *font_recover(FontRecovery *recovery,FPDF_FONT source) {
    RecoveredFont *font;
    size_t i,size=0,count=0,code_base=0;
    int kind;
    unsigned char *data,*encoded;
    uint16_t *map;
    char cmap[512];
    FPDF_FONT identity;
    for (i=0;i<recovery->fonts.n;i++)
        if (((RecoveredFont *)recovery->fonts.p)[i].source==source) return (RecoveredFont *)recovery->fonts.p+i;
    font=ds_push(&recovery->arena,&recovery->fonts,sizeof(*font));
    if (!font) return NULL;
    *font=(RecoveredFont){0}; font->source=source;
    if (!source || !p_FPDFFont_GetFontData(source,NULL,0,&size) || !size || size>32*1024*1024) return font;
    data=ds_alloc_uninit(&recovery->arena,size); map=ds_alloc_uninit(&recovery->arena,65536*sizeof(*map));
    if (!data || !map || !p_FPDFFont_GetFontData(source,data,size,&size)) return font;
    if (recovery->policy) {
        font->metadata=ds_alloc(&recovery->arena,65536*sizeof(*font->metadata));
        if (!font->metadata) return NULL;
    }
    kind=font_cid_map(data,size,map,&count,font->metadata);
    if (!kind) return font;
    if (!font_templates()) return font;
    if (!recovery->document) recovery->document=p_FPDF_CreateNewDocument();
    if (!recovery->document) return font;
    /* Keep synthetic TrueType character codes away from control/space codes.
     * PDFium gives those codes text semantics even in an identity font. */
    code_base=recovery->policy && kind==1?256:0;
    encoded=ds_alloc(&recovery->arena,(count+code_base)*2);
    if (!encoded) return NULL;
    for (i=0;i<count;i++) {
        /* PDFium's CFF face already maps CIDs to its subset glyph indices. */
        uint16_t index=kind==2?(uint16_t)i:map[i];
        encoded[2*(i+code_base)]=(unsigned char)(index>>8); encoded[2*(i+code_base)+1]=(unsigned char)index;
    }
    snprintf(cmap,sizeof(cmap),"/CIDInit /ProcSet findresource begin 12 dict begin begincmap "
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> def "
        "/CMapName /Recovery def /CMapType 2 def 1 begincodespacerange <0000> <FFFF> endcodespacerange "
        "1 beginbfrange <%04X> <%04X> <E000> endbfrange endcmap CMapName currentdict /CMap defineresource pop end end",
        (unsigned)code_base,(unsigned)(count+code_base-1));
    identity=p_FPDFText_LoadCidType2Font(recovery->document,data,(uint32_t)size,cmap,encoded,(uint32_t)((count+code_base)*2));
    if (!identity) return font;
    font->identity=identity; font->count=count;
    font->unicode=ds_alloc(&recovery->arena,count*sizeof(*font->unicode));
    font->quality=ds_alloc(&recovery->arena,count*sizeof(*font->quality));
    if (!font->unicode || !font->quality) return NULL;
    if (recovery->policy) {
        font->shapes=ds_alloc(&recovery->arena,count*sizeof(*font->shapes));
        if (!font->shapes) return NULL;
    }
    for (i=1;i<count;i++) if (map[i]) {
        if (font->metadata && font->metadata[i]) font->unicode[i]=font->metadata[i];
        else if (recovery->policy!=1) font->unicode[i]=recovery->policy==2?
            font_recognize_robust(identity,0xe000+(uint32_t)i,font->quality+i):
            font_recognize(identity,0xe000+(uint32_t)i,font->quality+i);
        if (font->shapes) font_shape(identity,0xe000+(uint32_t)i,font->shapes+i);
    }
    /* Resolve ambiguous straight stems and round glyphs only when two nearby
     * confident matches independently establish the same ordered ASCII run. */
    for (i=1;recovery->policy!=1 && i+1<count;i++) {
        size_t before=i-1,after=i+1;
        uint32_t guess;
        GlyphShape shape;
        if (!map[i] || !font->unicode[i] || (font->metadata && font->metadata[i])) continue;
        while (before && i-before<4 && (!font_sequence(font->unicode[before]) || font->quality[before]>.2)) before--;
        while (after<count && after-i<4 && (!font_sequence(font->unicode[after]) || font->quality[after]>.2)) after++;
        if (after==count || !font_sequence(font->unicode[before]) || !font_sequence(font->unicode[after])) continue;
        guess=font->unicode[before]+(uint32_t)(i-before);
        if (guess+(after-i)!=font->unicode[after] || !font_sequence(guess)) {
            if (after+1<count && font_sequence(font->unicode[after+1]) &&
                font->quality[after]<.2 && font->quality[after+1]<.2 &&
                font->unicode[after]+1==font->unicode[after+1] && font->unicode[after]>after-i)
                guess=font->unicode[after]-(uint32_t)(after-i);
            else continue;
        }
        if (!font_shape(identity,0xe000+(uint32_t)i,&shape)) {
            /* Some faces treat the CID equal to ASCII space as an empty path.
             * Two adjacent, confident alphabetic glyphs can still identify it. */
            if (((guess>='A' && guess<='Z') || (guess>='a' && guess<='z')) &&
                after+1<count && font->unicode[after]+1==font->unicode[after+1]) font->unicode[i]=guess;
            continue;
        }
        {
            size_t t;
            for (t=0;t<shape_template_count;t++)
                if (shape_templates[t].unicode==guess) {
                    GlyphShape target=shape_templates[t];
                    double distance=shape_distance(&shape,&target);
                    if (guess<='/' || (guess>=':' && guess<='@')) {
                        /* Commas and quotes can share an outline translated by
                         * the producer's text matrix; encoding order resolves it. */
                        target.bottom=shape.bottom; target.top=shape.top; target.advance=shape.advance;
                        distance=shape_distance(&shape,&target);
                    }
                    if (distance<font->quality[i]+.12) { font->unicode[i]=guess; break; }
                }
        }
    }
    return font;
}
static uint32_t font_decode(FontRecovery *recovery,FPDF_FONT source,uint32_t cp) {
    RecoveredFont *font=font_recover(recovery,source);
    return font && cp<font->count && font->unicode[cp]?font->unicode[cp]:cp;
}
static uint32_t font_remember(FontRecovery *recovery,RecoveredFont *font,Str key,uint32_t unicode) {
    uint32_t *value;
    if(!key.p || font->observed.n>=32768)return unicode;
    key=ds_copy(&recovery->arena,key);value=ds_alloc_uninit(&recovery->arena,sizeof(*value));
    if(key.p && value){*value=unicode;ds_map_put(&recovery->arena,&font->observed,key,value);}
    return unicode;
}
static uint32_t font_decode_geometry(FontRecovery *recovery,FPDF_FONT source,uint32_t cp,
                                     double left,double bottom,double right,double top,double advance) {
    RecoveredFont *font=font_recover(recovery,source);
    size_t i,best=0,named_best=0;
    double quality=1e30,named_quality=1e30;
    int32_t geometry[6];Str key={0};uint32_t *cached;
    if (!font || !font->shapes) return cp;
    if(isfinite(left+bottom+right+top) && fabs(left)<1000000 && fabs(bottom)<1000000 && fabs(right)<1000000 && fabs(top)<1000000) {
        geometry[0]=(int32_t)cp;geometry[1]=(int32_t)round(left/2);geometry[2]=(int32_t)round(bottom/2);
        geometry[3]=(int32_t)round((right-left)/2);geometry[4]=(int32_t)round((top-bottom)/2);
        geometry[5]=(int32_t)round(advance/2);
        key=(Str){(const char *)geometry,sizeof(geometry)};
        cached=ds_map_get(&font->observed,key);if(cached)return *cached;
    }
    if (top-bottom<2 && bottom>=-1 && top<=2) return font_remember(recovery,font,key,' ');
    if(font->raw_codes && cp<32 && cp<font->count && font->shapes[cp].width && font->unicode[cp]) {
        const GlyphShape *s=font->shapes+cp;
        double d=fabs(s->width-(right-left))+fabs(s->height-(top-bottom))+
            fabs(s->bottom-bottom)+fabs(s->top-top)+fabs(s->left-left);
        if(advance>0 && s->advance>0)d+=.5*fabs(s->advance-advance);
        if(d<50)return font_remember(recovery,font,key,font->unicode[cp]);
    }
    if (recovery->policy==2 && font->raw_codes && cp!=0xfffd) {
        GlyphShape shape;
        /* Printable fallback codes may still resolve through the PDF font's
         * own encoding. Prefer that displayed outline over a subset-index
         * guess, provided its geometry agrees with the character on the page. */
        if (font_shape(source,cp,&shape) &&
            (advance<0 || shape.advance<=0 || fabs(shape.advance-advance)<25) &&
            fabs(shape.width-(right-left))+fabs(shape.height-(top-bottom))+
            fabs(shape.bottom-bottom)+fabs(shape.top-top)+fabs(shape.left-left)<60) {
            double distance;
            double exact=1e30;size_t named=0;
            for (i=1;i<font->count;i++) if (font->unicode[i] && font->unicode[i]!=0xfffd && font->shapes[i].width) {
                GlyphShape candidate=font->shapes[i];
                double d;
                candidate.advance=shape.advance;
                d=shape_distance(&shape,&candidate);
                if(d<exact){exact=d;named=i;}
            }
            if(named && exact<.03) {
                uint32_t unicode=font->unicode[named];
                return font_remember(recovery,font,key,unicode);
            }
            uint32_t unicode=font_match_shape(&shape,&distance);
            if (distance<.28 && unicode!=0xfffd) {
                return font_remember(recovery,font,key,unicode);
            }
        }
    }
    if (font->raw_codes && cp<font->count && font->unicode[cp]==' ' && !font->shapes[cp].width) return ' ';
    /* The displayed glyph's bounds identify its actual subset glyph, even
     * when the document's character code is not the same as its glyph ID. */
    for (i=1;i<font->count;i++) {
        const GlyphShape *s=font->shapes+i;
        double distance;
        if (!s->width || !font->unicode[i]) continue;
        distance=fabs(s->width-(right-left))+fabs(s->height-(top-bottom))+
                 fabs(s->bottom-bottom)+fabs(s->top-top)+fabs(s->left-left);
        if(advance>0 && s->advance>0)distance+=.5*fabs(s->advance-advance);
        if(font->raw_codes && cp<32 && i==cp && distance<50){best=i;quality=distance;break;}
        if (i==cp) distance-=.25;
        if(font->metadata[i] && distance<named_quality){named_quality=distance;named_best=i;}
        if (distance<quality) { quality=distance;best=i; }
    }
    if(named_best && named_quality<50 && named_quality<quality+25)
        return font_remember(recovery,font,key,font->metadata[named_best]);
    if (best && quality<50) {
        uint32_t unicode=font->unicode[best];
        return font_remember(recovery,font,key,unicode);
    }
    return font_decode(recovery,source,cp);
}
/* PDFium marks non-printable fallback character codes as errors, but printable
 * codes in the same unmapped font can look like valid ASCII. Diagnose the font
 * before decoding a page, rather than repairing only its control characters. */
static void font_page_diagnose(FontRecovery *recovery,FPDF_TEXTPAGE text,size_t count) {
    size_t i;
    for (i=0;i<count;i++) {
        FPDF_PAGEOBJECT object;
        RecoveredFont *font=NULL;
        FPDF_FONT source;
        size_t j;
        int bad;
        if (p_FPDFText_IsGenerated(text,(int)i)>0 || !(object=p_FPDFText_GetTextObject(text,(int)i))) continue;
        source=p_FPDFTextObj_GetFont(object);
        bad=p_FPDFText_HasUnicodeMapError(text,(int)i)>0;
        for (j=0;j<recovery->fonts.n;j++)
            if (((RecoveredFont *)recovery->fonts.p)[j].source==source) { font=(RecoveredFont *)recovery->fonts.p+j;break; }
        if (!font && bad) font=font_recover(recovery,source);
        if (font) { font->bad_codes+=bad; font->total_codes++; }
    }
    for (i=0;i<recovery->fonts.n;i++) {
        RecoveredFont *font=(RecoveredFont *)recovery->fonts.p+i;
        if (font->bad_codes>=3 && font->bad_codes>.15*font->total_codes) font->raw_codes=1;
    }
}
static int font_has_raw_codes(FontRecovery *recovery,FPDF_FONT source) {
    size_t i;
    for (i=0;i<recovery->fonts.n;i++) {
        RecoveredFont *font=(RecoveredFont *)recovery->fonts.p+i;
        if (font->source==source) return font->raw_codes;
    }
    return 0;
}
static void font_recovery_free(FontRecovery *recovery) {
    size_t i;
    for (i=0;i<recovery->fonts.n;i++) {
        RecoveredFont *font=(RecoveredFont *)recovery->fonts.p+i;
        if (font->identity) p_FPDFFont_Close(font->identity);
    }
    if (recovery->document) p_FPDF_CloseDocument(recovery->document);
    ds_arena_free(&recovery->arena);
}
