#pragma once

#include <string>
#include <string_view>

namespace ttk::pdf_layout {

// Append styled Word paragraphs using the optional Xpdf geometry for one
// source page. Leave the output unchanged when the plain/table path is needed.
bool appendEditablePage(std::string& xml, std::string_view readingOrder,
                        std::string_view physicalLayout, std::string_view geometry = {});

} // namespace ttk::pdf_layout
