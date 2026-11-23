#include "key_glyphs.hh"

// clear of the engine's system font at 960,256 (64x48), in the same texture page
static constexpr psyqo::Vertex c_location = {{.x = 960, .y = 320}};

// rows top to bottom, bit 4 = leftmost of five columns
struct Glyph {
	char c;
	uint8_t rows[7];
};
static constexpr Glyph c_glyphs[] = {
	{'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
	{'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}},
	{'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
	{'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
	{'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
	{'*', {0x00, 0x15, 0x0e, 0x1f, 0x0e, 0x15, 0x00}}, {'#', {0x0a, 0x0a, 0x1f, 0x0a, 0x1f, 0x0a, 0x0a}},
};

void KeyGlyphs::Init(psyqo::GPU& gpu) {
	// psyqo's Font lays glyph (c - 32) at u = index * width along 256-texel rows; every glyph used
	// here is below index 42, so they all sit in the first row: 256x8 texels of 4 bits, 1 = ink
	static uint16_t texels[64 * GLYPH_H];
	for (auto& t : texels) {
		t = 0;
	}
	for (const auto& glyph : c_glyphs) {
		int u0 = (glyph.c - 32) * GLYPH_W;
		for (int y = 0; y < 7; y++) {
			for (int x = 0; x < 5; x++) {
				if (glyph.rows[y] & (0x10 >> x)) {
					int u = u0 + x;
					texels[y * 64 + u / 4] |= 1 << ((u % 4) * 4);
				}
			}
		}
	}
	gpu.uploadToVRAM(texels, {.pos = c_location, .size = {{.w = 64, .h = GLYPH_H}}});
	m_font.initialize(gpu, c_location, {{.w = GLYPH_W, .h = GLYPH_H}});
}

void KeyGlyphs::Draw(psyqo::GPU& gpu, char glyph, psyqo::Vertex pos, psyqo::Color colour) {
	char text[2] = {glyph, 0};
	m_font.chainprint(gpu, text, {{.x = int16_t(pos.x - 2), .y = int16_t(pos.y - 3)}}, colour);
}
