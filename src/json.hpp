// Small JSON value type with a strict parser and a compact serializer.
// Objects keep insertion order so dumps stay readable and stable.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(int v) : type_(Type::Number), num_(v), int_(v), isInt_(true) {}
    Json(unsigned v) : type_(Type::Number), num_(v), int_(v), isInt_(true) {}
    Json(long v) : type_(Type::Number), num_(v), int_(v), isInt_(true) {}
    Json(long long v) : type_(Type::Number), num_(double(v)), int_(v), isInt_(true) {}
    Json(unsigned long v) : type_(Type::Number), num_(double(v)), int_(int64_t(v)), isInt_(true) {}
    Json(unsigned long long v) : type_(Type::Number), num_(double(v)), int_(int64_t(v)), isInt_(true) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* v) : type_(Type::String), str_(v ? v : "") {}
    Json(std::string v) : type_(Type::String), str_(std::move(v)) {}
    Json(Array v) : type_(Type::Array), arr_(std::move(v)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    // Returns Null and fills *error on malformed input.
    static Json parse(const std::string& text, std::string* error = nullptr);
    std::string dump() const;

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Lenient accessors: wrong type yields the default.
    std::string str(const std::string& def = "") const { return type_ == Type::String ? str_ : def; }
    const std::string& strRef() const;
    double num(double def = 0) const { return type_ == Type::Number ? num_ : def; }
    int64_t i64(int64_t def = 0) const;
    bool boolean(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }

    // Object access. The const form returns a shared Null for missing keys.
    const Json& operator[](const std::string& key) const;
    const Json& operator[](const char* key) const { return (*this)[std::string(key)]; }
    Json& operator[](const std::string& key);
    Json& operator[](const char* key) { return (*this)[std::string(key)]; }
    bool has(const std::string& key) const;
    void erase(const std::string& key);

    // Array access.
    const Json& at(size_t i) const;
    Json& at(size_t i) { return arr_.at(i); }
    void push(Json v);

    size_t size() const;
    bool empty() const { return size() == 0; }
    const Array& items() const { return arr_; }
    Array& items() { return arr_; }
    const Object& fields() const { return obj_; }
    Object& fields() { return obj_; }

    bool operator==(const Json& o) const;
    bool operator!=(const Json& o) const { return !(*this == o); }

private:
    void dumpTo(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    int64_t int_ = 0;
    bool isInt_ = false;
    std::string str_;
    Array arr_;
    Object obj_;

    friend class JsonParser;
};

// Builds an object from key/value pairs: J({{"a", 1}, {"b", "x"}}).
Json J(std::initializer_list<Json::Member> members);
