#include "core/devices/device_package.hpp"

namespace machino { namespace devices {

const char* install_state_name(InstallState s) {
    switch (s) {
        case InstallState::Unsupported:  return "unsupported";
        case InstallState::Unavailable:  return "unavailable";
        case InstallState::NotInstalled:  return "not-installed";
        case InstallState::InstallPending:return "install-pending";
        case InstallState::Installed:     return "installed";
        case InstallState::RemovePending: return "remove-pending";
    }
    return "unsupported";
}

void DeviceManager::add(IDevicePackage* p) {
    if (p) packages_.push_back(p);
}

std::vector<DeviceStatus> DeviceManager::list() const {
    std::vector<DeviceStatus> out;
    out.reserve(packages_.size());
    for (IDevicePackage* p : packages_) out.push_back(p->status());
    return out;
}

IDevicePackage* DeviceManager::find(const std::string& id) const {
    for (IDevicePackage* p : packages_)
        if (p->id() == id) return p;
    return nullptr;
}

IDevicePackage* DeviceManager::active() const {
    // Ein Port, also hoechstens einer. Der erste, der sich meldet, gewinnt --
    // melden sich zwei, ist das ein Fehler weiter unten (usb.mode kennt nur
    // einen Wert) und nicht hier zu heilen.
    for (IDevicePackage* p : packages_)
        if (p->is_active()) return p;
    return nullptr;
}

}} // namespace machino::devices
