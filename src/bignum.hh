#pragma once

#include <stdint.h>

// A non-negative number that can grow past anything a u32 holds: value = m * 10^e, with m either 0
// or normalised to 9 decimal digits [100000000, 999999999]. Integers up to 999999999 are exact. All
// integer arithmetic, no FPU and no 64-bit division from libgcc. Plain old data, so it can go
// straight into a memory card save.
struct BigNum {
	uint32_t m = 0;
	int16_t e = 0;
	int16_t pad = 0;

	static constexpr uint32_t MANT_LO = 100000000;
	static constexpr uint32_t MANT_HI = 1000000000;
	// below this, Format prints the plain integer, from here on "1.23e45"
	static constexpr uint32_t PLAIN_BELOW = 1000000;

	static BigNum FromUint(uint32_t value);
	// value / 1000, for factors like 1.15
	static BigNum FromMilli(uint32_t milli);
	static BigNum Zero(void) { return BigNum(); }

	bool IsZero(void) const { return m == 0; }
	// -1, 0, 1
	static int Compare(const BigNum& a, const BigNum& b);
	bool operator<(const BigNum& o) const { return Compare(*this, o) < 0; }
	bool operator>(const BigNum& o) const { return Compare(*this, o) > 0; }
	bool operator<=(const BigNum& o) const { return Compare(*this, o) <= 0; }
	bool operator>=(const BigNum& o) const { return Compare(*this, o) >= 0; }
	bool operator==(const BigNum& o) const { return Compare(*this, o) == 0; }

	BigNum operator+(const BigNum& o) const;
	// saturates at zero, never wraps
	BigNum operator-(const BigNum& o) const;
	BigNum operator*(const BigNum& o) const;
	BigNum& operator+=(const BigNum& o) { return *this = *this + o; }
	BigNum& operator-=(const BigNum& o) { return *this = *this - o; }
	BigNum& operator*=(const BigNum& o) { return *this = *this * o; }

	BigNum Pow(uint32_t n) const;
	BigNum Sqrt(void) const;
	BigNum Floor(void) const;
	// saturates at 0xffffffff
	uint32_t ToUint(void) const;

	// "123456" below PLAIN_BELOW, else three significant digits "1.23e45". out needs 12 bytes.
	void Format(char* out) const;

	// the power of ten of the leading digit, for zero 0
	int32_t Magnitude(void) const { return m ? int32_t(e) + 8 : 0; }
};
static_assert(sizeof(BigNum) == 8, "BigNum is saved raw");
