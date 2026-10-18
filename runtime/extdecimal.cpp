#include "extdecimal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace vortex { namespace dec {

namespace {

// ============================================================
// Big-magnitude helpers (little-endian, base 1'000'000'000).
// ============================================================
constexpr uint32_t BASE = 1000000000u;
constexpr uint64_t BASE64 = 1000000000ull;

inline void trim(std::vector<uint32_t>& v) {
    while (!v.empty() && v.back() == 0) v.pop_back();
}

inline bool is_zero_vec(const std::vector<uint32_t>& v) { return v.empty(); }

// -1 / 0 / +1
int cmp_big(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

std::vector<uint32_t> add_big(const std::vector<uint32_t>& a,
                              const std::vector<uint32_t>& b) {
    std::vector<uint32_t> r;
    r.reserve(std::max(a.size(), b.size()) + 1);
    uint64_t carry = 0;
    size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n || carry; ++i) {
        uint64_t s = carry;
        if (i < a.size()) s += a[i];
        if (i < b.size()) s += b[i];
        r.push_back(static_cast<uint32_t>(s % BASE64));
        carry = s / BASE64;
    }
    trim(r);
    return r;
}

// Requires a >= b.
std::vector<uint32_t> sub_big(const std::vector<uint32_t>& a,
                              const std::vector<uint32_t>& b) {
    std::vector<uint32_t> r(a.size(), 0);
    int64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        int64_t s = static_cast<int64_t>(a[i]) - borrow -
                    (i < b.size() ? static_cast<int64_t>(b[i]) : 0);
        if (s < 0) { s += BASE; borrow = 1; } else { borrow = 0; }
        r[i] = static_cast<uint32_t>(s);
    }
    trim(r);
    return r;
}

// Multiply by a small factor m (m < 1e9).
std::vector<uint32_t> mul_small(const std::vector<uint32_t>& v, uint32_t m) {
    std::vector<uint32_t> r;
    r.reserve(v.size() + 1);
    uint64_t carry = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        uint64_t p = static_cast<uint64_t>(v[i]) * m + carry;
        r.push_back(static_cast<uint32_t>(p % BASE64));
        carry = p / BASE64;
    }
    while (carry) {
        r.push_back(static_cast<uint32_t>(carry % BASE64));
        carry /= BASE64;
    }
    return r;
}

// Integer division by a small divisor d (d < 1e9); remainder dropped.
std::vector<uint32_t> div_small(const std::vector<uint32_t>& v, uint32_t d) {
    std::vector<uint32_t> q(v.size(), 0);
    uint64_t rem = 0;
    for (size_t i = v.size(); i-- > 0;) {
        uint64_t cur = rem * BASE64 + v[i];
        q[i] = static_cast<uint32_t>(cur / d);
        rem = cur % d;
    }
    trim(q);
    return q;
}

// If v is exactly divisible by d, divide v in place and return true.
bool try_div_small(std::vector<uint32_t>& v, uint32_t d) {
    uint64_t rem = 0;
    for (size_t i = v.size(); i-- > 0;) {
        rem = (rem * BASE64 + v[i]) % d;
    }
    if (rem != 0) return false;
    v = div_small(v, d);
    return true;
}

std::vector<uint32_t> mul_big(const std::vector<uint32_t>& a,
                              const std::vector<uint32_t>& b) {
    if (a.empty() || b.empty()) return {};
    std::vector<uint32_t> r(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t carry = 0;
        uint64_t ai = a[i];
        for (size_t j = 0; j < b.size() || carry; ++j) {
            uint64_t cur = r[i + j] + (j < b.size() ? ai * b[j] : 0) + carry;
            r[i + j] = static_cast<uint32_t>(cur % BASE64);
            carry = cur / BASE64;
        }
    }
    trim(r);
    return r;
}

// Add a small constant c to a magnitude.
std::vector<uint32_t> add_small(std::vector<uint32_t> v, uint32_t c) {
    uint64_t carry = c;
    for (size_t i = 0; i < v.size() && carry; ++i) {
        uint64_t s = static_cast<uint64_t>(v[i]) + carry;
        v[i] = static_cast<uint32_t>(s % BASE64);
        carry = s / BASE64;
    }
    while (carry) {
        v.push_back(static_cast<uint32_t>(carry % BASE64));
        carry /= BASE64;
    }
    return v;
}

// 10^r for 0 <= r <= 8.
uint32_t pow10_small(unsigned r) {
    uint32_t v = 1;
    while (r--) v *= 10;
    return v;
}

// v * 10^k (k >= 0).
std::vector<uint32_t> shift10(const std::vector<uint32_t>& v, long long k) {
    if (v.empty() || k == 0) return v;
    long long q = k / 9;
    unsigned r = static_cast<unsigned>(k % 9);
    std::vector<uint32_t> res;
    res.reserve(v.size() + static_cast<size_t>(q));
    res.insert(res.begin(), static_cast<size_t>(q), 0u); // low zero limbs
    res.insert(res.end(), v.begin(), v.end());
    if (r) res = mul_small(res, pow10_small(r));
    trim(res);
    return res;
}

// floor(v / 10^k) (k >= 0), i.e. drop k least-significant decimal digits.
std::vector<uint32_t> shift10_down(const std::vector<uint32_t>& v, long long k) {
    if (v.empty() || k == 0) return v;
    long long q = k / 9;
    unsigned r = static_cast<unsigned>(k % 9);
    if (q >= static_cast<long long>(v.size())) return {};
    std::vector<uint32_t> res(v.begin() + q, v.end());
    if (r) res = div_small(res, pow10_small(r));
    trim(res);
    return res;
}

// Knuth Algorithm D: q = u / v, r = u % v (v must be non-zero).
void big_divmod(const std::vector<uint32_t>& u,
                const std::vector<uint32_t>& v,
                std::vector<uint32_t>& q,
                std::vector<uint32_t>& r) {
    q.clear();
    r.clear();
    if (v.empty() || u.empty()) return;             // 0 / v, or guard
    if (cmp_big(u, v) < 0) { r = u; trim(r); return; }

    if (v.size() == 1) {                            // single-limb divisor
        uint32_t d = v[0];
        q.assign(u.size(), 0);
        uint64_t rem = 0;
        for (size_t i = u.size(); i-- > 0;) {
            uint64_t cur = rem * BASE64 + u[i];
            q[i] = static_cast<uint32_t>(cur / d);
            rem = cur % d;
        }
        trim(q);
        if (rem) r.assign(1, static_cast<uint32_t>(rem));
        return;
    }

    const size_t n = v.size();
    const size_t m = u.size() - n;

    // Normalize so that the top divisor limb is >= BASE/2.
    uint32_t d = static_cast<uint32_t>(BASE64 / (static_cast<uint64_t>(v[n - 1]) + 1));
    std::vector<uint32_t> V = mul_small(v, d);
    std::vector<uint32_t> U = mul_small(u, d);
    U.resize(m + n + 1, 0u);

    q.assign(m + 1, 0u);
    for (size_t jj = m + 1; jj-- > 0;) {
        const size_t j = jj;
        uint64_t num = static_cast<uint64_t>(U[j + n]) * BASE64 + U[j + n - 1];
        uint64_t qhat = num / V[n - 1];
        uint64_t rhat = num % V[n - 1];
        while (qhat >= BASE || static_cast<uint64_t>(qhat) * V[n - 2] >
                                   rhat * BASE64 + U[j + n - 2]) {
            --qhat;
            rhat += V[n - 1];
            if (rhat >= BASE) break;
        }

        // U[j .. j+n] -= qhat * V
        uint64_t borrow = 0;
        for (size_t i = 0; i < n; ++i) {
            uint64_t p = static_cast<uint64_t>(qhat) * V[i] + borrow;
            uint64_t low = p % BASE64;
            borrow = p / BASE64;
            if (U[j + i] < low) {
                U[j + i] = static_cast<uint32_t>(U[j + i] + BASE - low);
                ++borrow;
            } else {
                U[j + i] = static_cast<uint32_t>(U[j + i] - low);
            }
        }
        if (U[j + n] < borrow) {
            // qhat was one too large: add V back.
            --qhat;
            uint64_t c = 0;
            for (size_t i = 0; i < n; ++i) {
                uint64_t s = static_cast<uint64_t>(U[j + i]) + V[i] + c;
                U[j + i] = static_cast<uint32_t>(s % BASE64);
                c = s / BASE64;
            }
            U[j + n] = static_cast<uint32_t>(U[j + n] + c);
        } else {
            U[j + n] = static_cast<uint32_t>(U[j + n] - borrow);
        }
        q[j] = static_cast<uint32_t>(qhat);
    }

    trim(q);
    // Denormalize remainder: R_scaled = U[0..n-1], r = R_scaled / d.
    std::vector<uint32_t> rscaled(U.begin(), U.begin() + n);
    r = (d == 1) ? rscaled : div_small(rscaled, d);
    trim(r);
}

// true if v divides u exactly (u mod v == 0).
bool big_divides(const std::vector<uint32_t>& u,
                 const std::vector<uint32_t>& v) {
    std::vector<uint32_t> q, r;
    big_divmod(u, v, q, r);
    return r.empty();
}

// Number of decimal digits (0 for the value zero).
int decimal_digits(uint32_t x) {
    int n = 1;
    while (x >= 10) { x /= 10; ++n; }
    return n;
}

long long digit_count(const std::vector<uint32_t>& v) {
    if (v.empty()) return 0;
    return static_cast<long long>(v.size() - 1) * 9 + decimal_digits(v.back());
}

std::string to_digits(const std::vector<uint32_t>& v) {
    if (v.empty()) return "0";
    std::string s = std::to_string(v.back());
    for (size_t i = v.size() - 1; i-- > 0;) {
        std::string part = std::to_string(v[i]);
        s += std::string(9 - part.size(), '0') + part;
    }
    return s;
}

std::vector<uint32_t> from_digits(const std::string& digits) {
    std::vector<uint32_t> v;
    int n = static_cast<int>(digits.size());
    for (int i = n; i > 0; i -= 9) {
        int start = std::max(0, i - 9);
        uint32_t val = 0;
        for (int k = start; k < i; ++k) val = val * 10 + (digits[k] - '0');
        v.push_back(val);
    }
    trim(v);
    return v;
}

// floor(log10(X / Y)) for positive magnitudes X, Y.  Result is t or t-1
// where t = digits(X) - digits(Y), computed exactly.
long long quotient_floor_log10(const std::vector<uint32_t>& X,
                               const std::vector<uint32_t>& Y) {
    long long t = digit_count(X) - digit_count(Y);
    bool ge;
    if (t >= 0) {
        ge = cmp_big(X, shift10(Y, t)) >= 0;
    } else {
        ge = cmp_big(shift10(X, -t), Y) >= 0;
    }
    return ge ? t : t - 1;
}

void normalize(Decimal& d) {
    trim(d.coeff);
    if (d.coeff.empty()) {
        d.neg = false;
        d.exp = 0;
    }
}

// Strip trailing decimal zeros from a magnitude, returning how many were
// removed (each removed zero multiplies the value's exponent by 10).
long long strip_decimal_zeros(std::vector<uint32_t>& v) {
    long long cnt = 0;
    while (!v.empty() && v[0] % 10 == 0) {
        v = div_small(v, 10);
        trim(v);
        ++cnt;
        if (v.empty()) break;
    }
    return cnt;
}

} // namespace

// ============================================================
// Public API
// ============================================================

std::string format(const Decimal& d) {
    if (d.coeff.empty()) return "0";
    std::string digits = to_digits(d.coeff);
    std::string out;
    if (d.neg) out.push_back('-');

    if (d.exp >= 0) {
        out += digits;
        out.append(static_cast<size_t>(d.exp), '0');
        return out;
    }

    long long nd = static_cast<long long>(digits.size());
    long long point = nd + d.exp; // digits to the left of the point
    if (point > 0 && point < nd) {
        out.append(digits, 0, static_cast<size_t>(point));
        out.push_back('.');
        out.append(digits, static_cast<size_t>(point), std::string::npos);
    } else if (point >= nd) {
        out += digits;
        out.append(static_cast<size_t>(point - nd), '0');
    } else {
        out += "0.";
        out.append(static_cast<size_t>(-point), '0');
        out += digits;
    }
    return out;
}

Decimal parse(const std::string& s) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = (s[i] == '-');
        ++i;
    }
    std::string intpart, fracpart;
    bool seen_dot = false;
    bool any_digit = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            any_digit = true;
            (seen_dot ? fracpart : intpart).push_back(c);
        } else if (c == '.') {
            if (seen_dot) throw std::runtime_error("invalid extfloat literal: '" + s + "'");
            seen_dot = true;
        } else {
            throw std::runtime_error("invalid extfloat literal: '" + s + "'");
        }
    }
    if (!any_digit) throw std::runtime_error("invalid extfloat literal: '" + s + "'");

    Decimal d;
    d.coeff = from_digits(intpart + fracpart);
    d.exp = -static_cast<long long>(fracpart.size());
    d.neg = neg;
    normalize(d);
    return d;
}

Decimal from_i64(long long v) {
    Decimal d;
    if (v == 0) return d;
    bool neg = v < 0;
    uint64_t mag = neg ? static_cast<uint64_t>(-(v + 1)) + 1
                       : static_cast<uint64_t>(v);
    d.neg = neg;
    while (mag) {
        d.coeff.push_back(static_cast<uint32_t>(mag % BASE64));
        mag /= BASE64;
    }
    return d;
}

namespace {
// Expand a %g/scientific string like "-1.25e-07" into plain decimal text
// accepted by parse().
std::string expand_exponent(const std::string& s) {
    size_t e = s.find_first_of("eE");
    if (e == std::string::npos) return s;
    std::string mant = s.substr(0, e);
    int exp = std::atoi(s.c_str() + e + 1);

    bool neg = false;
    size_t i = 0;
    if (i < mant.size() && (mant[i] == '+' || mant[i] == '-')) {
        neg = (mant[i] == '-');
        ++i;
    }
    std::string intp, fracp;
    bool dot = false;
    for (; i < mant.size(); ++i) {
        if (mant[i] == '.') { dot = true; continue; }
        (dot ? fracp : intp).push_back(mant[i]);
    }
    std::string digits = intp + fracp;
    long long point = static_cast<long long>(intp.size()) + exp; // digits before point

    std::string out;
    if (neg) out.push_back('-');
    long long nd = static_cast<long long>(digits.size());
    if (point >= nd) {
        out += digits;
        out.append(static_cast<size_t>(point - nd), '0');
    } else if (point > 0) {
        out.append(digits, 0, static_cast<size_t>(point));
        out.push_back('.');
        out.append(digits, static_cast<size_t>(point), std::string::npos);
    } else {
        out += "0.";
        out.append(static_cast<size_t>(-point), '0');
        out += digits;
    }
    return out;
}
} // namespace

Decimal from_f64(double v) {
    if (std::isnan(v)) throw std::runtime_error("cannot convert NaN to extfloat");
    if (std::isinf(v)) throw std::runtime_error("cannot convert infinity to extfloat");
    if (v == 0.0) return Decimal{};

    char buf[64];
    for (int p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof(buf), "%.*g", p, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return parse(expand_exponent(buf));
}

long long to_i64_trunc(const Decimal& d) {
    if (d.coeff.empty()) return 0;
    std::vector<uint32_t> intmag;
    if (d.exp >= 0) {
        intmag = shift10(d.coeff, d.exp);
    } else {
        intmag = shift10_down(d.coeff, -d.exp);
    }
    const uint64_t limit = 9223372036854775808ull; // |LLONG_MIN|
    uint64_t val = 0;
    bool overflow = false;
    for (size_t i = intmag.size(); i-- > 0;) {
        if (val > (std::numeric_limits<uint64_t>::max() - intmag[i]) / BASE64) {
            overflow = true;
            break;
        }
        val = val * BASE64 + intmag[i];
    }
    if (!overflow && val > limit) overflow = true;
    if (overflow) return d.neg ? std::numeric_limits<long long>::min()
                               : std::numeric_limits<long long>::max();
    if (d.neg) {
        if (val >= limit) return std::numeric_limits<long long>::min();
        return -static_cast<long long>(val);
    }
    return static_cast<long long>(val);
}

double to_f64(const Decimal& d) {
    std::string s = format(d);
    try {
        return std::stod(s);
    } catch (...) {
        return d.neg ? -std::numeric_limits<double>::infinity()
                     : std::numeric_limits<double>::infinity();
    }
}

Decimal add(const Decimal& a, const Decimal& b) {
    Decimal x = a, y = b;
    normalize(x);
    normalize(y);
    if (x.coeff.empty()) return y;
    if (y.coeff.empty()) return x;

    long long e = std::min(x.exp, y.exp);
    std::vector<uint32_t> A = shift10(x.coeff, x.exp - e);
    std::vector<uint32_t> B = shift10(y.coeff, y.exp - e);

    Decimal r;
    if (x.neg == y.neg) {
        r.coeff = add_big(A, B);
        r.neg = x.neg;
    } else {
        int c = cmp_big(A, B);
        if (c > 0) { r.coeff = sub_big(A, B); r.neg = x.neg; }
        else if (c < 0) { r.coeff = sub_big(B, A); r.neg = y.neg; }
        else return Decimal{};
    }
    r.exp = e;
    normalize(r);
    return r;
}

Decimal sub(const Decimal& a, const Decimal& b) {
    Decimal nb = b;
    if (!nb.coeff.empty()) nb.neg = !nb.neg;
    return add(a, nb);
}

Decimal mul(const Decimal& a, const Decimal& b) {
    if (a.coeff.empty() || b.coeff.empty()) return Decimal{};
    Decimal r;
    r.coeff = mul_big(a.coeff, b.coeff);
    r.exp = a.exp + b.exp;
    r.neg = a.neg != b.neg;
    normalize(r);
    return r;
}

Decimal div(const Decimal& a, const Decimal& b) {
    if (b.coeff.empty()) throw std::runtime_error("Division by zero");
    if (a.coeff.empty()) return Decimal{};

    const bool neg = a.neg != b.neg;
    const std::vector<uint32_t>& X = a.coeff;
    const std::vector<uint32_t>& Y = b.coeff;
    const long long E = a.exp - b.exp;

    // Position of the most significant digit of the quotient.
    long long f = quotient_floor_log10(X, Y);
    long long D = f + E;

    // Termination test: after removing all factors 2 and 5 from the
    // denominator, the remaining part must divide the numerator.
    std::vector<uint32_t> g = Y;
    while (try_div_small(g, 2)) {}
    while (try_div_small(g, 5)) {}
    const bool terminating = big_divides(X, g);

    // Capture 26 significant digits (25 kept + 1 guard for rounding).
    long long shift = 25 - f;
    std::vector<uint32_t> Q, R;
    {
        std::vector<uint32_t> num, den;
        if (shift >= 0) { num = shift10(X, shift); den = Y; }
        else { num = X; den = shift10(Y, -shift); }
        big_divmod(num, den, Q, R);
    }

    if (terminating) {
        long long extra = 0;
        while (!R.empty()) { // keep widening until the quotient is exact
            ++extra;
            ++shift;
            std::vector<uint32_t> num, den;
            if (shift >= 0) { num = shift10(X, shift); den = Y; }
            else { num = X; den = shift10(Y, -shift); }
            big_divmod(num, den, Q, R);
        }
        long long exp = D - 25 - extra;
        exp += strip_decimal_zeros(Q);
        Decimal r;
        r.coeff = Q;
        r.exp = exp;
        r.neg = neg;
        normalize(r);
        return r;
    }

    // Non-terminating: round half-up on the 26th significant digit.
    uint32_t guard = Q.empty() ? 0u : Q[0] % 10;
    std::vector<uint32_t> q25 = div_small(Q, 10);
    if (guard >= 5) q25 = add_small(q25, 1);
    long long exp = D - 24;
    exp += strip_decimal_zeros(q25);
    Decimal r;
    r.coeff = q25;
    r.exp = exp;
    r.neg = neg;
    normalize(r);
    return r;
}

Decimal rem(const Decimal& a, const Decimal& b) {
    if (b.coeff.empty()) throw std::runtime_error("Division by zero");
    if (a.coeff.empty()) return Decimal{};

    bool qneg = a.neg != b.neg;
    long long E = a.exp - b.exp;
    std::vector<uint32_t> num, den, q, r;
    if (E >= 0) { num = shift10(a.coeff, E); den = b.coeff; }
    else { num = a.coeff; den = shift10(b.coeff, -E); }
    big_divmod(num, den, q, r);

    Decimal quo;
    quo.coeff = q;
    quo.exp = 0;
    quo.neg = qneg;
    normalize(quo);
    return sub(a, mul(quo, b));
}

Decimal pow_int(const Decimal& a, long long e) {
    if (e < 0)
        throw std::runtime_error("extfloat power requires a non-negative integer exponent");
    Decimal result = from_i64(1);
    Decimal base = a;
    normalize(base);
    while (e > 0) {
        if (e & 1) result = mul(result, base);
        e >>= 1;
        if (e) base = mul(base, base);
    }
    return result;
}

int cmp(const Decimal& a, const Decimal& b) {
    Decimal x = a, y = b;
    normalize(x);
    normalize(y);
    if (x.coeff.empty()) return y.coeff.empty() ? 0 : (y.neg ? 1 : -1);
    if (y.coeff.empty()) return x.neg ? -1 : 1;
    if (x.neg != y.neg) return x.neg ? -1 : 1;

    long long oa = digit_count(x.coeff) - 1 + x.exp;
    long long ob = digit_count(y.coeff) - 1 + y.exp;
    int res;
    if (oa != ob) {
        res = oa < ob ? -1 : 1;
    } else {
        long long e = std::min(x.exp, y.exp);
        std::vector<uint32_t> A = shift10(x.coeff, x.exp - e);
        std::vector<uint32_t> B = shift10(y.coeff, y.exp - e);
        res = cmp_big(A, B);
    }
    return x.neg ? -res : res;
}

Decimal neg(const Decimal& a) {
    Decimal r = a;
    if (!r.coeff.empty()) r.neg = !r.neg;
    return r;
}

Decimal abs(const Decimal& a) {
    Decimal r = a;
    r.neg = false;
    return r;
}

}} // namespace vortex::dec
