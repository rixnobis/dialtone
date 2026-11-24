#pragma once

#include <psyqo/font.hh>
#include <psyqo/gpu.hh>

// the digits printed on the key caps: a 5x7 font in 6x8 cells, small enough to sit on a key
class KeyGlyphs final {
  public:
	static constexpr int16_t GLYPH_W = 6, GLYPH_H = 8;
	void Init(psyqo::GPU& gpu);
	// centred on pos
	void Draw(psyqo::GPU& gpu, char glyph, psyqo::Vertex pos, psyqo::Color colour);
	// a line of text from its top left corner: digits, * # - / and the letters ADEILNOQRSTY
	void DrawText(psyqo::GPU& gpu, const char* text, psyqo::Vertex pos, psyqo::Color colour);

  private:
	// one fragment per key and per readout row, for both frame buffers
	psyqo::Font<28> m_font;
};
