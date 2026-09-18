// Compatibility surface for OpenIPC/majestic-webui.
//
// This is deliberately an adapter at the HTTP boundary. Machino's native
// /api/v1 contract stays platform-neutral and keeps its own shape; the WebUI
// adapter only presents the legacy schema/config documents and translates
// writes back into native partial patches.
#pragma once
#include "core/json.hpp"
#include <string>

namespace machino { namespace compat {

struct MajesticTranslation {
    bool ok = false;
    int status = 400;
    std::string code;
    std::string path;
    std::string message;
    Json patch = Json::object();
};

// Build the schema shape consumed by majestic-webui's mj-settings.js.
// Only controls Machino reports as supported are advertised.
Json majestic_schema(const Json& capabilities);

// Flatten Machino's native config into the one-section-deep shape the current
// majestic-webui renderer expects (notably video.0 -> video0). Effective state
// fills optional image controls that have no explicit requested value yet.
Json majestic_config(const Json& native_config, const Json& state);

// Translate a majestic-webui POST body back into a native Machino PATCH body.
// Validation of values remains exclusively in ApiService::patch_config().
MajesticTranslation majestic_post_to_native(const std::string& body);

}} // namespace machino::compat
