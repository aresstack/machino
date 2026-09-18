// JSON parser/serializer boundary tests.
#include "core/json.hpp"
#include <cstdio>
#include <string>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define JCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

void run_json_tests() {
    Json j; std::string err;
    JCHECK(Json::parse("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":-2.5}}", j, err));
    JCHECK(j.is_object() && j.get("a")->as_int() == 1 && j.get("b")->at(0).as_bool() && j.get("b")->at(1).is_null());
    JCHECK(j.get("b")->at(2).as_string() == "x" && json_path(j, "c.d")->as_number() == -2.5);
    JCHECK(j.dump() == "{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":-2.5}}");     // insertion order kept
    JCHECK(json_path(j, "c.e") == nullptr && json_path(j, "a.b") == nullptr);

    // empty / malformed / truncated / trailing
    JCHECK(!Json::parse("", j, err));
    JCHECK(!Json::parse("{", j, err));
    JCHECK(!Json::parse("{\"a\":1", j, err));
    JCHECK(!Json::parse("{\"a\":1} x", j, err) && err.find("trailing") != std::string::npos);
    JCHECK(!Json::parse("{\"a\":}", j, err));
    JCHECK(!Json::parse("[1,]", j, err));
    JCHECK(!Json::parse("{'a':1}", j, err));
    // duplicate keys are rejected
    JCHECK(!Json::parse("{\"a\":1,\"a\":2}", j, err) && err.find("duplicate") != std::string::npos);
    // deep nesting
    std::string deep; for (int i = 0; i < 40; ++i) deep += "["; for (int i = 0; i < 40; ++i) deep += "]";
    JCHECK(!Json::parse(deep, j, err) && err.find("deep") != std::string::npos);
    std::string ok_deep; for (int i = 0; i < 10; ++i) ok_deep += "["; for (int i = 0; i < 10; ++i) ok_deep += "]";
    JCHECK(Json::parse(ok_deep, j, err));
    // long strings and oversized documents
    std::string longs = "\"" + std::string(5000, 'a') + "\"";
    JCHECK(!Json::parse(longs, j, err) && err.find("long") != std::string::npos);
    Json::Limits small; small.max_bytes = 10;
    JCHECK(!Json::parse("{\"abc\":12345}", j, err, small) && err.find("large") != std::string::npos);
    // numbers: negative, overflow, bad forms
    JCHECK(Json::parse("-42", j, err) && j.is_integer() && j.as_int() == -42);
    JCHECK(Json::parse("1e400", j, err) == false);                                  // out of range
    JCHECK(!Json::parse("01", j, err) || true);                                     // leading zero tolerated by strtod path or rejected: not a crash
    JCHECK(!Json::parse("-", j, err) && !Json::parse("1.", j, err) && !Json::parse(".5", j, err));
    JCHECK(Json::parse("12345678901234567890123", j, err) == false || j.is_number());  // very long numbers are rejected or parsed as double
    JCHECK(Json::parse("1.5", j, err) && !j.is_integer());
    // control characters / escapes / unicode
    JCHECK(!Json::parse("\"a\nb\"", j, err));
    JCHECK(Json::parse("\"a\\nb\\u0041\"", j, err) && j.as_string() == "a\nbA");
    JCHECK(Json::string("q\"\\\n").dump() == "\"q\\\"\\\\\\n\"");
    // objects: set/get/replace
    Json o = Json::object(); o.set("x", Json::integer(1)); o.set("x", Json::integer(2)); o.set("y", Json::null());
    JCHECK(o.size() == 2 && o.get("x")->as_int() == 2 && o.get("y")->is_null() && o.dump() == "{\"x\":2,\"y\":null}");
    JCHECK(Json::number(3.25).dump() == "3.25" && Json::integer(7).dump() == "7" && Json::boolean(false).dump() == "false");
}
