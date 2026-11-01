// A small JSON value: parse, read, build and write. Enough for Tailscale's
// control protocol (ts/tailscale.cpp).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace ws {

struct Json {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double n = 0;
    std::string s;  // a string's text, or a number as it was written
    std::vector<Json> a;
    std::map<std::string, Json> o;
    std::vector<std::string> keys;  // o's keys in the order they were set or read

    const Json &operator[](const std::string &k) const {
        static const Json none;
        auto it = o.find(k);
        return it == o.end() ? none : it->second;
    }
    double num(double def) const { return type == NUM ? n : def; }
    std::string str(const std::string &def = "") const { return type == STR ? s : def; }
    bool boolean(bool def) const { return type == BOOL ? b : def; }

    static Json string(const std::string &v) {
        Json j;
        j.type = STR;
        j.s = v;
        return j;
    }
    static Json object() {
        Json j;
        j.type = OBJ;
        return j;
    }
    static Json array() {
        Json j;
        j.type = ARR;
        return j;
    }
    Json &set(const std::string &k, const Json &v) {
        type = OBJ;
        if (!o.count(k)) keys.push_back(k);
        o[k] = v;
        return *this;
    }
    Json &push(const Json &v) {
        type = ARR;
        a.push_back(v);
        return *this;
    }

    static bool parse(const std::string &t, Json &out) {
        size_t i = 0;
        if (!value(t, i, out, 0)) return false;
        space(t, i);
        return i == t.size();
    }

    std::string dump() const {
        std::string r;
        write(r);
        return r;
    }

private:
    static void space(const std::string &t, size_t &i) {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
    }

    static void utf8(std::string &out, uint32_t c) {
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }

    static bool hex4(const std::string &t, size_t i, uint32_t &v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (size_t k = i; k < i + 4; k++) {
            const char c = t[k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static bool text(const std::string &t, size_t &i, std::string &out) {
        if (i >= t.size() || t[i] != '"') return false;
        i++;
        while (i < t.size()) {
            const char c = t[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= t.size()) return false;
            const char e = t[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t c1;
                    if (!hex4(t, i, c1)) return false;
                    i += 4;
                    if (c1 >= 0xD800 && c1 < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        uint32_t c2;
                        if (hex4(t, i + 2, c2) && c2 >= 0xDC00 && c2 < 0xE000) {
                            i += 6;
                            c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                        }
                    }
                    utf8(out, c1);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    static bool value(const std::string &t, size_t &i, Json &out, int depth) {
        if (depth > 64) return false;
        space(t, i);
        if (i >= t.size()) return false;
        const char c = t[i];
        if (c == '{') {
            out = object();
            i++;
            space(t, i);
            if (i < t.size() && t[i] == '}') {
                i++;
                return true;
            }
            while (true) {
                space(t, i);
                std::string k;
                if (!text(t, i, k)) return false;
                space(t, i);
                if (i >= t.size() || t[i] != ':') return false;
                i++;
                Json v;
                if (!value(t, i, v, depth + 1)) return false;
                out.set(k, v);
                space(t, i);
                if (i < t.size() && t[i] == ',') {
                    i++;
                    continue;
                }
                if (i < t.size() && t[i] == '}') {
                    i++;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            out = array();
            i++;
            space(t, i);
            if (i < t.size() && t[i] == ']') {
                i++;
                return true;
            }
            while (true) {
                Json v;
                if (!value(t, i, v, depth + 1)) return false;
                out.a.push_back(std::move(v));
                space(t, i);
                if (i < t.size() && t[i] == ',') {
                    i++;
                    continue;
                }
                if (i < t.size() && t[i] == ']') {
                    i++;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out = Json();
            out.type = STR;
            return text(t, i, out.s);
        }
        if (t.compare(i, 4, "true") == 0) {
            out = Json();
            out.type = BOOL;
            out.b = true;
            i += 4;
            return true;
        }
        if (t.compare(i, 5, "false") == 0) {
            out = Json();
            out.type = BOOL;
            i += 5;
            return true;
        }
        if (t.compare(i, 4, "null") == 0) {
            out = Json();
            i += 4;
            return true;
        }
        const size_t start = i;
        if (i < t.size() && t[i] == '-') i++;
        while (i < t.size() && ((t[i] >= '0' && t[i] <= '9') || t[i] == '.' || t[i] == 'e' || t[i] == 'E' ||
                                t[i] == '+' || t[i] == '-'))
            i++;
        if (i == start) return false;
        out = Json();
        out.type = NUM;
        out.s = t.substr(start, i - start);
        char *end = nullptr;
        out.n = strtod(out.s.c_str(), &end);
        return end && *end == 0;
    }

    static void quote(std::string &r, const std::string &v) {
        r += '"';
        for (unsigned char c : v) {
            switch (c) {
                case '"': r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n"; break;
                case '\r': r += "\\r"; break;
                case '\t': r += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char b[8];
                        snprintf(b, sizeof b, "\\u%04x", c);
                        r += b;
                    } else {
                        r += (char)c;
                    }
            }
        }
        r += '"';
    }

    void write(std::string &r) const {
        switch (type) {
            case NUL: r += "null"; break;
            case BOOL: r += b ? "true" : "false"; break;
            case NUM:
                if (!s.empty()) {
                    r += s;
                } else {
                    char buf[32];
                    snprintf(buf, sizeof buf, "%.17g", n);
                    r += buf;
                }
                break;
            case STR: quote(r, s); break;
            case ARR:
                r += '[';
                for (size_t k = 0; k < a.size(); k++) {
                    if (k) r += ',';
                    a[k].write(r);
                }
                r += ']';
                break;
            case OBJ: {
                r += '{';
                bool first = true;
                for (const std::string &k : keys) {
                    auto it = o.find(k);
                    if (it == o.end()) continue;
                    if (!first) r += ',';
                    first = false;
                    quote(r, k);
                    r += ':';
                    it->second.write(r);
                }
                r += '}';
                break;
            }
        }
    }
};

}  // namespace ws
