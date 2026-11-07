#ifndef DOC2STRUCT_PDFIUM_TEXT_MODE_H_
#define DOC2STRUCT_PDFIUM_TEXT_MODE_H_

namespace doc2struct {

// Only the private text-page loader enables this mode. Type 3 glyph programs
// temporarily disable it because their drawing objects determine glyph metrics.
inline thread_local bool text_only = false;

class ScopedTextMode {
 public:
  explicit ScopedTextMode(bool enabled) : previous_(text_only) {
    text_only = enabled;
  }
  ~ScopedTextMode() { text_only = previous_; }

  ScopedTextMode(const ScopedTextMode&) = delete;
  ScopedTextMode& operator=(const ScopedTextMode&) = delete;

 private:
  const bool previous_;
};

}  // namespace doc2struct

#endif  // DOC2STRUCT_PDFIUM_TEXT_MODE_H_
