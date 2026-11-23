#include "bignum.hh"

static constexpr uint32_t c_pow10[10] = {1,		 10,	  100,		1000,	   10000,
										 100000, 1000000, 10000000, 100000000, 1000000000};
static constexpr int16_t EXP_MAX = 30000; // saturate here rather than wrap the exponent

// n / d by shift and subtract: the toolchain links no libgcc, so a plain u64 divide does not link
static uint64_t DivU64(uint64_t n, uint32_t d) {
	uint64_t q = 0, r = 0;
	for (int i = 0; i < 64; i++) {
		r = (r << 1) | (n >> 63);
		n <<= 1;
		q <<= 1;
		if (r >= d) {
			r -= d;
			q |= 1;
		}
	}
	return q;
}

// m is anything that fits; brings it into [MANT_LO, MANT_HI) by whole digits
static BigNum Normalise(uint64_t m, int32_t e) {
	BigNum out;
	if (m == 0) {
		return out;
	}
	while (m >= 0xffffffffull) {
		m = DivU64(m + 5, 10);
		e++;
	}
	uint32_t m32 = uint32_t(m);
	while (m32 >= BigNum::MANT_HI) {
		m32 = (m32 + 5) / 10;
		e++;
	}
	while (m32 < BigNum::MANT_LO) {
		m32 *= 10;
		e--;
	}
	if (e > EXP_MAX) {
		e = EXP_MAX;
		m32 = BigNum::MANT_HI - 1;
	}
	out.m = m32;
	out.e = int16_t(e);
	return out;
}

BigNum BigNum::FromUint(uint32_t value) { return Normalise(value, 0); }

BigNum BigNum::FromMilli(uint32_t milli) { return Normalise(milli, -3); }

int BigNum::Compare(const BigNum& a, const BigNum& b) {
	if (a.m == 0 || b.m == 0) {
		return a.m == b.m ? 0 : (a.m == 0 ? -1 : 1);
	}
	if (a.e != b.e) {
		return a.e < b.e ? -1 : 1;
	}
	return a.m == b.m ? 0 : (a.m < b.m ? -1 : 1);
}

BigNum BigNum::operator+(const BigNum& o) const {
	if (m == 0) {
		return o;
	}
	if (o.m == 0) {
		return *this;
	}
	const BigNum& big = e >= o.e ? *this : o;
	const BigNum& small = e >= o.e ? o : *this;
	int32_t shift = int32_t(big.e) - small.e;
	if (shift > 9) {
		return big;
	}
	uint32_t add = shift == 9 ? (small.m >= 500000000 ? 1 : 0) : (small.m + c_pow10[shift] / 2) / c_pow10[shift];
	return Normalise(uint64_t(big.m) + add, big.e);
}

BigNum BigNum::operator-(const BigNum& o) const {
	if (Compare(*this, o) <= 0) {
		return BigNum();
	}
	if (o.m == 0) {
		return *this;
	}
	int32_t shift = int32_t(e) - o.e;
	if (shift > 9) {
		return *this;
	}
	// round what is taken UP, so spending never hands out a fraction for free
	uint32_t sub = shift == 9 ? 1 : (o.m + c_pow10[shift] - 1) / c_pow10[shift];
	return Normalise(m - sub, e);
}

BigNum BigNum::operator*(const BigNum& o) const {
	if (m == 0 || o.m == 0) {
		return BigNum();
	}
	uint64_t p = uint64_t(m) * o.m; // [1e16, 1e18)
	// round to 9 digits here, once, instead of digit by digit in Normalise
	uint32_t div = p >= 100000000000000000ull ? 1000000000 : 100000000;
	int32_t shift = div == 1000000000 ? 9 : 8;
	return Normalise(DivU64(p + div / 2, div), int32_t(e) + o.e + shift);
}

BigNum BigNum::Pow(uint32_t n) const {
	BigNum result = FromUint(1), base = *this;
	while (n) {
		if (n & 1) {
			result = result * base;
		}
		n >>= 1;
		if (n) {
			base = base * base;
		}
	}
	return result;
}

BigNum BigNum::Sqrt(void) const {
	if (m == 0) {
		return BigNum();
	}
	uint64_t mant = m;
	int32_t exp = e;
	if (exp & 1) {
		mant *= 10;
		exp--;
	}
	uint64_t n = mant * 100000000ull; // [1e16, 1e18), so the root has 9 digits
	uint64_t x = 1000000000ull;		   // >= the root, Newton walks down
	while (true) {
		uint64_t next = (x + DivU64(n, uint32_t(x))) >> 1;
		if (next >= x) {
			break;
		}
		x = next;
	}
	return Normalise(x, (exp - 8) / 2);
}

BigNum BigNum::Floor(void) const {
	if (m == 0 || e >= 0) {
		return *this;
	}
	if (e <= -9) {
		return BigNum();
	}
	uint32_t p = c_pow10[-e];
	BigNum out = *this;
	out.m = m / p * p;
	return out;
}

uint32_t BigNum::ToUint(void) const {
	if (m == 0) {
		return 0;
	}
	if (e < 0) {
		return e <= -9 ? 0 : m / c_pow10[-e];
	}
	if (e > 1) {
		return 0xffffffff;
	}
	uint64_t v = uint64_t(m) * c_pow10[e];
	return v > 0xffffffffull ? 0xffffffff : uint32_t(v);
}

static char* PutUint(char* out, uint32_t v) {
	char tmp[11];
	int n = 0;
	do {
		tmp[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n) {
		*out++ = tmp[--n];
	}
	return out;
}

void BigNum::Format(char* out) const {
	if (*this < FromUint(PLAIN_BELOW)) {
		*PutUint(out, ToUint()) = 0;
		return;
	}
	// three significant digits, rounded
	uint32_t d = (m + 500000) / 1000000;
	int32_t exp = int32_t(e) + 8;
	if (d >= 1000) {
		d /= 10;
		exp++;
	}
	*out++ = '0' + d / 100;
	*out++ = '.';
	*out++ = '0' + d / 10 % 10;
	*out++ = '0' + d % 10;
	*out++ = 'e';
	*PutUint(out, uint32_t(exp)) = 0;
}
