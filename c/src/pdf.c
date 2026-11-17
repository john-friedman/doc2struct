#include "pdf.h"
#include "fpdf_text.h"
#include "bridge.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static SRWLOCK engine_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&engine_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&engine_lock)
#else
#include <dlfcn.h>
#include <pthread.h>
static pthread_mutex_t engine_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&engine_lock)
#define UNLOCK() pthread_mutex_unlock(&engine_lock)
#endif

/* Function pointers retain the official ABI, including 32-bit Windows stdcall. */
#define PDF_FUNCTIONS(X) \
 X(void, FPDF_InitLibrary, (void)) \
 X(FPDF_DOCUMENT, FPDF_CreateNewDocument, (void)) \
 X(FPDF_DOCUMENT, FPDF_LoadMemDocument64, (const void *, size_t, FPDF_BYTESTRING)) \
 X(void, FPDF_CloseDocument, (FPDF_DOCUMENT)) \
 X(unsigned long, FPDF_GetLastError, (void)) \
 X(int, FPDF_GetPageCount, (FPDF_DOCUMENT)) \
 X(FPDF_PAGE, FPDF_LoadPage, (FPDF_DOCUMENT, int)) \
 X(FPDF_PAGE, DS_PDF_LoadTextPage, (FPDF_DOCUMENT, int)) \
 X(void, FPDF_ClosePage, (FPDF_PAGE)) \
 X(float, FPDF_GetPageWidthF, (FPDF_PAGE)) \
 X(float, FPDF_GetPageHeightF, (FPDF_PAGE)) \
 X(FPDF_BOOL, FPDF_PageToDevice, (FPDF_PAGE, int, int, int, int, int, double, double, int *, int *)) \
 X(FPDF_TEXTPAGE, FPDFText_LoadPage, (FPDF_PAGE)) \
 X(void, FPDFText_ClosePage, (FPDF_TEXTPAGE)) \
 X(int, FPDFText_CountChars, (FPDF_TEXTPAGE)) \
 X(unsigned int, FPDFText_GetUnicode, (FPDF_TEXTPAGE, int)) \
 X(int, FPDFText_IsGenerated, (FPDF_TEXTPAGE, int)) \
 X(int, FPDFText_HasUnicodeMapError, (FPDF_TEXTPAGE, int)) \
 X(FPDF_PAGEOBJECT, FPDFText_GetTextObject, (FPDF_TEXTPAGE, int)) \
 X(double, FPDFText_GetFontSize, (FPDF_TEXTPAGE, int)) \
 X(unsigned long, FPDFText_GetFontInfo, (FPDF_TEXTPAGE, int, void *, unsigned long, int *)) \
 X(int, FPDFText_GetFontWeight, (FPDF_TEXTPAGE, int)) \
 X(FPDF_BOOL, FPDFText_GetMatrix, (FPDF_TEXTPAGE, int, FS_MATRIX *)) \
 X(FPDF_BOOL, FPDFText_GetCharBox, (FPDF_TEXTPAGE, int, double *, double *, double *, double *)) \
 X(FPDF_BOOL, FPDFText_GetCharOrigin, (FPDF_TEXTPAGE, int, double *, double *)) \
 X(FPDF_FONT, FPDFTextObj_GetFont, (FPDF_PAGEOBJECT)) \
 X(FPDF_BOOL, FPDFFont_GetFontData, (FPDF_FONT, unsigned char *, size_t, size_t *)) \
 X(FPDF_FONT, FPDFText_LoadStandardFont, (FPDF_DOCUMENT, const char *)) \
 X(FPDF_FONT, FPDFText_LoadCidType2Font, (FPDF_DOCUMENT, const unsigned char *, uint32_t, const char *, const unsigned char *, uint32_t)) \
 X(void, FPDFFont_Close, (FPDF_FONT)) \
 X(FPDF_BOOL, FPDFFont_GetGlyphWidth, (FPDF_FONT, uint32_t, float, float *)) \
 X(FPDF_GLYPHPATH, FPDFFont_GetGlyphPath, (FPDF_FONT, uint32_t, float)) \
 X(int, FPDFGlyphPath_CountGlyphSegments, (FPDF_GLYPHPATH)) \
 X(FPDF_PATHSEGMENT, FPDFGlyphPath_GetGlyphPathSegment, (FPDF_GLYPHPATH, int)) \
 X(FPDF_BOOL, FPDFPathSegment_GetPoint, (FPDF_PATHSEGMENT, float *, float *)) \
 X(int, FPDFPathSegment_GetType, (FPDF_PATHSEGMENT)) \
 X(FPDF_BOOL, FPDFPathSegment_GetClose, (FPDF_PATHSEGMENT)) \
 X(int, FPDFPage_CountObjects, (FPDF_PAGE)) \
 X(FPDF_PAGEOBJECT, FPDFPage_GetObject, (FPDF_PAGE, int)) \
 X(int, FPDFPageObj_GetType, (FPDF_PAGEOBJECT)) \
 X(FPDF_BOOL, FPDFPageObj_GetMatrix, (FPDF_PAGEOBJECT, FS_MATRIX *)) \
 X(FPDF_BOOL, FPDFPageObj_GetStrokeWidth, (FPDF_PAGEOBJECT, float *)) \
 X(FPDF_BOOL, FPDFPath_GetDrawMode, (FPDF_PAGEOBJECT, int *, FPDF_BOOL *)) \
 X(int, FPDFPage_GetAnnotCount, (FPDF_PAGE)) \
 X(FPDF_ANNOTATION, FPDFPage_GetAnnot, (FPDF_PAGE, int)) \
 X(void, FPDFPage_CloseAnnot, (FPDF_ANNOTATION)) \
 X(FPDF_ANNOTATION_SUBTYPE, FPDFAnnot_GetSubtype, (FPDF_ANNOTATION)) \
 X(size_t, FPDFAnnot_CountAttachmentPoints, (FPDF_ANNOTATION)) \
 X(FPDF_BOOL, FPDFAnnot_GetAttachmentPoints, (FPDF_ANNOTATION, size_t, FS_QUADPOINTSF *)) \
 X(int, FPDFPath_CountSegments, (FPDF_PAGEOBJECT)) \
 X(FPDF_PATHSEGMENT, FPDFPath_GetPathSegment, (FPDF_PAGEOBJECT, int)) \
 X(int, FPDFFormObj_CountObjects, (FPDF_PAGEOBJECT)) \
 X(FPDF_PAGEOBJECT, FPDFFormObj_GetObject, (FPDF_PAGEOBJECT, unsigned long)) \
 X(unsigned, DS_PDF_BridgeVersion, (void)) \
 X(int, DS_PDF_ExtractPage, (FPDF_PAGE, DS_PDF_SINK, void *))
#define DECLARE(result, name, args) static result (FPDF_CALLCONV *p_##name) args;
PDF_FUNCTIONS(DECLARE)
#undef DECLARE
static char *engine_path;
_Static_assert(sizeof(DS_PDF_GLYPH) == sizeof(doc2struct_pdf_glyph), "PDF bridge glyph size");
_Static_assert(offsetof(DS_PDF_GLYPH, left) == offsetof(doc2struct_pdf_glyph, left), "PDF bridge geometry offset");
_Static_assert(offsetof(DS_PDF_GLYPH, dy) == offsetof(doc2struct_pdf_glyph, dy), "PDF bridge direction offset");
#include "pdf_font_recovery.h"

static int fail(doc2struct_error *error, const char *message) {
    if (error) snprintf(error->message, sizeof(error->message), "%s", message);
    return 0;
}

int doc2struct_pdfium_init(const char *path, doc2struct_error *error) {
#ifdef _WIN32
    HMODULE library = NULL;
    wchar_t *wide = NULL;
    int length;
#else
    void *library = NULL;
#endif
    char *copy = NULL;
    int ok = 0;
    if (error) error->message[0] = 0;
    if (!path || !*path) return fail(error, "PDFium library path is required");
    LOCK();
    if (engine_path) {
        ok = !strcmp(path, engine_path);
        if (!ok) fail(error, "PDFium is already initialized from a different path");
        goto done;
    }
    copy = malloc(strlen(path) + 1);
    if (!copy) { fail(error, "out of memory"); goto done; }
    memcpy(copy, path, strlen(path) + 1);
#ifdef _WIN32
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (length) wide = malloc((size_t)length * sizeof(*wide));
    if (!wide) { fail(error, "invalid PDFium path or out of memory"); goto done; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, length);
    library = LoadLibraryExW(wide, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!library) { fail(error, "cannot load required PDFium shared library"); goto done; }
#ifdef _WIN32
#define SYMBOL(name) GetProcAddress(library, #name)
#else
#define SYMBOL(name) dlsym(library, #name)
#endif
#define LOAD(result, name, args) \
    p_##name = (result (FPDF_CALLCONV *) args)SYMBOL(name); \
    if (!p_##name) { fail(error, "PDFium is missing " #name "; use the doc2struct PDFium build"); goto done; }
    PDF_FUNCTIONS(LOAD)
#undef LOAD
#undef SYMBOL
    if (p_DS_PDF_BridgeVersion() != DS_PDF_BRIDGE_ABI) {
        fail(error, "incompatible doc2struct PDFium bridge version"); goto done;
    }
    p_FPDF_InitLibrary();
    engine_path = copy;
    copy = NULL;
    ok = 1;
done:
    free(copy);
#ifdef _WIN32
    free(wide);
    if (!ok && library) FreeLibrary(library);
#else
    if (!ok && library) dlclose(library);
#endif
    UNLOCK();
    return ok;
}

typedef struct { double a, b, c, d, e, f; } Transform;
static void point(Transform m, double x, double y, double *out_x, double *out_y) {
    *out_x = m.a * x + m.c * y + m.e;
    *out_y = m.b * x + m.d * y + m.f;
}

/* Calibrate the page transform three times, not once per glyph. Device scaling
 * keeps rounding below 1/64 point and includes crop boxes and page rotation. */
static int transform(FPDF_PAGE page, double width, double height, Transform *m) {
    int x[3], y[3], i;
    const double px[] = {0, 1024, 0}, py[] = {0, 0, 1024};
    if (!(width > 0 && height > 0 && width < 1000000 && height < 1000000)) return 0;
    for (i = 0; i < 3; i++)
        if (!p_FPDF_PageToDevice(page, 0, 0, (int)ceil(width * 64),
                                (int)ceil(height * 64), 0, px[i], py[i], x+i, y+i)) return 0;
    *m = (Transform){(x[1]-(double)x[0])/65536, (y[1]-(double)y[0])/65536,
                     (x[2]-(double)x[0])/65536, (y[2]-(double)y[0])/65536,
                     x[0]/64.0, y[0]/64.0};
    return 1;
}
static int pdf_rule(Arena *arena,Vec *rules,Transform m,double ax,double ay,double bx,double by,
                    double thickness,int decorations,int structural) {
    doc2struct_pdf_rule *rule;
    double x1,y1,x2,y2;
    point(m,ax,ay,&x1,&y1); point(m,bx,by,&x2,&y2);
    if (!isfinite(x1+y1+x2+y2)) return 1;
    if (fabs(y1-y2)>.4 && (!decorations || fabs(x1-x2)>.4)) return 1;
    if (hypot(x1-x2,y1-y2)<(decorations?3:20)) return 1;
    rule=ds_push(arena,rules,sizeof(*rule));
    if (!rule) return 0;
    *rule=(doc2struct_pdf_rule){fmin(x1,x2),fmin(y1,y2),fmax(x1,x2),fmax(y1,y2),thickness,structural};
    return 1;
}
static int pdf_object_rules(Arena *arena,Vec *rules,FPDF_PAGEOBJECT object,Transform parent,unsigned depth,int decorations) {
    FS_MATRIX a={1,0,0,1,0,0};
    Transform m;
    int kind=p_FPDFPageObj_GetType(object),i,n;
    if (depth>40) return 1;
    p_FPDFPageObj_GetMatrix(object,&a);
    m=(Transform){parent.a*a.a+parent.c*a.b,parent.b*a.a+parent.d*a.b,
                  parent.a*a.c+parent.c*a.d,parent.b*a.c+parent.d*a.d,
                  parent.a*a.e+parent.c*a.f+parent.e,parent.b*a.e+parent.d*a.f+parent.f};
    if (kind==5) {
        n=p_FPDFFormObj_CountObjects(object);
        for (i=0;i<n;i++)
            if (!pdf_object_rules(arena,rules,p_FPDFFormObj_GetObject(object,(unsigned long)i),m,depth+1,decorations)) return 0;
    } else if (kind==2) {
        double px=0,py=0,sx=0,sy=0;
        int previous=0;
        int fill=0;FPDF_BOOL stroke=0;float width=0;
        p_FPDFPath_GetDrawMode(object,&fill,&stroke);
        p_FPDFPageObj_GetStrokeWidth(object,&width);
        width=(float)(width*fmax(hypot(m.a,m.b),hypot(m.c,m.d)));
        n=p_FPDFPath_CountSegments(object);
        if (n>100000) return 1;
        if (decorations && fill && !stroke) {
            double left=1e30,right=-1e30,top=1e30,bottom=-1e30;
            for (i=0;i<n;i++) {
                float x,y;double xx,yy;
                if (!p_FPDFPathSegment_GetPoint(p_FPDFPath_GetPathSegment(object,i),&x,&y)) continue;
                point(m,x,y,&xx,&yy);left=fmin(left,xx);right=fmax(right,xx);top=fmin(top,yy);bottom=fmax(bottom,yy);
            }
            width=(float)fmax(width,fmin(right-left,bottom-top));
        }
        for (i=0;i<n;i++) {
            FPDF_PATHSEGMENT segment=p_FPDFPath_GetPathSegment(object,i);
            float x,y;
            int type=p_FPDFPathSegment_GetType(segment);
            if (!p_FPDFPathSegment_GetPoint(segment,&x,&y)) continue;
            if (type==2) { sx=x; sy=y; }
            if (type==0 && previous && !pdf_rule(arena,rules,m,px,py,x,y,width,decorations,!fill&&!stroke)) return 0;
            if (p_FPDFPathSegment_GetClose(segment) && !pdf_rule(arena,rules,m,x,y,sx,sy,width,decorations,!fill&&!stroke)) return 0;
            px=x; py=y; previous=type!=1;
        }
    }
    return 1;
}
static int pdf_page_rules(Arena *arena,FPDF_PAGE page,doc2struct_pdf_page *out,int decorations) {
    Vec rules={0};
    Transform m;
    int i,n=p_FPDFPage_CountObjects(page);
    if (!transform(page,out->width,out->height,&m)) return 0;
    for (i=0;i<n;i++)
        if (!pdf_object_rules(arena,&rules,p_FPDFPage_GetObject(page,i),m,0,decorations)) return 0;
    out->rules=rules.p; out->rule_count=rules.n;
    return 1;
}
#include "pdf_decorations.h"

static int extract_page(FPDF_PAGE page, FPDF_TEXTPAGE text,
                        doc2struct_pdf_page *out, doc2struct_pdf_glyph *glyphs, FontRecovery *recovery) {
    Transform m;
    FPDF_PAGEOBJECT previous = NULL;
    double font = 12, dx = 1, dy = 0;
    unsigned flags = 0;
    int space_before = 0;
    size_t i, used = 0;
    out->width = p_FPDF_GetPageWidthF(page);
    out->height = p_FPDF_GetPageHeightF(page);
    if (!transform(page, out->width, out->height, &m)) return 0;
    if (recovery && recovery->policy) font_page_diagnose(recovery,text,out->count);
    for (i = 0; i < out->count; i++) {
        doc2struct_pdf_glyph *g = glyphs + used;
        int index = (int)i;
        uint32_t cp = p_FPDFText_GetUnicode(text, index);
        FPDF_PAGEOBJECT object = NULL;
        double l, r, t, b, x, y, xx[4], yy[4];
        int k;
        if (recovery) {
            if (p_FPDFText_IsGenerated(text,index)>0) {
                if (ds_space(cp)) space_before=1;
                continue;
            }
            object=p_FPDFText_GetTextObject(text,index);
            if (object && (p_FPDFText_HasUnicodeMapError(text,index)>0 ||
                (recovery->policy && (cp==0xfffd || font_has_raw_codes(recovery,p_FPDFTextObj_GetFont(object)))))) {
                if (!recovery->policy) cp=font_decode(recovery,p_FPDFTextObj_GetFont(object),cp);
                else {
                    FS_MATRIX matrix;
                    double ll,rr,bb,tt,ox,oy,scale=p_FPDFText_GetFontSize(text,index);
                    if (scale>0 && p_FPDFText_GetMatrix(text,index,&matrix) &&
                        p_FPDFText_GetCharBox(text,index,&ll,&rr,&bb,&tt) && p_FPDFText_GetCharOrigin(text,index,&ox,&oy)) {
                        double det=matrix.a*matrix.d-matrix.b*matrix.c;
                        if (fabs(det)>1e-12) {
                            double minx=1e30,maxx=-1e30,miny=1e30,maxy=-1e30;
                            int corner;
                            for (corner=0;corner<4;corner++) {
                                double px=(corner&1?rr:ll)-ox,py=(corner&2?tt:bb)-oy;
                                double gx=(matrix.d*px-matrix.c*py)/det*1000/scale;
                                double gy=(-matrix.b*px+matrix.a*py)/det*1000/scale;
                                minx=fmin(minx,gx);maxx=fmax(maxx,gx);miny=fmin(miny,gy);maxy=fmax(maxy,gy);
                            }
                            double advance=-1,nx,ny;
                            if(index+1<(int)out->count && p_FPDFText_IsGenerated(text,index+1)<=0 &&
                               p_FPDFText_GetTextObject(text,index+1)==object &&
                               p_FPDFText_GetCharOrigin(text,index+1,&nx,&ny)) {
                                double measured=(matrix.d*(nx-ox)-matrix.c*(ny-oy))/det*1000/scale;
                                if(isfinite(measured) && measured>1 && measured<2000)advance=measured;
                            }
                            cp=font_decode_geometry(recovery,p_FPDFTextObj_GetFont(object),cp,minx,miny,maxx,maxy,advance);
                        }
                    }
                }
            }
        }
        if (!cp || cp < 32 || ds_space(cp)) {
            if (cp && ds_space(cp)) space_before = 1;
            continue;
        }
        /* PDFium may expose a ToUnicode supplementary mapping as UTF-16 units. */
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < out->count) {
            uint32_t low = p_FPDFText_GetUnicode(text, (int)i + 1);
            if (low >= 0xdc00 && low <= 0xdfff) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                i++;
            }
        }
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
        if (!object) object = p_FPDFText_GetTextObject(text, index);
        if (!object) continue; /* PDFium-generated separators have no geometry. */
        if (object != previous) {
            FS_MATRIX matrix = {1,0,0,1,0,0};
            int font_flags = 0;
            char font_name[512] = {0};
            size_t name_index;
            p_FPDFText_GetMatrix(text, index, &matrix);
            font = p_FPDFText_GetFontSize(text, index) * hypot(matrix.c, matrix.d);
            if (!isfinite(font) || font <= 0) font = 12;
            p_FPDFText_GetFontInfo(text, index, font_name, sizeof(font_name), &font_flags);
            font_name[sizeof(font_name)-1]=0;
            for(name_index=0;font_name[name_index];name_index++)
                if(font_name[name_index]>='A' && font_name[name_index]<='Z') font_name[name_index]+=32;
            flags = (p_FPDFText_GetFontWeight(text, index) >= 600 || (font_flags & (1<<18)) ||
                     strstr(font_name,"bold") || strstr(font_name,"black") || strstr(font_name,"demi") ? BOLD : 0)
                    | ((font_flags & 64) || strstr(font_name,"italic") || strstr(font_name,"oblique") ? ITALIC : 0);
            dx = m.a * matrix.a + m.c * matrix.b;
            dy = m.b * matrix.a + m.d * matrix.b;
            { double length = hypot(dx, dy); if (length > 0) { dx /= length; dy /= length; } }
            previous = object;
        }
        if (!p_FPDFText_GetCharBox(text, index, &l, &r, &b, &t) ||
            !p_FPDFText_GetCharOrigin(text, index, &x, &y)) return 0;
        point(m,l,t,xx,yy); point(m,r,t,xx+1,yy+1);
        point(m,l,b,xx+2,yy+2); point(m,r,b,xx+3,yy+3);
        g->left=g->right=xx[0]; g->top=g->bottom=yy[0];
        for(k=1;k<4;k++) {
            g->left=fmin(g->left,xx[k]); g->right=fmax(g->right,xx[k]);
            g->top=fmin(g->top,yy[k]); g->bottom=fmax(g->bottom,yy[k]);
        }
        point(m,x,y,&g->x,&g->y);
        if (!isfinite(g->x) || !isfinite(g->y) || !isfinite(g->left) ||
            !isfinite(g->right) || !isfinite(g->top) || !isfinite(g->bottom)) return 0;
        g->unicode=cp; g->flags=flags; g->font=font; g->dx=dx; g->dy=dy;
        g->space_before=space_before; space_before=0;
        used++;
    }
    out->glyphs=glyphs; out->count=used;
    return 1;
}

typedef struct {
    doc2struct_pdf_glyph **glyphs;
    size_t *capacity;
    doc2struct_pdf_page *page;
} BridgeCopy;

static int copy_bridge_glyphs(void *context, const DS_PDF_GLYPH *glyphs, size_t count) {
    BridgeCopy *copy = context;
    if (count > SIZE_MAX / sizeof(**copy->glyphs)) return 0;
    if (count > *copy->capacity) {
        void *replacement = realloc(*copy->glyphs, count * sizeof(**copy->glyphs));
        if (!replacement) return 0;
        *copy->glyphs = replacement;
        *copy->capacity = count;
    }
    /* Own the view so public callbacks can run outside the PDFium mutex,
     * including callbacks that recursively parse another document. */
    if (count) memcpy(*copy->glyphs, glyphs, count * sizeof(**copy->glyphs));
    copy->page->glyphs = *copy->glyphs;
    copy->page->count = count;
    return 1;
}

static int pdf_visit(Str input, doc2struct_pdf_page_sink sink,
                     void *context, doc2struct_error *error, int experimental,const PdfOptions *options) {
    FPDF_DOCUMENT document = NULL;
    FPDF_PAGE page = NULL;
    FPDF_TEXTPAGE text = NULL;
    doc2struct_pdf_glyph *glyphs = NULL;
    size_t capacity = 0;
    int i, pages, ok = 0;
    int text_only = !(experimental || (options && options->detect_decorations)), previous_unsupported = 0;
    FontRecovery recovery={0};
    Arena geometry={0};
    recovery.policy=options?options->font_recovery:0;
    if (error) error->message[0] = 0;
    if (!input.p || !input.n || !sink) return fail(error, "PDF bytes and a page sink are required");
    LOCK();
    if (!engine_path) { fail(error, "initialize the required PDFium library first"); goto done; }
    document = p_FPDF_LoadMemDocument64(input.p, input.n, NULL);
    if (!document) {
        unsigned long code = p_FPDF_GetLastError();
        fail(error, code == FPDF_ERR_PASSWORD ? "PDF requires a password; password-protected documents are unsupported" : "cannot open PDF document");
        goto done;
    }
    pages = p_FPDF_GetPageCount(document);
    if (pages < 0) { fail(error,"cannot read PDF page count"); goto done; }
    for (i = 0; i < pages; i++) {
        int count;
        int reduced_page = text_only;
        doc2struct_pdf_page view = {0};
        ok = 0;
        view.index = (size_t)i;
        page = reduced_page ? p_DS_PDF_LoadTextPage(document, i) : p_FPDF_LoadPage(document, i);
        if (page) {
            BridgeCopy copy = {&glyphs, &capacity, &view};
            int fast;
            view.width = p_FPDF_GetPageWidthF(page);
            view.height = p_FPDF_GetPageHeightF(page);
            ds_arena_free(&geometry);
            if ((experimental || (options && options->detect_decorations)) &&
                !pdf_page_rules(&geometry,page,&view,options && options->detect_decorations)) { fail(error,"cannot extract PDF rules"); goto done; }
            fast = p_DS_PDF_ExtractPage(page, copy_bridge_glyphs, &copy);
            if (fast < 0) { fail(error,"PDF bridge extraction failed"); goto done; }
            if (fast>0 && experimental && view.count>64) {
                size_t k,lower=0;
                for (k=0;k<view.count;k++) lower+=view.glyphs[k].unicode>='a' && view.glyphs[k].unicode<='z';
                /* Missing Unicode maps commonly expose small CIDs as ASCII
                 * punctuation/capitals and discard control-range digits. */
                if (lower*100<view.count*5) fast=0;
            }
            /* The bridge rejects absent/control mappings already. A printable
             * replacement character also needs font diagnostics from the full
             * text API; well-mapped pages retain the direct extraction path. */
            if (fast>0 && options && options->font_recovery) {
                size_t k;
                for (k=0;k<view.count;k++) if (view.glyphs[k].unicode==0xfffd) { fast=0;break; }
            }
            if (fast > 0) {
                if (!pdf_prepare_glyphs(&geometry,page,&view,glyphs,options)) { fail(error,"cannot prepare PDF geometry"); goto done; }
                previous_unsupported = 0;
                p_FPDF_ClosePage(page); page = NULL;
                UNLOCK();
                ok = sink(context, &view);
                LOCK();
                if (!ok) { fail(error,"PDF page processing failed or was cancelled"); goto done; }
                continue;
            }
            /* Two consecutive unsupported pages disable speculative loading
             * for this document; ordinary pages can still use the text bridge. */
            if (previous_unsupported) text_only = 0;
            previous_unsupported = 1;
        }
        /* The full text fallback requires the original page representation. */
        if (page && reduced_page) {
            p_FPDF_ClosePage(page);
            page = p_FPDF_LoadPage(document, i);
        }
        if (page) text = p_FPDFText_LoadPage(page);
        if (!text || (count = p_FPDFText_CountChars(text)) < 0) {
            fail(error,"cannot extract PDF page"); goto done;
        }
        if ((size_t)count > capacity) {
            void *replacement;
            if ((size_t)count > SIZE_MAX / sizeof(*glyphs)) { fail(error,"PDF page too large"); goto done; }
            replacement = realloc(glyphs, (size_t)count * sizeof(*glyphs));
            if (!replacement) { fail(error,"out of memory"); goto done; }
            glyphs = replacement; capacity = (size_t)count;
        }
        view.count = (size_t)count;
        if (!extract_page(page, text, &view, glyphs,(experimental || recovery.policy)?&recovery:NULL)) { fail(error,"invalid PDF page geometry"); goto done; }
        if (!pdf_prepare_glyphs(&geometry,page,&view,glyphs,options)) { fail(error,"cannot prepare PDF geometry"); goto done; }
        p_FPDFText_ClosePage(text); text = NULL;
        p_FPDF_ClosePage(page); page = NULL;
        UNLOCK();
        ok = sink(context, &view);
        LOCK();
        if (!ok) { fail(error,"PDF page processing failed or was cancelled"); goto done; }
    }
    ok = 1;
done:
    ds_arena_free(&geometry);
    font_recovery_free(&recovery);
    if (text) p_FPDFText_ClosePage(text);
    if (page) p_FPDF_ClosePage(page);
    if (document) p_FPDF_CloseDocument(document);
    UNLOCK();
    free(glyphs);
    return ok;
}
int doc2struct_pdf_visit(Str input, doc2struct_pdf_page_sink sink,
                        void *context, doc2struct_error *error) {
    return pdf_visit(input,sink,context,error,0,NULL);
}

static Document *parse_pdf(ParserContext *parser, Str input, const Rules *rules,
                           const char *adapter, doc2struct_error *error,int standardize) {
    PdfLayout layout = {0};
    Document *document;
    int ok;
    doc2struct_pdf_page_sink sink = rules && rules->pdf.region_layout?ds_pdf_layout_experimental:ds_pdf_layout;
    if (adapter && !strcmp(adapter,"pdf_adapter")) sink=ds_pdf_layout;
    if (adapter && strcmp(adapter,"pdf_adapter")) {
        if (!strcmp(adapter,"pdf_adapter_experimental")) sink = ds_pdf_layout_experimental;
        else { fail(error,"unknown PDF adapter; expected pdf_adapter or pdf_adapter_experimental"); return NULL; }
    }
    if (parser && parser->closed) { fail(error,"Parser is closed"); return NULL; }
    document = doc2struct_document_new(error);
    if (!document) return NULL;
    if (parser) document->arena.pool = &parser->pool;
    layout.document = document;
    layout.rules=rules;
    layout.stream = standardize;
    layout.pool.limit = DS_DEFAULT_CACHE_SIZE;
    layout.scratch.pool = &layout.pool;
    document->join_wrapped_lines = rules ? rules->join_wrapped_lines : 1;
    if (rules) {
        document->retain_table_cells = rules->unwrap_layout;
        document->keep_single_row_tables = !rules->single_row_to_text;
        document->collect_hrefs = rules->collect_hrefs;
        document->preserve_styles = rules->preserve_styles;
        /* Instruction hints describe geometry even when standardization will
         * elect not to join the lines. Retain the original export behavior. */
        document->join_wrapped_lines = !standardize || rules->join_wrapped_lines;
    }
    ok = (!standardize || ds_flow_begin(document)) &&
         pdf_visit(input,sink,&layout,error,sink==ds_pdf_layout_experimental,rules?&rules->pdf:NULL) &&
         (!standardize || (ds_flow_finish(document) && ds_standardize(document,rules)));
    ds_arena_free(&layout.scratch);
    ds_pool_clear(&layout.pool);
    if (!ok) {
        if (document->arena.error[0]) fail(error,document->arena.error);
        doc2struct_document_free(document);
        return NULL;
    }
    return document;
}
Document *doc2struct_parse_pdf(ParserContext *parser, Str input, const Rules *rules,
                               doc2struct_error *error) {
    if (!rules) { fail(error,"rules are required"); return NULL; }
    return parse_pdf(parser,input,rules,NULL,error,1);
}
Document *doc2struct_pdf_instructions(ParserContext *parser, Str input, const Rules *rules,
                                      doc2struct_error *error) {
    if (!rules) { fail(error,"rules are required"); return NULL; }
    return parse_pdf(parser,input,rules,NULL,error,0);
}
Document *doc2struct_parse_pdf_with_adapter(ParserContext *parser, Str input, const Rules *rules,
                                           const char *adapter, doc2struct_error *error) {
    if (!rules) { fail(error,"rules are required"); return NULL; }
    return parse_pdf(parser,input,rules,adapter,error,1);
}
Document *doc2struct_pdf_instructions_with_adapter(ParserContext *parser, Str input, const Rules *rules,
                                                  const char *adapter, doc2struct_error *error) {
    if (!rules) { fail(error,"rules are required"); return NULL; }
    return parse_pdf(parser,input,rules,adapter,error,0);
}
