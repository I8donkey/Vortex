// ============================================================
// net_core.h — 网络模块核心（纯 C++，忽略语言类型系统）
// 解释器 (net_module.cpp) 与编译运行时 (runtime.cpp) 共享，
// 保证 URL 编解码与 HTTP 在两端语义完全一致。
// HTTP 使用 Windows WinHTTP，不依赖 libcurl。
// ============================================================
#ifndef VORTEX_NET_CORE_H
#define VORTEX_NET_CORE_H

#include <string>
#include <vector>
#include <cstdlib>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace vortex {

// ---------- URL 百分比编码 / 解码 ----------
inline std::string net_url_encode(const std::string& in) {
    static const char* HEX = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if (c == '-' || c == '_' || c == '.' || c == '~' ||
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out += (char)c;
        } else {
            out += '%';
            out += HEX[(c >> 4) & 0xf];
            out += HEX[c & 0xf];
        }
    }
    return out;
}

inline std::string net_url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+') { out += ' '; }
        else if (c == '%' && i + 2 < in.size()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int hi = hex(in[i+1]), lo = hex(in[i+2]);
            if (hi >= 0 && lo >= 0) { out += (char)((hi << 4) | lo); i += 2; }
            else out += c;
        } else out += c;
    }
    return out;
}

#ifdef _WIN32
// ---------- UTF-8 <-> UTF-16 ----------
inline std::wstring net_u8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string net_wide_to_u8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---------- HTTP：GET/POST，返回响应体 ----------
// 失败时抛出 std::runtime_error。
inline std::string net_http(const std::string& url, const std::string& method,
                            const std::string& body, bool follow_redirects = true) {
    if (url.empty()) throw std::runtime_error("net: empty URL");
    size_t p = url.find("://");
    if (p == std::string::npos) throw std::runtime_error("net: bad URL (missing scheme)");
    std::string scheme = url.substr(0, p);
    std::string rest = url.substr(p + 3);
    if (scheme != "http" && scheme != "https") throw std::runtime_error("net: unsupported scheme: " + scheme);
    // authority = host[:port]
    size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string target = slash == std::string::npos ? "/" : rest.substr(slash); // 含 query
    // 拆分 host 与端口
    std::string host = authority;
    int port = scheme == "https" ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(':') == colon) {
        host = authority.substr(0, colon);
        int pr = std::atoi(authority.substr(colon + 1).c_str());
        if (pr > 0) port = pr;
    }

    HINTERNET hSess = WinHttpOpen(L"Vortex/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) throw std::runtime_error("net: WinHttpOpen failed");
    LONG status = 0; // 任一失败就地跳出
    std::string result;
    try {
        HINTERNET hConn = WinHttpConnect(hSess, net_u8_to_wide(host).c_str(), (INTERNET_PORT)port, 0);
        if (!hConn) throw std::runtime_error("net: WinHttpConnect failed");
        DWORD flags = (scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConn, net_u8_to_wide(method).c_str(),
                                            net_u8_to_wide(target).c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hReq) throw std::runtime_error("net: WinHttpOpenRequest failed");
        std::wstring wbody = net_u8_to_wide(body);
        BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     wbody.empty() ? WINHTTP_NO_REQUEST_DATA : (void*)wbody.data(),
                                     wbody.empty() ? 0 : (DWORD)(wbody.size() * sizeof(wchar_t)),
                                     wbody.empty() ? 0 : (DWORD)(wbody.size() * sizeof(wchar_t)), 0);
        if (!ok) throw std::runtime_error("net: WinHttpSendRequest failed");
        if (!WinHttpReceiveResponse(hReq, nullptr))
            throw std::runtime_error("net: WinHttpReceiveResponse failed");
        // 读取响应标头判断状态
        DWORD size = 0;
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            nullptr, &size, WINHTTP_NO_HEADER_INDEX);
        std::string statusLine;
        if (size > 0) {
            std::vector<wchar_t> hdr(size / sizeof(wchar_t) + 4);
            if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                    hdr.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
                statusLine = net_wide_to_u8(std::wstring(hdr.data()));
            }
        }
        LONG code = 0;
        DWORD csz = sizeof(LONG);
        if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &csz, WINHTTP_NO_HEADER_INDEX)) {
            status = (LONG)code;
        }
        if (follow_redirects && status >= 300 && status < 400) {
            // 跟随 Location
            DWORD lsz = 0;
            WinHttpQueryHeaders(hReq, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                nullptr, &lsz, WINHTTP_NO_HEADER_INDEX);
            if (lsz > 0) {
                std::vector<wchar_t> loc(lsz / sizeof(wchar_t) + 4);
                if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                        loc.data(), &lsz, WINHTTP_NO_HEADER_INDEX)) {
                    std::string newUrl = net_wide_to_u8(std::wstring(loc.data(), lsz / sizeof(wchar_t)));
                    if (!newUrl.empty()) {
                        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn);
                        WinHttpCloseHandle(hSess);
                        return net_http(newUrl, method, body, false);
                    }
                }
            }
        }
        // 读取响应体
        std::wstring wchunk;
        std::vector<char> buf(8192);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hReq, &avail)) break;
            if (avail == 0) break;
            if (avail > buf.size()) buf.resize(avail);
            DWORD read = 0;
            if (!WinHttpReadData(hReq, buf.data(), avail, &read)) break;
            if (read == 0) break;
            result.append(buf.data(), read);
        }
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
    } catch (...) {
        WinHttpCloseHandle(hSess);
        throw;
    }
    WinHttpCloseHandle(hSess);
    return result;
}
#endif // _WIN32

} // namespace vortex
#endif // VORTEX_NET_CORE_H