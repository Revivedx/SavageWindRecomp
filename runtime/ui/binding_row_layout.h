#pragma once

// Native 1280x720 layout units, relative to the row's horizontal origin (screen x=640).
// Column x is its left edge. Future headers should use these same bounds.
struct BindingRowColumn {
    float x, width;
    constexpr float center() const { return x + width * .5f; }
};
struct BindingRowLayout {
    float label_right, label_width;
    BindingRowColumn key, pad;
    float text_inset, glow_extra;
};
constexpr BindingRowLayout binding_row_layout() {
    // Row band: x -270..270 (the native 540-wide plate). Label right edge, text left-aligned in its box (native rows: -14 / 246).
    return {-100.f, 160.f, {-90.f, 170.f}, {98.f, 170.f}, 20.f, 56.f};
}

// The native row plate (W_Base_02, 540 wide, centred on the row) lives in each button part. Only the key part keeps it.
constexpr float binding_row_plate_width = 540.f;
