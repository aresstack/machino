// A tiny durable key/value blob store for state that must survive a crash.
//
// This is not the config store. machino.conf holds what the user configured;
// this holds what the runtime is in the middle of doing -- specifically a
// network change that has been applied but not yet confirmed. Keeping the two
// apart matters: a half-applied transaction must never leak into the file the
// user edits.
//
// The write must be atomic, because the interesting failure is losing power
// between "applied the new WiFi settings" and "wrote down how to undo it".
#pragma once
#include <string>

namespace machino {

class IStateStore {
public:
    virtual ~IStateStore() = default;

    // false when the key is absent. A corrupt or truncated value is returned
    // as-is; interpreting it is the caller's job, and the caller must cope.
    virtual bool load(const std::string& key, std::string& out) const = 0;

    // Must be atomic: after this returns, a reader sees either the previous
    // value or the new one, never a fragment.
    virtual bool save(const std::string& key, const std::string& value) = 0;

    virtual void clear(const std::string& key) = 0;
};

// File-backed, one file per key, written via a temporary file and rename.
// On the camera this lives on the jffs2 overlay; rename is atomic there.
class FileStateStore : public IStateStore {
public:
    explicit FileStateStore(std::string dir);

    bool load(const std::string& key, std::string& out) const override;
    bool save(const std::string& key, const std::string& value) override;
    void clear(const std::string& key) override;

private:
    std::string path_for(const std::string& key) const;
    std::string dir_;
};

} // namespace machino
