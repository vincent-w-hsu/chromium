// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <memory>

#include "base/debug/elf_reader.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/format_macros.h"
#include "base/process/process_metrics.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "services/resource_coordinator/public/cpp/memory_instrumentation/os_metrics.h"

// Symbol with virtual address of the start of ELF header of the current binary.
extern char __ehdr_start;

namespace memory_instrumentation {

namespace {

using mojom::VmRegion;
using mojom::VmRegionPtr;

const uint32_t kMaxLineSize = 4096;

base::ScopedFD OpenStatm(base::ProcessId pid) {
  std::string name =
      "/proc/" +
      (pid == base::kNullProcessId ? "self" : base::NumberToString(pid)) +
      "/statm";
  base::ScopedFD fd = base::ScopedFD(open(name.c_str(), O_RDONLY));
  return fd;
}

bool GetResidentAndSharedPagesFromStatmFile(int fd,
                                            uint64_t* resident_pages,
                                            uint64_t* shared_pages) {
  lseek(fd, 0, SEEK_SET);
  char line[kMaxLineSize];
  int res = read(fd, line, kMaxLineSize - 1);
  if (res <= 0)
    return false;
  line[res] = '\0';
  int num_scanned =
      sscanf(line, "%*s %" SCNu64 " %" SCNu64, resident_pages, shared_pages);
  return num_scanned == 2;
}

std::unique_ptr<base::ProcessMetrics> CreateProcessMetrics(
    base::ProcessId pid) {
  if (pid == base::kNullProcessId) {
    return base::ProcessMetrics::CreateCurrentProcessMetrics();
  }
  return base::ProcessMetrics::CreateProcessMetrics(pid);
}

struct ModuleData {
  std::string path;
  std::string build_id;
};

ModuleData GetMainModuleData() {
  ModuleData module_data;
  Dl_info dl_info;
  if (dladdr(&__ehdr_start, &dl_info)) {
    base::debug::ElfBuildIdBuffer build_id;
    size_t build_id_length =
        base::debug::ReadElfBuildId(&__ehdr_start, true, build_id);
    if (build_id_length) {
      module_data.path = dl_info.dli_fname;
      module_data.build_id = std::string(build_id, build_id_length);
    }
  }
  return module_data;
}

bool ParseSmapsHeader(const char* header_line,
                      const ModuleData& main_module_data,
                      VmRegion* region) {
  // e.g., "00400000-00421000 r-xp 00000000 fc:01 1234  /foo.so\n"
  bool res = true;  // Whether this region should be appended or skipped.
  uint64_t end_addr = 0;
  char protection_flags[5] = {0};
  char mapped_file[kMaxLineSize];

  if (sscanf(header_line, "%" SCNx64 "-%" SCNx64 " %4c %*s %*s %*s%4095[^\n]\n",
             &region->start_address, &end_addr, protection_flags,
             mapped_file) != 4) {
    return false;
  }

  if (end_addr > region->start_address) {
    region->size_in_bytes = end_addr - region->start_address;
  } else {
    // This is not just paranoia, it can actually happen (See crbug.com/461237).
    region->size_in_bytes = 0;
    res = false;
  }

  region->protection_flags = 0;
  if (protection_flags[0] == 'r') {
    region->protection_flags |= VmRegion::kProtectionFlagsRead;
  }
  if (protection_flags[1] == 'w') {
    region->protection_flags |= VmRegion::kProtectionFlagsWrite;
  }
  if (protection_flags[2] == 'x') {
    region->protection_flags |= VmRegion::kProtectionFlagsExec;
  }
  if (protection_flags[3] == 's') {
    region->protection_flags |= VmRegion::kProtectionFlagsMayshare;
  }

  region->mapped_file = mapped_file;
  base::TrimWhitespaceASCII(region->mapped_file, base::TRIM_ALL,
                            &region->mapped_file);

  // Build ID is needed to symbolize heap profiles, and is generated only on
  // official builds. Build ID is only added for the current library (chrome)
  // since it is racy to read other libraries which can be unmapped any time.
#if defined(OFFICIAL_BUILD)
  if (!region->mapped_file.empty() &&
      base::StartsWith(main_module_data.path, region->mapped_file,
                       base::CompareCase::SENSITIVE) &&
      !main_module_data.build_id.empty()) {
    region->module_debugid = main_module_data.build_id;
  }
#endif  // defined(OFFICIAL_BUILD)

  return res;
}

uint64_t ReadCounterBytes(char* counter_line) {
  uint64_t counter_value = 0;
  int res = sscanf(counter_line, "%*s %" SCNu64 " kB", &counter_value);
  return res == 1 ? counter_value * 1024 : 0;
}

uint32_t ParseSmapsCounter(char* counter_line, VmRegion* region) {
  // A smaps counter lines looks as follows: "RSS:  0 Kb\n"
  uint32_t res = 1;
  char counter_name[20];
  int did_read = sscanf(counter_line, "%19[^\n ]", counter_name);
  if (did_read != 1)
    return 0;

  if (strcmp(counter_name, "Pss:") == 0) {
    region->byte_stats_proportional_resident = ReadCounterBytes(counter_line);
  } else if (strcmp(counter_name, "Private_Dirty:") == 0) {
    region->byte_stats_private_dirty_resident = ReadCounterBytes(counter_line);
  } else if (strcmp(counter_name, "Private_Clean:") == 0) {
    region->byte_stats_private_clean_resident = ReadCounterBytes(counter_line);
  } else if (strcmp(counter_name, "Shared_Dirty:") == 0) {
    region->byte_stats_shared_dirty_resident = ReadCounterBytes(counter_line);
  } else if (strcmp(counter_name, "Shared_Clean:") == 0) {
    region->byte_stats_shared_clean_resident = ReadCounterBytes(counter_line);
  } else if (strcmp(counter_name, "Swap:") == 0) {
    region->byte_stats_swapped = ReadCounterBytes(counter_line);
  } else {
    res = 0;
  }

  return res;
}

uint32_t ReadLinuxProcSmapsFile(FILE* smaps_file,
                                std::vector<VmRegionPtr>* maps) {
  if (!smaps_file)
    return 0;

  fseek(smaps_file, 0, SEEK_SET);

  char line[kMaxLineSize];
  const uint32_t kNumExpectedCountersPerRegion = 6;
  uint32_t counters_parsed_for_current_region = 0;
  uint32_t num_valid_regions = 0;
  bool should_add_current_region = false;
  VmRegion region;
  ModuleData main_module_data = GetMainModuleData();
  for (;;) {
    line[0] = '\0';
    if (fgets(line, kMaxLineSize, smaps_file) == nullptr || !strlen(line))
      break;
    if (isxdigit(line[0]) && !isupper(line[0])) {
      region = VmRegion();
      counters_parsed_for_current_region = 0;
      should_add_current_region =
          ParseSmapsHeader(line, main_module_data, &region);
    } else if (should_add_current_region) {
      counters_parsed_for_current_region += ParseSmapsCounter(line, &region);
      DCHECK_LE(counters_parsed_for_current_region,
                kNumExpectedCountersPerRegion);
      if (counters_parsed_for_current_region == kNumExpectedCountersPerRegion) {
        maps->push_back(VmRegion::New(region));
        ++num_valid_regions;
        should_add_current_region = false;
      }
    }
  }
  return num_valid_regions;
}

}  // namespace

FILE* g_proc_smaps_for_testing = nullptr;

// static
void OSMetrics::SetProcSmapsForTesting(FILE* f) {
  g_proc_smaps_for_testing = f;
}

// static
bool OSMetrics::FillOSMemoryDump(base::ProcessId pid,
                                 mojom::RawOSMemDump* dump) {
  base::ScopedFD autoclose = OpenStatm(pid);
  int statm_fd = autoclose.get();

  if (statm_fd == -1)
    return false;

  uint64_t resident_pages;
  uint64_t shared_pages;
  bool success = GetResidentAndSharedPagesFromStatmFile(
      statm_fd, &resident_pages, &shared_pages);

  if (!success)
    return false;

  auto process_metrics = CreateProcessMetrics(pid);

  const static size_t page_size = base::GetPageSize();
  uint64_t rss_anon_bytes = (resident_pages - shared_pages) * page_size;
  uint64_t vm_swap_bytes = process_metrics->GetVmSwapBytes();

  dump->platform_private_footprint->rss_anon_bytes = rss_anon_bytes;
  dump->platform_private_footprint->vm_swap_bytes = vm_swap_bytes;
  dump->resident_set_kb = process_metrics->GetResidentSetSize() / 1024;

  return true;
}

// static
std::vector<VmRegionPtr> OSMetrics::GetProcessMemoryMaps(base::ProcessId pid) {
  std::vector<VmRegionPtr> maps;
  uint32_t res = 0;
  if (g_proc_smaps_for_testing) {
    res = ReadLinuxProcSmapsFile(g_proc_smaps_for_testing, &maps);
  } else {
    std::string file_name =
        "/proc/" +
        (pid == base::kNullProcessId ? "self" : base::NumberToString(pid)) +
        "/smaps";
    base::ScopedFILE smaps_file(fopen(file_name.c_str(), "r"));
    res = ReadLinuxProcSmapsFile(smaps_file.get(), &maps);
  }

  if (!res)
    return std::vector<VmRegionPtr>();

  return maps;
}

}  // namespace memory_instrumentation
