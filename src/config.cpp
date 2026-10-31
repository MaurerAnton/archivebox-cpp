#include "arcbox/config.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

#include "arcbox/util.hpp"

namespace arcbox {

static long parse_long(const char* name, long fallback) {
    std::string value = getenv_or(name, "");
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stol(value);
    } catch (...) {
        return fallback;
    }
}

static double parse_double(const char* name, double fallback) {
    std::string value = getenv_or(name, "");
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stod(value);
    } catch (...) {
        return fallback;
    }
}

Config Config::load() {
    Config cfg;
    std::string data = getenv_or("ARCBOX_DATA", "");
    if (data.empty()) {
        data = getenv_or("HOME", ".") + "/.local/share/arcbox";
    }
    cfg.data_dir = std::filesystem::path(data);

    cfg.user_agent = getenv_or("ARCBOX_USER_AGENT", cfg.user_agent);
    cfg.max_body_bytes = parse_long("ARCBOX_MAX_BODY_BYTES", cfg.max_body_bytes);
    cfg.connect_timeout_seconds =
        parse_long("ARCBOX_CONNECT_TIMEOUT", cfg.connect_timeout_seconds);
    cfg.timeout_seconds =
        parse_long("ARCBOX_TIMEOUT", cfg.timeout_seconds);

    cfg.min_mem_available_kb =
        parse_long("ARCBOX_MIN_MEM_AVAILABLE_KB", cfg.min_mem_available_kb);
    cfg.psi_some_avg10_max =
        parse_double("ARCBOX_PSI_SOME_MAX", cfg.psi_some_avg10_max);
    cfg.throttle_sleep_seconds =
        parse_long("ARCBOX_THROTTLE_SLEEP", cfg.throttle_sleep_seconds);
    cfg.pause_sleep_seconds =
        parse_long("ARCBOX_PAUSE_SLEEP", cfg.pause_sleep_seconds);

    cfg.max_attempts =
        static_cast<int>(parse_long("ARCBOX_MAX_ATTEMPTS", cfg.max_attempts));
    cfg.retry_backoff_seconds =
        parse_long("ARCBOX_RETRY_BACKOFF", cfg.retry_backoff_seconds);
    cfg.retry_backoff_max_seconds = parse_long("ARCBOX_RETRY_BACKOFF_MAX",
                                               cfg.retry_backoff_max_seconds);

    cfg.use_cgroup = getenv_or("ARCBOX_NO_CGROUP", "") != "1";
    cfg.slice = getenv_or("ARCBOX_SLICE", "");

    cfg.render_enabled = getenv_or("ARCBOX_RENDER", "") == "1";
    cfg.render_pdf = getenv_or("ARCBOX_RENDER_PDF", "") == "1";
    cfg.browser_path = getenv_or("ARCBOX_BROWSER", "");
    cfg.render_timeout_seconds =
        parse_long("ARCBOX_RENDER_TIMEOUT", cfg.render_timeout_seconds);
    cfg.render_virtual_time_ms =
        parse_long("ARCBOX_RENDER_BUDGET_MS", cfg.render_virtual_time_ms);
    cfg.render_width =
        static_cast<int>(parse_long("ARCBOX_RENDER_WIDTH", cfg.render_width));
    cfg.render_height =
        static_cast<int>(parse_long("ARCBOX_RENDER_HEIGHT", cfg.render_height));
    cfg.memory_max = getenv_or("ARCBOX_MEMORY_MAX", cfg.memory_max);
    cfg.memory_high = getenv_or("ARCBOX_MEMORY_HIGH", cfg.memory_high);
    cfg.memory_swap_max =
        getenv_or("ARCBOX_MEMORY_SWAP_MAX", cfg.memory_swap_max);
    cfg.cpu_quota = getenv_or("ARCBOX_CPU_QUOTA", cfg.cpu_quota);
    cfg.tasks_max = static_cast<int>(parse_long("ARCBOX_TASKS_MAX", cfg.tasks_max));

    return cfg;
}

void Config::ensure_dirs() const {
    std::filesystem::create_directories(data_dir);
    std::filesystem::create_directories(warc_dir());
    std::filesystem::create_directories(snapshots_dir());
    std::filesystem::create_directories(objects_dir());

    if (!std::filesystem::exists(blocklist_path())) {
        std::ofstream out(blocklist_path());
        if (!out) {
            throw std::runtime_error("cannot create blocklist: " +
                                     blocklist_path().string());
        }
        out << "# ArchiveBox.cpp host blocklist\n"
            << "# One domain per line. Hosts-file and ABP host rules are accepted.\n"
            << "# Examples:\n"
            << "#   ads.example.com\n"
            << "#   0.0.0.0 tracker.example.net\n"
            << "#   ||ads.example.org^\n"
            << "#   @@news.example.com\n";
    }
}

std::filesystem::path Config::db_path() const {
    return data_dir / "index.sqlite3";
}

std::filesystem::path Config::warc_dir() const {
    return data_dir / "warc";
}

std::filesystem::path Config::snapshots_dir() const {
    return data_dir / "snapshots";
}

std::filesystem::path Config::objects_dir() const {
    return data_dir / "objects";
}

std::filesystem::path Config::blocklist_path() const {
    return data_dir / "blocklist.txt";
}

std::filesystem::path Config::resolve(const std::string& relative) const {
    std::filesystem::path p(relative);
    return p.is_absolute() ? p : data_dir / p;
}

} // namespace arcbox
