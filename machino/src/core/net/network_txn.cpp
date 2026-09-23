#include "core/net/network_txn.hpp"

#include <cstdio>
#include <cstdlib>

namespace machino { namespace net {

namespace {

// A length-prefixed format, so a configuration containing newlines or an
// equals sign cannot be mistaken for a field separator, and so a truncated
// file is detectable rather than silently short.
//
//   v1\n<token>\n<pending>\n<len>\n<confirmed><len>\n<candidate>
void append_blob(std::string& out, const std::string& s)
{
    char hdr[32];
    std::snprintf(hdr, sizeof(hdr), "%zu\n", s.size());
    out += hdr;
    out += s;
}

bool take_line(const std::string& s, size_t& pos, std::string& out)
{
    size_t nl = s.find('\n', pos);
    if (nl == std::string::npos) return false;
    out = s.substr(pos, nl - pos);
    pos = nl + 1;
    return true;
}

bool take_blob(const std::string& s, size_t& pos, std::string& out)
{
    std::string len_line;
    if (!take_line(s, pos, len_line) || len_line.empty()) return false;
    for (char c : len_line) if (c < '0' || c > '9') return false;
    const unsigned long n = std::strtoul(len_line.c_str(), nullptr, 10);
    if (pos + n > s.size()) return false;        // truncated write
    out = s.substr(pos, n);
    pos += n;
    return true;
}

} // namespace

std::string NetworkTxn::encode(const TxnRecord& r)
{
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "v1\n%llu\n%d\n", (unsigned long long)r.token, r.pending ? 1 : 0);
    std::string out = hdr;
    append_blob(out, r.confirmed);
    append_blob(out, r.candidate);
    return out;
}

bool NetworkTxn::decode(const std::string& text, TxnRecord& out)
{
    size_t pos = 0;
    std::string v, tok, pend;
    if (!take_line(text, pos, v) || v != "v1") return false;
    if (!take_line(text, pos, tok)) return false;
    if (!take_line(text, pos, pend)) return false;
    if (tok.empty()) return false;
    for (char c : tok) if (c < '0' || c > '9') return false;

    TxnRecord r;
    r.token = std::strtoull(tok.c_str(), nullptr, 10);
    r.pending = (pend == "1");
    if (!take_blob(text, pos, r.confirmed)) return false;
    if (!take_blob(text, pos, r.candidate)) return false;
    out = r;
    return true;
}

NetworkTxn::NetworkTxn(IStateStore& store, ApplyFn apply, std::string key)
    : store_(store), apply_(std::move(apply)), key_(std::move(key)) {}

bool NetworkTxn::recover(std::string& err)
{
    std::lock_guard<std::mutex> g(m_);

    std::string blob;
    if (!store_.load(key_, blob)) return false;      // nothing in flight

    TxnRecord r;
    if (!decode(blob, r)) {
        // A torn or garbage record. We cannot know what was applied, but we do
        // know the machine came up, so the safest action is to drop it and
        // leave whatever the boot scripts configured. Saying so matters more
        // than pretending it never happened.
        store_.clear(key_);
        err = "the stored network transaction was unreadable and has been discarded";
        return false;
    }

    // Continue the token sequence across the restart. Without this the
    // counter starts at 1 again, and a browser still holding a token from
    // before the crash could confirm a DIFFERENT change that happens to get
    // the same number -- which is precisely the confirmation-of-something-
    // -else this class is supposed to make impossible.
    if (r.token >= next_token_) next_token_ = r.token + 1;

    if (!r.pending) {
        rec_ = r;                                     // just the confirmed baseline
        return false;
    }

    // Applied but never confirmed, and then the process or the board went
    // away. No timer survives a reboot, so there is nothing to wait for.
    rec_ = r;
    rec_.pending = false;
    deadline_valid_ = false;

    Result rc = apply_ ? apply_(r.confirmed) : Result::ok();
    rec_.candidate.clear();
    store_.save(key_, encode(rec_));
    if (!rc.is_ok()) {
        err = "could not restore the last confirmed network configuration";
        return true;
    }
    err = "an unconfirmed network change was rolled back after restart";
    return true;
}

bool NetworkTxn::begin(const std::string& confirmed, const std::string& candidate,
                       uint32_t now_ms, uint32_t window_ms,
                       uint64_t& token_out, std::string& err)
{
    if (window_ms == 0) { err = "a confirmation window of zero would roll back instantly"; return false; }

    std::lock_guard<std::mutex> g(m_);
    if (rec_.pending) { err = "another change is still waiting for confirmation"; return false; }

    TxnRecord r;
    r.token = next_token_++;
    r.confirmed = confirmed;
    r.candidate = candidate;
    r.pending = true;

    // Written BEFORE applying. A crash between this line and the next leaves a
    // pending record, and recover() puts the confirmed configuration back --
    // which is the whole reason this class exists.
    if (!store_.save(key_, encode(r))) {
        err = "could not record the pending change; refusing to apply it";
        return false;
    }

    Result rc = apply_ ? apply_(candidate) : Result::ok();
    if (!rc.is_ok()) {
        // Put the known-good one back and forget the attempt.
        if (apply_) apply_(confirmed);
        TxnRecord base;
        base.token = r.token;
        base.confirmed = confirmed;
        base.pending = false;
        rec_ = base;
        store_.save(key_, encode(base));
        err = "the change could not be applied";
        return false;
    }

    rec_ = r;
    deadline_ms_ = now_ms + window_ms;
    deadline_valid_ = true;
    token_out = r.token;
    return true;
}

bool NetworkTxn::confirm(uint64_t token, std::string& err)
{
    std::lock_guard<std::mutex> g(m_);
    if (!rec_.pending)     { err = "nothing is waiting for confirmation"; return false; }
    if (token != rec_.token) { err = "that confirmation belongs to a different change"; return false; }

    TxnRecord r;
    r.token = rec_.token;
    r.confirmed = rec_.candidate;     // the candidate is now the known-good one
    r.pending = false;
    if (!store_.save(key_, encode(r))) { err = "could not record the confirmation"; return false; }

    rec_ = r;
    deadline_valid_ = false;
    return true;
}

bool NetworkTxn::rollback_locked(std::string& err)
{
    const std::string good = rec_.confirmed;
    TxnRecord base;
    base.token = rec_.token;
    base.confirmed = good;
    base.pending = false;

    rec_ = base;
    deadline_valid_ = false;
    store_.save(key_, encode(base));

    Result rc = apply_ ? apply_(good) : Result::ok();
    if (!rc.is_ok()) { err = "could not restore the last confirmed network configuration"; return true; }
    return true;
}

bool NetworkTxn::tick(uint32_t now_ms)
{
    std::lock_guard<std::mutex> g(m_);
    if (!rec_.pending || !deadline_valid_) return false;
    if ((int32_t)(now_ms - deadline_ms_) < 0) return false;
    std::string ignored;
    return rollback_locked(ignored);
}

bool NetworkTxn::pending() const
{
    std::lock_guard<std::mutex> g(m_);
    return rec_.pending;
}

uint64_t NetworkTxn::token() const
{
    std::lock_guard<std::mutex> g(m_);
    return rec_.token;
}

uint32_t NetworkTxn::remaining_ms(uint32_t now_ms) const
{
    std::lock_guard<std::mutex> g(m_);
    if (!rec_.pending || !deadline_valid_) return 0;
    int32_t d = (int32_t)(deadline_ms_ - now_ms);
    return d > 0 ? (uint32_t)d : 0u;
}

std::string NetworkTxn::confirmed_config() const
{
    std::lock_guard<std::mutex> g(m_);
    return rec_.confirmed;
}

}} // namespace machino::net
