The runtime uses the PDFium revision pinned in `build_scripts/build_pdfium.py`.
That script applies `text_extraction.patch`, installs `text_mode.h` into the
checkout, and builds the bridge and runtime together:

```sh
python build_scripts/build_pdfium.py --fetch
```

`DS_PDF_LoadTextPage` enables a private text-only page representation. The glyph
bridge consumes text, fonts and positions, so this mode omits painting state,
graphics objects and unused aggregate text bounds. Transforms, forms and marked
content remain available. Type 3 glyph programs temporarily use the full parser
because their artwork determines glyph metrics. Ordinary `FPDF_LoadPage` retains
the complete representation.

If `DS_PDF_ExtractPage` returns unsupported, the wrapper closes the reduced page
and reloads it normally before calling the PDFium text fallback. Two consecutive
unsupported pages disable speculative loading for the rest of that document.
The decision resets for each document; page callbacks still receive owned glyph
records outside the engine lock.

The patch also replaces opcode-map dispatch with a switch, caches resolved fonts
within each resource context, and reads simple `TJ` arrays into reusable text and
kerning buffers. Unhandled arrays rewind to the ordinary object reader, preserving
its number conversion, empty-string and spacing behavior.

Private bridge ABI 2 requires the matching native wrapper and runtime. The build
records the patch and binary hashes in `version.json`, and release caches include
the patch and mode header. Reapplying the same patch is supported; conflicting
checkout modifications fail before compilation.
