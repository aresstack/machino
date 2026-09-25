#include "core/devices/device_manifest.hpp"

#include <cstdio>

namespace machino { namespace devices {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

} // namespace

DeviceManifest DeviceManifest::load(const std::string& root, const std::string& id) {
    return load_file(root + "/etc/machino/devices/" + id + ".manifest");
}

DeviceManifest DeviceManifest::load_file(const std::string& path) {
    DeviceManifest m;
    FILE* f = ::fopen(path.c_str(), "rb");
    if (!f) return m;

    std::string text;
    char buf[1024];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof buf, f)) > 0) {
        text.append(buf, n);
        if (text.size() >= 64 * 1024) break;   // eine Konfigzeile-Datei, kein Blob
    }
    ::fclose(f);

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = trim(text.substr(pos, nl - pos));
        if (nl == text.size()) pos = nl + 1; else pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if      (key == "id")              m.id = val;
        else if (key == "title")           m.title = val;
        else if (key == "driver")          m.driver = val;
        else if (key == "openipc_profile") m.openipc_profile = val;
        else if (key == "usb_mode")        m.usb_mode = val;
        else if (key == "modules")         m.modules = split_ws(val);
        else if (key == "payload_modules") m.payload_modules = split_ws(val);
        else if (key == "firmware_dir")    m.firmware_dir = val;
        else if (key == "usb_vid")         m.usb_vid = val;
        else if (key == "usb_pid")         m.usb_pid = val;
    }

    // Ein Manifest ohne Id oder ohne Profil beschreibt nichts, was man im
    // Wirtssystem eintragen koennte. Es halb gelesen weiterzureichen hiesse,
    // den Fehler eine Ebene spaeter und unverstaendlicher zu melden.
    m.loaded = !m.id.empty() && !m.openipc_profile.empty();
    return m;
}

}} // namespace machino::devices
