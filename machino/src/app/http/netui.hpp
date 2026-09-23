// The network and USB pages, served by machino itself at /machino/net.
//
// Why a page of our own rather than an addition to the stock WebUI:
//
// The installer must not secretly patch p/header.cgi. That rule exists because
// an installer that edits the stock navigation makes an upgrade of the stock
// WebUI either revert the change or conflict with it, and because a user who
// did not ask for it should not find their files modified. So machino serves
// its own page under its own path and leaves the stock UI byte-identical. The
// menu entry that points at it is a separate, explicit step -- see
// openipc/install.sh --with-network-page.
//
// It is compiled in rather than installed as a file for two reasons: there is
// then no way for the page and the API it talks to to be different versions,
// and an upgrade of the binary cannot leave a stale asset behind.
//
// The page is deliberately plain. No framework, no build step, no fonts, no
// external anything -- a camera on its own access point has no internet, and a
// network page that needs a CDN to render is useless exactly when it is
// needed. It is about 20 kB and loads from the camera alone.
#pragma once
#include <cstddef>

namespace machino { namespace http {

// NUL-terminated HTML. Static storage; never freed, never modified.
const char* machino_net_page();
size_t      machino_net_page_len();

}} // namespace machino::http
