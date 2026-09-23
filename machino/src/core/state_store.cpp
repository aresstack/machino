#include "core/state_store.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace machino {

FileStateStore::FileStateStore(std::string dir) : dir_(std::move(dir)) {}

std::string FileStateStore::path_for(const std::string& key) const
{
    // Keys are internal constants, never user input, but a key with a slash
    // would silently write outside the directory -- refuse by flattening.
    std::string safe;
    safe.reserve(key.size());
    for (char c : key) safe += (c == '/' || c == '\\' || c == '.') ? '_' : c;
    return dir_ + "/" + safe + ".state";
}

bool FileStateStore::load(const std::string& key, std::string& out) const
{
    std::ifstream f(path_for(key).c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool FileStateStore::save(const std::string& key, const std::string& value)
{
    const std::string final_path = path_for(key);
    const std::string tmp_path = final_path + ".tmp";

    {
        std::ofstream f(tmp_path.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << value;
        f.flush();
        if (!f.good()) return false;
    }
    // rename over the old file: a reader sees one or the other, never a
    // fragment. This is the whole point of the class.
    //
    // On POSIX rename() replaces the target atomically. An earlier version
    // called remove() first "because Windows needs it" -- which opened a
    // window where the file did not exist at all, so a power cut in that
    // instant lost the confirmed configuration. Exactly the failure this
    // class is here to prevent. Try the atomic path first; fall back only
    // where the platform refuses it.
    if (std::rename(tmp_path.c_str(), final_path.c_str()) == 0) return true;

    if (std::remove(final_path.c_str()) != 0) return false;
    return std::rename(tmp_path.c_str(), final_path.c_str()) == 0;
}

void FileStateStore::clear(const std::string& key)
{
    std::remove(path_for(key).c_str());
}

} // namespace machino
