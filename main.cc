#include <pugixml.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <vector>
#include <wordexp.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

static bool s_verbose = false;

static void launch(const char *command)
{
    std::cout << " -> Launching " << command << std::endl;

    const int pid = fork();

    switch(pid) {
    case 0:
        if (s_verbose) {
            std::cout << "Child executing " << command << std::endl;
        }

        std::system(command);
        exit(0);
        break;
    case -1:
        std::cerr << "Error forking: " << strerror(errno) << std::endl;
        return;
    default:
        if (s_verbose) {
            std::cout << "Forked, parent PID: " << pid << std::endl;
        }

        break;
    }
}

static std::string resolvePath(const char *path)
{
    if (!path) {
        return {};
    }

    wordexp_t expanded;
    wordexp(path, &expanded, 0);
    std::string resolved(expanded.we_wordv[0]);
    wordfree(&expanded);

    return resolved;
}

static std::vector<std::string> globalConfigPaths()
{
    static const std::string defaultPath = "/etc/xdg/";

    char *rawPath = getenv("XDG_CONFIG_DIRS");
    if (!rawPath) {
        return { defaultPath };
    }

    std::vector<std::string> ret;
    std::string paths(rawPath);;
    if (paths.find(':') != std::string::npos) {
        std::istringstream stream(paths);
        std::string testPath;
        while (std::getline(stream, testPath, ':')) {
            if (!std::filesystem::exists(testPath)) {
                continue;
            }
            if (!std::filesystem::is_directory(testPath)) {
                continue;
            }
            ret.push_back(testPath);
            break;
        }
    } else {
        ret.push_back(paths);
    }
    if (ret.empty()) {
        return { defaultPath };
    }

    if (std::find(ret.begin(), ret.end(), defaultPath) == ret.end()) {
        ret.push_back(defaultPath);
    }

    return ret;
}

static std::string localConfigPath()
{
    std::string ret;

    char *rawPath = getenv("XDG_CONFIG_HOME");
    if (rawPath) {
        ret = std::string(rawPath);
    }
    if (!ret.empty()) {
        return resolvePath(ret.c_str());
    }

    if (ret.empty()) {
        rawPath = getenv("HOME");
        if (rawPath) {
            ret = getenv("HOME");
            ret += "/.config";
        }
    } else if (ret.find(':') != std::string::npos) {
        std::istringstream stream(ret);
        std::string testPath;
        while (std::getline(stream, testPath, ':')) {
            if (!std::filesystem::exists(testPath)) {
                continue;
            }
            if (!std::filesystem::is_directory(testPath)) {
                continue;
            }

            ret = testPath;
            break;
        }
    }

    if (ret.empty()) {
        ret = "~/.config";
    }

    return resolvePath(ret.c_str());
}

inline std::string trim(std::string string)
{
    string.erase(string.begin(), std::find_if(string.begin(), string.end(), [](int c) { return !std::isspace(c); }  ));
    string.erase(std::find_if(string.rbegin(), string.rend(), [](int c) { return !std::isspace(c); }).base(), string.end());
    return string;
}

inline std::vector<std::string> stringSplit(const std::string &string, const char delimiter = ' ')
{
    if (string.find(delimiter) == std::string::npos) {
        return {string};
    }

    std::vector<std::string> ret;
    std::istringstream stream(string);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        if (part.empty()) {
            continue;
        }

        ret.push_back(part);
    }
    if (ret.empty()) {
        return {string};
    }
    return ret;
}

struct Parser {
    std::unordered_set<std::string> toLaunch;
    std::unordered_set<std::string> disabled;

    void parseFile(const std::filesystem::path &path)
    {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "Failed to open " << path << std::endl;
            return;
        }

        std::string exec;
        bool hidden = false;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) {
                if (s_verbose) std::cout << "Empty line in " << path << std::endl;
                continue;
            }

            line = trim(line);

            if (line[0] == '#') {
                if (s_verbose) std::cout << "Skipping comment " << line << " in " << path << std::endl;
                continue;
            }
            if (line[0] == '[') {
                if (s_verbose) std::cout << "Skipping group " << line << " in " << path << std::endl;
                continue;
            }
            size_t nameEnd = line.find('=');

            if (nameEnd == std::string::npos) {
                if (s_verbose) std::cout << "Invalid line " << line << " in " << path << std::endl;
                continue;
            }

            const std::string name = trim(line.substr(0, nameEnd));
            const std::string value = trim(line.substr(nameEnd + 1));

            if (name == "" || value == "") {
                if (s_verbose) std::cout << "Invalid line " << line << " in " << path << std::endl;
                continue;
            }

            if (name == "Exec") {
                exec = value;
                continue;
            }

            if (name == "Hidden") {
                hidden = true;
                continue;
            }

            // TODO, parse conditions and stuff
            if (name == "OnlyShowIn") {
                if (s_verbose) std::cout << "Ignoring " << path << " because of " << line << std::endl;
                hidden = true;
                continue;
            }
            if (name == "X-KDE-autostart-condition") {
                if (s_verbose) std::cout << "Ignoring " << path << " because of " << line << std::endl;
                hidden = true;
                continue;
            }
            if (name == "TryExec") {
                if (s_verbose) std::cout << "Ignoring " << path << " because of " << line << std::endl;
                hidden = true;
                continue;
            }
        }

        if (exec.empty()) {
            std::cerr << "Unable to find execution in " << path << std::endl;
            return;
        }
        if (hidden) {
            std::cout << path << " disabled" << std::endl;
            disabled.insert(stringSplit(exec)[0]);
            disabled.insert(path.stem());
        } else {
            toLaunch.insert(exec);
        }
    }

    void handleDir(const std::filesystem::path &dir)
    {
        for (const filesystem::path &file : std::filesystem::directory_iterator(dir)) {
            parseFile(file);
        }
    }

    void doExec() {
        for (const std::string &exec : toLaunch) {
            const std::string executable = stringSplit(exec)[0];
            if (disabled.count(executable)) {
                std::cout << "Skipping disabled " << exec << std::endl;
                continue;
            }
            if (std::filesystem::path(executable).has_parent_path() && !std::filesystem::exists(executable)) {
                std::cerr << executable << " does not exist, ignoring" << std::endl;
                continue;
            }
            if (std::filesystem::exists(executable)) {
                if (!std::filesystem::is_regular_file(executable) &&
                    !std::filesystem::is_symlink(executable)) {
                    std::cerr << executable << " not a file" << std::endl;
                }
                std::filesystem::perms permissions = std::filesystem::status(executable).permissions();
                if ((permissions & std::filesystem::perms::owner_exec) == std::filesystem::perms::none &&
                    (permissions & std::filesystem::perms::group_exec) == std::filesystem::perms::none &&
                    (permissions & std::filesystem::perms::others_exec) == std::filesystem::perms::none)
                {
                    std::cerr << executable << " is not executable" << std::endl;
                }

            }

            launch(exec.c_str());
        }
    }
};

void print_usage(const char *executable)
{
    std::cout << "Usage:\n\t" << executable << " (--system|--user|--both) [--verbose]" << std::endl;
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argv[1][0] != '-' || strlen(argv[1]) < 3) {
        print_usage(argv[0]);
        return 1;
    }

    bool user = false, global = false;
    switch(argv[1][2]) {
        case 's':
            global = true;
            break;
        case 'u':
            user = true;
            break;
        case 'b':
            user = true;
            global = true;
            break;

        default:
            std::cout << "Invalid option " << argv[1] << std::endl;
            print_usage(argv[0]);
            return 1;
    }
    if (argc > 2) {
        s_verbose = true;
    }

    Parser parser;

    bool globalFailed = false, userFailed = false;
    if (global) {
        for (const std::string &directory : globalConfigPaths()) {
            std::filesystem::path path(directory + "/autostart/");
            if (std::filesystem::exists(path)) {
                parser.handleDir(path);
                globalFailed = false;
            }
        }
        if (globalFailed) {
            std::cerr << "Failed to find system directories" << std::endl;

            if (s_verbose) {
                std::cout << ", tried:" << std::endl;
                for (const std::string &directory : globalConfigPaths()) {
                    std::cerr << directory << std::endl;
                }
            } else {
                std::cerr << std::endl;
            }
        } else if (s_verbose) {
            std::cout << "Handled system dirs" << std::endl;
        }
    }

    if (user) {
        std::filesystem::path path(localConfigPath() + "/autostart/");

        if (std::filesystem::exists(path)) {
            parser.handleDir(path);
        } else {
            std::cerr << "User directory " << path << " does not exist" << std::endl;
            userFailed = true;
        }
    }

    parser.doExec();

    return userFailed || globalFailed;
}
