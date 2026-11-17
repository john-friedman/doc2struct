/* Bounded readers for exact font metadata. Glyph IDs are never Unicode values. */
#include "pdf_font_names.h"
static uint32_t font_named_unicode(const unsigned char *name,size_t length) {
    size_t first=0,end=sizeof(font_glyph_names)/sizeof(*font_glyph_names),i;
    uint32_t cp=0;
    for (i=0;i<length;i++) if (name[i]=='.') { length=i; break; }
    if (length==1 && name[0]>=33 && name[0]<=126) return name[0];
    if (length==7 && !memcmp(name,"uni",3)) { name+=3; length=4; }
    else if (length>=5 && length<=7 && name[0]=='u') { name++; length--; }
    else {
        while (first<end) {
            size_t middle=first+(end-first)/2,n=strlen(font_glyph_names[middle].name);
            int compare=memcmp(name,font_glyph_names[middle].name,length<n?length:n);
            if (!compare) compare=(length>n)-(length<n);
            if (compare>0) first=middle+1;
            else if (compare<0) end=middle;
            else return font_glyph_names[middle].unicode;
        }
        return 0;
    }
    for (i=0;i<length;i++) {
        unsigned c=name[i],digit=c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:16;
        if (digit==16) return 0;
        cp=(cp<<4)|digit;
    }
    return cp && cp<=0x10ffff && !(cp>=0xd800 && cp<=0xdfff)?cp:0;
}
static const unsigned char *font_table(const unsigned char *data,size_t size,const char *tag,size_t *length) {
    size_t n,i;
    if (size<12) return NULL;
    n=font_u16(data+4);if (n>(size-12)/16) return NULL;
    for (i=0;i<n;i++) {
        const unsigned char *p=data+12+i*16;
        size_t offset,len;
        if (memcmp(p,tag,4)) continue;
        offset=font_u32(p+8);len=font_u32(p+12);
        if (offset>size || len>size-offset) return NULL;
        *length=len;return data+offset;
    }
    return NULL;
}
static void font_metadata_cmap(const unsigned char *data,size_t size,uint32_t *unicode,size_t glyphs) {
    size_t length=0,n,i,budget=1048576;
    const unsigned char *table=font_table(data,size,"cmap",&length);
    if (!table || length<4) return;
    n=font_u16(table+2);if (n>(length-4)/8) return;
    for (i=0;i<n && budget;i++) {
        const unsigned char *entry=table+4+i*8,*sub;
        size_t offset=font_u32(entry+4),bytes,j;
        unsigned platform=font_u16(entry),encoding=font_u16(entry+2),format;
        if (platform!=0 && !(platform==3 && (encoding==1 || encoding==10))) continue;
        if (offset>length || length-offset<4) continue;
        sub=table+offset;format=font_u16(sub);
        bytes=format>=8?(length-offset>=8?font_u32(sub+4):0):font_u16(sub+2);
        if (bytes<4 || bytes>length-offset) continue;
        if (format==4 && bytes>=16) {
            size_t segments=font_u16(sub+6)/2;
            if (!segments || segments>(bytes-16)/8) continue;
            for (j=0;j<segments;j++) {
                uint32_t end=font_u16(sub+14+2*j),start=font_u16(sub+16+2*segments+2*j),cp;
                uint16_t delta=font_u16(sub+16+4*segments+2*j),range=font_u16(sub+16+6*segments+2*j);
                if (end<start || end==65535) continue;
                for (cp=start;cp<=end && budget;cp++,budget--) {
                    size_t gid;
                    if (range) {
                        size_t address=16+6*segments+2*j+range+2*(cp-start);
                        if (address>bytes || bytes-address<2) break;
                        gid=font_u16(sub+address);if (gid) gid=(gid+delta)&65535;
                    } else gid=(cp+delta)&65535;
                    if (gid && gid<glyphs && (!unicode[gid] || (cp>=32 && cp<127))) unicode[gid]=cp;
                }
            }
        } else if (format==12 && bytes>=16) {
            size_t groups=font_u32(sub+12);
            if (groups>(bytes-16)/12) continue;
            for (j=0;j<groups;j++) {
                const unsigned char *g=sub+16+j*12;
                uint32_t start=font_u32(g),end=font_u32(g+4),gid=font_u32(g+8),k;
                if (end<start || end>0x10ffff || gid>=glyphs) continue;
                for (k=0;k<glyphs-gid && k<=end-start && budget;k++,budget--)
                    if (!unicode[gid+k] || (start+k>=32 && start+k<127)) unicode[gid+k]=start+k;
            }
        } else if (format==0 && bytes>=262) {
            for (j=0;j<256;j++) if (sub[6+j] && sub[6+j]<glyphs && !unicode[sub[6+j]]) unicode[sub[6+j]]=(uint32_t)j;
        }
    }
}
static void font_metadata_post(const unsigned char *data,size_t size,uint32_t *unicode,size_t glyphs) {
    size_t length=0,i;
    const unsigned char *table=font_table(data,size,"post",&length);
    if (!table || length<32) return;
    if (font_u32(table)==0x00010000) {
        for (i=0;i<glyphs && i<258;i++) if (!unicode[i]) unicode[i]=post_standard_unicode[i];
    } else if (font_u32(table)==0x00020000 && length>=34) {
        size_t n=font_u16(table+32),position,custom=0;
        uint32_t *names;
        if (n>(length-34)/2) return;
        names=calloc(65536-258,sizeof(*names));if(!names)return;
        for (i=0;i<n && i<glyphs;i++) {
            size_t index=font_u16(table+34+2*i);
            if (index<258 && !unicode[i]) unicode[i]=post_standard_unicode[index];
        }
        position=34+2*n;
        while (position<length && custom<65536-258) {
            size_t bytes=table[position++];
            if (bytes>length-position) break;
            names[custom]=font_named_unicode(table+position,bytes);
            position+=bytes;custom++;
        }
        for (i=0;i<n && i<glyphs;i++) {
            size_t index=font_u16(table+34+2*i);
            if (index>=258 && index-258<custom && !unicode[i])unicode[i]=names[index-258];
        }
        free(names);
    }
}
