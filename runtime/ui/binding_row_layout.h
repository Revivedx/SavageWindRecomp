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
    // Row band: x -300..300 (the native 540-wide plate, widened by 60 on the label side and the whole row re-centred:
    // the label box grew from 160 to 220, key and controller columns moved 30 right). Label right edge, text left-aligned.
    return {-70.f, 220.f, {-60.f, 170.f}, {128.f, 170.f}, 20.f, 56.f};
}

// The row plate (W_Base_02, natively 540 wide, centred on the row) lives in each button part. Only the key part keeps it.
// Widened to hold the longer label box; the row stays centred (x -300..300).
constexpr float binding_row_plate_width = 600.f;

// Uniform scale for the Key bindings column header text panes (overridable via SAVAGEWIND_HEADER_SCALE).
constexpr float binding_row_header_scale = 0.85f;

// Vertical offset of the header text from the header row's centre (layout units, negative = lower, closer to row 1).
constexpr float binding_row_header_y = -14.f;
