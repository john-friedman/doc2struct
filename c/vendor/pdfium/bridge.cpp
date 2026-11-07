// Direct text-object frontend for PDFium chromium/7825.
// Unsupported text semantics fall back to CPDF_TextPage.
#include "bridge.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/page/doc2struct_text_mode.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "fpdfsdk/cpdfsdk_helpers.h"


namespace {
struct Transform {
  double a,b,c,d,e,f;
  void Point(double x,double y,double* ox,double* oy) const {
    *ox=a*x+c*y+e; *oy=b*x+d*y+f;
  }
};
struct Character { WideString unicode; FX_RECT box; };
struct Font {
  RetainPtr<CPDF_Font> owner;
  unsigned flags=0;
  std::unordered_map<uint32_t,Character> chars;
};
struct Extraction {
  Transform display;
  std::vector<DS_PDF_GLYPH> glyphs;
  std::unordered_map<CPDF_Font*,Font> fonts;
  bool pending_space=false;
};
Extraction extraction;
std::array<uint64_t,4> stats={};

bool Space(uint32_t cp) {
  return cp==32 || cp==160 || (cp>=9 && cp<=13) || cp==0x1680 ||
         (cp>=0x2000 && cp<=0x200a) || cp==0x2028 || cp==0x2029 ||
         cp==0x202f || cp==0x205f || cp==0x3000;
}
bool Complex(uint32_t cp) {
  // Bidirectional ordering, presentation forms and discretionary hyphens must
  // continue to use PDFium's text semantics until separately implemented.
  return (cp>=0x590 && cp<=0x8ff) || (cp>=0x200b && cp<=0x200f) ||
         (cp>=0x202a && cp<=0x202e) || (cp>=0x2066 && cp<=0x2069) ||
         (cp>=0xfb00 && cp<=0xfeff) || cp==0xad;
}

Font& FontInfo(CPDF_Font* font) {
  auto [entry, inserted]=extraction.fonts.try_emplace(font);
  if(inserted) {
    entry->second.owner=pdfium::WrapRetain(font);
    std::string name(font->GetBaseFontName().c_str());
    for(char& c:name) if(c>='A' && c<='Z') c+=32;
    int flags=font->GetFontFlags();
    bool bold=font->GetFontWeight().value_or(0)>=600 || (flags&(1<<18)) ||
              name.find("bold")!=std::string::npos || name.find("black")!=std::string::npos ||
              name.find("demi")!=std::string::npos;
    bool italic=(flags&64) || name.find("italic")!=std::string::npos || name.find("oblique")!=std::string::npos;
    entry->second.flags=(bold?1u:0u)|(italic?2u:0u);
  }
  return entry->second;
}

bool Text(CPDF_TextObject* object, const CFX_Matrix& form) {
  const auto* marks=object->GetContentMarks();
  for(size_t i=0;i<marks->CountItems();i++) {
    auto dict=marks->GetItem(i)->GetParam();
    if(dict && dict->KeyExist("ActualText")) return false;
  }
  auto font=object->GetFont();
  if(!font || font->IsVertWriting() || font->IsType3Font()) return false;
  Font& info=FontInfo(font.Get());
  CFX_Matrix matrix=object->GetTextMatrix()*form;
  double fontsize=object->GetFontSize()*std::hypot(matrix.c,matrix.d);
  if(!std::isfinite(fontsize) || fontsize<=0) return false;
  double dx=extraction.display.a*matrix.a+extraction.display.c*matrix.b;
  double dy=extraction.display.b*matrix.a+extraction.display.d*matrix.b;
  double length=std::hypot(dx,dy);
  if(!std::isfinite(length) || length<=0) return false;
  dx/=length; dy/=length;
  // Infer word boundaries represented by TJ positioning rather than literal
  // spaces. Match PDFium's base-spacing/space-width policy without constructing
  // a CPDF_TextPage (core/fpdftext/cpdf_textpage.cpp, chromium/7825).
  float horizontal=object->text_state().GetFontSizeH();
  float charspace=object->text_state().GetCharSpace();
  float transformed=matrix.TransformDistance(charspace);
  float base=0;
  const auto& kernings=object->GetCharKernings();
  if(charspace!=0 && object->CountItems()>1) {
    base=transformed;
    bool has_kerning=false;
    for(float adjustment:kernings) if(adjustment!=0) {
      base=std::min(base,transformed-horizontal*adjustment/1000);
      has_kerning=true;
    }
    if(base<0 || (object->CountItems()==2 && has_kerning)) base=0;
  }
  if(std::fabs(charspace)>.001f) base-=transformed;
  uint32_t spacecode=font->CharCodeFromUnicode(' ');
  float spacewidth=spacecode==CPDF_Font::kInvalidCharCode?0:horizontal*font->GetCharWidth(spacecode)/1000;
  float threshold=spacewidth>horizontal/3?0:spacewidth/2;
  for(size_t i=0;i<object->CountItems();i++) {
    auto item=object->GetItemInfo(i);
    auto [entry, inserted]=info.chars.try_emplace(item.char_code_);
    if(inserted) {
      entry->second.unicode=font->UnicodeFromCharCode(item.char_code_);
      entry->second.box=font->GetCharBBox(item.char_code_);
    }
    const Character& decoded=entry->second;
    if(decoded.unicode.IsEmpty()) return false;
    if(i>0 && !extraction.pending_space) {
      float spacing=-horizontal*kernings[i-1]/1000-base;
      float boundary=threshold;
      if(spacing>0 && boundary==0) {
        int width=font->GetCharWidth(item.char_code_);
        if(width<=0) return false; // Let PDFium handle unusual width fallbacks.
        int divisor=width<300?2:width<500?4:width<700?5:6;
        boundary=horizontal*(float(width)/divisor)/1000;
      }
      if(boundary>0 && spacing>=boundary) extraction.pending_space=true;
    }
    const auto& box=decoded.box;
    float scale=object->GetFontSize()/1000;
    CFX_FloatRect rect(box.left*scale+item.origin_.x,box.bottom*scale+item.origin_.y,
                       box.right*scale+item.origin_.x,box.top*scale+item.origin_.y);
    if(std::fabs(rect.top-rect.bottom)<.01f) rect.top=rect.bottom+scale;
    if(std::fabs(rect.right-rect.left)<.01f) rect.right=rect.left+object->GetCharWidth(item.char_code_);
    rect=matrix.TransformRect(rect);
    CFX_PointF origin=matrix.Transform(item.origin_);
    DS_PDF_GLYPH g={};
    double xx[4],yy[4];
    const auto& m=extraction.display;
    m.Point(rect.left,rect.top,&xx[0],&yy[0]); m.Point(rect.right,rect.top,&xx[1],&yy[1]);
    m.Point(rect.left,rect.bottom,&xx[2],&yy[2]); m.Point(rect.right,rect.bottom,&xx[3],&yy[3]);
    g.left=*std::min_element(xx,xx+4); g.right=*std::max_element(xx,xx+4);
    g.top=*std::min_element(yy,yy+4); g.bottom=*std::max_element(yy,yy+4);
    m.Point(origin.x,origin.y,&g.x,&g.y);
    g.font=fontsize; g.dx=dx; g.dy=dy; g.flags=info.flags;
    if(!std::isfinite(g.x) || !std::isfinite(g.y)) return false;
    auto unicode=decoded.unicode.span();
    for(size_t u=0;u<unicode.size();u++) {
      uint32_t cp=static_cast<uint32_t>(unicode[u]);
      if(cp>=0xd800 && cp<=0xdbff && u+1<unicode.size()) {
        uint32_t low=static_cast<uint32_t>(unicode[u+1]);
        if(low>=0xdc00 && low<=0xdfff) { cp=0x10000+((cp-0xd800)<<10)+low-0xdc00; u++; }
      }
      if(Complex(cp)) return false;
      if(Space(cp)) { extraction.pending_space=true; continue; }
      if(cp<32 || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return false;
      g.unicode=cp;
      g.space_before=extraction.pending_space?1:0;
      extraction.pending_space=false;
      extraction.glyphs.push_back(g);
    }
  }
  return true;
}

bool Visit(CPDF_PageObjectHolder* holder,const CFX_Matrix& matrix,unsigned depth) {
  if(depth>40) return false;
  for(const auto& object:*holder) {
    if(!object->IsActive()) continue;
    if(object->IsText()) {
      if(!Text(object->AsText(),matrix)) return false;
    } else if(object->IsForm()) {
      auto* form=object->AsForm();
      if(!Visit(form->form(),form->form_matrix()*matrix,depth+1)) return false;
    }
  }
  return true;
}
bool Initialize(FPDF_PAGE page) {
  auto* internal=CPDFPageFromFPDFPage(page);
  if(!internal) return false;
  double width=internal->GetPageWidth(),height=internal->GetPageHeight();
  if(!(width>0 && height>0 && width<1000000 && height<1000000)) return false;
  int x[3],y[3];
  const double px[]={0,1024,0},py[]={0,0,1024};
  for(int i=0;i<3;i++)
    if(!FPDF_PageToDevice(page,0,0,static_cast<int>(std::ceil(width*64)),
                          static_cast<int>(std::ceil(height*64)),0,px[i],py[i],&x[i],&y[i])) return false;
  extraction.display={(x[1]-double(x[0]))/65536,(y[1]-double(y[0]))/65536,
                      (x[2]-double(x[0]))/65536,(y[2]-double(y[0]))/65536,x[0]/64.0,y[0]/64.0};
  extraction.glyphs.clear(); extraction.fonts.clear(); extraction.pending_space=false;
  return true;
}
}  // namespace

FPDF_EXPORT FPDF_PAGE FPDF_CALLCONV DS_PDF_LoadTextPage(FPDF_DOCUMENT document, int index) {
  doc2struct::ScopedTextMode text_mode(true);
  return FPDF_LoadPage(document, index);
}

FPDF_EXPORT int FPDF_CALLCONV DS_PDF_ExtractPage(FPDF_PAGE page,DS_PDF_SINK sink,void* context) {
  stats[0]++;
  if(!sink || !Initialize(page)) return -1;
  if(!Visit(CPDFPageFromFPDFPage(page),CFX_Matrix(),0)) {
    extraction.fonts.clear(); stats[2]++; return 0;
  }
  stats[1]++; stats[3]+=extraction.glyphs.size();
  extraction.fonts.clear();
  return sink(context,extraction.glyphs.data(),extraction.glyphs.size())?1:-1;
}
FPDF_EXPORT void FPDF_CALLCONV DS_PDF_GetStats(uint64_t* counters) {
  if(counters) std::copy(stats.begin(),stats.end(),counters);
}
FPDF_EXPORT unsigned FPDF_CALLCONV DS_PDF_BridgeVersion(void) { return DS_PDF_BRIDGE_ABI; }
