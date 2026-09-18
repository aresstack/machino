// Machino core: minimal JSON value, parser and serializer.
// Bounded on purpose (depth, string length, document size); objects keep
// insertion order; duplicate keys are a parse error. No exceptions.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace machino {

struct JsonLimits { size_t max_depth = 16; size_t max_string = 4096; size_t max_bytes = 16384; };

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static Json null()  { return Json(); }
    static Json boolean(bool b) { Json j; j.t_ = Type::Bool; j.b_ = b; return j; }
    static Json number(double d) { Json j; j.t_ = Type::Number; j.n_ = d; return j; }
    static Json integer(long long i) { Json j; j.t_ = Type::Number; j.n_ = (double)i; return j; }
    static Json string(const std::string& s) { Json j; j.t_ = Type::String; j.s_ = s; return j; }
    static Json array() { Json j; j.t_ = Type::Array; return j; }
    static Json object() { Json j; j.t_ = Type::Object; return j; }

    Type type() const { return t_; }
    bool is_null() const   { return t_ == Type::Null; }
    bool is_bool() const   { return t_ == Type::Bool; }
    bool is_number() const { return t_ == Type::Number; }
    bool is_string() const { return t_ == Type::String; }
    bool is_array() const  { return t_ == Type::Array; }
    bool is_object() const { return t_ == Type::Object; }
    bool is_integer() const;                      // number with no fractional part within int64 range

    bool        as_bool() const   { return b_; }
    double      as_number() const { return n_; }
    long long   as_int() const    { return (long long)n_; }
    const std::string& as_string() const { return s_; }

    // arrays
    Json& push(const Json& v) { items_.push_back(v); return *this; }
    size_t size() const { return t_ == Type::Object ? members_.size() : items_.size(); }
    const Json& at(size_t i) const { return items_[i]; }

    // objects (insertion-ordered)
    Json& set(const std::string& key, const Json& v);
    const Json* get(const std::string& key) const;
    bool has(const std::string& key) const { return get(key) != nullptr; }
    const std::vector<std::pair<std::string, Json>>& members() const { return members_; }

    std::string dump() const;                     // compact
    void dump_to(std::string& out) const;

    using Limits = JsonLimits;
    // Returns false and fills `err` (position + reason) on any violation.
    static bool parse(const std::string& text, Json& out, std::string& err, const JsonLimits& lim = JsonLimits());

private:
    Type   t_ = Type::Null;
    bool   b_ = false;
    double n_ = 0.0;
    std::string s_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

// Dotted-path lookup: "video.0.bitrate_kbps" (objects only).
const Json* json_path(const Json& root, const std::string& path);

} // namespace machino
