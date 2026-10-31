#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "arcbox/commands.hpp"
#include "arcbox/config.hpp"
#include "arcbox/util.hpp"

namespace {

void usage() {
    std::cout
        << "ArchiveBox.cpp (arcbox) 0.1.0\n\n"
        << "Usage:\n"
        << "  arcbox [--data DIR] init\n"
        << "  arcbox [--data DIR] add URL [--capture] [--no-block]\n"
        << "  arcbox [--data DIR] capture ID|URL [--no-cgroup] [--no-block]\n"
        << "  arcbox [--data DIR] list [--limit N]\n"
        << "  arcbox [--data DIR] search QUERY [--limit N]\n"
        << "  arcbox [--data DIR] show ID\n"
        << "  arcbox [--data DIR] daemon [--once] [--no-cgroup] [--no-block]\n"
        << "  arcbox version\n\n"
        << "Environment:\n"
        << "  ARCBOX_DATA            data directory (default ~/.local/share/arcbox)\n"
        << "  ARCBOX_NO_CGROUP=1     disable systemd-run memory wrapping\n"
        << "  ARCBOX_MEMORY_MAX      cgroup hard limit (default 1800M)\n"
        << "  ARCBOX_MEMORY_HIGH     cgroup throttle threshold (default 1200M)\n"
        << "  ARCBOX_MEMORY_SWAP_MAX cgroup swap limit (default 512M)\n";
}

bool take_value(const std::vector<std::string>& args,
                std::size_t& index,
                std::string& value) {
    if (index + 1 >= args.size()) {
        return false;
    }
    value = args[index + 1];
    index += 2;
    return true;
}

int parse_limit(const std::string& value) {
    try {
        int limit = std::stoi(value);
        if (limit <= 0) {
            throw std::invalid_argument("limit must be positive");
        }
        return limit;
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid limit: " + value);
    }
}

} // namespace

int main(int argc, char** argv) {
    arcbox::Config cfg = arcbox::Config::load();

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    std::size_t index = 0;
    while (index < args.size()) {
        if (args[index] == "--data") {
            std::string value;
            if (!take_value(args, index, value)) {
                std::cerr << "arcbox: --data requires a value\n";
                return 1;
            }
            cfg.data_dir = value;
            continue;
        }
        if (arcbox::starts_with(args[index], "--data=")) {
            cfg.data_dir = args[index].substr(7);
            ++index;
            continue;
        }
        break;
    }

    if (index >= args.size()) {
        usage();
        return 1;
    }

    const std::string command = args[index++];
    try {
        if (command == "version") {
            std::cout << "arcbox 0.1.0\n";
            return 0;
        }
        if (command == "help" || command == "--help" || command == "-h") {
            usage();
            return 0;
        }
        if (command == "init") {
            return arcbox::cmd_init(cfg);
        }
        if (command == "add") {
            std::string url;
            bool capture_now = false;
            bool use_blocklist = true;
            while (index < args.size()) {
                const std::string& arg = args[index];
                if (arg == "--capture") {
                    capture_now = true;
                    ++index;
                } else if (arg == "--no-block") {
                    use_blocklist = false;
                    ++index;
                } else if (arg == "--help" || arg == "-h") {
                    usage();
                    return 0;
                } else if (arg.rfind("--", 0) == 0) {
                    std::cerr << "arcbox: unknown option: " << arg << "\n";
                    return 1;
                } else if (url.empty()) {
                    url = arg;
                    ++index;
                } else {
                    std::cerr << "arcbox: only one URL is accepted per add\n";
                    return 1;
                }
            }
            if (url.empty()) {
                std::cerr << "arcbox: add requires a URL\n";
                return 1;
            }
            return arcbox::cmd_add(cfg, url, capture_now, use_blocklist);
        }
        if (command == "capture") {
            std::string target;
            bool use_cgroup = true;
            bool use_blocklist = true;
            while (index < args.size()) {
                const std::string& arg = args[index];
                if (arg == "--no-cgroup") {
                    use_cgroup = false;
                    ++index;
                } else if (arg == "--no-block") {
                    use_blocklist = false;
                    ++index;
                } else if (arg == "--id") {
                    std::string value;
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --id requires a value\n";
                        return 1;
                    }
                    target = value;
                } else if (arg == "--help" || arg == "-h") {
                    usage();
                    return 0;
                } else if (arg.rfind("--", 0) == 0) {
                    std::cerr << "arcbox: unknown option: " << arg << "\n";
                    return 1;
                } else if (target.empty()) {
                    target = arg;
                    ++index;
                } else {
                    std::cerr << "arcbox: capture accepts one ID or URL\n";
                    return 1;
                }
            }
            if (target.empty()) {
                std::cerr << "arcbox: capture requires an ID or URL\n";
                return 1;
            }
            return arcbox::cmd_capture(cfg, target, use_cgroup, use_blocklist);
        }
        if (command == "list") {
            int limit = 20;
            while (index < args.size()) {
                if (args[index] == "--limit") {
                    std::string value;
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --limit requires a value\n";
                        return 1;
                    }
                    limit = parse_limit(value);
                } else {
                    std::cerr << "arcbox: unknown list option: " << args[index]
                              << "\n";
                    return 1;
                }
            }
            return arcbox::cmd_list(cfg, limit);
        }
        if (command == "search") {
            std::string query;
            int limit = 20;
            while (index < args.size()) {
                if (args[index] == "--limit") {
                    std::string value;
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --limit requires a value\n";
                        return 1;
                    }
                    limit = parse_limit(value);
                } else {
                    if (!query.empty()) {
                        query.push_back(' ');
                    }
                    query += args[index];
                    ++index;
                }
            }
            if (query.empty()) {
                std::cerr << "arcbox: search requires a query\n";
                return 1;
            }
            return arcbox::cmd_search(cfg, query, limit);
        }
        if (command == "show") {
            if (index + 1 != args.size()) {
                std::cerr << "arcbox: show requires exactly one ID\n";
                return 1;
            }
            int64_t id = 0;
            try {
                id = std::stoll(args[index]);
            } catch (const std::exception&) {
                std::cerr << "arcbox: invalid ID: " << args[index] << "\n";
                return 1;
            }
            return arcbox::cmd_show(cfg, id);
        }
        if (command == "daemon") {
            bool once = false;
            bool use_cgroup = true;
            bool use_blocklist = true;
            while (index < args.size()) {
                const std::string& arg = args[index];
                if (arg == "--once") {
                    once = true;
                    ++index;
                } else if (arg == "--no-cgroup") {
                    use_cgroup = false;
                    ++index;
                } else if (arg == "--no-block") {
                    use_blocklist = false;
                    ++index;
                } else {
                    std::cerr << "arcbox: unknown daemon option: " << arg
                              << "\n";
                    return 1;
                }
            }
            return arcbox::cmd_daemon(cfg, once, use_cgroup, use_blocklist);
        }
        if (command == "__worker") {
            int64_t id = 0;
            bool use_blocklist = true;
            bool have_id = false;
            while (index < args.size()) {
                if (args[index] == "--id") {
                    std::string value;
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --id requires a value\n";
                        return 1;
                    }
                    try {
                        id = std::stoll(value);
                    } catch (const std::exception&) {
                        std::cerr << "arcbox: invalid ID: " << value << "\n";
                        return 1;
                    }
                    have_id = true;
                } else if (args[index] == "--no-block") {
                    use_blocklist = false;
                    ++index;
                } else {
                    std::cerr << "arcbox: unknown worker option: "
                              << args[index] << "\n";
                    return 1;
                }
            }
            if (!have_id) {
                std::cerr << "arcbox: __worker requires --id\n";
                return 1;
            }
            return arcbox::cmd_worker(cfg, id, use_blocklist);
        }

        std::cerr << "arcbox: unknown command: " << command << "\n";
        usage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "arcbox: " << error.what() << "\n";
        return 1;
    }
}
