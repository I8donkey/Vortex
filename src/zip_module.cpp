// ============================================================
// zip_module.cpp — zip 归档模块（解压/打包/列表）
// 解释器端自持实现（zlib），与 runtime/zip 转发逻辑一致。
// ============================================================
#include "zip_module.h"
#include "value.h"
#include "interpreter.h"
#include <zlib.h>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>

namespace vortex {

namespace zzip {
struct Entry { std::string name; std::vector<unsigned char> data; };

static std::vector<Entry> read_archive(const std::string& path) {
    std::vector<Entry> out;
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), {});
    if (buf.size() < 22) return out;
    size_t eocd = (size_t)-1;
    for (size_t i = buf.size() >= 22 ? buf.size() - 22 : 0; i + 4 <= buf.size(); ++i)
        if (buf[i]==0x50 && (size_t)buf[i+1]==0x4b && (size_t)buf[i+2]==0x05 && (size_t)buf[i+3]==0x06) { eocd = i; break; }
    if (eocd == (size_t)-1) return out;
    auto rd16 = [&](size_t p){ return (size_t)buf[p] | ((size_t)buf[p+1]<<8); };
    auto rd32 = [&](size_t p){ return (size_t)buf[p] | ((size_t)buf[p+1]<<8) | ((size_t)buf[p+2]<<16) | ((size_t)buf[p+3]<<24); };
    size_t cd = rd32(eocd+16);
    size_t n = rd16(eocd+10);
    size_t p = cd;
    for (size_t k = 0; k < n && p + 46 <= buf.size(); ++k) {
        if (!(buf[p]==0x50 && (size_t)buf[p+1]==0x4b)) break;
        unsigned method = rd16(p+10);
        unsigned csize = (unsigned)rd32(p+20);
        unsigned usize = (unsigned)rd32(p+24);
        unsigned nlen = (unsigned)rd16(p+28);
        unsigned lho = (unsigned)rd32(p+42);
        std::string name(reinterpret_cast<const char*>(&buf[p+46]), nlen);
        if (lho + 30 <= buf.size() && buf[lho]==0x50 && (size_t)buf[lho+1]==0x4b) {
            unsigned lnlen = (unsigned)rd16(lho+26);
            unsigned lelen = (unsigned)rd16(lho+28);
            size_t ds = lho + 30 + lnlen + lelen;
            if (ds + csize <= buf.size()) {
                Entry e; e.name = name;
                if (method == 0) e.data.assign(buf.begin()+ds, buf.begin()+ds+csize);
                else if (method == 8) {
                    uLongf dlen = usize ? usize : csize*4+1024;
                    std::vector<unsigned char> raw(dlen);
                    int rc = uncompress(raw.data(), &dlen, &buf[ds], csize);
                    if (rc == Z_OK) raw.resize(dlen); else raw.clear();
                    e.data = std::move(raw);
                }
                out.push_back(std::move(e));
            }
        }
        p += 46 + nlen + rd16(p+30) + rd16(p+32);
    }
    return out;
}

static void write_archive(const std::string& path, std::vector<Entry>& es) {
    std::vector<unsigned char> out;
    std::vector<unsigned> cd_lho; cd_lho.reserve(es.size());
    std::vector<std::tuple<unsigned,unsigned,unsigned,unsigned>> cd_info; cd_info.reserve(es.size());
    auto put16 = [&](unsigned v){ out.push_back(v&0xff); out.push_back((v>>8)&0xff); };
    auto put32 = [&](unsigned v){ out.push_back(v&0xff); out.push_back((v>>8)&0xff); out.push_back((v>>16)&0xff); out.push_back((v>>24)&0xff); };
    for (auto& e : es) {
        uLongf clen = compressBound(e.data.size());
        std::vector<unsigned char> c(clen ? clen : 1);
        unsigned method = 0; unsigned csize = (unsigned)e.data.size(); unsigned usize = (unsigned)e.data.size();
        if (!e.data.empty()) {
            uLongf olen = clen;
            int rc = compress2(c.data(), &olen, e.data.data(), e.data.size(), 6);
            if (rc == Z_OK && olen < e.data.size()) { method = 8; csize = (unsigned)olen; clen = (unsigned)olen; }
        }
        unsigned crc = (unsigned)crc32(0L, Z_NULL, 0);
        if (!e.data.empty()) crc = (unsigned)crc32(crc, e.data.data(), e.data.size());
        unsigned lho = (unsigned)out.size();
        put32(0x04034b50); put16(20); put16(0x0800); put16(method);
        put16(0); put16(0); put32(crc); put32(csize); put32(usize);
        put16((unsigned)e.name.size()); put16(0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        if (method==0) out.insert(out.end(), e.data.begin(), e.data.end());
        else out.insert(out.end(), c.begin(), c.begin()+csize);
        cd_lho.push_back(lho);
        cd_info.push_back({crc, method, csize, usize});
    }
    unsigned cd_start = (unsigned)out.size();
    for (size_t j = 0; j < es.size(); ++j) {
        auto& e = es[j];
        auto cdi = cd_info[j];
        put32(0x02014b50); put16(20); put16(20); put16(0x0800); put16(std::get<1>(cdi));
        put16(0); put16(0); put32(std::get<0>(cdi)); put32(std::get<2>(cdi)); put32(std::get<3>(cdi));
        put16((unsigned)e.name.size()); put16(0); put16(0); put16(0); put16(0);
        put32(0); put32(cd_lho[j]);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    unsigned cd_size = (unsigned)out.size() - cd_start;
    put32(0x06054b50); put16(0); put16(0); put16((unsigned)es.size()); put16((unsigned)es.size());
    put32(cd_size); put32(cd_start); put16(0);
    std::ofstream f(path, std::ios::binary);
    f.write((char*)out.data(), (std::streamsize)out.size());
}
} // namespace zzip

void register_zip_module(std::unordered_map<std::string, ValuePtr>& std_modules) {
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
        u[n] = mk_fn("zip", n, a0, a1, std::move(f));
    };

    add("add", 3, 3, [&](const ValueVec& a) {
        std::string zf = a[0]->to_string();
        auto es = zzip::read_archive(zf);
        std::string n = a[1]->to_string();
        std::string d = a[2]->to_string();
        bool rep = false;
        for (auto& e : es) if (e.name == n) { e.data.assign(d.begin(), d.end()); rep = true; }
        if (!rep) { zzip::Entry e; e.name = n; e.data.assign(d.begin(), d.end()); es.push_back(std::move(e)); }
        std::sort(es.begin(), es.end(), [](const zzip::Entry& x, const zzip::Entry& y){ return x.name < y.name; });
        std::vector<zzip::Entry> copy = es;
        zzip::write_archive(zf, copy);
        return Value::make_none();
    });

    add("extract", 2, 2, [&](const ValueVec& a) {
        auto es = zzip::read_archive(a[0]->to_string());
        std::string n = a[1]->to_string();
        for (const auto& e : es) if (e.name == n)
            return Value::make_str(std::string(e.data.begin(), e.data.end()));
        throw RuntimeError("zip.extract: entry not found");
    });

    add("count", 1, 1, [&](const ValueVec& a) {
        return Value::make_int((long long)zzip::read_archive(a[0]->to_string()).size());
    });

    add("has", 2, 2, [&](const ValueVec& a) {
        auto es = zzip::read_archive(a[0]->to_string());
        std::string n = a[1]->to_string();
        for (const auto& e : es) if (e.name == n) return Value::make_bool(true);
        return Value::make_bool(false);
    });

    add("names", 1, 1, [&](const ValueVec& a) {
        auto r = Value::make_list();
        auto es = zzip::read_archive(a[0]->to_string());
        std::vector<std::string> names;
        for (const auto& e : es) names.push_back(e.name);
        std::sort(names.begin(), names.end());  // 排序保证确定性输出
        for (auto& n : names) r->list_rep->push_back(Value::make_str(n));
        return r;
    });

    std_modules["zip"] = mod;
}

} // namespace vortex