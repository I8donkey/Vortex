// ============================================================
// extdecimal.h — Exact decimal numbers backing the `extfloat` type.
//
// This module is intentionally self-contained and dependency-free
// (standard library only).  The tree-walking interpreter and the
// LLVM-compiled runtime both use it so their text output is identical.
//
// Representation
// --------------
//   value = (neg ? -1 : +1) * coeff * 10^exp
//
//   * coeff is an arbitrary-precision unsigned magnitude stored as
//     little-endian limbs in base 1'000'000'000 (index 0 = least
//     significant limb).  Each limb is in [0, 1e9).
//   * exp is the decimal exponent (a power of ten applied to coeff).
//
// Normalization invariants (every function returns values obeying them)
// --------------------------------------------------------------------
//   * No leading-most zero limbs: the most significant limb is never 0.
//   * Zero is always { neg=false, coeff={}, exp=0 }.
//   * Trailing decimal zeros INSIDE the magnitude are kept on purpose:
//     they carry the value's scale.
//
// Formatting rule (documented, one consistent rule)
// -------------------------------------------------
//   format() prints coeff's digits with the decimal point placed
//   according to exp, i.e. it reproduces the stored scale.  It never
//   uses exponent notation and always keeps at least one digit before
//   the point.  Zero prints exactly "0".
//
//   Examples:
//     {coeff={1},  exp=-1} -> "0.1"
//     {coeff={10}, exp=-1} -> "1.0"     (scale from parsing "1.0")
//     {coeff={1},  exp=-7} -> "0.0000001"
//     {coeff={1},  exp= 3} -> "1000"
//
// Scale preservation (Python `Decimal`-like)
// ------------------------------------------
//   add/sub align to min(a.exp, b.exp) and keep that exponent
//   (a zero result normalizes to exp==0); mul uses a.exp + b.exp.
//
// Division
// --------
//   div() returns the exact value when the quotient terminates, and
//   otherwise rounds to 25 significant digits, half-up (away from zero).
//   In both cases unnecessary trailing zeros of the produced magnitude
//   are stripped so the result is canonical (1/2 -> "0.5", 1/8 ->
//   "0.125", 1/1 -> "1", 1/3 -> 25 threes).
// ============================================================
#ifndef VORTEX_EXTDECIMAL_H
#define VORTEX_EXTDECIMAL_H

#include <cstdint>
#include <string>
#include <vector>

namespace vortex { namespace dec {

// Exact decimal: value = (neg ? -1 : +1) * coeff * 10^exp
struct Decimal {
    bool neg = false;              // sign flag (zero is always false)
    std::vector<uint32_t> coeff;   // little-endian base-1e9 magnitude
    long long exp = 0;             // decimal exponent
};

// Canonical plain decimal text (never exponent notation).  Zero -> "0".
std::string format(const Decimal& d);

// Parse "0.1", "-12.5", "123", ".5", "7.", "+0.25".
// Malformed input throws std::runtime_error("invalid extfloat literal: '...'").
Decimal parse(const std::string& s);

// Exact integer -> decimal with exp == 0 (0 normalizes to zero).
Decimal from_i64(long long v);

// Shortest round-trip decimal representation of the double, e.g.
// from_f64(0.1) yields exactly the decimal 0.1.
Decimal from_f64(double v);

// Truncate toward zero; saturates to LLONG_MIN/LLONG_MAX when out of range.
long long to_i64_trunc(const Decimal& d);

// Nearest double: parses format(d) with std::stod.
double to_f64(const Decimal& d);

// Scale-preserving arithmetic (see header notes).
Decimal add(const Decimal& a, const Decimal& b);
Decimal sub(const Decimal& a, const Decimal& b);
Decimal mul(const Decimal& a, const Decimal& b);

// Exact quotient if terminating, else 25 significant digits, half-up.
// Division by zero throws std::runtime_error("Division by zero").
Decimal div(const Decimal& a, const Decimal& b);

// Exact remainder a - trunc(a/b)*b (C-like %), keeps min(a.exp, b.exp).
// Division by zero throws std::runtime_error("Division by zero").
Decimal rem(const Decimal& a, const Decimal& b);

// Exact integer power, e >= 0.  e < 0 throws
// std::runtime_error("extfloat power requires a non-negative integer exponent").
Decimal pow_int(const Decimal& a, long long e);

// Exact comparison: -1 / 0 / +1.
int cmp(const Decimal& a, const Decimal& b);

// Unary negation / absolute value (zero stays zero).
Decimal neg(const Decimal& a);
Decimal abs(const Decimal& a);

}} // namespace vortex::dec

#endif // VORTEX_EXTDECIMAL_H
