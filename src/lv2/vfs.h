// SPDX-License-Identifier: GPL-2.0-or-later
// gt5_cellos — lv2/vfs.h
//
// Guest path -> host path. lv2 exposes devices as path prefixes (/dev_flash, /dev_bdvd,
// /dev_hdd0, /dev_hdd1, /app_home, /host_root); the user points each at a folder of their dump:
//   --dev-flash  <dir>   firmware files (dev_flash/sys/external/libsre.sprx, decrypted)
//   --dev-bdvd   <dir>   the disc root (contains PS3_GAME/); detected automatically when the
//                        EBOOT sits at <root>/PS3_GAME/USRDIR/EBOOT.*
//   --dev-hdd0   <dir>   game data / saves
//   --dev-hdd1   <dir>   system cache (cellSysCacheMount)
//   --app-home   <dir>   /app_home (defaults to the folder of the EBOOT)
// Resolution takes the longest mounted prefix at a path-component boundary and refuses ".."
// so a guest path can never reach outside its mount. Read-only devices (/dev_bdvd,
// /dev_flash) are flagged so the file system answers EROFS to writes, as on the console.
//
// Case: PS3 paths are case-sensitive and games use the exact names from the disc, but disc
// dumps are not always consistent (ISO extractors upper-case names). ResolveExisting() falls
// back to a case-insensitive match per component on hosts whose file system is
// case-sensitive (Linux); on Windows the host already ignores case.
#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cell::lv2 {

struct VfsMount {
    std::string prefix;  // "/dev_bdvd"
    std::filesystem::path host;
    bool read_only = false;
};

struct VfsResolved {
    std::filesystem::path host;  // full host path
    const VfsMount* mount = nullptr;  // valid while the mount table is unchanged (mounts are set at boot)
    std::string relative;  // guest path below the mount point, '/'-separated, no leading slash
};

class Vfs {
public:
    // `guest_prefix` like "/dev_flash" (no trailing slash). Re-mounting a prefix replaces it
    // (the old entry stays allocated, so resolved `mount` pointers never dangle).
    void Mount(std::string guest_prefix, std::filesystem::path host_dir, bool read_only = false);
    [[nodiscard]] bool IsMounted(std::string_view guest_prefix) const;
    [[nodiscard]] std::optional<std::filesystem::path> Resolve(std::string_view guest_path) const;
    [[nodiscard]] std::optional<VfsResolved> ResolveDetailed(std::string_view guest_path) const;
    // Like ResolveDetailed, then, if the host path does not exist, retries each component
    // case-insensitively (non-Windows hosts). Returns the exact path when nothing matches,
    // so callers can create new files there.
    [[nodiscard]] std::optional<VfsResolved> ResolveExisting(std::string_view guest_path) const;
    [[nodiscard]] std::vector<VfsMount> Mounts() const;

    // Default read-only flag for a device prefix (/dev_bdvd, /dev_flash, /dev_bdvd...).
    [[nodiscard]] static bool IsReadOnlyDevice(std::string_view prefix) noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<VfsMount>> mounts_;   // stable addresses for VfsResolved::mount
    std::vector<std::unique_ptr<VfsMount>> retired_;  // replaced mounts, kept alive for the same reason
};

}  // namespace cell::lv2
