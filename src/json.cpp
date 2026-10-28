#include "json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
const Json kNull;
}

class JsonParser {
public:
    JsonParser(const std::string& s) : s_(s) {}

    bool parseDocument(Json& out, std::string& err) {
        skipWs();
        if (!parseValue(out, 0)) { err = error_; return false; }
        skipWs();
        if (pos_ != s_.size()) { err = "trailing characters at " + std::to_string(pos_); return false; }
        return true;
    }

private:
    bool fail(const char* msg) {
        error_ = std::string(msg) + " at " + std::to_string(pos_);
        return false;
    }

    void skipWs() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool parseValue(Json& out, int depth) {
        if (depth > 256) return fail("nesting too deep");
        if (pos_ >= s_.size()) return fail("unexpected end");
        char c = s_[pos_];
        switch (c) {
        case '{': return parseObject(out, depth);
        case '[': return parseArray(out, depth);
        case '"': {
            std::string v;
            if (!parseString(v)) return false;
            out = Json(std::move(v));
            return true;
        }
        case 't': return parseLiteral("true", Json(true), out);
        case 'f': return parseLiteral("false", Json(false), out);
        case 'n': return parseLiteral("null", Json(), out);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
            return fail("unexpected character");
        }
    }

    bool parseLiteral(const char* word, Json value, Json& out) {
        size_t n = std::strlen(word);
        if (s_.compare(pos_, n, word) != 0) return fail("bad literal");
        pos_ += n;
        out = std::move(value);
        return true;
    }

    bool parseNumber(Json& out) {
        size_t start = pos_;
        bool isInt = true;
        if (s_[pos_] == '-') ++pos_;
        if (pos_ >= s_.size() || !isdigit((unsigned char)s_[pos_])) return fail("bad number");
        while (pos_ < s_.size() && isdigit((unsigned char)s_[pos_])) ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            isInt = false;
            ++pos_;
            if (pos_ >= s_.size() || !isdigit((unsigned char)s_[pos_])) return fail("bad fraction");
            while (pos_ < s_.size() && isdigit((unsigned char)s_[pos_])) ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            isInt = false;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (pos_ >= s_.size() || !isdigit((unsigned char)s_[pos_])) return fail("bad exponent");
            while (pos_ < s_.size() && isdigit((unsigned char)s_[pos_])) ++pos_;
        }
        std::string text = s_.substr(start, pos_ - start);
        if (isInt && text.size() < 19) {
            out = Json((long long)std::strtoll(text.c_str(), nullptr, 10));
        } else {
            out = Json(std::strtod(text.c_str(), nullptr));
        }
        return true;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool parseHex4(uint32_t& v) {
        if (pos_ + 4 > s_.size()) return fail("short unicode escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return fail("bad unicode escape");
        }
        return true;
    }

    bool parseString(std::string& out) {
        ++pos_;  // opening quote
        while (true) {
            if (pos_ >= s_.size()) return fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return fail("control character in string");
            if (c != '\\') { out += c; continue; }
            if (pos_ >= s_.size()) return fail("unterminated escape");
            char e = s_[pos_++];
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
                uint32_t cp;
                if (!parseHex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // High surrogate: a low surrogate must follow, otherwise emit U+FFFD.
                    if (pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        uint32_t lo;
                        if (!parseHex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else { appendUtf8(out, 0xFFFD); cp = lo; }
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                appendUtf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
    }

    bool parseArray(Json& out, int depth) {
        ++pos_;
        out = Json::array();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        while (true) {
            Json v;
            skipWs();
            if (!parseValue(v, depth + 1)) return false;
            out.arr_.push_back(std::move(v));
            skipWs();
            if (pos_ >= s_.size()) return fail("unterminated array");
            char c = s_[pos_++];
            if (c == ']') return true;
            if (c != ',') return fail("expected , or ]");
        }
    }

    bool parseObject(Json& out, int depth) {
        ++pos_;
        out = Json::object();
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        while (true) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected key");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected :");
            ++pos_;
            skipWs();
            Json v;
            if (!parseValue(v, depth + 1)) return false;
            // Duplicate keys: last one wins, matching JavaScript.
            bool replaced = false;
            for (auto& m : out.obj_) {
                if (m.first == key) { m.second = std::move(v); replaced = true; break; }
            }
            if (!replaced) out.obj_.emplace_back(std::move(key), std::move(v));
            skipWs();
            if (pos_ >= s_.size()) return fail("unterminated object");
            char c = s_[pos_++];
            if (c == '}') return true;
            if (c != ',') return fail("expected , or }");
        }
    }

    const std::string& s_;
    size_t pos_ = 0;
    std::string error_;
};

Json Json::parse(const std::string& text, std::string* error) {
    JsonParser p(text);
    Json out;
    std::string err;
    if (!p.parseDocument(out, err)) {
        if (error) *error = err;
        return Json();
    }
    if (error) error->clear();
    return out;
}

static void dumpString(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
    }
    out += '"';
}

void Json::dumpTo(std::string& out) const {
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: {
        if (isInt_) {
            out += std::to_string(int_);
        } else if (!std::isfinite(num_)) {
            out += "null";
        } else if (num_ == std::floor(num_) && std::fabs(num_) < 9007199254740992.0) {
            out += std::to_string((long long)num_);
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.17g", num_);
            out += buf;
        }
        break;
    }
    case Type::String: dumpString(str_, out); break;
    case Type::Array: {
        out += '[';
        for (size_t i = 0; i < arr_.size(); ++i) {
            if (i) out += ',';
            arr_[i].dumpTo(out);
        }
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        for (size_t i = 0; i < obj_.size(); ++i) {
            if (i) out += ',';
            dumpString(obj_[i].first, out);
            out += ':';
            obj_[i].second.dumpTo(out);
        }
        out += '}';
        break;
    }
    }
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

const std::string& Json::strRef() const {
    static const std::string empty;
    return type_ == Type::String ? str_ : empty;
}

int64_t Json::i64(int64_t def) const {
    if (type_ != Type::Number) return def;
    return isInt_ ? int_ : (int64_t)num_;
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return kNull;
    for (const auto& m : obj_) {
        if (m.first == key) return m.second;
    }
    return kNull;
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        *this = Json::object();
    }
    for (auto& m : obj_) {
        if (m.first == key) return m.second;
    }
    obj_.emplace_back(key, Json());
    return obj_.back().second;
}

bool Json::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& m : obj_) {
        if (m.first == key) return true;
    }
    return false;
}

void Json::erase(const std::string& key) {
    if (type_ != Type::Object) return;
    for (auto it = obj_.begin(); it != obj_.end(); ++it) {
        if (it->first == key) { obj_.erase(it); return; }
    }
}

const Json& Json::at(size_t i) const {
    if (type_ != Type::Array || i >= arr_.size()) return kNull;
    return arr_[i];
}

void Json::push(Json v) {
    if (type_ != Type::Array) *this = Json::array();
    arr_.push_back(std::move(v));
}

size_t Json::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

bool Json::operator==(const Json& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
    case Type::Null: return true;
    case Type::Bool: return bool_ == o.bool_;
    case Type::Number: return (isInt_ && o.isInt_) ? int_ == o.int_ : num_ == o.num_;
    case Type::String: return str_ == o.str_;
    case Type::Array: return arr_ == o.arr_;
    case Type::Object: {
        if (obj_.size() != o.obj_.size()) return false;
        for (const auto& m : obj_) {
            if (!o.has(m.first) || o[m.first] != m.second) return false;
        }
        return true;
    }
    }
    return false;
}

Json J(std::initializer_list<Json::Member> members) {
    Json j = Json::object();
    for (const auto& m : members) j[m.first] = m.second;
    return j;
}
