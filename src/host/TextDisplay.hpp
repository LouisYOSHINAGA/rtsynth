#pragma once

#include <string>

namespace rtsynth {

// Abstraction over a small character display (16x2 LCD, OLED in text
// mode, a console for testing...). DisplayUi renders through this, so
// swapping the physical display means implementing these four methods.
// Called only from the display UI thread.
class TextDisplay {
public:
    virtual ~TextDisplay() = default;

    virtual int columns() const = 0;
    virtual int rows() const = 0;

    // Replace one whole row; text is truncated/padded to columns().
    virtual void writeLine(int row, const std::string& text) = 0;

    virtual void clear() = 0;
};

}  // namespace rtsynth
