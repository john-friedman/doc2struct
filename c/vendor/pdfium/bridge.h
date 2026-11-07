#ifndef DS_PDF_BRIDGE_H
#define DS_PDF_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#include "fpdfview.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DS_PDF_BRIDGE_ABI 2u
/* Glyph layout matches doc2struct_pdf_glyph. Borrowed until the next call.
 * Serialize PDFium calls and copy records before releasing the engine lock.
 * Direct text extraction intentionally differs from CPDF_TextPage's policies
 * for overprints, hyphens and reading order. */
typedef struct {
    uint32_t unicode;
    unsigned flags;
    int space_before;
    double left, top, right, bottom, x, y, font, dx, dy;
} DS_PDF_GLYPH;
typedef int (*DS_PDF_SINK)(void *, const DS_PDF_GLYPH *, size_t);
/* Reduced page representation for DS_PDF_ExtractPage only. If extraction returns
 * 0, close the page and reload with FPDF_LoadPage before using FPDFText APIs. */
FPDF_EXPORT FPDF_PAGE FPDF_CALLCONV DS_PDF_LoadTextPage(FPDF_DOCUMENT document, int index);
/* 1: emitted; 0: unsupported, caller must use stock PDFium; -1: failed. */
FPDF_EXPORT int FPDF_CALLCONV DS_PDF_ExtractPage(FPDF_PAGE page, DS_PDF_SINK sink, void *context);
/* Counters: calls, fast pages, fallback pages, emitted glyphs. */
FPDF_EXPORT void FPDF_CALLCONV DS_PDF_GetStats(uint64_t *counters);
FPDF_EXPORT unsigned FPDF_CALLCONV DS_PDF_BridgeVersion(void);
#ifdef __cplusplus
}
#endif
#endif
