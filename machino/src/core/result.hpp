// Machino core: status codes. No exceptions anywhere in the runtime; every
// fallible call returns a Result and the caller decides.
#pragma once

namespace machino {

enum class Status : int {
    Ok = 0,
    Error,        // generic failure, `code` carries the vendor/errno value
    Timeout,      // nothing available within the requested time
    Busy,         // resource already in use / wrong state
    Unsupported,  // platform lacks the capability
};

struct Result {
    Status status;
    int    code;   // vendor return value or errno; 0 when Ok

    static Result ok()                       { return {Status::Ok, 0}; }
    static Result error(int c = -1)          { return {Status::Error, c}; }
    static Result timeout()                  { return {Status::Timeout, 0}; }
    static Result busy()                     { return {Status::Busy, 0}; }
    static Result unsupported()              { return {Status::Unsupported, 0}; }

    bool is_ok() const  { return status == Status::Ok; }
    explicit operator bool() const { return is_ok(); }
};

inline const char* status_name(Status s) {
    switch (s) {
        case Status::Ok:          return "ok";
        case Status::Error:       return "error";
        case Status::Timeout:     return "timeout";
        case Status::Busy:        return "busy";
        case Status::Unsupported: return "unsupported";
    }
    return "?";
}

} // namespace machino
