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

  private:
	// one fragment per key per frame buffer
	psyqo::Font<26> m_font;
};
