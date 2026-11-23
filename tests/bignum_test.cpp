// Host-side check of src/bignum.cpp against long double: tools/test_bignum.sh
#include "../src/bignum.hh"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

static int s_fail = 0, s_checks = 0;

static long double Val(const BigNum& b) { return b.m * powl(10.0L, b.e); }

static void Near(const char* what, const BigNum& got, long double want, long double tol = 2e-8L) {
	s_checks++;
	long double g = Val(got);
	long double err = want == 0 ? fabsl(g) : fabsl(g - want) / want;
	if (err > tol) {
		s_fail++;
		printf("FAIL %s: got %.12Le want %.12Le (rel %.3Le)\n", what, g, want, err);
	}
}

static void Str(const char* what, const BigNum& b, const char* want) {
	s_checks++;
	char out[16];
	b.Format(out);
	if (strcmp(out, want)) {
		s_fail++;
		printf("FAIL %s: format \"%s\" want \"%s\"\n", what, out, want);
	}
}

static void True(const char* what, bool ok) {
	s_checks++;
	if (!ok) {
		s_fail++;
		printf("FAIL %s\n", what);
	}
}

int main() {
	// exact small integers survive every operation the game does to cash
	for (uint32_t v : {0u, 1u, 7u, 25u, 999u, 123456u, 999999u, 1000000u, 999999999u}) {
		True("roundtrip", BigNum::FromUint(v).ToUint() == v);
	}
	True("add exact", (BigNum::FromUint(12) + BigNum::FromUint(30)).ToUint() == 42);
	True("sub exact", (BigNum::FromUint(300) - BigNum::FromUint(25)).ToUint() == 275);
	True("sub saturates", (BigNum::FromUint(5) - BigNum::FromUint(9)).IsZero());
	True("mul exact", (BigNum::FromUint(25) * BigNum::FromUint(4)).ToUint() == 100);
	True("mul by 1.0", (BigNum::FromUint(24) * BigNum::FromMilli(1000)).ToUint() == 24);
	True("cmp", BigNum::FromUint(3) < BigNum::FromUint(4) && BigNum::FromUint(4) > BigNum::Zero() &&
					BigNum::Zero() < BigNum::FromMilli(1) && BigNum::FromUint(10) == BigNum::FromUint(10));
	True("toUint saturates", BigNum::FromUint(4000000000u) * BigNum::FromUint(10) > BigNum::FromUint(4000000000u) &&
								 (BigNum::FromUint(4000000000u) * BigNum::FromUint(10)).ToUint() == 0xffffffffu);
	True("floor", BigNum::FromMilli(3999).Floor().ToUint() == 3 && BigNum::FromMilli(999).Floor().IsZero());

	Str("plain 0", BigNum::Zero(), "0");
	Str("plain", BigNum::FromUint(999999), "999999");
	Str("sci 1e6", BigNum::FromUint(1000000), "1.00e6");
	Str("sci round", BigNum::FromUint(1234567), "1.23e6");
	Str("sci round up", BigNum::FromUint(9996000), "1.00e7");
	Str("sci big", BigNum::FromUint(123).Pow(20) * BigNum::FromUint(1), "6.28e41");
	Str("price", BigNum::FromUint(25) * BigNum::FromMilli(1150).Pow(3), "38");

	// pow, sqrt and mixed arithmetic against long double over a wide range
	std::mt19937 rng(1234);
	for (int i = 0; i < 20000; i++) {
		uint32_t a = rng(), b = rng() % 100000 + 1;
		BigNum A = BigNum::FromUint(a), B = BigNum::FromUint(b);
		Near("add", A + B, (long double)a + b);
		Near("mul", A * B, (long double)a * b);
		Near("sqrt", A.Sqrt(), sqrtl((long double)a));
		if (a > b) {
			Near("sub", A - B, (long double)a - b, 1e-7L);
		}
		True("cmp rand", (A < B) == (a < b) && (A == B) == (a == b));
		uint32_t g = 1050 + rng() % 150, n = rng() % 400;
		Near("pow", BigNum::FromMilli(g).Pow(n), powl(g / 1000.0L, n), 1e-6L);
	}
	// a long run of incremental growth: add a growing prize 10000 times
	BigNum cash;
	long double ref = 0;
	BigNum prize = BigNum::FromUint(12);
	long double pref = 12;
	for (int i = 0; i < 10000; i++) {
		cash += prize;
		ref += pref;
		prize *= BigNum::FromMilli(1010);
		pref *= 1.010L;
	}
	Near("accumulate", cash, ref, 1e-6L);
	printf("BIGNUM %d checks, %d failed, sample cash %Le\n", s_checks, s_fail, Val(cash));
	return s_fail ? 1 : 0;
}
