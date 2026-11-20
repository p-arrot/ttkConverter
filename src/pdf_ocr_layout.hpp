#pragma once

#include <string>
#include <string_view>

namespace ttk::pdf_ocr_layout {

// Rebuild one editable page from matching Tesseract TXT and TSV output.
// Leave XML unchanged on incomplete or unsupported recognition data.
bool appendOcrPage(std::string& xml, std::string_view recognizedText,
                   std::string_view tsv, int rasterDpi);

} // namespace ttk::pdf_ocr_layout
