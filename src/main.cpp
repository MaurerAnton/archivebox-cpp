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
        << "ArchiveBox.cpp (arcbox) 0.2.0\n\n"
        << "Usage:\n"
        << "  arcbox [--data DIR] init\n"
        << "  arcbox [--data DIR] add URL [--capture] [--priority N] [--no-block]\n"
        << "  arcbox [--data DIR] capture ID|URL [--no-cgroup] [--no-block]\n"
        << "  arcbox [--data DIR] list [--limit N] [--tag TAG]\n"
        << "  arcbox [--data DIR] search QUERY [--limit N] [--tag TAG]\n"
        << "  arcbox [--data DIR] show ID\n"
        << "  arcbox [--data DIR] tag ID TAG [TAG...]\n"
        << "  arcbox [--data DIR] untag ID TAG [TAG...]\n"
        << "  arcbox [--data DIR] tags\n"
        << "  arcbox [--data DIR] memcheck\n"
        << "  arcbox [--data DIR] import FILE|- [--capture] [--no-cgroup] [--no-block]\n"
        << "  arcbox [--data DIR] export [--limit N] [--tag TAG] [--urls]\n"
        << "  arcbox [--data DIR] serve [--host HOST] [--port PORT]\n"
        << "  arcbox [--data DIR] daemon [--once] [--no-cgroup] [--no-block]\n"
        << "  arcbox [--data DIR] wacz FILE [--limit N] [--tag TAG]\n"
        << "  arcbox [--data DIR] retry ID|all\n"
        << "  arcbox [--data DIR] recrawl [--older-than 30d] [--limit N]\n"
        << "  arcbox [--data DIR] prune --keep N [--older-than 90d] [--dry-run]\n"
        << "  arcbox [--data DIR] blocklist check URL\n"
        << "  arcbox [--data DIR] blocklist import FILE\n"
        << "  arcbox version\n\n"
        << "Environment:\n"
        << "  ARCBOX_DATA            data directory (default ~/.local/share/arcbox)\n"
        << "  ARCBOX_NO_CGROUP=1     disable systemd-run memory wrapping\n"
        << "  ARCBOX_MEMORY_MAX      cgroup hard limit (default 1800M)\n"
        << "  ARCBOX_MEMORY_HIGH     cgroup throttle threshold (default 1200M)\n"
        << "  ARCBOX_MEMORY_SWAP_MAX cgroup swap limit (default 512M)\n"
        << "  ARCBOX_RENDER          1 enables headless rendering\n"
        << "  ARCBOX_RENDER_PDF      1 also prints the page to PDF\n"
        << "  ARCBOX_BROWSER         path to a headless Chromium\n"
        << "  ARCBOX_RENDER_DEGRADE  0 disables the artifact ladder\n"
        << "  ARCBOX_RENDER_MAX_SOURCE_BYTES  skip larger sources (20 MiB)\n"
        << "  ARCBOX_MIN_MEM_AVAILABLE_KB  memory floor in kB (default 1536000)\n"
        << "  ARCBOX_ZRAM_SWAP_WEIGHT  share of free zram swap counted as memory (0.5)\n"
        << "  ARCBOX_PSI_SOME_MAX      PSI some avg10 that throttles the queue (10)\n"
        << "  ARCBOX_MEMINFO_FILE    override /proc/meminfo for the guard\n"
        << "  ARCBOX_SWAPS_FILE      override /proc/swaps for the guard\n"
        << "  ARCBOX_MAX_ATTEMPTS    attempts before giving up (default 5)\n"
        << "  ARCBOX_RETRY_BACKOFF   first retry delay in seconds (default 30)\n";
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
            if (value.empty()) {
                std::cerr << "arcbox: --data must not be empty\n";
                return 1;
            }
            cfg.data_dir = value;
            continue;
        }
        if (arcbox::starts_with(args[index], "--data=")) {
            std::string value = args[index].substr(7);
            if (value.empty()) {
                std::cerr << "arcbox: --data must not be empty\n";
                return 1;
            }
            cfg.data_dir = value;
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
            std::cout << "arcbox 0.2.0\n";
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
            int priority = 0;
            while (index < args.size()) {
                const std::string& arg = args[index];
                if (arg == "--capture") {
                    capture_now = true;
                    ++index;
                } else if (arg == "--no-block") {
                    use_blocklist = false;
                    ++index;
                } else if (arg == "--priority") {
                    std::string value;
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --priority requires a value\n";
                        return 1;
                    }
                    try {
                        priority = std::stoi(value);
                    } catch (const std::exception&) {
                        std::cerr << "arcbox: invalid priority: " << value
                                  << "\n";
                        return 1;
                    }
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
            return arcbox::cmd_add(cfg, url, capture_now, use_blocklist,
                                   priority);
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
            std::string tag;
            while (index < args.size()) {
                if (args[index] == "--limit") {
                    std::string value;
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --limit requires a value\n";
                        return 1;
                    }
                    limit = parse_limit(value);
                } else if (args[index] == "--tag") {
                    if (!take_value(args, index, tag)) {
                        std::cerr << "arcbox: --tag requires a value\n";
                        return 1;
                    }
                } else if (args[index] == "--help" || args[index] == "-h") {
                    usage();
                    return 0;
                } else {
                    std::cerr << "arcbox: unknown list option: " << args[index]
                              << "\n";
                    return 1;
                }
            }
            return arcbox::cmd_list(cfg, limit, tag);
        }
        if (command == "search") {
            std::string query;
            std::string tag;
            int limit = 20;
            if (index < args.size() &&
                (args[index] == "--help" || args[index] == "-h")) {
                usage();
                return 0;
            }
            while (index < args.size()) {
                if (args[index] == "--limit") {
                    std::string value;
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --limit requires a value\n";
                        return 1;
                    }
                    limit = parse_limit(value);
                } else if (args[index] == "--tag") {
                    if (!take_value(args, index, tag)) {
                        std::cerr << "arcbox: --tag requires a value\n";
                        return 1;
                    }
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
            return arcbox::cmd_search(cfg, query, limit, tag);
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
            if (id <= 0) {
                std::cerr << "arcbox: invalid ID: " << args[index] << "\n";
                return 1;
            }
            return arcbox::cmd_show(cfg, id);
        }
        if (command == "tag" || command == "untag") {
            if (index >= args.size()) {
                usage();
                return 1;
            }
            if (args[index] == "--help" || args[index] == "-h") {
                usage();
                return 0;
            }
            int64_t id = 0;
            try {
                id = std::stoll(args[index]);
            } catch (const std::exception&) {
                std::cerr << "arcbox: invalid ID: " << args[index] << "\n";
                return 1;
            }
            if (id <= 0) {
                std::cerr << "arcbox: invalid ID: " << args[index] << "\n";
                return 1;
            }
            ++index;
            std::vector<std::string> tags;
            while (index < args.size()) {
                tags.push_back(args[index]);
                ++index;
            }
            if (tags.empty()) {
                std::cerr << "arcbox: " << command
                          << " requires at least one tag\n";
                return 1;
            }
            return arcbox::cmd_tag(cfg, id, tags, command == "untag");
        }
        if (command == "tags") {
            if (index < args.size() &&
                (args[index] == "--help" || args[index] == "-h")) {
                usage();
                return 0;
            }
            if (index != args.size()) {
                std::cerr << "arcbox: tags accepts no arguments\n";
                return 1;
            }
            return arcbox::cmd_tags(cfg);
        }
        if (command == "memcheck") {
            if (index != args.size() &&
                (args[index] == "--help" || args[index] == "-h")) {
                usage();
                return 0;
            }
            if (index != args.size()) {
                std::cerr << "arcbox: memcheck accepts no arguments\n";
                return 1;
            }
            return arcbox::cmd_memcheck(cfg);
        }
        if (command == "import") {
            std::string path;
            bool capture_now = false;
            bool use_cgroup = true;
            bool use_blocklist = true;
            while (index < args.size()) {
                const std::string& arg = args[index];
                if (arg == "--capture") {
                    capture_now = true;
                    ++index;
                } else if (arg == "--no-cgroup") {
                    use_cgroup = false;
                    ++index;
                } else if (arg == "--no-block") {
                    use_blocklist = false;
                    ++index;
                } else if (arg == "--help" || arg == "-h") {
                    usage();
                    return 0;
                } else if (arg.rfind("--", 0) == 0) {
                    std::cerr << "arcbox: unknown import option: " << arg
                              << "\n";
                    return 1;
                } else if (path.empty()) {
                    path = arg;
                    ++index;
                } else {
                    std::cerr << "arcbox: import accepts one file path\n";
                    return 1;
                }
            }
            if (path.empty()) {
                std::cerr << "arcbox: import requires a file path or -\n";
                return 1;
            }
            return arcbox::cmd_import(cfg, path, capture_now, use_cgroup,
                                      use_blocklist);
        }
        if (command == "wacz") {
            if (index >= args.size()) {
                std::cerr << "arcbox: wacz needs an output file\n";
                return 1;
            }
            const std::string output = args[index++];
            int limit = 1000;
            std::string tag;
            while (index < args.size()) {
                if (args[index] == "--limit") {
                    std::string value;
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --limit requires a value\n";
                        return 1;
                    }
                    limit = parse_limit(value);
                } else if (args[index] == "--tag") {
                    if (!take_value(args, index, tag)) {
                        std::cerr << "arcbox: --tag requires a value\n";
                        return 1;
                    }
                } else {
                    std::cerr << "arcbox: unknown wacz option: " << args[index]
                              << "\n";
                    return 1;
                }
            }
            return arcbox::cmd_wacz(cfg, output, limit, tag);
        }
        if (command == "export") {
            int limit = 1000;
            std::string tag;
            bool urls_only = false;
            while (index < args.size()) {
                if (args[index] == "--limit") {
                    std::string value;
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --limit requires a value\n";
                        return 1;
                    }
                    limit = parse_limit(value);
                } else if (args[index] == "--tag") {
                    if (!take_value(args, index, tag)) {
                        std::cerr << "arcbox: --tag requires a value\n";
                        return 1;
                    }
                } else if (args[index] == "--urls") {
                    urls_only = true;
                    ++index;
                } else if (args[index] == "--help" || args[index] == "-h") {
                    usage();
                    return 0;
                } else {
                    std::cerr << "arcbox: unknown export option: "
                              << args[index] << "\n";
                    return 1;
                }
            }
            return arcbox::cmd_export(cfg, limit, tag, urls_only);
        }
        if (command == "serve") {
            std::string host = "127.0.0.1";
            int port = 8080;
            while (index < args.size()) {
                if (args[index] == "--host") {
                    if (!take_value(args, index, host)) {
                        std::cerr << "arcbox: --host requires a value\n";
                        return 1;
                    }
                } else if (args[index] == "--port") {
                    std::string value;
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --port requires a value\n";
                        return 1;
                    }
                    try {
                        port = std::stoi(value);
                    } catch (const std::exception&) {
                        std::cerr << "arcbox: invalid port: " << value << "\n";
                        return 1;
                    }
                    if (port <= 0 || port > 65535) {
                        std::cerr << "arcbox: invalid port: " << value << "\n";
                        return 1;
                    }
                } else if (args[index] == "--help" || args[index] == "-h") {
                    usage();
                    return 0;
                } else {
                    std::cerr << "arcbox: unknown serve option: "
                              << args[index] << "\n";
                    return 1;
                }
            }
            return arcbox::cmd_serve(cfg, host, port);
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
                } else if (arg == "--help" || arg == "-h") {
                    usage();
                    return 0;
                } else {
                    std::cerr << "arcbox: unknown daemon option: " << arg
                              << "\n";
                    return 1;
                }
            }
            return arcbox::cmd_daemon(cfg, once, use_cgroup, use_blocklist);
        }
        if (command == "retry") {
            std::string target;
            while (index < args.size()) {
                const std::string& arg = args[index];
                if (arg == "--help" || arg == "-h") {
                    usage();
                    return 0;
                } else if (target.empty()) {
                    target = arg;
                    ++index;
                } else {
                    std::cerr << "arcbox: retry accepts one ID or 'all'\n";
                    return 1;
                }
            }
            if (target.empty()) {
                std::cerr << "arcbox: retry requires a snapshot ID or 'all'\n";
                return 1;
            }
            return arcbox::cmd_retry(cfg, target);
        }
        if (command == "recrawl") {
            int64_t older_than = 0;
            int limit = 0;
            while (index < args.size()) {
                std::string value;
                if (args[index] == "--older-than") {
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --older-than requires a value\n";
                        return 1;
                    }
                    if (!arcbox::parse_duration_seconds(value, older_than)) {
                        std::cerr << "arcbox: invalid duration: " << value
                                  << " (use 30d, 12h, 90m)\n";
                        return 1;
                    }
                } else if (args[index] == "--limit") {
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --limit requires a value\n";
                        return 1;
                    }
                    try {
                        limit = std::stoi(value);
                    } catch (const std::exception&) {
                        std::cerr << "arcbox: invalid limit: " << value << "\n";
                        return 1;
                    }
                } else {
                    std::cerr << "arcbox: unknown option: " << args[index]
                              << "\n";
                    return 1;
                }
            }
            return arcbox::cmd_recrawl(cfg, older_than, limit);
        }
        if (command == "prune") {
            int keep = 0;
            int64_t older_than = 0;
            bool dry_run = false;
            while (index < args.size()) {
                std::string value;
                if (args[index] == "--keep") {
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --keep requires a value\n";
                        return 1;
                    }
                    try {
                        keep = std::stoi(value);
                    } catch (const std::exception&) {
                        std::cerr << "arcbox: invalid keep: " << value << "\n";
                        return 1;
                    }
                } else if (args[index] == "--older-than") {
                    if (!take_value(args, index, value)) {
                        std::cerr << "arcbox: --older-than requires a value\n";
                        return 1;
                    }
                    if (!arcbox::parse_duration_seconds(value, older_than)) {
                        std::cerr << "arcbox: invalid duration: " << value
                                  << " (use 90d, 12h)\n";
                        return 1;
                    }
                } else if (args[index] == "--dry-run") {
                    dry_run = true;
                    ++index;
                } else {
                    std::cerr << "arcbox: unknown option: " << args[index]
                              << "\n";
                    return 1;
                }
            }
            return arcbox::cmd_prune(cfg, keep, older_than, dry_run);
        }
        if (command == "blocklist") {
            if (index >= args.size()) {
                std::cerr << "arcbox: blocklist requires 'check URL' or "
                             "'import FILE'\n";
                return 1;
            }
            const std::string action = args[index++];
            if (action == "check") {
                if (index >= args.size()) {
                    std::cerr << "arcbox: blocklist check requires a URL\n";
                    return 1;
                }
                return arcbox::cmd_blocklist_check(cfg, args[index]);
            }
            if (action == "import") {
                if (index >= args.size()) {
                    std::cerr << "arcbox: blocklist import requires a file\n";
                    return 1;
                }
                return arcbox::cmd_blocklist_import(cfg, args[index]);
            }
            std::cerr << "arcbox: unknown blocklist action: " << action
                      << "\n";
            return 1;
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
