// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/vfs.cpp
#include "lv2/vfs.h"

#include <algorithm>
#include <cctype>

namespace cell::lv2 {

bool Vfs::IsReadOnlyDevice(std::string_view prefix) noexcept { return prefix == "/dev_bdvd" || prefix == "/dev_flash"; }

void Vfs::Mount(std::string guest_prefix, std::filesystem::path host_dir, bool read_only) {
    while (guest_prefix.size() > 1 && guest_prefix.back() == '/') guest_prefix.pop_back();
    std::lock_guard lock(mutex_);
    for (auto it = mounts_.begin(); it != mounts_.end();) {
        if ((*it)->prefix == guest_prefix) {
            retired_.push_back(std::move(*it));  // a resolved VfsResolved::mount may still point at it
            it = mounts_.erase(it);
        } else {
            ++it;
        }
    }
    mounts_.push_back(std::make_unique<VfsMount>(VfsMount{std::move(guest_prefix), std::move(host_dir), read_only}));
}

bool Vfs::IsMounted(std::string_view guest_prefix) const {
    std::lock_guard lock(mutex_);
    return std::any_of(mounts_.begin(), mounts_.end(), [&](const auto& m) { return m->prefix == guest_prefix; });
}

std::optional<VfsResolved> Vfs::ResolveDetailed(std::string_view guest_path) const {
    std::lock_guard lock(mutex_);
    const VfsMount* best = nullptr;
    for (const auto& m : mounts_) {
        const std::string& prefix = m->prefix;
        if (guest_path.size() < prefix.size() || guest_path.compare(0, prefix.size(), prefix) != 0) continue;
        if (guest_path.size() > prefix.size() && guest_path[prefix.size()] != '/') continue;  // "/dev_flash2"
        if (!best || prefix.size() > best->prefix.size()) best = m.get();
    }
    if (!best) return std::nullopt;
    VfsResolved out;
    out.mount = best;
    out.host = best->host;
    std::string_view rest = guest_path.substr(best->prefix.size());
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.find('\\') != std::string_view::npos || part.find(':') != std::string_view::npos)
            return std::nullopt;
        out.host /= std::filesystem::path(std::string(part));
        if (!out.relative.empty()) out.relative += '/';
        out.relative += part;
    }
    return out;
}

std::optional<std::filesystem::path> Vfs::Resolve(std::string_view guest_path) const {
    auto r = ResolveDetailed(guest_path);
    if (!r) return std::nullopt;
    return std::move(r->host);
}

std::optional<VfsResolved> Vfs::ResolveExisting(std::string_view guest_path) const {
    auto r = ResolveDetailed(guest_path);
    if (!r) return r;
#ifndef _WIN32
    std::error_code ec;
    if (std::filesystem::exists(r->host, ec) || r->relative.empty()) return r;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::filesystem::path at = r->mount->host;
    std::string_view rest = r->relative;
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string part(rest.substr(0, slash));
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        std::filesystem::path next = at / part;
        if (!std::filesystem::exists(next, ec)) {
            const std::string want = lower(part);
            bool found = false;
            for (std::filesystem::directory_iterator it(at, ec), end; !ec && it != end; it.increment(ec))
                if (lower(it->path().filename().string()) == want) {
                    next = it->path();
                    found = true;
                    break;
                }
            if (!found) return r;  // genuinely missing: keep the exact spelling (for O_CREAT)
        }
        at = std::move(next);
    }
    r->host = std::move(at);
#endif
    return r;
}

std::vector<VfsMount> Vfs::Mounts() const {
    std::lock_guard lock(mutex_);
    std::vector<VfsMount> out;
    for (const auto& m : mounts_) out.push_back(*m);
    return out;
}

}  // namespace cell::lv2
