#include "core/net/network_txn.hpp"

#include <cstdio>
#include <cstdlib>

namespace machino { namespace net {

namespace {

// Length-prefixed, so a configuration containing newlines cannot be mistaken
// for a field separator and a truncated file is detectable rather than
// silently short.
//
//   v1\n<token>\n<len>\n<candidate>
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

// The confirmed record carries the same length prefix, so a half-written one
// is recognisable instead of being read as a shorter configuration.
const char kConfirmedTag[] = "c1\n";

std::string encode_confirmed(const std::string& cfg)
{
    std::string out = kConfirmedTag;
    append_blob(out, cfg);
    return out;
}

bool decode_confirmed(const std::string& text, std::string& out)
{
    size_t pos = 0;
    std::string tag;
    if (!take_line(text, pos, tag) || tag != "c1") return false;
    return take_blob(text, pos, out);
}

} // namespace

std::string NetworkTxn::encode_pending(const PendingRecord& r)
{
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "v1\n%llu\n", (unsigned long long)r.token);
    std::string out = hdr;
    append_blob(out, r.candidate);
    return out;
}

bool NetworkTxn::decode_pending(const std::string& text, PendingRecord& out)
{
    size_t pos = 0;
    std::string v, tok;
    if (!take_line(text, pos, v) || v != "v1") return false;
    if (!take_line(text, pos, tok) || tok.empty()) return false;
    for (char c : tok) if (c < '0' || c > '9') return false;

    PendingRecord r;
    r.token = std::strtoull(tok.c_str(), nullptr, 10);
    if (!take_blob(text, pos, r.candidate)) return false;
    out = r;
    return true;
}

NetworkTxn::NetworkTxn(IStateStore& store, ApplyFn apply,
                       std::string confirmed_key, std::string pending_key)
    : store_(store), apply_(std::move(apply)),
      ck_(std::move(confirmed_key)), pk_(std::move(pending_key)) {}

bool NetworkTxn::load_confirmed(std::string& out) const
{
    std::string blob;
    if (!store_.load(ck_, blob)) return false;
    return decode_confirmed(blob, out);
}

SeedOutcome NetworkTxn::seed_confirmed(const std::string& config, std::string& err)
{
    std::lock_guard<std::mutex> g(m_);
    std::string existing;
    if (load_confirmed(existing)) {
        err = "a confirmed configuration already exists";
        return SeedOutcome::AlreadyPresent;
    }
    if (!store_.save(ck_, encode_confirmed(config))) {
        err = "could not write the confirmed configuration";
        return SeedOutcome::Failed;
    }
    return SeedOutcome::Seeded;
}

RecoverOutcome NetworkTxn::recover(std::string& err)
{
    std::lock_guard<std::mutex> g(m_);

    std::string pending_blob;
    const bool had_pending = store_.load(pk_, pending_blob);

    PendingRecord pr;
    const bool pending_readable = had_pending && decode_pending(pending_blob, pr);
    if (pending_readable && pr.token >= next_token_) next_token_ = pr.token + 1;

    if (!had_pending) return RecoverOutcome::Nothing;

    // Something was in flight. Whether we can READ it does not change that --
    // an unreadable pending record still means a change was applied and never
    // confirmed, so the known-good configuration goes back on either way.
    std::string good;
    if (!load_confirmed(good)) {
        // Fail closed. We will not invent a network configuration; the camera
        // stays on whatever the boot scripts set and the operator is told.
        err = "an unconfirmed network change was found, but the last confirmed "
              "configuration is missing or unreadable -- nothing was applied";
        return RecoverOutcome::ConfirmedUnusable;
    }

    pending_ = false;
    deadline_valid_ = false;
    store_.clear(pk_);

    Result rc = apply_ ? apply_(good) : Result::ok();
    if (!rc.is_ok()) {
        err = "could not restore the last confirmed network configuration";
        return RecoverOutcome::RolledBack;
    }
    err = pending_readable
        ? "an unconfirmed network change was rolled back after restart"
        : "an unreadable pending change was found; the last confirmed configuration was restored";
    return RecoverOutcome::RolledBack;
}

bool NetworkTxn::begin(const std::string& candidate, uint32_t now_ms, uint32_t window_ms,
                       uint64_t& token_out, std::string& err)
{
    if (window_ms == 0) { err = "a confirmation window of zero would roll back instantly"; return false; }

    std::lock_guard<std::mutex> g(m_);
    if (pending_) { err = "another change is still waiting for confirmation"; return false; }

    std::string good;
    if (!load_confirmed(good)) {
        // Without a known-good configuration there is nothing to fall back to,
        // so applying a candidate would be a one-way door.
        err = "there is no confirmed configuration to fall back to";
        return false;
    }

    PendingRecord pr;
    pr.token = next_token_++;
    pr.candidate = candidate;

    // Written BEFORE applying. A crash between this line and the next leaves a
    // pending record, and recover() puts the confirmed configuration back.
    if (!store_.save(pk_, encode_pending(pr))) {
        err = "could not record the pending change; refusing to apply it";
        return false;
    }

    Result rc = apply_ ? apply_(candidate) : Result::ok();
    if (!rc.is_ok()) {
        store_.clear(pk_);
        if (apply_) apply_(good);        // put the known-good one back
        err = "the change could not be applied";
        return false;
    }

    pending_ = true;
    token_ = pr.token;
    deadline_ms_ = now_ms + window_ms;
    deadline_valid_ = true;
    token_out = pr.token;
    return true;
}

bool NetworkTxn::confirm(uint64_t token, std::string& err)
{
    std::lock_guard<std::mutex> g(m_);
    if (!pending_)        { err = "nothing is waiting for confirmation"; return false; }
    if (token != token_)  { err = "that confirmation belongs to a different change"; return false; }

    std::string blob;
    PendingRecord pr;
    if (!store_.load(pk_, blob) || !decode_pending(blob, pr) || pr.token != token) {
        err = "the pending change is no longer on record";
        return false;
    }

    // Confirmed first, pending second. Crashing between the two leaves a
    // pending record next to an already-correct confirmed one, and recovery
    // then re-applies what is already in place -- harmless. The other order
    // would lose the new configuration.
    if (!store_.save(ck_, encode_confirmed(pr.candidate))) {
        err = "could not record the confirmation";
        return false;
    }
    store_.clear(pk_);

    pending_ = false;
    deadline_valid_ = false;
    return true;
}

bool NetworkTxn::rollback_locked(std::string& err)
{
    std::string good;
    const bool have = load_confirmed(good);

    pending_ = false;
    deadline_valid_ = false;
    store_.clear(pk_);

    if (!have) { err = "no confirmed configuration to restore"; return true; }
    Result rc = apply_ ? apply_(good) : Result::ok();
    if (!rc.is_ok()) err = "could not restore the last confirmed network configuration";
    return true;
}

bool NetworkTxn::tick(uint32_t now_ms)
{
    std::lock_guard<std::mutex> g(m_);
    if (!pending_ || !deadline_valid_) return false;
    if ((int32_t)(now_ms - deadline_ms_) < 0) return false;
    std::string ignored;
    return rollback_locked(ignored);
}

bool NetworkTxn::pending() const
{
    std::lock_guard<std::mutex> g(m_);
    return pending_;
}

uint64_t NetworkTxn::token() const
{
    std::lock_guard<std::mutex> g(m_);
    return token_;
}

uint32_t NetworkTxn::remaining_ms(uint32_t now_ms) const
{
    std::lock_guard<std::mutex> g(m_);
    if (!pending_ || !deadline_valid_) return 0;
    int32_t d = (int32_t)(deadline_ms_ - now_ms);
    return d > 0 ? (uint32_t)d : 0u;
}

bool NetworkTxn::confirmed_config(std::string& out) const
{
    std::lock_guard<std::mutex> g(m_);
    return load_confirmed(out);
}

}} // namespace machino::net
