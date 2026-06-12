// Copyright (C) 2026 UBports Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Migrates an ext4 partition to an LVM physical volume in-place.
//
// Layout after migration:
//
//   [ LVM metadata 1MB ][ ext4 data ][ ext4 superblock copy 1MB ][ reserved ]
//
// The logical volume exposes extents in this order:
//   PE(last) — the superblock copy (LV offset 0 = original filesystem start)
//   PE(0..N-2) — the ext4 data
//
// This preserves the original ext4 superblock at LV sector 0 so the
// filesystem can be mounted without modification.

#include "lvm_migrator_utils.h"

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

#include <android-base/file.h>

static bool CopyHeader(const std::string& device_path, uint64_t src_offset,
                       uint64_t dst_offset) {
    std::vector<uint8_t> buf(kLvmExtentSizeBytes);
    int fd = open(device_path.c_str(), O_RDWR);
    if (fd < 0) {
        std::cerr << "Failed to open " << device_path << ": " << strerror(errno) << "\n";
        return false;
    }
    if (pread(fd, buf.data(), buf.size(), src_offset) != (ssize_t)buf.size()) {
        std::cerr << "Failed to read header: " << strerror(errno) << "\n";
        close(fd);
        return false;
    }
    if (pwrite(fd, buf.data(), buf.size(), dst_offset) != (ssize_t)buf.size()) {
        std::cerr << "Failed to write header copy: " << strerror(errno) << "\n";
        close(fd);
        return false;
    }
    fsync(fd);
    close(fd);
    return true;
}

static std::string BuildMetadata(const std::string& vg_name, const std::string& lv_name,
                                  const std::string& device_path, uint64_t total_extents,
                                  uint64_t main_data_extents, time_t creation_time,
                                  const std::string& vg_id, const std::string& pv_id,
                                  const std::string& lv_id) {
    std::string s = BuildLvmHeader(vg_name, vg_id, pv_id, device_path, total_extents,
                                    creation_time);
    s += "    logical_volumes {\n";
    s += "        " + lv_name + " {\n";
    s += "            id = \"" + lv_id + "\"\n";
    s += "            status = [\"READ\", \"WRITE\", \"VISIBLE\"]\n";
    s += "            flags = []\n";
    s += "            creation_time = " + std::to_string(creation_time) + "\n";
    s += "            creation_host = \"localhost\"\n";
    s += "            segment_count = 2\n\n";
    // LV extent 0 maps to the header copy placed at the last physical extent.
    s += "            segment1 {\n";
    s += "                start_extent = 0\n";
    s += "                extent_count = 1\n";
    s += "                type = \"striped\"\n";
    s += "                stripe_count = 1\n";
    s += "                stripes = [\n";
    s += "                    \"pv0\", " + std::to_string(total_extents - 1) + "\n";
    s += "                ]\n";
    s += "            }\n";
    // LV extents 1..N map to the main data starting at physical extent 0.
    s += "            segment2 {\n";
    s += "                start_extent = 1\n";
    s += "                extent_count = " + std::to_string(main_data_extents) + "\n";
    s += "                type = \"striped\"\n";
    s += "                stripe_count = 1\n";
    s += "                stripes = [\n";
    s += "                    \"pv0\", 0\n";
    s += "                ]\n";
    s += "            }\n";
    s += "        }\n";
    s += "    }\n";
    s += "}\n";
    return s;
}

int main(int argc, char* argv[]) {
    std::string device_path, vg_name, lv_name;
    uint64_t reserved_size_mb = 0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "Usage: lvm-fs-migrator --device=<dev> --vg-name=<name>"
                         " --lv-name=<name> [--reserve=<mb>]\n";
            return 0;
        } else if (arg.rfind("--device=", 0) == 0) {
            device_path = arg.substr(9);
        } else if (arg.rfind("--vg-name=", 0) == 0) {
            vg_name = arg.substr(10);
        } else if (arg.rfind("--lv-name=", 0) == 0) {
            lv_name = arg.substr(10);
        } else if (arg.rfind("--reserve=", 0) == 0) {
            const std::string val = arg.substr(10);
            if (val.empty() || val.find_first_not_of("0123456789") != std::string::npos) {
                std::cerr << "Invalid --reserve value: " << val << "\n";
                return 1;
            }
            reserved_size_mb = strtoull(val.c_str(), nullptr, 10);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    if (device_path.empty() || vg_name.empty() || lv_name.empty()) {
        std::cerr << "Error: --device, --vg-name, and --lv-name are required\n";
        return 1;
    }

    if (VolumeGroupExists(vg_name)) {
        std::cerr << "Error: volume group '" << vg_name << "' already exists\n";
        return 1;
    }

    uint64_t device_size = GetDeviceSize(device_path);
    if (device_size == 0) {
        std::cerr << "Failed to get device size for " << device_path << "\n";
        return 1;
    }
    std::cout << "Device size: " << device_size << " bytes ("
              << device_size / kLvmExtentSizeBytes << " MB)\n";

    // All sizes rounded down to 1MB (one LVM extent) boundaries.
    uint64_t reserved_bytes = reserved_size_mb * kLvmExtentSizeBytes;
    uint64_t filesystem_size_mb =
            (device_size - kLvmExtentSizeBytes - reserved_bytes) / kLvmExtentSizeBytes;
    uint64_t filesystem_size = filesystem_size_mb * kLvmExtentSizeBytes;

    // total_extents = device area after the 1MB LVM metadata region.
    uint64_t total_extents = (device_size - kLvmExtentSizeBytes) / kLvmExtentSizeBytes;
    // 1 extent reserved for header copy at the end.
    uint64_t main_data_extents = total_extents - 1 - reserved_size_mb;

    std::cout << "Checking filesystem on " << device_path << "...\n";
    RunCommand({"e2fsck", "-f", "-y", device_path});

    // Workaround for resize2fs in case of clock inconsistency
    FixupFsCheckTime(device_path);

    std::cout << "Resizing filesystem to " << filesystem_size_mb << " MB...\n";
    if (RunCommand({"resize2fs", device_path,
                    std::to_string(filesystem_size_mb) + "M"}) != 0) {
        std::cerr << "resize2fs failed\n";
        return 1;
    }

    // Copy the 1MB superblock to the end of the filesystem area so LVM can
    // use the start of the device for its own metadata
    std::cout << "Copying 1MB header to offset " << filesystem_size << "...\n";
    if (!CopyHeader(device_path, 0, filesystem_size)) return 1;

    std::string vg_id = GenerateId();
    std::string pv_id = GenerateId();
    std::string lv_id = GenerateId();
    time_t creation_time = time(nullptr);

    std::string metadata = BuildMetadata(vg_name, lv_name, device_path, total_extents,
                                          main_data_extents, creation_time,
                                          vg_id, pv_id, lv_id);

    if (!android::base::WriteStringToFile(metadata, kMetadataPath)) {
        std::cerr << "Failed to write metadata file\n";
        return 1;
    }

    std::cout << "Creating LVM volume group '" << vg_name << "'...\n";
    bool ok = CreateAndActivateLvm(device_path, vg_name, pv_id, kMetadataPath);
    unlink(kMetadataPath);

    if (!ok) return 1;

    std::cout << "Migration successful: " << vg_name << "/" << lv_name << "\n";
    return 0;
}
