// vmake - a lightweight CMake-like configure/build tool.
//
// Single translation unit, C++17, standard library only. Parses a
// VmakeLists.txt script, produces a Ninja build file and a small cache.
// See vmake/examples/hello for a minimal project.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

static const char* kVmakeVersion = "0.1.0";
static const char* kDefaultProjectFile = "VmakeLists.txt";
static const char* kCacheFileName = "VmakeCache.txt";
static const char* kNinjaFileName = "build.ninja";

static fs::path g_vmakeExe;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

struct FatalError : std::runtime_error {
    explicit FatalError(const std::string& msg) : std::runtime_error(msg) {}
};

struct UsageError : std::runtime_error {
    explicit UsageError(const std::string& msg) : std::runtime_error(msg) {}
};

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

enum class LogLevel { Off = 0, ErrorLvl, WarningLvl, StatusLvl, VerboseLvl, DebugLvl, TraceLvl };

static LogLevel g_logLevel = LogLevel::StatusLvl;
static bool g_trace = false;

static bool logEnabled(LogLevel level) {
    return static_cast<int>(g_logLevel) >= static_cast<int>(level);
}

static void printStatus(const std::string& msg) {
    if (logEnabled(LogLevel::StatusLvl)) std::cout << "-- " << msg << std::endl;
}

static void printWarning(const std::string& msg) {
    if (logEnabled(LogLevel::WarningLvl)) std::cerr << "vmake warning: " << msg << std::endl;
}

static void printError(const std::string& msg) {
    std::cerr << "vmake error: " << msg << std::endl;
}

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------

static std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string joinStrings(const std::vector<std::string>& parts, const std::string& sep) {
    std::string r;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) r += sep;
        r += parts[i];
    }
    return r;
}

static std::vector<std::string> splitList(const std::string& value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = value.find(';', start);
        std::string part = (pos == std::string::npos) ? value.substr(start) : value.substr(start, pos - start);
        if (!part.empty()) out.push_back(part);
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}

static bool isTruthy(const std::string& value) {
    std::string s = toLower(trim(value));
    if (s.empty()) return false;
    if (s == "0" || s == "off" || s == "no" || s == "false" || s == "n" || s == "ignore" ||
        s == "notfound") {
        return false;
    }
    return true;
}

static bool hasWildcard(const std::string& s) {
    return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

static bool wildcardMatch(const std::string& pat, const std::string& str) {
    size_t p = 0, s = 0;
    size_t star = std::string::npos, mark = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            ++p;
            ++s;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = s;
        } else if (star != std::string::npos) {
            p = star + 1;
            s = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

static std::vector<fs::path> globOne(const fs::path& pattern) {
    std::vector<fs::path> res;
    fs::path dir = pattern.parent_path();
    std::string mask = pattern.filename().string();
    if (dir.empty()) dir = ".";
    if (!hasWildcard(mask)) {
        std::error_code ec;
        if (fs::exists(pattern, ec)) res.push_back(pattern);
        return res;
    }
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return res;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (wildcardMatch(mask, it->path().filename().string())) res.push_back(it->path());
    }
    std::sort(res.begin(), res.end());
    return res;
}

static bool toNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || errno != 0) return false;
    out = v;
    return true;
}

static std::vector<long> versionParts(const std::string& v) {
    std::vector<long> parts;
    std::string cur;
    for (char c : v) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            cur += c;
        } else if (!cur.empty()) {
            parts.push_back(std::strtol(cur.c_str(), nullptr, 10));
            cur.clear();
        }
    }
    if (!cur.empty()) parts.push_back(std::strtol(cur.c_str(), nullptr, 10));
    return parts;
}

static int versionCompare(const std::string& a, const std::string& b) {
    std::vector<long> pa = versionParts(a);
    std::vector<long> pb = versionParts(b);
    size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; ++i) {
        long x = (i < pa.size()) ? pa[i] : 0;
        long y = (i < pb.size()) ? pb[i] : 0;
        if (x < y) return -1;
        if (x > y) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Vortex (.vt) source helpers
// ---------------------------------------------------------------------------

static bool isVtSource(const fs::path& p) {
    std::string e = toLower(p.extension().string());
    return e == ".vt" || e == ".vtp";
}

static const std::set<std::string>& builtinVortexModules() {
    static const std::set<std::string> names = {
        "math", "time", "random", "thread", "log", "file", "zip", "xml", "html",
        "sql", "os", "regex", "json", "base64", "datetime", "csv", "hash", "str",
        "net", "sys", "game2d", "render3d", "gui"};
    return names;
}

// `import name;` or `import name as alias;` -> "name" ("" when not an import).
static std::string parseImportName(const std::string& line) {
    std::string s = line;
    size_t h = s.find('#');
    if (h != std::string::npos) s = s.substr(0, h);
    s = trim(s);
    if (s.rfind("import", 0) != 0) return std::string();
    if (s.size() > 6 && (s[6] == '_' || std::isalnum(static_cast<unsigned char>(s[6]))))
        return std::string();
    std::string rest = trim(s.substr(6));
    size_t end = rest.find_first_of(" \t\r;");
    std::string name = (end == std::string::npos) ? rest : rest.substr(0, end);
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
        name = name.substr(1, name.size() - 2);
    return name;
}

static fs::path findVortexModule(const std::string& name, const std::vector<fs::path>& dirs) {
    for (const fs::path& d : dirs) {
        fs::path a = d / (name + ".vt");
        if (fs::exists(a)) return a;
        fs::path b = d / (name + ".vtp");
        if (fs::exists(b)) return b;
    }
    return fs::path();
}

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------

static std::string quoteCommandArg(const std::string& arg) {
    if (arg.find_first_of(" \t\n\"") == std::string::npos) return arg;
    std::string r = "\"";
    for (char c : arg) {
        if (c == '"') r += "\\\"";
        else r += c;
    }
    r += "\"";
    return r;
}

static int runProcess(const std::vector<std::string>& args) {
    std::string cmd;
    for (const std::string& a : args) {
        if (!cmd.empty()) cmd += " ";
        cmd += quoteCommandArg(a);
    }
    int status = std::system(cmd.c_str());
#ifdef _WIN32
    return status;
#else
    if (status == -1) return 1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
#endif
}

// ---------------------------------------------------------------------------
// Script tokenizer / parser
// ---------------------------------------------------------------------------

struct Command {
    std::string name;
    std::vector<std::string> args;
    int line = 0;
};

static std::vector<Command> parseScriptFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw FatalError("cannot open script file: '" + file.string() + "'");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::vector<Command> cmds;
    size_t i = 0;
    const size_t n = text.size();
    int line = 1;

    while (i < n) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n') {
            ++line;
            ++i;
            continue;
        }
        if (std::isspace(c)) {
            ++i;
            continue;
        }
        if (c == '#') {
            while (i < n && text[i] != '\n') ++i;
            continue;
        }
        if (!(std::isalpha(c) || c == '_')) {
            throw FatalError(file.string() + ":" + std::to_string(line) + ": unexpected character '" +
                             std::string(1, static_cast<char>(c)) + "'");
        }
        size_t start = i;
        while (i < n && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) ++i;
        Command cmd;
        cmd.name = text.substr(start, i - start);
        cmd.line = line;

        while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) {
            if (text[i] == '\n') ++line;
            ++i;
        }
        if (i >= n || text[i] != '(') {
            throw FatalError(file.string() + ":" + std::to_string(cmd.line) + ": expected '(' after '" +
                             cmd.name + "'");
        }
        ++i;

        std::string cur;
        bool has = false;
        int depth = 0;
        bool closed = false;
        while (i < n) {
            char d = text[i];
            if (d == '\n') {
                ++line;
                ++i;
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(d))) {
                if (has) {
                    cmd.args.push_back(cur);
                    cur.clear();
                    has = false;
                }
                ++i;
                continue;
            }
            if (d == '#' && !has) {
                while (i < n && text[i] != '\n') ++i;
                continue;
            }
            if (d == ')') {
                if (depth == 0) {
                    ++i;
                    closed = true;
                    break;
                }
                cur += d;
                --depth;
                has = true;
                ++i;
                continue;
            }
            if (d == '(') {
                cur += d;
                ++depth;
                has = true;
                ++i;
                continue;
            }
            if (d == '"') {
                ++i;
                bool quoteClosed = false;
                while (i < n) {
                    char q = text[i];
                    if (q == '"') {
                        ++i;
                        quoteClosed = true;
                        break;
                    }
                    if (q == '\\' && i + 1 < n) {
                        char e = text[i + 1];
                        if (e == '"') {
                            cur += '"';
                            i += 2;
                            continue;
                        }
                        if (e == '\\') {
                            cur += '\\';
                            i += 2;
                            continue;
                        }
                        if (e == 'n') {
                            cur += '\n';
                            i += 2;
                            continue;
                        }
                        if (e == 't') {
                            cur += '\t';
                            i += 2;
                            continue;
                        }
                        if (e == 'r') {
                            cur += '\r';
                            i += 2;
                            continue;
                        }
                        if (e == '\n') {
                            ++line;
                            i += 2;
                            continue;
                        }
                        cur += '\\';
                        cur += e;
                        i += 2;
                        continue;
                    }
                    if (q == '\n') ++line;
                    cur += q;
                    ++i;
                }
                if (!quoteClosed) {
                    throw FatalError(file.string() + ":" + std::to_string(cmd.line) +
                                     ": unterminated quoted argument");
                }
                has = true;
                continue;
            }
            cur += d;
            has = true;
            ++i;
        }
        if (!closed) {
            throw FatalError(file.string() + ":" + std::to_string(cmd.line) +
                             ": unterminated command '" + cmd.name + "'");
        }
        if (has) cmd.args.push_back(cur);
        cmds.push_back(std::move(cmd));
    }
    return cmds;
}

// ---------------------------------------------------------------------------
// Project model
// ---------------------------------------------------------------------------

struct CacheEntry {
    std::string type;
    std::string value;
    std::string doc;
};

struct Target {
    enum class Type { Executable, StaticLibrary, SharedLibrary, ModuleLibrary };

    std::string name;
    Type type = Type::Executable;
    std::vector<fs::path> sources;
    std::vector<fs::path> incPrivate, incPublic, incInterface;
    std::vector<std::string> defsPrivate, defsPublic, defsInterface;
    std::vector<std::string> coptPrivate, coptPublic, coptInterface;
    std::vector<std::string> loptPrivate, loptPublic, loptInterface;
    std::vector<std::string> linkPrivate, linkPublic, linkInterface;
    std::string outputName;
    std::string cxxStandard;
    bool pic = false;
    fs::path srcDir;
    std::vector<std::string> installRuntime, installLibrary, installArchive;
    // Vortex (.vt) targets
    std::string vtMain;                 // VT_MAIN property: entry script
    std::vector<fs::path> vtModuleDirs; // extra `import` search directories
};

struct InstallFile {
    fs::path source;
    std::string dest;
};

struct Scope {
    Scope* parent = nullptr;
    std::map<std::string, std::string> vars;
    fs::path srcDir;
    fs::path binDir;
};

enum class FlowKind { Normal, Break, Continue, Return };

struct Flow {
    FlowKind kind = FlowKind::Normal;
};

struct Interp;

// Condition expression evaluator (defined after Interp).
struct CondParser {
    const Interp& it;
    std::vector<std::string> tokens;
    size_t pos = 0;

    CondParser(const Interp& interp, std::vector<std::string> toks)
        : it(interp), tokens(std::move(toks)) {}

    bool parseOr();
    bool parseAnd();
    bool parseUnary();

    bool atEnd() const { return pos >= tokens.size(); }
    bool peekKeyword(const char* kw) const {
        return !atEnd() && toLower(tokens[pos]) == kw;
    }
    std::string nextToken() {
        if (atEnd()) return std::string();
        return tokens[pos++];
    }

    // CMake-like semantics: unquoted operands are dereferenced as variables when
    // a variable with that name exists, otherwise they are used literally.
    std::string operand(const std::string& token) const;
};

struct Interp {
    fs::path sourceDir;
    fs::path buildDir;
    bool scriptMode = false;
    bool hadError = false;
    bool seenProject = false;
    std::string projectFile = kDefaultProjectFile;
    fs::path currentListDir;

    std::deque<Scope> scopes;
    Scope* scope = nullptr;

    std::map<std::string, Target> targets;
    std::vector<std::string> targetOrder;
    std::vector<InstallFile> installFiles;
    std::map<std::string, CacheEntry> cache;
    // target -> interpreter entry inside the build tree (vt/<main>.vt)
    std::map<std::string, std::string> vtEntries;

    // ---- scopes and variables ----

    void initScope(const fs::path& src, const fs::path& bin) {
        scopes.emplace_back();
        scope = &scopes.back();
        scope->srcDir = src;
        scope->binDir = bin;
        scope->vars["VMAKE_SOURCE_DIR"] = src.string();
        scope->vars["VMAKE_BINARY_DIR"] = bin.string();
    }

    Scope* pushScope(Scope* parent, const fs::path& src, const fs::path& bin) {
        scopes.emplace_back();
        Scope* s = &scopes.back();
        s->parent = parent;
        s->srcDir = src;
        s->binDir = bin;
        return s;
    }

    bool lookup(const std::string& name, std::string& out) const {
        for (Scope* s = scope; s != nullptr; s = s->parent) {
            auto it = s->vars.find(name);
            if (it != s->vars.end()) {
                out = it->second;
                return true;
            }
        }
        auto c = cache.find(name);
        if (c != cache.end()) {
            out = c->second.value;
            return true;
        }
        return false;
    }

    std::string getVar(const std::string& name) const {
        std::string v;
        lookup(name, v);
        return v;
    }

    bool hasVar(const std::string& name) const {
        std::string v;
        return lookup(name, v);
    }

    void setVar(const std::string& name, const std::string& value, bool parentScope = false) {
        if (parentScope && scope != nullptr && scope->parent != nullptr) {
            scope->parent->vars[name] = value;
        } else if (scope != nullptr) {
            scope->vars[name] = value;
        }
    }

    std::string expand(const std::string& input, int depth = 0) const {
        if (depth > 32) return input;
        std::string out;
        out.reserve(input.size());
        size_t i = 0;
        const size_t n = input.size();
        while (i < n) {
            if (input[i] == '$' && i + 1 < n) {
                char c = input[i + 1];
                if (c == '{') {
                    size_t close = input.find('}', i + 2);
                    if (close != std::string::npos) {
                        std::string name = input.substr(i + 2, close - i - 2);
                        std::string value;
                        if (lookup(name, value)) out += expand(value, depth + 1);
                        i = close + 1;
                        continue;
                    }
                } else if ((c == 'E' || c == 'e') && i + 5 <= n &&
                           toLower(input.substr(i + 1, 4)) == "env{") {
                    size_t close = input.find('}', i + 5);
                    if (close != std::string::npos) {
                        std::string name = input.substr(i + 5, close - (i + 5));
                        const char* v = std::getenv(name.c_str());
                        if (v != nullptr) out += v;
                        i = close + 1;
                        continue;
                    }
                } else if ((c == 'C' || c == 'c') && i + 7 <= n &&
                           toLower(input.substr(i + 1, 6)) == "cache{") {
                    size_t close = input.find('}', i + 7);
                    if (close != std::string::npos) {
                        std::string name = input.substr(i + 7, close - (i + 7));
                        auto entry = cache.find(name);
                        if (entry != cache.end()) out += entry->second.value;
                        i = close + 1;
                        continue;
                    }
                }
            }
            out += input[i++];
        }
        return out;
    }

    std::vector<std::string> expandArgs(const std::vector<std::string>& in) const {
        std::vector<std::string> out;
        out.reserve(in.size());
        for (const std::string& s : in) out.push_back(expand(s));
        return out;
    }

    static std::vector<std::string> flattenLists(const std::vector<std::string>& in) {
        std::vector<std::string> out;
        for (const std::string& s : in) {
            std::vector<std::string> parts = splitList(s);
            out.insert(out.end(), parts.begin(), parts.end());
        }
        return out;
    }

    fs::path resolveSourcePath(const std::string& p) const {
        fs::path path(p);
        if (path.is_relative()) path = scope->srcDir / path;
        return path.lexically_normal();
    }

    fs::path resolveBinaryPath(const std::string& p) const {
        fs::path path(p);
        if (path.is_relative()) path = scope->binDir / path;
        return path.lexically_normal();
    }

    // ---- cache ----

    void loadCache() {
        cache.clear();
        std::ifstream in(buildDir / kCacheFileName, std::ios::binary);
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string keyPart = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            size_t colon = keyPart.find(':');
            std::string name = keyPart;
            std::string type = "UNINITIALIZED";
            if (colon != std::string::npos) {
                name = keyPart.substr(0, colon);
                type = keyPart.substr(colon + 1);
            }
            CacheEntry entry;
            entry.type = type;
            entry.value = value;
            cache[name] = entry;
        }
    }

    void writeCache() const {
        fs::create_directories(buildDir);
        std::ofstream out(buildDir / kCacheFileName, std::ios::binary | std::ios::trunc);
        if (!out) throw FatalError("cannot write cache file: '" + (buildDir / kCacheFileName).string() + "'");
        out << "# vmake cache file, generated automatically. Do not edit.\n";
        for (const auto& kv : cache) {
            std::string value = kv.second.value;
            std::string clean;
            for (char c : value) {
                if (c != '\n' && c != '\r') clean += c;
            }
            out << kv.first << ":" << kv.second.type << "=" << clean << "\n";
        }
    }

    void applyDefine(const std::string& def) {
        size_t eq = def.find('=');
        std::string keyPart = (eq == std::string::npos) ? def : def.substr(0, eq);
        std::string value = (eq == std::string::npos) ? std::string() : def.substr(eq + 1);
        std::string name = keyPart;
        std::string type = "UNINITIALIZED";
        size_t colon = keyPart.find(':');
        if (colon != std::string::npos) {
            name = keyPart.substr(0, colon);
            type = keyPart.substr(colon + 1);
        }
        CacheEntry entry;
        auto existing = cache.find(name);
        if (existing != cache.end()) entry = existing->second;
        entry.type = type;
        entry.value = value;
        cache[name] = entry;
    }

    void unsetCache(const std::string& pattern) {
        for (auto it = cache.begin(); it != cache.end();) {
            if (wildcardMatch(pattern, it->first)) it = cache.erase(it);
            else ++it;
        }
    }

    void initDefaults() {
        auto setDefault = [&](const char* name, const char* type, const char* value, const char* doc) {
            auto it = cache.find(name);
            if (it == cache.end()) cache[name] = CacheEntry{type, value, doc};
            else if (it->second.doc.empty()) it->second.doc = doc;
        };
        setDefault("VMAKE_GENERATOR", "STRING", "Ninja", "Generator used by vmake");
        setDefault("VMAKE_BUILD_TYPE", "STRING", "Release", "Build configuration type");
        setDefault("VMAKE_INSTALL_PREFIX", "PATH", "/usr/local", "Install prefix");
        setDefault("VMAKE_CXX_COMPILER", "FILEPATH", "g++", "C++ compiler");
        setDefault("VMAKE_C_COMPILER", "FILEPATH", "gcc", "C compiler");
        setDefault("VMAKE_CXX_FLAGS", "STRING", "-O2 -Wall", "Global C++ compiler flags");
        setDefault("VMAKE_C_FLAGS", "STRING", "-O2 -Wall", "Global C compiler flags");
        setDefault("VMAKE_EXE_LINKER_FLAGS", "STRING", "", "Global executable linker flags");
        setDefault("VMAKE_CXX_STANDARD", "STRING", "17", "Default C++ standard");
        setDefault("VMAKE_VTCC", "FILEPATH", "vtcc", "Vortex native compiler used for .vt targets");
        setDefault("VMAKE_VORTEX", "FILEPATH", "vortex", "Vortex interpreter used by --run");
        cache["VMAKE_HOME_DIRECTORY"] = CacheEntry{"PATH", sourceDir.string(), "Source directory"};

        if (toLower(cache["VMAKE_GENERATOR"].value) != "ninja") {
            throw FatalError("generator '" + cache["VMAKE_GENERATOR"].value +
                             "' is not supported; the only supported generator is 'Ninja'");
        }
    }

    void printCacheVars(bool advanced, bool withHelp, const std::string& regexText) const {
        std::regex re;
        bool useRegex = false;
        if (!regexText.empty()) {
            try {
                re = std::regex(regexText);
                useRegex = true;
            } catch (const std::regex_error&) {
                throw FatalError("invalid cache filter regex: '" + regexText + "'");
            }
        }
        printStatus("Cache values");
        for (const auto& kv : cache) {
            if (!advanced && kv.second.type == "INTERNAL") continue;
            if (useRegex && !std::regex_search(kv.first, re)) continue;
            if (withHelp && !kv.second.doc.empty()) std::cout << "// " << kv.second.doc << "\n";
            std::cout << kv.first << ":" << kv.second.type << "=" << kv.second.value << "\n";
        }
    }

    // ---- execution ----

    void runScriptFile(const fs::path& file) {
        fs::path saved = currentListDir;
        currentListDir = file.parent_path();
        std::vector<Command> cmds = parseScriptFile(file);
        Flow flow = execRange(cmds, 0, cmds.size());
        currentListDir = saved;
        if (flow.kind != FlowKind::Normal) {
            throw FatalError("'" + file.string() + "': flow control statement outside of a loop");
        }
    }

    Flow execRange(const std::vector<Command>& cmds, size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const Command& cmd = cmds[i];
            std::string name = toLower(cmd.name);
            if (g_trace) {
                std::string line = "[vmake trace] " + cmd.name + "(";
                for (size_t k = 0; k < cmd.args.size(); ++k) {
                    if (k) line += " ";
                    line += cmd.args[k];
                }
                line += ")";
                std::cout << line << std::endl;
            }
            if (name == "if") {
                size_t endPos = 0;
                std::vector<size_t> branches;
                findBlockEnd(cmds, i, end, "endif", {"elseif", "else"}, branches, endPos);

                size_t selBegin = std::string::npos;
                size_t selEnd = std::string::npos;
                if (evalCondition(cmd.args)) {
                    selBegin = i + 1;
                    selEnd = branches.empty() ? endPos : branches[0];
                } else {
                    for (size_t bi = 0; bi < branches.size(); ++bi) {
                        size_t nextPos = (bi + 1 < branches.size()) ? branches[bi + 1] : endPos;
                        const Command& br = cmds[branches[bi]];
                        if (toLower(br.name) == "elseif") {
                            if (evalCondition(br.args)) {
                                selBegin = branches[bi] + 1;
                                selEnd = nextPos;
                                break;
                            }
                        } else {
                            selBegin = branches[bi] + 1;
                            selEnd = nextPos;
                            break;
                        }
                    }
                }
                i = endPos;
                if (selBegin != std::string::npos) {
                    Flow flow = execRange(cmds, selBegin, selEnd);
                    if (flow.kind != FlowKind::Normal) return flow;
                }
                continue;
            }
            if (name == "foreach") {
                size_t endPos = 0;
                std::vector<size_t> none;
                findBlockEnd(cmds, i, end, "endforeach", {}, none, endPos);

                const std::vector<std::string>& raw = cmd.args;
                if (raw.empty()) throw FatalError(where(cmd) + "foreach() requires a variable name");
                std::string loopVar = raw[0];
                std::vector<std::string> items;

                if (raw.size() >= 2 && toLower(raw[1]) == "range") {
                    long startV = 0, stopV = 0, stepV = 1;
                    try {
                        if (raw.size() == 3) {
                            stopV = std::stol(expand(raw[2]));
                        } else if (raw.size() >= 4) {
                            startV = std::stol(expand(raw[2]));
                            stopV = std::stol(expand(raw[3]));
                            if (raw.size() >= 5) stepV = std::stol(expand(raw[4]));
                        } else {
                            throw FatalError(where(cmd) + "foreach(RANGE) needs at least one number");
                        }
                    } catch (const std::invalid_argument&) {
                        throw FatalError(where(cmd) + "foreach(RANGE) expects integer values");
                    } catch (const std::out_of_range&) {
                        throw FatalError(where(cmd) + "foreach(RANGE) number out of range");
                    }
                    if (stepV == 0) throw FatalError(where(cmd) + "foreach(RANGE) step must not be zero");
                    for (long v = startV; (stepV > 0) ? (v < stopV) : (v > stopV); v += stepV) {
                        items.push_back(std::to_string(v));
                    }
                } else if (raw.size() >= 3 && toLower(raw[1]) == "in" && toLower(raw[2]) == "lists") {
                    for (size_t k = 3; k < raw.size(); ++k) {
                        std::vector<std::string> parts = splitList(getVar(expand(raw[k])));
                        items.insert(items.end(), parts.begin(), parts.end());
                    }
                } else {
                    for (size_t k = 1; k < raw.size(); ++k) {
                        std::vector<std::string> parts = splitList(expand(raw[k]));
                        items.insert(items.end(), parts.begin(), parts.end());
                    }
                }

                for (const std::string& item : items) {
                    setVar(loopVar, item);
                    Flow flow = execRange(cmds, i + 1, endPos);
                    if (flow.kind == FlowKind::Break) break;
                    if (flow.kind == FlowKind::Continue) continue;
                    if (flow.kind == FlowKind::Return) return flow;
                }
                i = endPos;
                continue;
            }
            if (name == "break") return Flow{FlowKind::Break};
            if (name == "continue") return Flow{FlowKind::Continue};
            if (name == "elseif" || name == "else" || name == "endif" || name == "endforeach") {
                throw FatalError(where(cmd) + "unexpected '" + cmd.name + "' (no matching block opener)");
            }
            execSimple(cmd);
        }
        return Flow{};
    }

    std::string where(const Command& cmd) const {
        return "line " + std::to_string(cmd.line) + ": ";
    }

    void findBlockEnd(const std::vector<Command>& cmds, size_t start, size_t limit,
                      const std::string& closer, const std::set<std::string>& branchNames,
                      std::vector<size_t>& branches, size_t& endPos) const {
        int depth = 0;
        for (size_t j = start + 1; j < limit; ++j) {
            std::string ln = toLower(cmds[j].name);
            if (ln == "if" || ln == "foreach") {
                ++depth;
                continue;
            }
            if (ln == "endif" || ln == "endforeach") {
                if (depth == 0) {
                    if (ln != closer) {
                        throw FatalError(where(cmds[j]) + "unexpected '" + cmds[j].name + "'");
                    }
                    endPos = j;
                    return;
                }
                --depth;
                continue;
            }
            if (depth == 0 && branchNames.count(ln)) branches.push_back(j);
        }
        throw FatalError(where(cmds[start]) + "missing '" + closer + "' for '" + cmds[start].name + "'");
    }

    // ---- condition evaluation ----

    bool evalCondition(const std::vector<std::string>& args) {
        std::vector<std::string> tokens;
        for (const std::string& a : args) {
            std::string cur;
            for (char c : a) {
                if (c == '(' || c == ')') {
                    if (!cur.empty()) {
                        tokens.push_back(cur);
                        cur.clear();
                    }
                    tokens.push_back(std::string(1, c));
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) tokens.push_back(cur);
        }
        CondParser parser(*this, std::move(tokens));
        return parser.parseOr();
    }

    // ---- target lookup ----

    Target& requireTarget(const std::string& name, const Command& cmd) {
        auto it = targets.find(name);
        if (it == targets.end()) {
            throw FatalError(where(cmd) + "target '" + name + "' does not exist");
        }
        return it->second;
    }

    // ---- command dispatch ----

    void execSimple(const Command& cmd) {
        const std::string name = toLower(cmd.name);
        if (name == "vmake_minimum_required" || name == "cmake_minimum_required" ||
            name == "include_guard") {
            return;
        }
        if (name == "project") {
            std::vector<std::string> a = expandArgs(cmd.args);
            if (a.empty()) throw FatalError(where(cmd) + "project() requires a project name");
            setVar("PROJECT_NAME", a[0]);
            setVar("VMAKE_PROJECT_NAME", a[0]);
            setVar("PROJECT_SOURCE_DIR", scope->srcDir.string());
            setVar("PROJECT_BINARY_DIR", scope->binDir.string());
            if (!seenProject) {
                seenProject = true;
                setVar("VMAKE_SOURCE_DIR", sourceDir.string());
                setVar("VMAKE_BINARY_DIR", buildDir.string());
            }
            return;
        }
        if (name == "set") {
            if (cmd.args.empty()) throw FatalError(where(cmd) + "set() requires a variable name");
            std::string varName = cmd.args[0];
            std::vector<std::string> values;
            bool parentScope = false;
            size_t cachePos = std::string::npos;
            for (size_t k = 1; k < cmd.args.size(); ++k) {
                std::string la = toLower(cmd.args[k]);
                if (la == "cache") {
                    cachePos = k;
                    break;
                }
                if (la == "parent_scope") {
                    parentScope = true;
                    continue;
                }
                values.push_back(expand(cmd.args[k]));
            }
            if (cachePos != std::string::npos) {
                std::string type = (cachePos + 1 < cmd.args.size()) ? expand(cmd.args[cachePos + 1])
                                                                   : std::string("STRING");
                std::string doc = (cachePos + 2 < cmd.args.size()) ? expand(cmd.args[cachePos + 2])
                                                                  : std::string();
                bool force = false;
                for (size_t k = cachePos + 3; k < cmd.args.size(); ++k) {
                    if (toLower(cmd.args[k]) == "force") force = true;
                }
                auto it = cache.find(varName);
                if (it == cache.end() || force) {
                    cache[varName] = CacheEntry{type, joinStrings(values, ";"), doc};
                }
                return;
            }
            setVar(varName, joinStrings(values, ";"), parentScope);
            return;
        }
        if (name == "unset") {
            std::vector<std::string> a = expandArgs(cmd.args);
            if (a.empty()) throw FatalError(where(cmd) + "unset() requires a variable name");
            for (size_t k = 1; k < a.size(); ++k) {
                if (toLower(a[k]) == "cache") {
                    cache.erase(a[0]);
                    return;
                }
            }
            if (scope != nullptr) scope->vars.erase(a[0]);
            return;
        }
        if (name == "option") {
            std::vector<std::string> a = expandArgs(cmd.args);
            if (a.empty()) throw FatalError(where(cmd) + "option() requires a variable name");
            if (cache.find(a[0]) == cache.end()) {
                std::string doc = (a.size() > 1) ? a[1] : std::string();
                std::string value = (a.size() > 2) ? a[2] : std::string("OFF");
                cache[a[0]] = CacheEntry{"BOOL", value, doc};
            }
            return;
        }
        if (name == "list") {
            std::vector<std::string> a = flattenLists(expandArgs(cmd.args));
            if (a.empty()) throw FatalError(where(cmd) + "list() requires a sub-command");
            std::string sub = toLower(a[0]);
            if (sub == "append" || sub == "prepend") {
                if (a.size() < 2) throw FatalError(where(cmd) + "list(" + a[0] + ") requires a variable");
                std::string current = getVar(a[1]);
                std::vector<std::string> parts = splitList(current);
                if (sub == "append") {
                    for (size_t k = 2; k < a.size(); ++k) parts.push_back(a[k]);
                } else {
                    std::vector<std::string> newer;
                    for (size_t k = 2; k < a.size(); ++k) newer.push_back(a[k]);
                    newer.insert(newer.end(), parts.begin(), parts.end());
                    parts = newer;
                }
                setVar(a[1], joinStrings(parts, ";"));
                return;
            }
            if (sub == "length") {
                if (a.size() < 3) throw FatalError(where(cmd) + "list(LENGTH) requires a variable and output");
                setVar(a[2], std::to_string(splitList(getVar(a[1])).size()));
                return;
            }
            printStatus("vmake: 'list(" + a[0] + ")' is not supported yet, ignored");
            return;
        }
        if (name == "message") {
            if (cmd.args.empty()) return;
            std::string mode = toLower(cmd.args[0]);
            static const std::set<std::string> modes = {
                "status", "warning", "author_warning", "error", "fatal_error", "verbose",
                "debug", "trace", "notice", "deprecation", "send_error"};
            bool known = modes.count(mode) != 0;
            size_t start = known ? 1 : 0;
            std::string text;
            for (size_t k = start; k < cmd.args.size(); ++k) text += expand(cmd.args[k]);
            if (!known) {
                std::cout << text << std::endl;
            } else if (mode == "status") {
                printStatus(text);
            } else if (mode == "verbose") {
                if (logEnabled(LogLevel::VerboseLvl)) std::cout << "-- " << text << std::endl;
            } else if (mode == "debug") {
                if (logEnabled(LogLevel::DebugLvl)) std::cout << "-- " << text << std::endl;
            } else if (mode == "trace") {
                if (logEnabled(LogLevel::TraceLvl)) std::cout << "-- " << text << std::endl;
            } else if (mode == "warning" || mode == "author_warning" || mode == "deprecation") {
                printWarning(text);
            } else if (mode == "notice") {
                std::cout << text << std::endl;
            } else if (mode == "error" || mode == "send_error") {
                printError(text);
                hadError = true;
            } else {
                throw FatalError(text);
            }
            return;
        }
        if (name == "add_subdirectory") {
            std::vector<std::string> a = expandArgs(cmd.args);
            if (a.empty()) throw FatalError(where(cmd) + "add_subdirectory() requires a directory");
            fs::path subSrc = resolveSourcePath(a[0]);
            fs::path subBin = (a.size() >= 2 && !a[1].empty()) ? resolveBinaryPath(a[1])
                                                               : (scope->binDir / a[0]);
            fs::path listFile = subSrc / projectFile;
            if (!fs::exists(listFile)) {
                throw FatalError(where(cmd) + "cannot find '" + listFile.string() + "'");
            }
            fs::create_directories(subBin);
            Scope* parent = scope;
            Scope* child = pushScope(parent, subSrc, subBin);
            scope = child;
            runScriptFile(listFile);
            scope = parent;
            return;
        }
        if (name == "include") {
            std::vector<std::string> a = expandArgs(cmd.args);
            if (a.empty()) throw FatalError(where(cmd) + "include() requires a file name");
            fs::path f(a[0]);
            if (f.is_relative()) f = currentListDir / f;
            if (!fs::exists(f) && f.extension().empty()) {
                fs::path alt = f;
                alt += ".vmake";
                if (fs::exists(alt)) f = alt;
            }
            f = f.lexically_normal();
            if (!fs::exists(f)) {
                throw FatalError(where(cmd) + "include file not found: '" + f.string() + "'");
            }
            runScriptFile(f);
            return;
        }
        if (name == "add_executable" || name == "add_library") {
            std::vector<std::string> a = flattenLists(expandArgs(cmd.args));
            if (a.empty()) throw FatalError(where(cmd) + "command requires a target name");
            std::string tname = a[0];
            if (targets.count(tname)) throw FatalError(where(cmd) + "target '" + tname + "' already exists");
            Target t;
            t.name = tname;
            t.srcDir = scope->srcDir;
            size_t srcStart = 1;
            if (name == "add_library") {
                t.type = Target::Type::StaticLibrary;
                if (a.size() >= 2) {
                    std::string ty = toLower(a[1]);
                    if (ty == "static") {
                        t.type = Target::Type::StaticLibrary;
                        srcStart = 2;
                    } else if (ty == "shared") {
                        t.type = Target::Type::SharedLibrary;
                        srcStart = 2;
                    } else if (ty == "module") {
                        t.type = Target::Type::ModuleLibrary;
                        srcStart = 2;
                    } else if (ty == "interface" || ty == "object") {
                        throw FatalError(where(cmd) + "library type '" + a[1] +
                                         "' is not supported by vmake");
                    }
                }
            }
            for (size_t k = srcStart; k < a.size(); ++k) {
                std::string la = toLower(a[k]);
                if (la == "win32" || la == "macosx_bundle" || la == "exclude_from_all" ||
                    la == "imported" || la == "global") {
                    continue;
                }
                fs::path p = a[k];
                if (p.is_relative()) p = scope->srcDir / p;
                p = p.lexically_normal();
                if (hasWildcard(p.filename().string())) {
                    std::vector<fs::path> matches = globOne(p);
                    t.sources.insert(t.sources.end(), matches.begin(), matches.end());
                } else {
                    t.sources.push_back(p);
                }
            }
            targets[tname] = t;
            targetOrder.push_back(tname);
            return;
        }
        if (name == "target_sources") {
            std::vector<std::string> a = flattenLists(expandArgs(cmd.args));
            if (a.empty()) throw FatalError(where(cmd) + "target_sources() requires a target");
            Target& t = requireTarget(a[0], cmd);
            for (size_t k = 1; k < a.size(); ++k) {
                std::string la = toLower(a[k]);
                if (la == "public" || la == "private" || la == "interface") continue;
                fs::path p = a[k];
                if (p.is_relative()) p = scope->srcDir / p;
                p = p.lexically_normal();
                if (hasWildcard(p.filename().string())) {
                    std::vector<fs::path> matches = globOne(p);
                    t.sources.insert(t.sources.end(), matches.begin(), matches.end());
                } else {
                    t.sources.push_back(p);
                }
            }
            return;
        }
        if (name == "target_vortex_modules") {
            std::vector<std::string> a = flattenLists(expandArgs(cmd.args));
            if (a.empty())
                throw FatalError(where(cmd) + "target_vortex_modules() requires a target");
            Target& t = requireTarget(a[0], cmd);
            for (size_t k = 1; k < a.size(); ++k) {
                fs::path p = a[k];
                if (p.is_relative()) p = scope->srcDir / p;
                t.vtModuleDirs.push_back(p.lexically_normal());
            }
            return;
        }
        if (name == "target_include_directories" || name == "target_link_libraries" ||
            name == "target_compile_definitions" || name == "target_compile_options" ||
            name == "target_link_options") {
            std::vector<std::string> a = flattenLists(expandArgs(cmd.args));
            if (a.empty()) throw FatalError(where(cmd) + "command requires a target");
            Target& t = requireTarget(a[0], cmd);
            int vis = 0;  // 0 private, 1 public, 2 interface
            for (size_t k = 1; k < a.size(); ++k) {
                std::string la = toLower(a[k]);
                if (la == "public" || la == "link_public") {
                    vis = 1;
                    continue;
                }
                if (la == "private" || la == "link_private") {
                    vis = 0;
                    continue;
                }
                if (la == "interface" || la == "link_interface") {
                    vis = 2;
                    continue;
                }
                if (la == "system" || la == "before") continue;

                if (name == "target_include_directories") {
                    fs::path p = a[k];
                    if (p.is_relative()) p = scope->srcDir / p;
                    p = p.lexically_normal();
                    if (vis == 0) t.incPrivate.push_back(p);
                    else if (vis == 1) t.incPublic.push_back(p);
                    else t.incInterface.push_back(p);
                } else if (name == "target_link_libraries") {
                    if (vis == 0) t.linkPrivate.push_back(a[k]);
                    else if (vis == 1) t.linkPublic.push_back(a[k]);
                    else t.linkInterface.push_back(a[k]);
                } else if (name == "target_compile_definitions") {
                    if (vis == 0) t.defsPrivate.push_back(a[k]);
                    else if (vis == 1) t.defsPublic.push_back(a[k]);
                    else t.defsInterface.push_back(a[k]);
                } else if (name == "target_compile_options") {
                    if (vis == 0) t.coptPrivate.push_back(a[k]);
                    else if (vis == 1) t.coptPublic.push_back(a[k]);
                    else t.coptInterface.push_back(a[k]);
                } else {
                    if (vis == 0) t.loptPrivate.push_back(a[k]);
                    else if (vis == 1) t.loptPublic.push_back(a[k]);
                    else t.loptInterface.push_back(a[k]);
                }
            }
            return;
        }
        if (name == "set_target_properties") {
            std::vector<std::string> a = expandArgs(cmd.args);
            if (a.empty()) throw FatalError(where(cmd) + "set_target_properties() requires a target");
            Target& t = requireTarget(a[0], cmd);
            size_t k = 1;
            while (k < a.size() && toLower(a[k]) != "properties") ++k;
            ++k;
            for (; k + 1 < a.size(); k += 2) {
                std::string key = toUpper(a[k]);
                std::string value = a[k + 1];
                if (key == "OUTPUT_NAME") t.outputName = value;
                else if (key == "CXX_STANDARD") t.cxxStandard = value;
                else if (key == "POSITION_INDEPENDENT_CODE") t.pic = isTruthy(value);
                else if (key == "VT_MAIN") t.vtMain = value;
            }
            return;
        }
        if (name == "file") {
            execFile(cmd);
            return;
        }
        if (name == "get_filename_component") {
            std::vector<std::string> a = expandArgs(cmd.args);
            if (a.size() < 3) throw FatalError(where(cmd) + "get_filename_component() needs 3 arguments");
            fs::path p(a[1]);
            std::string mode = toUpper(a[2]);
            if (mode == "ABSOLUTE") {
                if (p.is_relative()) {
                    fs::path base = scope->srcDir;
                    if (a.size() >= 5 && toUpper(a[3]) == "BASE_DIR") base = fs::path(a[4]);
                    p = base / p;
                }
                setVar(a[0], p.lexically_normal().string());
            } else if (mode == "NAME") {
                setVar(a[0], p.filename().string());
            } else if (mode == "NAME_WE") {
                setVar(a[0], p.stem().string());
            } else if (mode == "EXT") {
                setVar(a[0], p.extension().string());
            } else if (mode == "DIR") {
                std::string d = p.parent_path().string();
                setVar(a[0], d.empty() ? std::string(".") : d);
            } else {
                throw FatalError(where(cmd) + "get_filename_component(): unsupported mode '" + a[2] + "'");
            }
            return;
        }
        if (name == "configure_file") {
            std::vector<std::string> a = expandArgs(cmd.args);
            if (a.size() < 2) throw FatalError(where(cmd) + "configure_file() needs input and output");
            fs::path in = resolveSourcePath(a[0]);
            fs::path out = resolveBinaryPath(a[1]);
            std::ifstream f(in, std::ios::binary);
            if (!f) throw FatalError(where(cmd) + "cannot read '" + in.string() + "'");
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string replaced;
            for (size_t i = 0; i < text.size();) {
                if (text[i] == '@') {
                    size_t close = text.find('@', i + 1);
                    if (close != std::string::npos) {
                        std::string varName = text.substr(i + 1, close - i - 1);
                        bool valid = !varName.empty();
                        for (char ch : varName) {
                            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
                                valid = false;
                                break;
                            }
                        }
                        if (valid) {
                            replaced += getVar(varName);
                            i = close + 1;
                            continue;
                        }
                    }
                }
                replaced += text[i++];
            }
            replaced = expand(replaced);
            fs::create_directories(out.parent_path());
            std::ofstream of(out, std::ios::binary | std::ios::trunc);
            if (!of) throw FatalError(where(cmd) + "cannot write '" + out.string() + "'");
            of << replaced;
            return;
        }
        if (name == "install") {
            execInstall(cmd);
            return;
        }
        if (name == "find_package" || name == "add_custom_command" || name == "add_custom_target" ||
            name == "try_compile") {
            printStatus("vmake: '" + cmd.name + "' is not supported yet, ignored");
            return;
        }
        throw FatalError(where(cmd) + "unknown command '" + cmd.name + "'");
    }

    void execFile(const Command& cmd) {
        std::vector<std::string> a = expandArgs(cmd.args);
        if (a.empty()) throw FatalError(where(cmd) + "file() requires a sub-command");
        std::string sub = toLower(a[0]);

        if (sub == "glob") {
            if (a.size() < 2) throw FatalError(where(cmd) + "file(GLOB) requires an output variable");
            std::string var = a[1];
            size_t k = 2;
            fs::path relDir;
            bool haveRel = false;
            while (k < a.size()) {
                std::string la = toLower(a[k]);
                if (la == "relative" && k + 1 < a.size()) {
                    relDir = resolveSourcePath(a[k + 1]);
                    haveRel = true;
                    k += 2;
                    continue;
                }
                if ((la == "list_directories" || la == "follow_symlinks") && k + 1 < a.size()) {
                    k += 2;
                    continue;
                }
                break;
            }
            std::vector<std::string> found;
            for (; k < a.size(); ++k) {
                fs::path pattern = a[k];
                if (pattern.is_relative()) pattern = scope->srcDir / pattern;
                std::vector<fs::path> matches = globOne(pattern);
                for (const fs::path& m : matches) {
                    if (haveRel) {
                        std::error_code ec;
                        fs::path rel = fs::relative(m, relDir, ec);
                        if (!ec) found.push_back(rel.generic_string());
                        else found.push_back(m.generic_string());
                    } else {
                        found.push_back(m.lexically_normal().string());
                    }
                }
            }
            setVar(var, joinStrings(found, ";"));
            return;
        }
        if (sub == "make_directory") {
            for (size_t k = 1; k < a.size(); ++k) {
                fs::create_directories(resolveSourcePath(a[k]));
            }
            return;
        }
        if (sub == "write" || sub == "append") {
            if (a.size() < 2) throw FatalError(where(cmd) + "file(WRITE) requires a file name");
            fs::path path = resolveSourcePath(a[1]);
            std::string content;
            for (size_t k = 2; k < a.size(); ++k) content += a[k];
            if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
            std::ofstream of(path, std::ios::binary | (sub == "append" ? std::ios::app : std::ios::trunc));
            if (!of) throw FatalError(where(cmd) + "cannot write '" + path.string() + "'");
            of << content;
            return;
        }
        if (sub == "remove") {
            for (size_t k = 1; k < a.size(); ++k) {
                std::error_code ec;
                fs::remove(resolveSourcePath(a[k]), ec);
            }
            return;
        }
        printStatus("vmake: 'file(" + a[0] + ")' is not supported yet, ignored");
    }

    void execInstall(const Command& cmd) {
        std::vector<std::string> a = expandArgs(cmd.args);
        if (a.empty()) throw FatalError(where(cmd) + "install() requires a sub-command");
        std::string sub = toLower(a[0]);

        if (sub == "targets") {
            std::vector<std::string> names;
            std::string destAll;
            std::map<std::string, std::string> destBySection;
            std::string curSection;
            for (size_t k = 1; k < a.size(); ++k) {
                std::string la = toLower(a[k]);
                if (la == "destination") {
                    std::string d = (k + 1 < a.size()) ? a[++k] : std::string();
                    if (curSection.empty()) destAll = d;
                    else destBySection[curSection] = d;
                    continue;
                }
                if (la == "runtime" || la == "library" || la == "archive") {
                    curSection = la;
                    continue;
                }
                if (la == "configurations" || la == "component" || la == "namelink_component" ||
                    la == "permissions") {
                    ++k;
                    continue;
                }
                if (la == "optional" || la == "exclude_from_all" || la == "namelink_only" ||
                    la == "namelink_skip" || la == "object" || la == "private_header" ||
                    la == "public_header" || la == "includes") {
                    continue;
                }
                names.push_back(a[k]);
            }
            for (const std::string& nm : names) {
                auto it = targets.find(nm);
                if (it == targets.end()) {
                    throw FatalError(where(cmd) + "install(TARGETS): unknown target '" + nm + "'");
                }
                Target& t = it->second;
                std::string section = (t.type == Target::Type::Executable)
                                          ? "runtime"
                                          : (t.type == Target::Type::StaticLibrary ? "archive"
                                                                                   : "library");
                std::string dest;
                auto secIt = destBySection.find(section);
                if (secIt != destBySection.end()) dest = secIt->second;
                else dest = destAll;
                if (dest.empty()) {
                    throw FatalError(where(cmd) + "install(TARGETS '" + nm + "') requires DESTINATION");
                }
                if (section == "runtime") t.installRuntime.push_back(dest);
                else if (section == "archive") t.installArchive.push_back(dest);
                else t.installLibrary.push_back(dest);
            }
            return;
        }
        if (sub == "files" || sub == "directory") {
            std::vector<fs::path> items;
            std::string dest;
            for (size_t k = 1; k < a.size(); ++k) {
                std::string la = toLower(a[k]);
                if (la == "destination") {
                    dest = (k + 1 < a.size()) ? a[++k] : std::string();
                    continue;
                }
                if (la == "type" || la == "permissions" || la == "component" ||
                    la == "configurations" || la == "file_permissions" ||
                    la == "directory_permissions") {
                    ++k;
                    continue;
                }
                if (la == "optional" || la == "exclude_from_all" || la == "use_source_permissions") {
                    continue;
                }
                items.push_back(resolveSourcePath(a[k]));
            }
            if (dest.empty()) throw FatalError(where(cmd) + "install(" + a[0] + ") requires DESTINATION");
            for (const fs::path& item : items) {
                if (sub == "files") {
                    installFiles.push_back(InstallFile{item, dest});
                } else {
                    std::error_code ec;
                    if (!fs::is_directory(item, ec)) continue;
                    for (fs::recursive_directory_iterator it(item, ec), end; it != end && !ec;
                         it.increment(ec)) {
                        if (!fs::is_regular_file(it->path(), ec)) continue;
                        fs::path rel = fs::relative(it->path(), item, ec);
                        if (ec) continue;
                        fs::path subDest = fs::path(dest) / rel.parent_path();
                        installFiles.push_back(InstallFile{it->path(), subDest.generic_string()});
                    }
                }
            }
            return;
        }
        printStatus("vmake: 'install(" + a[0] + ")' is not supported yet, ignored");
    }

    // ---- generator model helpers ----

    static std::string executableSuffix() {
#ifdef _WIN32
        return ".exe";
#else
        return std::string();
#endif
    }

    fs::path outputPath(const Target& t) const {
        std::string name = t.outputName.empty() ? t.name : t.outputName;
        switch (t.type) {
            case Target::Type::Executable:
                return fs::path("bin") / (name + executableSuffix());
            case Target::Type::StaticLibrary:
                return fs::path("lib") / ("lib" + name + ".a");
            case Target::Type::SharedLibrary:
            case Target::Type::ModuleLibrary:
#ifdef _WIN32
                return fs::path("lib") / ("lib" + name + ".dll");
#elif defined(__APPLE__)
                return fs::path("lib") / ("lib" + name + ".dylib");
#else
                return fs::path("lib") / ("lib" + name + ".so");
#endif
        }
        return fs::path(name);
    }

    std::string objectPath(const Target& t, const fs::path& src) const {
        std::error_code ec;
        fs::path rel = fs::relative(src, sourceDir, ec);
        std::string relStr;
        if (ec || rel.empty()) relStr = src.filename().string();
        else relStr = rel.generic_string();
        if (relStr.rfind("..", 0) == 0) relStr = src.filename().string();
        return "vmake_files/" + t.name + "/" + relStr + ".o";
    }

    void collectUsage(const Target& t, std::vector<std::string>& incs,
                      std::vector<std::string>& defs, std::vector<std::string>& copts,
                      std::vector<std::string>& lopts) const {
        std::set<std::string> seen;
        std::vector<std::string> queue = t.linkPrivate;
        queue.insert(queue.end(), t.linkPublic.begin(), t.linkPublic.end());
        while (!queue.empty()) {
            std::string nm = queue.back();
            queue.pop_back();
            auto it = targets.find(nm);
            if (it == targets.end()) continue;
            if (!seen.insert(nm).second) continue;
            const Target& dep = it->second;
            for (const fs::path& p : dep.incPublic) incs.push_back(p.string());
            for (const fs::path& p : dep.incInterface) incs.push_back(p.string());
            defs.insert(defs.end(), dep.defsPublic.begin(), dep.defsPublic.end());
            defs.insert(defs.end(), dep.defsInterface.begin(), dep.defsInterface.end());
            copts.insert(copts.end(), dep.coptPublic.begin(), dep.coptPublic.end());
            copts.insert(copts.end(), dep.coptInterface.begin(), dep.coptInterface.end());
            lopts.insert(lopts.end(), dep.loptPublic.begin(), dep.loptPublic.end());
            lopts.insert(lopts.end(), dep.loptInterface.begin(), dep.loptInterface.end());
            for (const std::string& x : dep.linkPublic) queue.push_back(x);
            for (const std::string& x : dep.linkInterface) queue.push_back(x);
        }
    }

    void collectLinkLine(const Target& t, std::vector<std::string>& inputs,
                         std::vector<std::string>& libs) const {
        std::set<std::string> seen;
        std::function<void(const std::string&)> add = [&](const std::string& entry) {
            auto it = targets.find(entry);
            if (it != targets.end()) {
                if (!seen.insert(entry).second) return;
                inputs.push_back(outputPath(it->second).generic_string());
                for (const std::string& x : it->second.linkPublic) add(x);
                for (const std::string& x : it->second.linkInterface) add(x);
                return;
            }
            if (!entry.empty() && entry[0] == '-') {
                libs.push_back(entry);
                return;
            }
            std::string lower = toLower(entry);
            bool looksLikePath = entry.find('/') != std::string::npos ||
                                 entry.find('\\') != std::string::npos ||
                                 (lower.size() > 2 &&
                                  (lower.substr(lower.size() - 2) == ".a" ||
                                   lower.substr(lower.size() - 2) == ".o"));
            if (looksLikePath) {
                fs::path p(entry);
                if (p.is_relative()) p = scope->srcDir / p;
                inputs.push_back(p.lexically_normal().string());
            } else {
                libs.push_back("-l" + entry);
            }
        };
        for (const std::string& x : t.linkPrivate) add(x);
        for (const std::string& x : t.linkPublic) add(x);
    }

    fs::path installDestination(const std::string& dest) const {
        fs::path d(dest);
        if (d.is_absolute()) return d;
        return fs::path(getVar("VMAKE_INSTALL_PREFIX")) / d;
    }

    // ---- ninja file generation ----

    static std::string escapeNinjaPath(const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '$') r += "$$";
            else if (c == ' ') r += "$ ";
            else if (c == ':') r += "$:";
            else if (c == '\n') r += "$\n";
            else r += c;
        }
        return r;
    }

    static std::string escapeNinjaValue(const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '$') r += "$$";
            else if (c == '\n') r += "$\n";
            else r += c;
        }
        return r;
    }

    static std::string makeIncludeFlags(const std::vector<std::string>& dirs) {
        std::set<std::string> seen;
        std::string r;
        for (const std::string& d : dirs) {
            if (!seen.insert(d).second) continue;
            if (!r.empty()) r += ' ';
            if (d.find(' ') != std::string::npos) r += "-I\"" + d + "\"";
            else r += "-I" + d;
        }
        return r;
    }

    static std::string makeDefineFlags(const std::vector<std::string>& defs) {
        std::set<std::string> seen;
        std::string r;
        for (const std::string& d : defs) {
            if (!seen.insert(d).second) continue;
            if (!r.empty()) r += ' ';
            if (d.find(' ') != std::string::npos) r += "-D\"" + d + "\"";
            else r += "-D" + d;
        }
        return r;
    }

    // Gather the .vt sources of a target plus every transitively imported
    // module. Both vtcc and the interpreter then see the same flattened tree
    // (<build>/vt), so `import <name>` resolves identically in both.
    void collectVortexSources(const Target& t, std::vector<fs::path>& out,
                              fs::path& entry) const {
        std::vector<fs::path> search;
        for (const fs::path& s : t.sources) search.push_back(s.parent_path());
        for (const fs::path& p : t.incPrivate) search.push_back(p);
        for (const fs::path& p : t.incPublic) search.push_back(p);
        for (const fs::path& p : t.vtModuleDirs) search.push_back(p);
        search.push_back(t.srcDir);
        search.push_back(sourceDir);

        entry = t.sources.empty() ? fs::path() : t.sources.front();
        if (!t.vtMain.empty()) {
            fs::path m = t.vtMain;
            if (m.is_relative()) m = t.srcDir / m;
            entry = m.lexically_normal();
        }

        std::map<std::string, fs::path> chosen;   // file name -> source path
        std::set<std::string> visited;
        std::vector<fs::path> queue;
        auto add = [&](const fs::path& p) {
            std::string name = p.filename().string();
            auto it = chosen.find(name);
            if (it == chosen.end()) { chosen[name] = p; return; }
            if (!fs::equivalent(it->second, p)) {
                throw FatalError("vortex module name collision: '" + name + "' ('" +
                                 it->second.string() + "' vs '" + p.string() + "')");
            }
        };
        if (entry.empty()) throw FatalError("target '" + t.name + "' has no sources");
        add(entry);
        queue.push_back(entry);
        for (const fs::path& s : t.sources) {
            if (s.lexically_normal() == entry.lexically_normal()) continue;
            add(s);
            queue.push_back(s);
        }

        while (!queue.empty()) {
            fs::path src = queue.back();
            queue.pop_back();
            std::string key = src.lexically_normal().string();
            if (!visited.insert(key).second) continue;
            std::ifstream in(src, std::ios::binary);
            if (!in) throw FatalError("cannot read vortex source '" + src.string() + "'");
            std::string line;
            while (std::getline(in, line)) {
                std::string name = parseImportName(line);
                if (name.empty() || builtinVortexModules().count(name)) continue;
                fs::path found = findVortexModule(name, search);
                if (found.empty()) continue;   // built-in or provided at run time
                add(found);
                queue.push_back(found);
            }
        }

        out.clear();
        for (const auto& kv : chosen) out.push_back(kv.second);
    }

    // Resolve a tool variable: an explicit cache value wins, otherwise use the
    // executable sitting next to vmake when present (release layout), else PATH.
    std::string resolveTool(const std::string& varName, const char* exeName) const {
        std::string v = getVar(varName);
        if (!v.empty() && v != exeName) return v;
        fs::path sibling = g_vmakeExe.parent_path() / exeName;
#ifdef _WIN32
        sibling += ".exe";
#endif
        if (!sibling.empty() && fs::exists(sibling)) return sibling.string();
        return v;
    }

    void generateNinjaFile() {
        fs::create_directories(buildDir / "bin");
        fs::create_directories(buildDir / "lib");
        fs::create_directories(buildDir / "vmake_files");

        std::ostringstream o;
        o << "# Generated by vmake " << kVmakeVersion << " \xE2\x80\x94 do not edit.\n";
        o << "# vmake source: " << sourceDir.generic_string() << "\n\n";
        o << "ninja_required_version = 1.5\n\n";

        std::string vmakeExe = g_vmakeExe.empty() ? std::string("vmake") : g_vmakeExe.string();
        o << "CXX = \"" << escapeNinjaValue(getVar("VMAKE_CXX_COMPILER")) << "\"\n";
        o << "CC = \"" << escapeNinjaValue(getVar("VMAKE_C_COMPILER")) << "\"\n";
        o << "AR = ar\n";
        o << "VMAKE = \"" << escapeNinjaValue(vmakeExe) << "\"\n";
        o << "VTCC = \"" << escapeNinjaValue(resolveTool("VMAKE_VTCC", "vtcc")) << "\"\n";
        o << "VORTEX = \"" << escapeNinjaValue(resolveTool("VMAKE_VORTEX", "vortex")) << "\"\n\n";

        o << "rule cxx\n";
        o << "  command = $CXX -std=c++$STD $FLAGS $DEFINES $INCLUDES -MMD -MF $DEP -c $in -o $out\n";
        o << "  depfile = $out.d\n";
        o << "  deps = gcc\n";
        o << "  description = Building CXX object $out\n\n";

        o << "rule cc\n";
        o << "  command = $CC $FLAGS $DEFINES $INCLUDES -MMD -MF $DEP -c $in -o $out\n";
        o << "  depfile = $out.d\n";
        o << "  deps = gcc\n";
        o << "  description = Building C object $out\n\n";

        o << "rule link_exe\n";
        o << "  command = $CXX $FLAGS $in $LIBS $LINKFLAGS -o $out\n";
        o << "  description = Linking CXX executable $out\n\n";

        o << "rule link_static\n";
        o << "  command = $AR rcs $out $in\n";
        o << "  description = Linking C static library $out\n\n";

        o << "rule link_shared\n";
        o << "  command = $CXX -shared $FLAGS $in $LIBS $LINKFLAGS -o $out\n";
        o << "  description = Linking CXX shared library $out\n\n";

        o << "rule install\n";
        o << "  command = $VMAKE -E copy $in $out\n";
        o << "  description = Installing $out\n\n";

        o << "rule vcopy\n";
        o << "  command = $VMAKE -E copy_if_different $in $out\n";
        o << "  description = COPY $out\n\n";

        o << "rule vtcc\n";
        o << "  command = $VTCC build $ENTRY -o $out\n";
        o << "  description = VTCC $out\n\n";

        o << "rule vrun\n";
        o << "  command = $VORTEX $in\n";
        o << "  description = RUN $in\n\n";

        const std::string config = toUpper(getVar("VMAKE_BUILD_TYPE"));

        for (const std::string& tname : targetOrder) {
            const Target& t = targets.at(tname);

            // ---- Vortex (.vt) targets: compile with vtcc, run with vortex ----
            bool hasVt = false, hasOther = false;
            for (const fs::path& s : t.sources) {
                std::string e = toLower(s.extension().string());
                if (e.empty()) continue;
                if (isVtSource(s)) hasVt = true;
                else if (e != ".h" && e != ".hh" && e != ".hpp" && e != ".hxx" &&
                         e != ".h++" && e != ".inc") hasOther = true;
            }
            if (hasVt) {
                if (hasOther)
                    throw FatalError("target '" + tname +
                                     "' mixes .vt sources with C/C++ sources");
                if (t.type != Target::Type::Executable)
                    throw FatalError("target '" + tname +
                                     "': only executables can be built from .vt sources");
                std::vector<fs::path> vtSources;
                fs::path entry;
                collectVortexSources(t, vtSources, entry);
                fs::create_directories(buildDir / "vt");
                std::vector<std::string> rels;
                std::string entryRel;
                for (const fs::path& src : vtSources) {
                    std::string rel = "vt/" + src.filename().string();
                    // Copy now so `vmake --run` works without building (and
                    // without vtcc); the Ninja edge below keeps it in sync.
                    std::error_code cec;
                    fs::copy_file(src, buildDir / "vt" / src.filename().string(),
                                  fs::copy_options::overwrite_existing, cec);
                    if (cec)
                        throw FatalError("cannot copy '" + src.string() + "' into the build tree: " +
                                         cec.message());
                    if (src.lexically_normal() == entry.lexically_normal()) entryRel = rel;
                    rels.push_back(rel);
                    o << "build " << escapeNinjaPath(rel) << ": vcopy "
                      << escapeNinjaPath(src.generic_string()) << "\n\n";
                }
                if (entryRel.empty()) entryRel = "vt/" + entry.filename().string();
                o << "build " << escapeNinjaPath(outputPath(t).generic_string()) << ": vtcc";
                for (const std::string& r : rels) o << " " << escapeNinjaPath(r);
                o << "\n  ENTRY = " << escapeNinjaValue(entryRel) << "\n\n";
                o << "build " << escapeNinjaPath(tname + "-run") << ": vrun "
                  << escapeNinjaPath(entryRel) << "\n\n";
                vtEntries[tname] = entryRel;
                continue;
            }

            std::string stdStr = t.cxxStandard.empty() ? getVar("VMAKE_CXX_STANDARD") : t.cxxStandard;
            if (stdStr.empty()) stdStr = "17";

            std::vector<std::string> usageInc, usageDefs, usageCopts, usageLopts;
            collectUsage(t, usageInc, usageDefs, usageCopts, usageLopts);

            std::vector<std::string> incs;
            for (const fs::path& p : t.incPrivate) incs.push_back(p.string());
            for (const fs::path& p : t.incPublic) incs.push_back(p.string());
            incs.insert(incs.end(), usageInc.begin(), usageInc.end());

            std::vector<std::string> defs = t.defsPrivate;
            defs.insert(defs.end(), t.defsPublic.begin(), t.defsPublic.end());
            defs.insert(defs.end(), usageDefs.begin(), usageDefs.end());

            std::vector<std::string> copts = t.coptPrivate;
            copts.insert(copts.end(), t.coptPublic.begin(), t.coptPublic.end());
            copts.insert(copts.end(), usageCopts.begin(), usageCopts.end());

            std::string cxxFlags = getVar("VMAKE_CXX_FLAGS");
            std::string cxxCfg = getVar("VMAKE_CXX_FLAGS_" + config);
            if (!cxxCfg.empty()) cxxFlags += " " + cxxCfg;
            for (const std::string& opt : copts) cxxFlags += " " + opt;

            std::string cFlags = getVar("VMAKE_C_FLAGS");
            std::string cCfg = getVar("VMAKE_C_FLAGS_" + config);
            if (!cCfg.empty()) cFlags += " " + cCfg;
            for (const std::string& opt : copts) cFlags += " " + opt;

#ifndef _WIN32
            if (t.type == Target::Type::SharedLibrary || t.type == Target::Type::ModuleLibrary ||
                t.pic) {
                cxxFlags += " -fPIC";
                cFlags += " -fPIC";
            }
#else
            if (t.pic) {
                cxxFlags += " -fPIC";
                cFlags += " -fPIC";
            }
#endif

            std::string includeLine = makeIncludeFlags(incs);
            std::string defineLine = makeDefineFlags(defs);

            std::vector<std::string> objs;
            std::set<std::string> seenObj;
            for (const fs::path& src : t.sources) {
                std::string ext = toLower(src.extension().string());
                if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh" ||
                    ext == ".h++" || ext == ".inc") {
                    continue;
                }
                std::string obj = objectPath(t, src);
                if (!seenObj.insert(obj).second) continue;
                objs.push_back(obj);
                bool isC = (ext == ".c");
                o << "build " << escapeNinjaPath(obj) << ": " << (isC ? "cc" : "cxx") << " "
                  << escapeNinjaPath(src.string()) << "\n";
                if (!isC) o << "  STD = " << escapeNinjaValue(stdStr) << "\n";
                o << "  FLAGS = " << escapeNinjaValue(isC ? cFlags : cxxFlags) << "\n";
                o << "  DEFINES = " << escapeNinjaValue(defineLine) << "\n";
                o << "  INCLUDES = " << escapeNinjaValue(includeLine) << "\n";
                o << "  DEP = \"" << escapeNinjaValue(obj) << ".d\"\n\n";
            }

            std::vector<std::string> linkInputs, libs;
            collectLinkLine(t, linkInputs, libs);

            std::string outStr = outputPath(t).generic_string();
            o << "build " << escapeNinjaPath(outStr) << ": ";
            if (t.type == Target::Type::Executable) o << "link_exe";
            else if (t.type == Target::Type::StaticLibrary) o << "link_static";
            else o << "link_shared";
            for (const std::string& ob : objs) o << " " << escapeNinjaPath(ob);
            for (const std::string& in : linkInputs) o << " " << escapeNinjaPath(in);
            o << "\n";

            if (t.type != Target::Type::StaticLibrary) {
                std::string linkFlags = getVar("VMAKE_EXE_LINKER_FLAGS");
                std::string linkCfg = getVar("VMAKE_EXE_LINKER_FLAGS_" + config);
                if (!linkCfg.empty()) linkFlags += " " + linkCfg;
                for (const std::string& opt : t.loptPrivate) linkFlags += " " + opt;
                for (const std::string& opt : usageLopts) linkFlags += " " + opt;
                o << "  FLAGS = " << escapeNinjaValue(cxxFlags) << "\n";
                o << "  LINKFLAGS = " << escapeNinjaValue(linkFlags) << "\n";
                o << "  LIBS = " << escapeNinjaValue(joinStrings(libs, " ")) << "\n";
            }
            o << "\n";
        }

        // Install edges.
        std::vector<std::string> installOutputs;
        std::set<std::string> emittedDest;
        auto emitInstall = [&](const fs::path& srcPath, const std::string& destDir) {
            fs::path dstDir = installDestination(destDir);
            fs::path dst = dstDir / srcPath.filename();
            std::string dstStr = dst.string();
            if (!emittedDest.insert(dstStr).second) return;
            o << "build " << escapeNinjaPath(dstStr) << ": install "
              << escapeNinjaPath(srcPath.generic_string()) << "\n\n";
            installOutputs.push_back(dstStr);
        };
        for (const std::string& tname : targetOrder) {
            const Target& t = targets.at(tname);
            fs::path out = outputPath(t);
            for (const std::string& d : t.installRuntime) emitInstall(out, d);
            for (const std::string& d : t.installLibrary) emitInstall(out, d);
            for (const std::string& d : t.installArchive) emitInstall(out, d);
        }
        for (const InstallFile& f : installFiles) emitInstall(f.source, f.dest);

        // Per-target aliases so that "--target <name>" works.
        std::set<std::string> aliasNames;
        for (const std::string& tname : targetOrder) {
            if (tname == "all" || tname == "install") continue;
            if (!aliasNames.insert(tname).second) continue;
            o << "build " << escapeNinjaPath(tname) << ": phony "
              << escapeNinjaPath(outputPath(targets.at(tname)).generic_string()) << "\n";
        }
        if (!aliasNames.empty()) o << "\n";

        o << "build install: phony";
        for (const std::string& s : installOutputs) o << " " << escapeNinjaPath(s);
        o << "\n\n";

        o << "build all: phony";
        for (const std::string& tname : targetOrder) {
            o << " " << escapeNinjaPath(outputPath(targets.at(tname)).generic_string());
        }
        o << "\n\n";
        o << "default all\n";

        // Manifest consumed by `vmake --run`: target -> interpreter entry.
        if (!vtEntries.empty()) {
            fs::path manifest = buildDir / "vmake_run.txt";
            std::ofstream mf(manifest, std::ios::binary | std::ios::trunc);
            if (!mf) throw FatalError("cannot write '" + manifest.string() + "'");
            for (const auto& kv : vtEntries)
                mf << kv.first << "\t" << kv.second << "\n";
            if (!mf) throw FatalError("failed while writing '" + manifest.string() + "'");
        }

        fs::path ninjaPath = buildDir / kNinjaFileName;
        std::ofstream out(ninjaPath, std::ios::binary | std::ios::trunc);
        if (!out) throw FatalError("cannot write '" + ninjaPath.string() + "'");
        out << o.str();
        if (!out) throw FatalError("failed while writing '" + ninjaPath.string() + "'");
    }

    void printTargets() const {
        printStatus("Targets:");
        for (const std::string& tname : targetOrder) {
            const Target& t = targets.at(tname);
            const char* typeName = "executable";
            if (t.type == Target::Type::StaticLibrary) typeName = "static library";
            else if (t.type == Target::Type::SharedLibrary) typeName = "shared library";
            else if (t.type == Target::Type::ModuleLibrary) typeName = "module library";
            std::cout << "  " << t.name << " (" << typeName << ")\n";
            for (const fs::path& s : t.sources) std::cout << "    " << s.string() << "\n";
        }
    }
};

// ---------------------------------------------------------------------------
// Condition parser implementation
// ---------------------------------------------------------------------------

std::string CondParser::operand(const std::string& token) const {
    std::string expanded = it.expand(token);
    std::string value;
    if (it.lookup(expanded, value)) return value;
    return expanded;
}

bool CondParser::parseOr() {
    bool value = parseAnd();
    while (peekKeyword("or")) {
        ++pos;
        bool rhs = parseAnd();
        value = value || rhs;
    }
    return value;
}

bool CondParser::parseAnd() {
    bool value = parseUnary();
    while (peekKeyword("and")) {
        ++pos;
        bool rhs = parseUnary();
        value = value && rhs;
    }
    return value;
}

bool CondParser::parseUnary() {
    if (atEnd()) return false;

    if (peekKeyword("not")) {
        ++pos;
        return !parseUnary();
    }
    if (tokens[pos] == "(") {
        ++pos;
        bool value = parseOr();
        if (!atEnd() && tokens[pos] == ")") ++pos;
        return value;
    }
    if (peekKeyword("defined")) {
        ++pos;
        std::string name = it.expand(nextToken());
        std::string dummy;
        return it.lookup(name, dummy);
    }
    if (peekKeyword("exists")) {
        ++pos;
        std::string p = it.expand(nextToken());
        std::error_code ec;
        return fs::exists(it.resolveSourcePath(p), ec);
    }
    if (peekKeyword("command")) {
        ++pos;
        std::string name = toLower(it.expand(nextToken()));
        static const std::set<std::string> builtins = {
            "vmake_minimum_required", "project", "set", "unset", "option", "list", "message",
            "add_subdirectory", "include", "add_executable", "add_library", "target_sources",
            "target_include_directories", "target_link_libraries", "target_compile_definitions",
            "target_compile_options", "target_link_options", "set_target_properties", "file",
            "get_filename_component", "configure_file", "install", "if", "foreach", "break",
            "continue", "find_package"};
        return builtins.count(name) != 0 || it.targets.count(name) != 0;
    }
    if (peekKeyword("target")) {
        ++pos;
        std::string name = it.expand(nextToken());
        return it.targets.count(name) != 0;
    }

    std::string lhs = operand(nextToken());
    if (!atEnd()) {
        std::string op = toLower(tokens[pos]);
        static const std::set<std::string> operators = {
            "strequal", "equal", "version_less", "version_greater", "version_equal",
            "version_less_equal", "version_greater_equal", "matches", "less", "greater",
            "less_equal", "greater_equal"};
        if (operators.count(op)) {
            ++pos;
            std::string rhs = operand(nextToken());
            if (op == "strequal") return lhs == rhs;
            if (op == "matches") {
                try {
                    std::regex re(rhs);
                    return std::regex_search(lhs, re);
                } catch (const std::regex_error&) {
                    throw FatalError("invalid regular expression in condition: '" + rhs + "'");
                }
            }
            if (op == "version_less") return versionCompare(lhs, rhs) < 0;
            if (op == "version_greater") return versionCompare(lhs, rhs) > 0;
            if (op == "version_equal") return versionCompare(lhs, rhs) == 0;
            if (op == "version_less_equal") return versionCompare(lhs, rhs) <= 0;
            if (op == "version_greater_equal") return versionCompare(lhs, rhs) >= 0;
            double a = 0, b = 0;
            bool na = toNumber(lhs, a);
            bool nb = toNumber(rhs, b);
            if (op == "equal") {
                if (na && nb) return a == b;
                return lhs == rhs;
            }
            if (!na || !nb) {
                if (op == "less") return lhs < rhs;
                if (op == "greater") return lhs > rhs;
                if (op == "less_equal") return lhs <= rhs;
                return lhs >= rhs;
            }
            if (op == "less") return a < b;
            if (op == "greater") return a > b;
            if (op == "less_equal") return a <= b;
            return a >= b;
        }
    }
    return isTruthy(lhs);
}

// ---------------------------------------------------------------------------
// Command line options
// ---------------------------------------------------------------------------

struct Options {
    bool showVersion = false;
    bool showHelp = false;
    bool fresh = false;
    bool viewOnly = false;
    bool trace = false;
    bool listVars = false;
    bool listAdvanced = false;
    bool listHelp = false;
    bool haveListRegex = false;
    std::string listRegex;
    bool haveSource = false;
    bool haveBuild = false;
    std::string sourceDir;
    std::string buildDir;
    std::string generator;
    std::vector<std::string> defines;
    std::vector<std::string> unsets;
    std::vector<std::string> preload;
    bool haveInstallPrefix = false;
    std::string installPrefix;
    std::string projectFile = kDefaultProjectFile;
    std::string toolchain;
    std::string scriptFile;
    std::string buildModeDir;
    std::string installModeDir;
    std::string runModeDir;
    std::string target;
    std::string jobs;
    std::string logLevel;
    std::string positional;
    bool eMode = false;
    std::vector<std::string> eArgs;
};

static void printUsage(std::ostream& os) {
    os << "Usage: vmake [options] <path-to-source>\n"
          "       vmake [options] <path-to-existing-build>\n"
          "       vmake [options] -S <path-to-source> -B <path-to-build>\n"
          "       vmake --build <dir> [--target <t>] [-j <n>]\n"
          "       vmake --run <dir> [--target <t>]\n"
          "       vmake --install <dir>\n"
          "       vmake -E <command> [args...]\n"
          "       vmake --version | --help\n"
          "\n"
          "Options:\n"
          "  -S <dir>                 Source directory (default: current directory)\n"
          "  -B <dir>                 Build directory (default: current directory)\n"
          "  -D <var>[:<type>]=<val>  Set a cache variable\n"
          "  -U <glob>                Remove matching cache variables\n"
          "  -G <generator>           Generator to use (only 'Ninja' is supported)\n"
          "  -C <file>                Pre-load a script before the project file\n"
          "  --fresh                  Remove the cache and generator files first\n"
          "  -L[A][H]                 List cache variables (A: advanced, H: help)\n"
          "  -LR[A][H] <regex>        List cache variables matching a regex\n"
          "  --install-prefix <dir>   Set the install prefix\n"
          "  --project-file <name>    Project file name (default: VmakeLists.txt)\n"
          "  --toolchain <file>       Pre-load a toolchain script\n"
          "  -N                       View mode: resolve targets but write nothing\n"
          "  -P <file>                Run a script\n"
          "  --build <dir>            Build an existing build directory\n"
          "  --run <dir>              Run a .vt target with the interpreter\n"
          "  --install <dir>          Install from an existing build directory\n"
          "  --target <name>          Target to build (with --build)\n"
          "  -j <n>                   Parallel build jobs (with --build)\n"
          "  --log-level=<level>      off|error|warning|status|verbose|debug|trace\n"
          "  --trace                  Print every script command while configuring\n"
          "  --version                Print the version and exit\n"
          "  -h, --help               Print this help and exit\n"
          "\n"
          "Examples:\n"
          "  vmake -S . -B build -G Ninja\n"
          "  vmake --build build -j 8\n"
          "  vmake --install build\n"
          "  vmake -E copy a.txt b.txt\n";
}

static Options parseOptions(int argc, char** argv) {
    Options o;
    int positionalCount = 0;

    auto needValue = [&](int& i, const std::string& opt) -> std::string {
        if (i + 1 >= argc) throw UsageError("option '" + opt + "' requires an argument");
        return std::string(argv[++i]);
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "--version") {
            o.showVersion = true;
        } else if (a == "-h" || a == "--help") {
            o.showHelp = true;
        } else if (a == "-E") {
            o.eMode = true;
            for (int k = i + 1; k < argc; ++k) o.eArgs.push_back(argv[k]);
            break;
        } else if (a == "-S" || a == "--source") {
            o.sourceDir = needValue(i, a);
            o.haveSource = true;
        } else if (a.size() > 2 && a.rfind("-S", 0) == 0) {
            o.sourceDir = a.substr(2);
            o.haveSource = true;
        } else if (a == "-B") {
            o.buildDir = needValue(i, a);
            o.haveBuild = true;
        } else if (a.size() > 2 && a.rfind("-B", 0) == 0) {
            o.buildDir = a.substr(2);
            o.haveBuild = true;
        } else if (a == "-D") {
            o.defines.push_back(needValue(i, a));
        } else if (a.size() > 2 && a.rfind("-D", 0) == 0) {
            o.defines.push_back(a.substr(2));
        } else if (a == "-U") {
            o.unsets.push_back(needValue(i, a));
        } else if (a.size() > 2 && a.rfind("-U", 0) == 0) {
            o.unsets.push_back(a.substr(2));
        } else if (a == "-G") {
            o.generator = needValue(i, a);
        } else if (a.size() > 2 && a.rfind("-G", 0) == 0) {
            o.generator = a.substr(2);
        } else if (a == "-C") {
            o.preload.push_back(needValue(i, a));
        } else if (a.size() > 2 && a.rfind("-C", 0) == 0) {
            o.preload.push_back(a.substr(2));
        } else if (a.rfind("-LR", 0) == 0) {
            std::string rest = a.substr(3);
            for (char c : rest) {
                if (c != 'A' && c != 'H') throw UsageError("unknown option '" + a + "'");
            }
            o.listVars = true;
            o.listAdvanced = rest.find('A') != std::string::npos;
            o.listHelp = rest.find('H') != std::string::npos;
            o.listRegex = needValue(i, a);
            o.haveListRegex = true;
        } else if (a.rfind("-L", 0) == 0) {
            std::string rest = a.substr(2);
            bool ok = true;
            for (char c : rest) {
                if (c != 'A' && c != 'H') ok = false;
            }
            if (!ok) throw UsageError("unknown option '" + a + "'");
            o.listVars = true;
            o.listAdvanced = rest.find('A') != std::string::npos;
            o.listHelp = rest.find('H') != std::string::npos;
        } else if (a == "--fresh") {
            o.fresh = true;
        } else if (a == "--install-prefix") {
            o.installPrefix = needValue(i, a);
            o.haveInstallPrefix = true;
        } else if (a.rfind("--install-prefix=", 0) == 0) {
            o.installPrefix = a.substr(std::string("--install-prefix=").size());
            o.haveInstallPrefix = true;
        } else if (a == "--project-file") {
            o.projectFile = needValue(i, a);
        } else if (a == "--toolchain") {
            o.toolchain = needValue(i, a);
        } else if (a == "-N") {
            o.viewOnly = true;
        } else if (a == "-P") {
            o.scriptFile = needValue(i, a);
        } else if (a.size() > 2 && a.rfind("-P", 0) == 0) {
            o.scriptFile = a.substr(2);
        } else if (a == "--build") {
            o.buildModeDir = needValue(i, a);
        } else if (a.rfind("--build=", 0) == 0) {
            o.buildModeDir = a.substr(std::string("--build=").size());
        } else if (a == "--install") {
            o.installModeDir = needValue(i, a);
        } else if (a.rfind("--install=", 0) == 0) {
            o.installModeDir = a.substr(std::string("--install=").size());
        } else if (a == "--run") {
            o.runModeDir = needValue(i, a);
        } else if (a.rfind("--run=", 0) == 0) {
            o.runModeDir = a.substr(std::string("--run=").size());
        } else if (a == "--target") {
            o.target = needValue(i, a);
        } else if (a == "-j") {
            o.jobs = needValue(i, a);
        } else if (a.size() > 2 && a.rfind("-j", 0) == 0) {
            o.jobs = a.substr(2);
        } else if (a.rfind("--log-level=", 0) == 0) {
            o.logLevel = a.substr(std::string("--log-level=").size());
        } else if (a == "--trace") {
            o.trace = true;
        } else if (!a.empty() && a[0] == '-') {
            throw UsageError("unknown option '" + a + "'");
        } else {
            ++positionalCount;
            if (positionalCount > 1) throw UsageError("too many positional arguments");
            o.positional = a;
        }
    }
    return o;
}

static LogLevel parseLogLevel(const std::string& text) {
    std::string s = toLower(trim(text));
    if (s == "off") return LogLevel::Off;
    if (s == "error") return LogLevel::ErrorLvl;
    if (s == "warning") return LogLevel::WarningLvl;
    if (s == "status") return LogLevel::StatusLvl;
    if (s == "verbose") return LogLevel::VerboseLvl;
    if (s == "debug") return LogLevel::DebugLvl;
    if (s == "trace") return LogLevel::TraceLvl;
    throw UsageError("unknown log level '" + text + "'");
}

// ---------------------------------------------------------------------------
// -E commands
// ---------------------------------------------------------------------------

static bool filesEqual(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    auto sizeA = fs::file_size(a, ec);
    if (ec) return false;
    auto sizeB = fs::file_size(b, ec);
    if (ec || sizeA != sizeB) return false;
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::string ca((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
    std::string cb((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
    return ca == cb;
}

static void printEchoCommands() {
    std::cout << "Available -E commands:\n"
                 "  echo <text...>\n"
                 "  copy <file> <file>\n"
                 "  copy_if_different <file> <file>\n"
                 "  make_directory <dir...>\n"
                 "  remove <file...>\n"
                 "  remove_directory <dir...>\n"
                 "  cat <file...>\n"
                 "  touch <file...>\n"
                 "  compare_files <file1> <file2>\n";
}

static int handleE(const std::vector<std::string>& args) {
    if (args.empty()) {
        printEchoCommands();
        return 0;
    }
    std::string cmd = toLower(args[0]);

    if (cmd == "echo" || cmd == "echo_append") {
        std::string text = joinStrings(std::vector<std::string>(args.begin() + 1, args.end()), " ");
        if (cmd == "echo") std::cout << text << std::endl;
        else std::cout << text << std::flush;
        return 0;
    }
    if (cmd == "copy" || cmd == "copy_if_different") {
        if (args.size() < 3) {
            printError("-E " + args[0] + " requires a source and a destination");
            return 2;
        }
        fs::path src(args[1]);
        fs::path dst(args[2]);
        std::error_code ec;
        if (fs::is_directory(dst, ec)) dst /= src.filename();
        if (!dst.parent_path().empty()) fs::create_directories(dst.parent_path(), ec);
        if (cmd == "copy_if_different" && fs::exists(dst, ec) && filesEqual(src, dst)) return 0;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            printError("failed to copy '" + src.string() + "' to '" + dst.string() + "': " +
                       ec.message());
            return 1;
        }
        return 0;
    }
    if (cmd == "make_directory") {
        for (size_t k = 1; k < args.size(); ++k) {
            std::error_code ec;
            fs::create_directories(args[k], ec);
            if (ec) {
                printError("failed to create directory '" + args[k] + "': " + ec.message());
                return 1;
            }
        }
        return 0;
    }
    if (cmd == "remove") {
        for (size_t k = 1; k < args.size(); ++k) {
            std::error_code ec;
            fs::remove(args[k], ec);
            if (ec) printWarning("failed to remove '" + args[k] + "': " + ec.message());
        }
        return 0;
    }
    if (cmd == "remove_directory") {
        for (size_t k = 1; k < args.size(); ++k) {
            std::error_code ec;
            fs::remove_all(args[k], ec);
            if (ec) {
                printError("failed to remove directory '" + args[k] + "': " + ec.message());
                return 1;
            }
        }
        return 0;
    }
    if (cmd == "cat") {
        for (size_t k = 1; k < args.size(); ++k) {
            std::ifstream in(args[k], std::ios::binary);
            if (!in) {
                printError("cannot read file '" + args[k] + "'");
                return 1;
            }
            std::cout << in.rdbuf();
        }
        return 0;
    }
    if (cmd == "touch") {
        for (size_t k = 1; k < args.size(); ++k) {
            std::error_code ec;
            fs::path p(args[k]);
            if (!fs::exists(p, ec)) {
                if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);
                std::ofstream of(p, std::ios::binary | std::ios::app);
                if (!of) {
                    printError("cannot create file '" + p.string() + "'");
                    return 1;
                }
            } else {
                fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
            }
        }
        return 0;
    }
    if (cmd == "compare_files") {
        if (args.size() < 3) {
            printError("-E compare_files requires two files");
            return 2;
        }
        std::error_code ec;
        if (!fs::exists(args[1], ec)) {
            printError("file does not exist: '" + args[1] + "'");
            return 1;
        }
        if (!fs::exists(args[2], ec)) {
            printError("file does not exist: '" + args[2] + "'");
            return 1;
        }
        return filesEqual(args[1], args[2]) ? 0 : 1;
    }
    printError("unknown -E command '" + args[0] + "'");
    printEchoCommands();
    return 2;
}

// ---------------------------------------------------------------------------
// Configure / build / install / script modes
// ---------------------------------------------------------------------------

static int doConfigure(const Options& opts) {
    Interp it;
    bool explicitSource = false;
    bool explicitBuild = false;
    fs::path src;
    fs::path bld;

    if (opts.haveSource) {
        src = fs::absolute(fs::path(opts.sourceDir));
        explicitSource = true;
    }
    if (opts.haveBuild) {
        bld = fs::absolute(fs::path(opts.buildDir));
        explicitBuild = true;
    }
    if (!opts.positional.empty() && !explicitSource) {
        fs::path p = fs::absolute(fs::path(opts.positional));
        if (fs::exists(p / kCacheFileName)) {
            bld = p;
            explicitBuild = true;
        } else {
            src = p;
            explicitSource = true;
        }
    }
    if (!explicitSource) src = fs::current_path();
    if (!explicitBuild) bld = fs::current_path();

    if (!opts.generator.empty() && toLower(opts.generator) != "ninja") {
        throw FatalError("generator '" + opts.generator +
                         "' is not supported; the only supported generator is 'Ninja'");
    }

    it.sourceDir = src.lexically_normal();
    it.buildDir = bld.lexically_normal();
    it.projectFile = opts.projectFile;

    if (opts.fresh) {
        std::error_code ec;
        fs::remove(it.buildDir / kCacheFileName, ec);
        fs::remove(it.buildDir / kNinjaFileName, ec);
    }

    it.loadCache();
    if (!explicitSource) {
        auto home = it.cache.find("VMAKE_HOME_DIRECTORY");
        if (home != it.cache.end() && !home->second.value.empty()) {
            it.sourceDir = fs::path(home->second.value).lexically_normal();
        }
    }

    for (const std::string& u : opts.unsets) it.unsetCache(u);
    for (const std::string& d : opts.defines) it.applyDefine(d);
    it.initDefaults();
    if (opts.haveInstallPrefix) {
        it.cache["VMAKE_INSTALL_PREFIX"] = CacheEntry{"PATH", fs::absolute(fs::path(opts.installPrefix)).string(), ""};
    }

    fs::create_directories(it.buildDir);

    printStatus("vmake version " + std::string(kVmakeVersion));
    printStatus("Configuring: " + it.sourceDir.string());
    printStatus("Build directory: " + it.buildDir.string());

    it.initScope(it.sourceDir, it.buildDir);

    for (const std::string& file : opts.preload) {
        fs::path p = fs::absolute(fs::path(file));
        if (!fs::exists(p)) throw FatalError("pre-load script not found: '" + p.string() + "'");
        it.runScriptFile(p);
    }
    if (!opts.toolchain.empty()) {
        fs::path p = fs::absolute(fs::path(opts.toolchain));
        if (!fs::exists(p)) throw FatalError("toolchain file not found: '" + p.string() + "'");
        it.runScriptFile(p);
    }

    fs::path projectPath = it.sourceDir / it.projectFile;
    if (!fs::exists(projectPath)) {
        throw FatalError("project file not found: '" + projectPath.string() +
                         "' (use --project-file to override the name)");
    }
    it.runScriptFile(projectPath);

    if (it.hadError) {
        printError("configuration failed");
        return 1;
    }

    if (opts.viewOnly) {
        it.printTargets();
        printStatus("Configuring done (view mode, no files written)");
        return 0;
    }

    it.generateNinjaFile();
    it.writeCache();

    printStatus("Configuring done");
    printStatus("Generating done");
    printStatus("Build files have been written to: " + it.buildDir.string());

    if (opts.listVars) {
        it.printCacheVars(opts.listAdvanced, opts.listHelp,
                          opts.haveListRegex ? opts.listRegex : std::string());
    }
    return 0;
}

static int doScriptMode(const Options& opts) {
    Interp it;
    fs::path script = fs::absolute(fs::path(opts.scriptFile));
    if (!fs::exists(script)) throw FatalError("script file not found: '" + script.string() + "'");
    it.scriptMode = true;
    it.sourceDir = script.parent_path();
    it.buildDir = fs::current_path();
    it.projectFile = opts.projectFile;
    it.initScope(script.parent_path(), fs::current_path());
    for (const std::string& d : opts.defines) it.applyDefine(d);
    it.runScriptFile(script);
    return it.hadError ? 1 : 0;
}

static int doBuild(const Options& opts) {
    fs::path dir = fs::absolute(fs::path(opts.buildModeDir));
    if (!fs::exists(dir / kNinjaFileName)) {
        printError("build.ninja not found in '" + dir.string() +
                   "'; run vmake to configure the project first");
        return 1;
    }
    std::vector<std::string> args = {"ninja", "-C", dir.string()};
    if (!opts.jobs.empty()) {
        args.push_back("-j");
        args.push_back(opts.jobs);
    }
    if (!opts.target.empty()) args.push_back(opts.target);
    return runProcess(args);
}

static int doInstall(const Options& opts) {
    fs::path dir = fs::absolute(fs::path(opts.installModeDir));

    if (opts.haveInstallPrefix) {
        if (!fs::exists(dir / kCacheFileName)) {
            printError("no " + std::string(kCacheFileName) + " in '" + dir.string() +
                       "'; configure the project first");
            return 1;
        }
        Options reconfigure;
        reconfigure.haveBuild = true;
        reconfigure.buildDir = dir.string();
        reconfigure.installPrefix = opts.installPrefix;
        reconfigure.haveInstallPrefix = true;
        reconfigure.trace = opts.trace;
        reconfigure.logLevel = opts.logLevel;
        int rc = doConfigure(reconfigure);
        if (rc != 0) return rc;
    }

    if (!fs::exists(dir / kNinjaFileName)) {
        printError("build.ninja not found in '" + dir.string() +
                   "'; run vmake to configure the project first");
        return 1;
    }
    return runProcess({"ninja", "-C", dir.string(), "install"});
}

static int doRun(const Options& opts) {
    fs::path dir = fs::absolute(fs::path(opts.runModeDir));
    fs::path manifest = dir / "vmake_run.txt";
    std::ifstream mf(manifest, std::ios::binary);
    if (!mf) {
        printError("no vmake_run.txt in '" + dir.string() +
                   "'; configure a project with .vt targets first");
        return 1;
    }

    const std::string want = opts.target;
    std::string chosenTarget, entry, line;
    while (std::getline(mf, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string tname = line.substr(0, tab);
        if (!want.empty() && tname != want) continue;
        chosenTarget = tname;
        entry = trim(line.substr(tab + 1));
        break;
    }
    if (entry.empty()) {
        printError(want.empty() ? std::string("no .vt target in this project")
                                : "target '" + want + "' is not a .vt target");
        return 1;
    }

    // Interpreter binary: cache value, else the sibling of vmake, else PATH.
    std::string vortexBin = "vortex";
    std::ifstream cf(dir / kCacheFileName, std::ios::binary);
    while (std::getline(cf, line)) {
        if (line.rfind("VMAKE_VORTEX:", 0) != 0) continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string v = trim(line.substr(eq + 1));
            if (!v.empty() && v != "vortex") vortexBin = v;
        }
        break;
    }
    if (vortexBin == "vortex") {
        fs::path sibling = g_vmakeExe.parent_path() / "vortex";
#ifdef _WIN32
        sibling += ".exe";
#endif
        if (fs::exists(sibling)) vortexBin = sibling.string();
    }

    fs::path entryPath = dir / entry;
    if (!fs::exists(entryPath)) {
        printError("entry script not found: '" + entryPath.string() +
                   "'; run vmake --build first");
        return 1;
    }
    printStatus("Running '" + chosenTarget + "' with the interpreter: " + entryPath.string());
    return runProcess({vortexBin, entryPath.string()});
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    try {
        {
            std::error_code ec;
            g_vmakeExe = fs::absolute(fs::path(argv[0]), ec);
            if (ec) g_vmakeExe = fs::path(argv[0]);
        }

        Options opts = parseOptions(argc, argv);
        if (!opts.logLevel.empty()) g_logLevel = parseLogLevel(opts.logLevel);
        if (opts.trace) g_trace = true;

        if (opts.showVersion) {
            std::cout << "vmake version " << kVmakeVersion << " (Ninja generator)" << std::endl;
            return 0;
        }
        if (opts.showHelp) {
            printUsage(std::cout);
            return 0;
        }
        if (opts.eMode) return handleE(opts.eArgs);
        if (!opts.buildModeDir.empty()) return doBuild(opts);
        if (!opts.runModeDir.empty()) return doRun(opts);
        if (!opts.installModeDir.empty()) return doInstall(opts);
        if (!opts.scriptFile.empty()) return doScriptMode(opts);
        return doConfigure(opts);
    } catch (const UsageError& e) {
        printError(e.what());
        std::cerr << "Usage: vmake [options] <path-to-source>\n"
                     "Try 'vmake --help' for more information." << std::endl;
        return 2;
    } catch (const FatalError& e) {
        printError(e.what());
        return 1;
    } catch (const std::exception& e) {
        printError(std::string("unexpected failure: ") + e.what());
        return 1;
    }
}
