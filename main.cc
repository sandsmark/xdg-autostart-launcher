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
namespace fs = std::filesystem;

static const fs::perms execPermissions =
    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;

inline std::string trim(std::string string)
{
    if (string.empty()) {
        return string;
    }

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
        part = trim(part);
        if (!part.empty()) {
            ret.push_back(part);
        }
    }
    if (ret.empty()) {
        return {string};
    }
    return ret;
}

static void launch(const char *command)
{
    std::cout << " -> Launching " << command << std::endl;

    const int pid = fork();
    switch(pid) {
    case 0:
        if (s_verbose) std::cout << "Child executing " << command << std::endl;

        std::system(command);
        exit(0);
        break;
    case -1:
        std::cerr << " ! Error forking: " << strerror(errno) << std::endl;
        return;
    default:
        if (s_verbose) std::cout << "Forked, parent PID: " << pid << std::endl;
        break;
    }
}

static std::vector<std::string> globalConfigPaths()
{
    static const std::string defaultPath = "/etc/xdg/";

    char *rawPath = getenv("XDG_CONFIG_DIRS");
    if (!rawPath) {
        return { defaultPath };
    }

    std::vector<std::string> ret;
    std::string paths(rawPath);
    if (paths.find(':') != std::string::npos) {
        std::istringstream stream(paths);
        std::string testPath;
        while (std::getline(stream, testPath, ':')) {
            if (!fs::exists(testPath)) {
                continue;
            }
            if (!fs::is_directory(testPath)) {
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

static std::string localConfigPath()
{
    std::string ret;

    char *rawPath = getenv("XDG_CONFIG_HOME");
    if (rawPath) {
        ret = std::string(rawPath);
    }
    if (!ret.empty()) {
        ret = resolvePath(ret.c_str());

        if (fs::exists(ret)) {
            return ret;
        } else {
            ret = "";
        }
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
            if (!fs::exists(testPath)) {
                continue;
            }
            if (!fs::is_directory(testPath)) {
                continue;
            }

            ret = testPath;
            break;
        }
    }

    if (ret.empty() || !fs::exists(ret)) {
        ret = "~/.config";
    }

    return resolvePath(ret.c_str());
}

struct Parser {
    std::unordered_set<std::string> toLaunch;
    std::unordered_set<std::string> disabled;

    void parseFile(const fs::path &path)
    {
        std::ifstream file(path);
        if (!file.good()) {
            std::cerr << " ! Failed to open " << path << std::endl;
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
                if (s_verbose) std::cout << "Skipping comment '" << line << "' in " << path << std::endl;
                continue;
            }
            if (line[0] == '[') {
                if (s_verbose) std::cout << "Skipping group '" << line << "' in " << path << std::endl;
                continue;
            }
            size_t nameEnd = line.find('=');

            if (nameEnd == std::string::npos) {
                std::cerr << " ! Invalid line '" << line << "' in " << path << std::endl;
                continue;
            }

            const std::string name = trim(line.substr(0, nameEnd));
            const std::string value = trim(line.substr(nameEnd + 1));

            if (name == "" || value == "") {
                std::cerr << " ! Invalid line '" << line << "' in " << path << std::endl;
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
                if (s_verbose) std::cout << "Ignoring " << path << " because of only show in: " << line << std::endl;
                hidden = true;
                continue;
            }
            if (name == "X-KDE-autostart-condition") {
                if (s_verbose) std::cout << "Ignoring " << path << " because of KDE condition: " << line << std::endl;
                hidden = true;
                continue;
            }
            if (name == "TryExec") {
                if (s_verbose) std::cout << "Ignoring " << path << " because of TryExec: " << line << std::endl;
                hidden = true;
                continue;
            }
        }

        if (exec.empty()) {
            std::cerr << " ! Unable to find Exec in " << path << std::endl;
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

    void handleDir(const fs::path &dir)
    {
        for (const fs::path &file : fs::directory_iterator(dir)) {
            parseFile(file);
        }
    }

    void doExec() {
        for (const std::string &exec : toLaunch) {
            const std::string executable = stringSplit(exec)[0];
            if (disabled.count(executable)) {
                std::cout << " - Skipping disabled " << exec << std::endl;
                continue;
            }
            if (fs::path(executable).has_parent_path() && !fs::exists(executable)) {
                std::cerr << " ! " << executable << " does not exist, ignoring" << std::endl;
                continue;
            }
            if (fs::exists(executable)) {
                if (!fs::is_regular_file(executable) &&
                    !fs::is_symlink(executable)) {
                    std::cerr << " ! " << executable << " not a file" << std::endl;
                }
                fs::perms permissions = fs::status(executable).permissions();
                if ((permissions & execPermissions) == fs::perms::none) {
                    std::cerr << " ! " << executable << " is not executable" << std::endl;
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
            std::cerr << " ! Invalid option " << argv[1] << std::endl;
            print_usage(argv[0]);
            return 1;
    }
    if (!user && !global) {
        print_usage(argv[0]);
    }

    // lol don't care
    if (argc > 2) {
        s_verbose = true;
    }

    Parser parser;

    bool globalFailed = false, userFailed = false;
    if (global) {
        for (const std::string &directory : globalConfigPaths()) {
            fs::path path(directory + "/autostart/");
            if (fs::exists(path)) {
                parser.handleDir(path);
                globalFailed = false;
            }
        }
        if (globalFailed) {
            std::cerr << " ! Failed to find system directories" << std::endl;

            if (s_verbose) {
                std::cout << ", tried:" << std::endl;
                for (const std::string &directory : globalConfigPaths()) {
                    std::cout << directory << std::endl;
                }
            } else {
                std::cerr << std::endl;
            }
        } else if (s_verbose) { std::cout << "Handled system dirs" << std::endl; }
    }

    if (user) {
        fs::path path(localConfigPath() + "/autostart/");

        if (fs::exists(path)) {
            parser.handleDir(path);
        } else {
            std::cerr << " ! User directory " << path << " does not exist" << std::endl;
            userFailed = true;
        }
    }

    parser.doExec();

    return userFailed || globalFailed;
}
