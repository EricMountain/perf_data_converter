// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "perf_parser.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>

#include <memory>
#include <set>
#include <sstream>

#include "base/logging.h"

#include "address_mapper.h"
#include "binary_data_utils.h"
#include "compat/proto.h"
#include "compat/string.h"
#include "dso.h"
#include "huge_page_deducer.h"

namespace quipper {

using BranchStackEntry = PerfDataProto_BranchStackEntry;
using CommEvent = PerfDataProto_CommEvent;
using ForkEvent = PerfDataProto_ForkEvent;
using MMapEvent = PerfDataProto_MMapEvent;
using SampleEvent = PerfDataProto_SampleEvent;

namespace {

// MMAPs are aligned to pages of this many bytes.
const uint64_t kMmapPageAlignment = sysconf(_SC_PAGESIZE);

// Name and ID of the kernel swapper process.
const char kSwapperCommandName[] = "swapper";
const uint32_t kSwapperPid = 0;

// Returns the offset within a page of size |kMmapPageAlignment|, given an
// address. Requires that |kMmapPageAlignment| be a power of 2.
uint64_t GetPageAlignedOffset(uint64_t addr) {
  return addr % kMmapPageAlignment;
}

bool IsNullBranchStackEntry(const BranchStackEntry& entry) {
  return (!entry.from_ip() && !entry.to_ip());
}

}  // namespace

PerfParser::PerfParser(PerfReader* reader) : reader_(reader) {}

PerfParser::~PerfParser() {}

PerfParser::PerfParser(PerfReader* reader, const PerfParserOptions& options)
    : reader_(reader), options_(options) {}

bool PerfParser::ParseRawEvents() {
  if (options_.sort_events_by_time) {
    reader_->MaybeSortEventsByTime();
  }

  // Just in case there was data from a previous call.
  process_mappers_.clear();

  // Find huge page mappings.
  if (options_.deduce_huge_page_mappings) {
      DeduceHugePages(reader_->mutable_events());
  }

  // Combine split mappings.
  if (options_.combine_mappings) {
    CombineMappings(reader_->mutable_events());
  }

  // Clear the parsed events to reset their fields. Otherwise, non-sample events
  // may have residual DSO+offset info.
  parsed_events_.clear();

  // Events of type PERF_RECORD_FINISHED_ROUND don't have a timestamp, and are
  // not needed.
  // use the partial-sorting of events between rounds to sort faster.
  parsed_events_.resize(reader_->events().size());
  size_t write_index = 0;
  for (int i = 0; i < reader_->events().size(); ++i) {
    if (reader_->events().Get(i).header().type() == PERF_RECORD_FINISHED_ROUND)
      continue;
    parsed_events_[write_index++].event_ptr =
        reader_->mutable_events()->Mutable(i);
  }
  parsed_events_.resize(write_index);

  ProcessEvents();

  if (!options_.discard_unused_events) return true;

  // Some MMAP/MMAP2 events' mapped regions will not have any samples. These
  // MMAP/MMAP2 events should be dropped. |parsed_events_| should be
  // reconstructed without these events.
  write_index = 0;
  size_t read_index;
  for (read_index = 0; read_index < parsed_events_.size(); ++read_index) {
    const ParsedEvent& event = parsed_events_[read_index];
    if (event.event_ptr->has_mmap_event() &&
        event.num_samples_in_mmap_region == 0) {
      continue;
    }
    if (read_index != write_index) parsed_events_[write_index] = event;
    ++write_index;
  }
  CHECK_LE(write_index, parsed_events_.size());
  parsed_events_.resize(write_index);

  // Update the events in |reader_| to match the updated events.
  UpdatePerfEventsFromParsedEvents();

  return true;
}

bool PerfParser::ProcessUserEvents(PerfEvent& event) {
  // New user events from PERF-4.13 is not yet supported
  switch (event.header().type()) {
    case PERF_RECORD_AUXTRACE_INFO:
    case PERF_RECORD_AUXTRACE:
    case PERF_RECORD_AUXTRACE_ERROR:
    case PERF_RECORD_THREAD_MAP:
    case PERF_RECORD_STAT_CONFIG:
    case PERF_RECORD_STAT:
    case PERF_RECORD_STAT_ROUND:
    case PERF_RECORD_TIME_CONV:
      VLOG(1) << "Parsed event: " << GetEventName(event.header().type())
              << ". Doing nothing.";
      break;
    default:
      VLOG(1) << "Unsupported event: " << GetEventName(event.header().type());
      break;
  }
  return true;
}

bool PerfParser::ProcessEvents() {
  stats_ = {0};

  stats_.did_remap = false;  // Explicitly clear the remap flag.

  // Pid 0 is called the swapper process. Even though perf does not record a
  // COMM event for pid 0, we act like we did receive a COMM event for it. Perf
  // does this itself, example:
  //   http://lxr.free-electrons.com/source/tools/perf/util/session.c#L1120
  commands_.insert(kSwapperCommandName);
  pidtid_to_comm_map_[std::make_pair(kSwapperPid, kSwapperPid)] =
      &(*commands_.find(kSwapperCommandName));

  // Keep track of the first MMAP or MMAP2 event associated with the kernel.
  // First such mapping corresponds to the kernel image, and requires special
  // handling. It's possible for a perf.data file not to include kernel mappings
  // if the user didn't have permissions to profile the kernel, see b/197005460,
  // and it's possible for some user mappings to come before the kernel mapping,
  // see b/137139473..
  bool first_kernel_mmap = true;

  // NB: Not necessarily actually sorted by time.
  for (size_t i = 0; i < parsed_events_.size(); ++i) {
    ParsedEvent& parsed_event = parsed_events_[i];
    PerfEvent& event = *parsed_event.event_ptr;

    // Process user events
    if (event.header().type() >= PERF_RECORD_USER_TYPE_START) {
      if (!ProcessUserEvents(event)) {
        return false;
      }
      continue;
    }

    switch (event.header().type()) {
      case PERF_RECORD_SAMPLE:
        // SAMPLE doesn't have any fields to log at a fixed,
        // previously-endian-swapped location. This used to log ip.
        VLOG(1) << "SAMPLE";
        ++stats_.num_sample_events;
        MapSampleEvent(&parsed_event);
        break;
      case PERF_RECORD_MMAP:
      case PERF_RECORD_MMAP2: {
        const char* mmap_type_name =
            event.header().type() == PERF_RECORD_MMAP ? "MMAP" : "MMAP2";
        VLOG(1) << mmap_type_name << ": " << event.mmap_event().filename();
        ++stats_.num_mmap_events;
        bool is_kernel =
            first_kernel_mmap &&
            (event.header().misc() & quipper::PERF_RECORD_MISC_CPUMODE_MASK) ==
                quipper::PERF_RECORD_MISC_KERNEL;
        // Use the array index of the current mmap event as a unique identifier.
        CHECK(MapMmapEvent(event.mutable_mmap_event(), i, is_kernel))
            << "Unable to map " << mmap_type_name << " event!";
        // No samples in this MMAP region yet, hopefully.
        parsed_event.num_samples_in_mmap_region = 0;
        DSOInfo dso_info;
        dso_info.name = event.mmap_event().filename();
        if (event.header().type() == PERF_RECORD_MMAP2) {
          dso_info.maj = event.mmap_event().maj();
          dso_info.min = event.mmap_event().min();
          dso_info.ino = event.mmap_event().ino();
        }
        name_to_dso_.emplace(dso_info.name, dso_info);
        if (is_kernel) {
          first_kernel_mmap = false;
        }
        break;
      }
      case PERF_RECORD_FORK:
        // clang-format off
        VLOG(1) << "FORK: " << event.fork_event().ppid()
                << ":" << event.fork_event().ptid()
                << " -> " << event.fork_event().pid()
                << ":" << event.fork_event().tid();
        // clang-format on
        ++stats_.num_fork_events;
        CHECK(MapForkEvent(event.fork_event())) << "Unable to map FORK event!";
        break;
      case PERF_RECORD_EXIT:
        // EXIT events have the same structure as FORK events.
        // clang-format off
        VLOG(1) << "EXIT: " << event.fork_event().ppid()
                << ":" << event.fork_event().ptid();
        // clang-format on
        ++stats_.num_exit_events;
        break;
      case PERF_RECORD_COMM:
      {
        // clang-format off
        VLOG(1) << "COMM: " << event.comm_event().pid()
                << ":" << event.comm_event().tid() << ": "
                << event.comm_event().comm();
        // clang-format on
        ++stats_.num_comm_events;
        CHECK(MapCommEvent(event.comm_event()));
        commands_.insert(event.comm_event().comm());
        const PidTid pidtid =
            std::make_pair(event.comm_event().pid(), event.comm_event().tid());
        pidtid_to_comm_map_[pidtid] =
            &(*commands_.find(event.comm_event().comm()));
        break;
      }
      case PERF_RECORD_LOST:
      case PERF_RECORD_THROTTLE:
      case PERF_RECORD_UNTHROTTLE:
      case PERF_RECORD_AUX:
      case PERF_RECORD_ITRACE_START:
      case PERF_RECORD_LOST_SAMPLES:
      case PERF_RECORD_SWITCH:
      case PERF_RECORD_SWITCH_CPU_WIDE:
      case PERF_RECORD_NAMESPACES:
      case PERF_RECORD_CGROUP:
        VLOG(1) << "Parsed event type: " << GetEventName(event.header().type())
                << ". Doing nothing.";
        break;
      default:
        LOG(ERROR) << "Unknown event type: "
                   << GetEventName(event.header().type());
        return false;
    }
  }
  if (!FillInDsoBuildIds()) return false;

  // Print stats collected from parsing.
  // clang-format off
  LOG(INFO) << "Parser processed: "
            << stats_.num_mmap_events << " MMAP/MMAP2 events, "
            << stats_.num_comm_events << " COMM events, "
            << stats_.num_fork_events << " FORK events, "
            << stats_.num_exit_events << " EXIT events, "
            << stats_.num_sample_events << " SAMPLE events, "
            << stats_.num_sample_events_mapped << " of these were mapped, "
            << stats_.num_data_sample_events
            << " SAMPLE events with a data address, "
            << stats_.num_data_sample_events_mapped << " of these were mapped";
  // clang-format on

  if (stats_.num_sample_events == 0) {
    if (reader_->event_types_to_skip_when_serializing().find(
            PERF_RECORD_SAMPLE) !=
        reader_->event_types_to_skip_when_serializing().end()) {
      LOG(INFO) << "Input perf.data has no sample events due to "
                   "PERF_RECORD_SAMPLE being skipped.";
    } else {
      LOG(ERROR) << "Input perf.data has no sample events.";
    }
    return false;
  }

  float sample_mapping_percentage =
      static_cast<float>(stats_.num_sample_events_mapped) /
      stats_.num_sample_events * 100.;
  float threshold = options_.sample_mapping_percentage_threshold;
  if (sample_mapping_percentage < threshold) {
    LOG(ERROR) << "Only " << static_cast<int>(sample_mapping_percentage)
               << "% of samples had all locations mapped to a module, expected "
               << "at least " << static_cast<int>(threshold) << "%";
    return false;
  }
  stats_.did_remap = options_.do_remap;
  return true;
}

namespace {

class FdCloser {
 public:
  explicit FdCloser(int fd) : fd_(fd) {}
  ~FdCloser() {
    if (fd_ != -1) close(fd_);
  }

 private:
  FdCloser() = delete;
  FdCloser(FdCloser&) = delete;

  int fd_;
};

// Merges two uint32_t into a uint64_t for hashing in an unordered_set because
// there is no default hash method for a pair.
uint64_t mergeTwoU32(uint32_t first, uint32_t second) {
  return (uint64_t)first << 32 | second;
}

// Splits a given uint64_t into two uint32_t. This reverts the above merge
// operation to retrieve the two uint32_t from an unordered_set.
std::pair<uint32_t, uint32_t> splitU64(uint64_t value) {
  return std::make_pair(value >> 32,
                        std::numeric_limits<uint32_t>::max() & value);
}

bool ReadElfBuildIdIfSameInode(const string& dso_path, const DSOInfo& dso,
                               string* buildid) {
  int fd = open(dso_path.c_str(), O_RDONLY);
  FdCloser fd_closer(fd);
  if (fd == -1) {
    if (errno != ENOENT) LOG(ERROR) << "Failed to open ELF file: " << dso_path;
    return false;
  }

  struct stat s;
  CHECK_GE(fstat(fd, &s), 0);
  // Only reject based on inode if we actually have device info (from MMAP2).
  if (dso.maj != 0 && dso.min != 0 && !SameInode(dso, &s)) return false;

  return ReadElfBuildId(fd, buildid);
}

// Looks up build ID of a given DSO by reading directly from the file system.
// - Does not support reading build ID of the main kernel binary.
// - Reads build IDs of kernel modules and other DSOs using functions in dso.h.
string FindDsoBuildId(const DSOInfo& dso_info) {
  string buildid_bin;
  const string& dso_name = dso_info.name;
  if (IsKernelNonModuleName(dso_name)) return buildid_bin;  // still empty
  // Does this look like a kernel module?
  if (dso_name.size() >= 2 && dso_name[0] == '[' && dso_name.back() == ']') {
    // This may not be successful, but either way, just return. buildid_bin
    // will be empty if the module was not found.
    ReadModuleBuildId(dso_name.substr(1, dso_name.size() - 2), &buildid_bin);
    return buildid_bin;
  }
  // Try normal files, possibly inside containers.
  u32 last_pid = 0;
  std::vector<uint64_t> threads(dso_info.threads.begin(),
                                dso_info.threads.end());
  std::sort(threads.begin(), threads.end());
  for (auto pidtid_it : threads) {
    uint32_t pid, tid;
    std::tie(pid, tid) = splitU64(pidtid_it);
    std::stringstream dso_path_stream;
    dso_path_stream << "/proc/" << tid << "/root/" << dso_name;
    string dso_path = dso_path_stream.str();
    if (ReadElfBuildIdIfSameInode(dso_path, dso_info, &buildid_bin)) {
      return buildid_bin;
    }
    // Avoid re-trying the parent process if it's the same for multiple threads.
    // dso_info.threads is sorted, so threads in a process should be adjacent.
    if (pid == last_pid || pid == tid) continue;
    last_pid = pid;
    // Try the parent process:
    std::stringstream parent_dso_path_stream;
    parent_dso_path_stream << "/proc/" << pid << "/root/" << dso_name;
    string parent_dso_path = parent_dso_path_stream.str();
    if (ReadElfBuildIdIfSameInode(parent_dso_path, dso_info, &buildid_bin)) {
      return buildid_bin;
    }
  }
  // Still don't have a buildid. Try our own filesystem:
  if (ReadElfBuildIdIfSameInode(dso_name, dso_info, &buildid_bin)) {
    return buildid_bin;
  }
  return buildid_bin;  // still empty.
}

}  // namespace

bool PerfParser::FillInDsoBuildIds() {
  std::map<string, string> filenames_to_build_ids;
  reader_->GetFilenamesToBuildIDs(&filenames_to_build_ids);

  std::map<string, string> new_buildids;

  for (std::pair<const string, DSOInfo>& kv : name_to_dso_) {
    DSOInfo& dso_info = kv.second;
    const auto it = filenames_to_build_ids.find(dso_info.name);
    if (it != filenames_to_build_ids.end()) {
      dso_info.build_id = it->second;
    }
    // If there is both an existing build ID and a new build ID returned by
    // FindDsoBuildId(), overwrite the existing build ID.
    if (options_.read_missing_buildids && dso_info.hit) {
      string buildid_bin = FindDsoBuildId(dso_info);
      if (!buildid_bin.empty()) {
        dso_info.build_id = RawDataToHexString(buildid_bin);
        new_buildids[dso_info.name] = dso_info.build_id;
      }
    }
  }

  if (new_buildids.empty()) return true;
  return reader_->InjectBuildIDs(new_buildids);
}

void PerfParser::UpdatePerfEventsFromParsedEvents() {
  // Reorder the events in |reader_| to match the order of |parsed_events_|.
  // The |event_ptr|'s in |parsed_events_| are pointers to existing events in
  // |reader_|.
  RepeatedPtrField<PerfEvent> new_events;
  new_events.Reserve(parsed_events_.size());
  for (ParsedEvent& parsed_event : parsed_events_) {
    PerfEvent* new_event = new_events.Add();
    new_event->Swap(parsed_event.event_ptr);
    parsed_event.event_ptr = new_event;
  }

  reader_->mutable_events()->Swap(&new_events);
}

void PerfParser::MapSampleEvent(ParsedEvent* parsed_event) {
  const PerfEvent& event = *parsed_event->event_ptr;
  if (!event.has_sample_event()) return;

  SampleEvent& sample_info = *parsed_event->event_ptr->mutable_sample_event();

  // Find the associated command.
  PidTid pidtid = std::make_pair(sample_info.pid(), sample_info.tid());
  const auto comm_iter = pidtid_to_comm_map_.find(pidtid);
  if (comm_iter != pidtid_to_comm_map_.end())
    parsed_event->set_command(comm_iter->second);

  const uint64_t unmapped_event_ip = sample_info.ip();
  uint64_t remapped_event_ip = 0;

  bool mapping_ok = true;
  // Map the event IP itself.
  if (!MapIPAndPidAndGetNameAndOffset(sample_info.ip(), pidtid,
                                      &remapped_event_ip,
                                      &parsed_event->dso_and_offset)) {
    mapping_ok = false;
  } else {
    sample_info.set_ip(remapped_event_ip);
  }

  if (sample_info.has_addr() && sample_info.addr() != 0) {
    ++stats_.num_data_sample_events;
    uint64_t remapped_addr = 0;
    if (MapIPAndPidAndGetNameAndOffset(sample_info.addr(), pidtid,
                                       &remapped_addr,
                                       &parsed_event->data_dso_and_offset)) {
      ++stats_.num_data_sample_events_mapped;
      sample_info.set_addr(remapped_addr);
    }
  }

  if (sample_info.callchain_size() &&
      !MapCallchain(sample_info.ip(), pidtid, unmapped_event_ip,
                    sample_info.mutable_callchain(), parsed_event)) {
    mapping_ok = false;
  }

  if (sample_info.branch_stack_size() &&
      !MapBranchStack(pidtid, sample_info.mutable_branch_stack(),
                      parsed_event)) {
    mapping_ok = false;
  }

  if (mapping_ok) {
    ++stats_.num_sample_events_mapped;
  }
}

bool PerfParser::MapCallchain(const uint64_t ip, const PidTid pidtid,
                              const uint64_t original_event_addr,
                              RepeatedField<uint64>* callchain,
                              ParsedEvent* parsed_event) {
  if (!callchain) {
    LOG(ERROR) << "NULL call stack data.";
    return false;
  }

  // If the callchain is empty, there is no work to do.
  if (callchain->empty()) return true;

  // Keeps track of whether the current entry is kernel or user.
  parsed_event->callchain.resize(callchain->size());
  int num_entries_mapped = 0;
  bool mapping_ok = true;
  for (int i = 0; i < callchain->size(); ++i) {
    uint64_t entry = callchain->Get(i);
    // When a callchain context entry is found, do not attempt to symbolize it.
    if (entry >= PERF_CONTEXT_MAX) {
      continue;
    }
    // The sample address has already been mapped so no need to map it.
    if (entry == original_event_addr) {
      callchain->Set(i, ip);
      continue;
    }
    uint64_t mapped_addr = 0;
    if (!MapIPAndPidAndGetNameAndOffset(
            entry, pidtid, &mapped_addr,
            &parsed_event->callchain[num_entries_mapped++])) {
      mapping_ok = false;
      // During the remapping process, callchain ips that are not mapped to the
      // quipper space will have their original addresses passed on, based on an
      // earlier logic. This would sometimes lead to incorrect assignment of
      // such addresses to certain mmap regions in the quipper space by
      // perf_data_handler. Therefore, the unmapped address needs to be
      // explicitly marked by setting its highest bit. This operation considers
      // potential collision with the address space when options_.do_remap is
      // set to true or false. When options_.do_remap is true, this marked
      // address is guaranteed to be larger than the mapped quipper space. When
      // options_.do_remap is false, the kernel addresses of x86 and ARM have
      // the high 16 bit set and PowerPC has a reserved space from
      // 0x1000000000000000 to 0xBFFFFFFFFFFFFFFF. Thus, setting highest bit of
      // the unmapped address, which starts with 0x8, should not collide with
      // any existing addresses or mapped quipper addresses.
      callchain->Set(i, entry | 1ULL << 63);
    } else {
      callchain->Set(i, mapped_addr);
    }
  }
  // Not all the entries were mapped.  Trim |parsed_event->callchain| to
  // remove unused entries at the end.
  parsed_event->callchain.resize(num_entries_mapped);

  return mapping_ok;
}

bool PerfParser::MapBranchStack(
    const PidTid pidtid, RepeatedPtrField<BranchStackEntry>* branch_stack,
    ParsedEvent* parsed_event) {
  if (!branch_stack) {
    LOG(ERROR) << "NULL branch stack data.";
    return false;
  }

  // First, trim the branch stack to remove trailing null entries.
  size_t trimmed_size = 0;
  for (const BranchStackEntry& entry : *branch_stack) {
    // Count the number of non-null entries before the first null entry.
    if (IsNullBranchStackEntry(entry)) break;
    ++trimmed_size;
  }

  // If a null entry was found, make sure all subsequent null entries are NULL
  // as well.
  for (int i = trimmed_size; i < branch_stack->size(); ++i) {
    const BranchStackEntry& entry = branch_stack->Get(i);
    if (!IsNullBranchStackEntry(entry)) {
      LOG(ERROR) << "Non-null branch stack entry found after null entry: "
                 << reinterpret_cast<void*>(entry.from_ip()) << " -> "
                 << reinterpret_cast<void*>(entry.to_ip());
      return false;
    }
  }

  // Map branch stack addresses.
  parsed_event->branch_stack.resize(trimmed_size);
  for (unsigned int i = 0; i < trimmed_size; ++i) {
    BranchStackEntry* entry = branch_stack->Mutable(i);
    ParsedEvent::BranchEntry& parsed_entry = parsed_event->branch_stack[i];

    uint64_t from_mapped = 0;
    if (!MapIPAndPidAndGetNameAndOffset(entry->from_ip(), pidtid, &from_mapped,
                                        &parsed_entry.from)) {
      return false;
    }
    entry->set_from_ip(from_mapped);

    uint64_t to_mapped = 0;
    if (!MapIPAndPidAndGetNameAndOffset(entry->to_ip(), pidtid, &to_mapped,
                                        &parsed_entry.to)) {
      return false;
    }
    entry->set_to_ip(to_mapped);

    parsed_entry.mispredicted = entry->mispredicted();
    parsed_entry.predicted = entry->predicted();
    parsed_entry.in_transaction = entry->in_transaction();
    parsed_entry.aborted_transaction = entry->abort();
    parsed_entry.cycles = entry->cycles();
  }

  return true;
}

bool PerfParser::MapIPAndPidAndGetNameAndOffset(
    uint64_t ip, PidTid pidtid, uint64_t* new_ip,
    ParsedEvent::DSOAndOffset* dso_and_offset) {
  DCHECK(dso_and_offset);
  // Attempt to find the synthetic address of the IP sample in this order:
  // 1. Address space of the kernel.
  // 2. Address space of its own process.
  // 3. Address space of the parent process.

  uint64_t mapped_addr = 0;

  // Sometimes the first event we see is a SAMPLE event and we don't have the
  // time to create an address mapper for a process. Example, for pid 0.
  AddressMapper* mapper = GetOrCreateProcessMapper(pidtid.first).first;
  AddressMapper::MappingList::const_iterator ip_iter;
  bool mapped =
      mapper->GetMappedAddressAndListIterator(ip, &mapped_addr, &ip_iter);
  if (mapped) {
    uint64_t id = UINT64_MAX;
    mapper->GetMappedIDAndOffset(ip, ip_iter, &id, &dso_and_offset->offset_);
    // Make sure the ID points to a valid event.
    CHECK_LE(id, parsed_events_.size());
    ParsedEvent& parsed_event = parsed_events_[id];
    const auto& event = parsed_event.event_ptr;
    DCHECK(event->has_mmap_event()) << "Expected MMAP or MMAP2 event";

    // Find the mmap DSO filename in the set of known DSO names.
    auto dso_iter = name_to_dso_.find(event->mmap_event().filename());
    CHECK(dso_iter != name_to_dso_.end());
    dso_and_offset->dso_info_ = &dso_iter->second;

    dso_iter->second.hit = true;
    dso_iter->second.threads.insert(mergeTwoU32(pidtid.first, pidtid.second));
    ++parsed_event.num_samples_in_mmap_region;

    if (options_.do_remap) {
      if (GetPageAlignedOffset(mapped_addr) != GetPageAlignedOffset(ip)) {
        LOG(ERROR) << "Remapped address " << std::hex << mapped_addr << " "
                   << "does not have the same page alignment offset as "
                   << "original address " << ip;
        return false;
      }
      *new_ip = mapped_addr;
    } else {
      *new_ip = ip;
    }
  }
  return mapped;
}

bool PerfParser::MapMmapEvent(PerfDataProto_MMapEvent* event, uint64_t id,
                              bool is_kernel) {
  // We need to hide only the real kernel addresses.  However, to make things
  // more secure, and make the mapping idempotent, we should remap all
  // addresses, both kernel and non-kernel.

  AddressMapper* mapper = GetOrCreateProcessMapper(event->pid()).first;

  uint64_t start = event->start();
  uint64_t len = event->len();
  uint64_t pgoff = event->pgoff();

  // We have several cases for the kernel mmap:
  //
  // For ARM and x86, in sudo mode, pgoff == start, example:
  // start=0x80008200
  // pgoff=0x80008200
  // len  =0xfffffff7ff7dff
  //
  // For x86-64, in sudo mode, pgoff is between start and start + len. SAMPLE
  // events lie between pgoff and pgoff + length of the real kernel binary,
  // example:
  // start=0x3bc00000
  // pgoff=0xffffffffbcc00198
  // len  =0xffffffff843fffff
  // SAMPLE events will be found after pgoff. For kernels with ASLR, pgoff will
  // be something only visible to the root user, and will be randomized at
  // startup. With |remap| set to true, we should hide pgoff in this case. So we
  // normalize all SAMPLE events relative to pgoff.
  //
  // For non-sudo mode, the kernel will be mapped from 0 to the pointer limit,
  // example:
  // start=0x0
  // pgoff=0x0
  // len  =0xffffffff
  if (is_kernel) {
    // If pgoff is between start and len, we normalize the event by setting
    // start to be pgoff just like how it is for ARM and x86. We also set len to
    // be a much smaller number (closer to the real length of the kernel binary)
    // because SAMPLEs are actually only seen between |event->pgoff| and
    // |event->pgoff + kernel text size|.
    if (pgoff > start && pgoff < start + len) {
      len = len + start - pgoff;
      start = pgoff;
    }
    // For kernels with ALSR pgoff is critical information that should not be
    // revealed when |remap| is true.
    pgoff = 0;
  }
  bool is_jit_event = false;
  if (options_.allow_unaligned_jit_mappings) {
    is_jit_event = event->filename().find("jitted-") != std::string::npos;
  }
  if (!mapper->MapWithID(start, len, id, pgoff, true, is_jit_event)) {
    mapper->DumpToLog();
    return false;
  }

  if (options_.do_remap) {
    uint64_t mapped_addr;
    AddressMapper::MappingList::const_iterator start_iter;
    if (!mapper->GetMappedAddressAndListIterator(start, &mapped_addr,
                                                 &start_iter)) {
      LOG(ERROR) << "Failed to map starting address " << std::hex << start;
      return false;
    }
    if (GetPageAlignedOffset(mapped_addr) != GetPageAlignedOffset(start)) {
      LOG(ERROR) << "Remapped address " << std::hex << mapped_addr << " "
                 << "does not have the same page alignment offset as start "
                 << "address " << start;
      return false;
    }

    event->set_start(mapped_addr);
    event->set_len(len);
    event->set_pgoff(pgoff);
  }
  return true;
}

bool PerfParser::MapCommEvent(const PerfDataProto_CommEvent& event) {
  GetOrCreateProcessMapper(event.pid());
  return true;
}

bool PerfParser::MapForkEvent(const PerfDataProto_ForkEvent& event) {
  PidTid parent = std::make_pair(event.ppid(), event.ptid());
  PidTid child = std::make_pair(event.pid(), event.tid());
  if (parent != child) {
    auto parent_iter = pidtid_to_comm_map_.find(parent);
    if (parent_iter != pidtid_to_comm_map_.end())
      pidtid_to_comm_map_[child] = parent_iter->second;
  }

  const uint32_t pid = event.pid();

  // If the parent and child pids are the same, this is just a new thread
  // within the same process, so don't do anything.
  if (event.ppid() == pid) return true;

  if (!GetOrCreateProcessMapper(pid, event.ppid()).second) {
    DVLOG(1) << "Found an existing process mapper with pid: " << pid;
  }

  return true;
}

std::pair<AddressMapper*, bool> PerfParser::GetOrCreateProcessMapper(
    uint32_t pid, uint32_t ppid) {
  const auto& search = process_mappers_.find(pid);
  if (search != process_mappers_.end()) {
    return std::make_pair(search->second.get(), false);
  }

  auto parent_mapper = process_mappers_.find(ppid);
  // Recent perf implementations (at least as recent as perf 4.4), add an
  // explicit FORK event from the swapper process to the init process. There may
  // be no explicit memory mappings created for the swapper process. In such
  // cases, we must use the mappings from the kernel process, which are used by
  // default for a new PID in the absence of an explicit FORK event.
  if (parent_mapper == process_mappers_.end()) {
    parent_mapper = process_mappers_.find(kKernelPid);
  }
  std::unique_ptr<AddressMapper> mapper;
  if (parent_mapper != process_mappers_.end()) {
    mapper.reset(new AddressMapper(*parent_mapper->second));
  } else {
    mapper.reset(new AddressMapper());
    mapper->set_page_alignment(kMmapPageAlignment);
  }

  const auto inserted =
      process_mappers_.insert(search, std::make_pair(pid, std::move(mapper)));
  return std::make_pair(inserted->second.get(), true);
}

}  // namespace quipper
