#pragma once

#include <cstdint>
#include <string>

#include "arcbox/config.hpp"

namespace arcbox {

struct HostMemory {
    uint64_t mem_total_kb = 0;
    uint64_t mem_available_kb = 0;
    uint64_t swap_total_kb = 0;
    uint64_t swap_free_kb = 0;
    /// True when at least one swap device is backed by zram, i.e. the swap is
    /// compressed RAM rather than a disk. Only that kind of swap may be
    /// credited towards MemAvailable.
    bool swap_on_zram = false;
    double psi_some_avg10 = 0.0;
    double psi_full_avg10 = 0.0;
    bool psi_supported = false;
};

/// Where the host memory picture is read from. The defaults are the real
/// /proc files; ARCBOX_MEMINFO_FILE, ARCBOX_PSI_FILE and ARCBOX_SWAPS_FILE
/// override them so the guard can be exercised without a big machine.
struct MemorySources {
    std::string meminfo_path = "/proc/meminfo";
    std::string psi_path = "/proc/pressure/memory";
    std::string swaps_path = "/proc/swaps";
};

/// Sample host memory state from /proc/meminfo, /proc/swaps and
/// /proc/pressure/memory. psi_supported is false when CONFIG_PSI is
/// unavailable; PSI fields stay 0.
bool read_host_memory(HostMemory& out);
bool read_host_memory(HostMemory& out, const MemorySources& sources);

/// MemAvailable plus the part of zram swap that can be treated as usable
/// memory. Disk-backed swap is deliberately not credited: swapping to disk
/// keeps the box alive but makes captures unusably slow, so it must not
/// convince the guard to start another browser.
uint64_t effective_available_kb(const HostMemory& mem,
                                double zram_swap_weight);

enum class Admission {
    Proceed,
    Throttle,
    Pause,
};

struct AdmissionDecision {
    Admission action = Admission::Proceed;
    std::string reason;
    long sleep_seconds = 0;
};

/// Decide whether the queue may start the next capture.
AdmissionDecision admit_capture(const Config& cfg, const HostMemory& mem);

const char* admission_name(Admission action);

} // namespace arcbox
