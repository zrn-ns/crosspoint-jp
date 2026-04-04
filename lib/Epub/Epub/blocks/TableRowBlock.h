#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Block.h"

class GfxRenderer;

// Shared column layout computed once per table, referenced by all rows
struct TableColumnLayout {
  std::vector<uint16_t> colWidths;  // pixel width of each column
  int fontId = 0;                   // font used for table cell text
  int16_t lineHeight = 0;           // single line height in pixels
  int16_t cellPadding = 3;          // padding inside each cell
};

// Represents one row of a table grid.
// Each cell stores pre-wrapped lines (computed during layout, not during render).
class TableRowBlock final : public Block {
 public:
  // cellLines[col] = vector of wrapped lines for that cell
  TableRowBlock(std::vector<std::vector<std::string>> cellLines, std::vector<bool> cellIsHeader,
                std::shared_ptr<TableColumnLayout> layout, int16_t rowHeight, bool isFirstRow, bool isLastRow);
  ~TableRowBlock() override = default;

  BlockType getType() override { return TABLE_ROW_BLOCK; }
  bool isEmpty() override { return cellLines.empty(); }

  void render(GfxRenderer& renderer, int x, int y, int viewportWidth) const;
  bool serialize(FsFile& file) const;
  static std::unique_ptr<TableRowBlock> deserialize(FsFile& file);

  int16_t getRowHeight() const { return rowHeight; }

 private:
  std::vector<std::vector<std::string>> cellLines;  // pre-wrapped lines per cell
  std::vector<bool> cellIsHeader;
  std::shared_ptr<TableColumnLayout> layout;
  int16_t rowHeight;  // per-row height (varies based on max lines in row)
  bool isFirstRow;
  bool isLastRow;
};
