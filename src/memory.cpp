#include "arcbox/memory.hpp"

#include "arcbox/util.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace arcbox {

namespace {

bool parse_kb_value(const std::string& line, uint64_t& out) {
    auto colon = line.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    std::istringstream iss(line.substr(colon + 1));
    uint64_t value = 0;
    if (!(iss >> value)) {
        return false;
    }
    out = value;
    return true;
}

bool parse_psi_avg10(const std::string& line, const char* key, double& out) {
    std::string prefix(key);
    if (line.rfind(prefix, 0) != 0) {
        return false;
    }
    auto pos = line.find("avg10=");
    if (pos == std::string::npos) {
        return false;
    }
    try {
        out = std::stod(line.substr(pos + 6));
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace

MemorySources default_memory_sources() {
    MemorySources sources;
    sources.meminfo_path = getenv_or("ARCBOX_MEMINFO_FILE", "/proc/meminfo");
    sources.psi_path = getenv_or("ARCBOX_PSI_FILE", "/proc/pressure/memory");
    sources.swaps_path = getenv_or("ARCBOX_SWAPS_FILE", "/proc/swaps");
    return sources;
}

bool read_host_memory(HostMemory& out) {
    return read_host_memory(out, default_memory_sources());
}

bool read_host_memory(HostMemory& out, const MemorySources& sources) {
    out = HostMemory{};
    bool have_meminfo = false;

    std::ifstream meminfo(sources.meminfo_path);
    if (meminfo) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                if (parse_kb_value(line, out.mem_total_kb)) {
                    have_meminfo = true;
                }
            } else if (line.rfind("MemAvailable:", 0) == 0) {
                if (parse_kb_value(line, out.mem_available_kb)) {
                    have_meminfo = true;
                }
            } else if (line.rfind("SwapTotal:", 0) == 0) {
                parse_kb_value(line, out.swap_total_kb);
            } else if (line.rfind("SwapFree:", 0) == 0) {
                parse_kb_value(line, out.swap_free_kb);
            }
        }
    }

    // /proc/swaps tells swap *type*, which /proc/meminfo does not: a zram
    // device is compressed RAM and can be counted (partly) as available
    // memory, while a swapfile or partition cannot.
    std::ifstream swaps(sources.swaps_path);
    if (swaps) {
        std::string line;
        std::getline(swaps, line); // header
        while (std::getline(swaps, line)) {
            std::istringstream iss(line);
            std::string device;
            if (!(iss >> device)) {
                continue;
            }
            const auto slash = device.find_last_of('/');
            const std::string base =
                slash == std::string::npos ? device : device.substr(slash + 1);
            if (base.rfind("zram", 0) == 0) {
                out.swap_on_zram = true;
                break;
            }
        }
    }

    std::ifstream psi(sources.psi_path);
    if (psi) {
        std::string line;
        while (std::getline(psi, line)) {
            parse_psi_avg10(line, "some ", out.psi_some_avg10);
            parse_psi_avg10(line, "full ", out.psi_full_avg10);
        }
        out.psi_supported = true;
    }

    return have_meminfo;
}

uint64_t effective_available_kb(const HostMemory& mem,
                                double zram_swap_weight) {
    uint64_t available = mem.mem_available_kb;
    if (!mem.swap_on_zram || zram_swap_weight <= 0.0) {
        return available;
    }
    if (zram_swap_weight > 1.0) {
        zram_swap_weight = 1.0;
    }
    return available +
           static_cast<uint64_t>(static_cast<double>(mem.swap_free_kb) *
                                 zram_swap_weight);
}

AdmissionDecision admit_capture(const Config& cfg, const HostMemory& mem) {
    AdmissionDecision decision;

    // A zero reading means /proc/meminfo was unreadable; fail open rather than
    // stalling the queue forever.
    const uint64_t available =
        effective_available_kb(mem, cfg.zram_swap_weight);
    if (available > 0 &&
        static_cast<long>(available) < cfg.min_mem_available_kb) {
        decision.action = Admission::Pause;
        decision.reason = "effective available " +
                          std::to_string(available / 1024) + "M below floor " +
                          std::to_string(cfg.min_mem_available_kb / 1024) + "M";
        if (mem.swap_on_zram && mem.swap_free_kb > 0) {
            decision.reason += " (MemAvailable " +
                               std::to_string(mem.mem_available_kb / 1024) +
                               "M plus zram swap " +
                               std::to_string(mem.swap_free_kb / 1024) + "M)";
        }
        decision.sleep_seconds = cfg.pause_sleep_seconds;
        return decision;
    }

    if (mem.psi_supported && mem.psi_some_avg10 > cfg.psi_some_avg10_max) {
        decision.action = Admission::Throttle;
        decision.reason = "PSI memory some avg10 " +
                          std::to_string(mem.psi_some_avg10) + " above " +
                          std::to_string(cfg.psi_some_avg10_max);
        decision.sleep_seconds = cfg.throttle_sleep_seconds;
        return decision;
    }

    decision.action = Admission::Proceed;
    decision.reason = "host memory healthy";
    return decision;
}

const char* admission_name(Admission action) {
    switch (action) {
        case Admission::Proceed:
            return "proceed";
        case Admission::Throttle:
            return "throttle";
        case Admission::Pause:
            return "pause";
    }
    return "unknown";
}

} // namespace arcbox
