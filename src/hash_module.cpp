// ============================================================
// hash_module.cpp — 哈希模块（MD5 / SHA-1 / SHA-256）
// 全部输入字符串、输出小写十六进制 str，解释器与编译端一致。
// ============================================================
#include "hash_module.h"
#include "value.h"
#include "interpreter.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>

namespace vortex {

// ---------- 通用十六进制输出 ----------
static void hex_out(const uint8_t* d, size_t n, std::string& out) {
    static const char* HEX = "0123456789abcdef";
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out += HEX[(d[i] >> 4) & 0xf];
        out += HEX[d[i] & 0xf];
    }
}

// ================= MD5 =================
static const uint32_t MD5_K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
static const int MD5_S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5,9,14,20, 5,9,14,20, 5,9,14,20, 5,9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21};
static void md5_final(const uint8_t* msg, size_t len, uint8_t out[16]) {
    uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    size_t padded = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> buf(padded, 0);
    std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) buf[padded - 8 + i] = (uint8_t)(bitlen >> (8*i));
    // 主循环
    #define LOAD(i) ((uint32_t)buf[i] | ((uint32_t)buf[i+1]<<8) | ((uint32_t)buf[i+2]<<16) | ((uint32_t)buf[i+3]<<24))
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t x[16];
        for (int i = 0; i < 16; ++i) x[i] = LOAD(off + i*4);
        uint32_t A = st[0], B = st[1], C = st[2], D = st[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t F, g;
            if (i < 16)      { F = (B & C) | (~B & D);       g = i; }
            else if (i < 32) { F = (D & B) | (~D & C);       g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;                g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);             g = (7*i) % 16; }
            F = F + A + MD5_K[i] + x[g];
            A = D; D = C; C = B;
            B = B + ((F << MD5_S[i]) | (F >> (32 - MD5_S[i])));
        }
        st[0]+=A; st[1]+=B; st[2]+=C; st[3]+=D;
    }
    #undef LOAD
    for (int i = 0; i < 4; ++i) { out[i*4]=(uint8_t)st[i]; out[i*4+1]=(uint8_t)(st[i]>>8); out[i*4+2]=(uint8_t)(st[i]>>16); out[i*4+3]=(uint8_t)(st[i]>>24); }
}

// ================= SHA-1 =================
static void sha1_final(const uint8_t* msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t padded = ((len + 9) / 64 + 1) * 64;
    std::vector<uint8_t> buf(padded, 0);
    std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) buf[padded - 8 + i] = (uint8_t)(bitlen >> (8*(7-i)));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)buf[off+i*4]<<24) | ((uint32_t)buf[off+i*4+1]<<16) | ((uint32_t)buf[off+i*4+2]<<8) | (uint32_t)buf[off+i*4+3];
        for (int i = 16; i < 80; ++i) { uint32_t t = w[i-3]^w[i-8]^w[i-14]^w[i-16]; w[i] = (t<<1)|(t>>31); }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i<20)      { f = (b&c)|(~b&d);      k = 0x5A827999; }
            else if (i<40) { f = b^c^d;             k = 0x6ED9EBA1; }
            else if (i<60) { f = (b&c)|(b&d)|(c&d); k = 0x8F1BBCDC; }
            else           { f = b^c^d;             k = 0xCA62C1D6; }
            uint32_t temp = ((a<<5)|(a>>27)) + f + e + k + w[i];
            e = d; d = c; c = (b<<30)|(b>>2); b = a; a = temp;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    for (int i = 0; i < 5; ++i) { out[i*4]=(uint8_t)(h[i]>>24); out[i*4+1]=(uint8_t)(h[i]>>16); out[i*4+2]=(uint8_t)(h[i]>>8); out[i*4+3]=(uint8_t)h[i]; }
}

// ================= SHA-256 =================
static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static void sha256_final(const uint8_t* msg, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t padded = ((len + 9) / 64 + 1) * 64;
    std::vector<uint8_t> buf(padded, 0);
    std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) buf[padded - 8 + i] = (uint8_t)(bitlen >> (8*(7-i)));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)buf[off+i*4]<<24) | ((uint32_t)buf[off+i*4+1]<<16) | ((uint32_t)buf[off+i*4+2]<<8) | (uint32_t)buf[off+i*4+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ((w[i-15]>>7)|(w[i-15]<<25)) ^ ((w[i-15]>>18)|(w[i-15]<<14)) ^ (w[i-15]>>3);
            uint32_t s1 = ((w[i-2]>>17)|(w[i-2]<<15)) ^ ((w[i-2]>>19)|(w[i-2]<<13)) ^ (w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],H=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ((e>>6)|(e<<26)) ^ ((e>>11)|(e<<21)) ^ ((e>>25)|(e<<7));
            uint32_t ch = (e&f)^(~e&g);
            uint32_t t1 = H + S1 + ch + SHA256_K[i] + w[i];
            uint32_t S0 = ((a>>2)|(a<<30)) ^ ((a>>13)|(a<<19)) ^ ((a>>22)|(a<<10));
            uint32_t maj = (a&b)^(a&c)^(b&c);
            uint32_t t2 = S0 + maj;
            H=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=H;
    }
    for (int i = 0; i < 8; ++i) { out[i*4]=(uint8_t)(h[i]>>24); out[i*4+1]=(uint8_t)(h[i]>>16); out[i*4+2]=(uint8_t)(h[i]>>8); out[i*4+3]=(uint8_t)h[i]; }
}

void register_hash_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
    auto mod = Value::make_module();
    auto& u = *mod->module_rep;

    auto mk_fn = [](const std::string& mname, const std::string& name, size_t min_a, size_t max_a,
                    std::function<ValuePtr(const ValueVec&)> fn) {
        auto fv = std::make_shared<FunctionValue>();
        fv->name = name; fv->is_builtin = true;
        fv->builtin_fn = [mname, name, min_a, max_a, fn](const ValueVec& args, Environment&) -> ValuePtr {
            if (args.size() < min_a || (max_a != (size_t)-1 && args.size() > max_a))
                throw RuntimeError(mname + "." + name + " expects " +
                    std::to_string(min_a) + "~" + std::to_string(max_a) + " args, got " +
                    std::to_string(args.size()));
            return fn(args);
        };
        auto v = Value::make_none(); v->type = ValueType::Function; v->fn_rep = fv;
        return v;
    };
    auto add = [&](const std::string& n, size_t a0, size_t a1,
                   std::function<ValuePtr(const ValueVec&)> f) {
        u[n] = mk_fn("hash", n, a0, a1, std::move(f));
    };

    add("md5", 1, 1, [&](const ValueVec& a) {
        std::string in = a[0]->to_string();
        uint8_t d[16]; md5_final((const uint8_t*)in.data(), in.size(), d);
        std::string hex; hex_out(d, 16, hex);
        return Value::make_str(hex);
    });
    add("sha1", 1, 1, [&](const ValueVec& a) {
        std::string in = a[0]->to_string();
        uint8_t d[20]; sha1_final((const uint8_t*)in.data(), in.size(), d);
        std::string hex; hex_out(d, 20, hex);
        return Value::make_str(hex);
    });
    add("sha256", 1, 1, [&](const ValueVec& a) {
        std::string in = a[0]->to_string();
        uint8_t d[32]; sha256_final((const uint8_t*)in.data(), in.size(), d);
        std::string hex; hex_out(d, 32, hex);
        return Value::make_str(hex);
    });

    std_modules["hash"] = mod;
}

} // namespace vortex