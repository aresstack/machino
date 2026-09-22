#include "app/http/setup.hpp"
#include "app/http/session.hpp"

namespace machino { namespace http {

namespace {

// setup.html links and fetches /eula.<lang>.txt. Matching on the shape rather
// than on a list of languages keeps a new translation on the image working
// without a code change, while still refusing anything that is not one of
// these documents (no slashes, so it cannot climb out of the web root).
bool is_eula_doc(const std::string& path) {
    if (path.compare(0, 6, "/eula.") != 0) return false;
    if (path.size() < 11 || path.compare(path.size() - 4, 4, ".txt") != 0) return false;
    return path.find('/', 1) == std::string::npos;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

} // namespace

bool SetupGate::is_setup_path(const std::string& method, const std::string& path) {
    if (method == "POST") return path == "/setup";
    if (method != "GET") return false;
    return path == "/setup.html" || path == "/favicon.ico" || is_eula_doc(path);
}

bool SetupGate::valid_public_key(const std::string& key, std::string& err) {
    const std::string k = trim(key);
    if (k.empty()) { err = "The key is empty."; return false; }
    if (k.size() > MAX_SSHKEY) { err = "The key is too long."; return false; }
    // A PRIVATE key must never be accepted. The page already refuses to send
    // one, but the page is not what decides - this is.
    if (k.find("PRIVATE KEY") != std::string::npos) {
        err = "That is a private key. Send the public half (id_*.pub).";
        return false;
    }
    // One line: authorized_keys is line-oriented, and a newline in the middle
    // would smuggle in a second, unreviewed entry.
    if (k.find('\n') != std::string::npos || k.find('\r') != std::string::npos) {
        err = "A public key must be a single line.";
        return false;
    }
    // "<type> <base64>[ comment]" with a type OpenSSH actually names.
    const size_t sp = k.find(' ');
    if (sp == std::string::npos) { err = "That does not look like a public key."; return false; }
    const std::string type = k.substr(0, sp);
    static const char* TYPES[] = {"ssh-rsa", "ssh-ed25519", "ssh-dss",
                                  "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
                                  "ecdsa-sha2-nistp521", "sk-ssh-ed25519@openssh.com",
                                  "sk-ecdsa-sha2-nistp256@openssh.com"};
    bool known = false;
    for (const char* t : TYPES) if (type == t) { known = true; break; }
    if (!known) { err = "Unknown key type '" + type + "'."; return false; }
    const std::string rest = trim(k.substr(sp + 1));
    const size_t blob_end = rest.find(' ');
    const std::string blob = blob_end == std::string::npos ? rest : rest.substr(0, blob_end);
    if (blob.size() < 16) { err = "That does not look like a public key."; return false; }
    for (char ch : blob) {
        const bool b64 = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                         (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '=';
        if (!b64) { err = "That does not look like a public key."; return false; }
    }
    return true;
}

SetupOutcome SetupGate::post(const std::string& form_body) {
    SetupOutcome out;

    // Claimed cameras: 403, exactly as upstream. This is checked FIRST, before
    // anything is parsed, so a late or replayed POST cannot do work.
    if (!unclaimed()) {
        out.status = 403;
        out.body = "This camera has already been set up.";
        return out;
    }
    if (form_body.size() > MAX_BODY) {
        out.status = 413;
        out.body = "The request is too large.";
        return out;
    }

    const std::string password = SessionGate::form_value(form_body, "password");
    const std::string confirm  = SessionGate::form_value(form_body, "confirm");

    // Order matches the page's own wording so a rule that drifts reads the
    // same on both sides.
    if (password.empty()) {
        out.status = 400; out.body = "Enter a password."; return out;
    }
    if (password.size() < MIN_PASSWORD) {
        out.status = 400; out.body = "The password must be at least 8 characters."; return out;
    }
    if (password.size() > MAX_PASSWORD) {
        out.status = 400; out.body = "The password is too long."; return out;
    }
    if (password != confirm) {
        out.status = 400; out.body = "The passwords do not match."; return out;
    }
    // The password is handed to the system as "root:<password>" on one line.
    // A colon or a line break there is not a weak password, it is a second
    // record - so it is refused rather than escaped.
    if (password.find(':') != std::string::npos ||
        password.find('\n') != std::string::npos ||
        password.find('\r') != std::string::npos ||
        password.find('\0') != std::string::npos) {
        out.status = 400;
        out.body = "The password cannot contain a colon or a line break.";
        return out;
    }

    // Upstream enforces acceptance whenever the document is on the image, and
    // the server checks it again even though the page already did.
    if (eula_ && eula_()) {
        if (SessionGate::form_value(form_body, "eula") != "accepted") {
            out.status = 400;
            out.body = "The licence agreement has to be accepted.";
            return out;
        }
    }

    // The optional key is validated BEFORE the camera is claimed. A refused key
    // has to cost a 400 on a camera that is STILL UNCLAIMED - a form that can
    // be corrected - because /setup is gone afterwards and there is no second
    // chance to install one through this flow.
    const std::string sshkey = trim(SessionGate::form_value(form_body, "sshkey"));
    if (!sshkey.empty()) {
        std::string kerr;
        if (!valid_public_key(sshkey, kerr)) {
            out.status = 400;
            out.body = kerr;
            return out;
        }
    }

    std::string err;
    if (!claim_ || !claim_(password, err)) {
        out.status = 500;
        out.body = err.empty() ? "The password could not be set." : err;
        return out;
    }

    // An exit status is not proof the hash was written. Upstream authenticates
    // against the new password to check it landed, and so does this.
    if (!verify_ || !verify_("root", password)) {
        out.status = 500;
        out.body = "The password was not stored. The camera is still unclaimed.";
        return out;
    }

    // Claimed from here on. Anything that fails now must NOT undo the claim or
    // report failure - the camera really is set up. It is reported as a 200
    // with a message, which is the page's "claimed, with something left
    // undone" path: it stays put and shows the text instead of navigating on.
    out.status = 200;
    out.mint_session = true;
    if (!sshkey.empty()) {
        std::string kerr;
        if (!key_) {
            out.body = "The camera is set up, but this build cannot install an SSH key. "
                       "Add it over SSH once you have signed in.";
        } else if (!key_(sshkey, kerr)) {
            out.body = "The camera is set up, but the SSH key was not installed" +
                       (kerr.empty() ? std::string(".") : ": " + kerr);
        }
    }
    return out;
}

}} // namespace machino::http
