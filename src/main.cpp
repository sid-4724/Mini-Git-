#include "minigit/repository.hpp"
#include "minigit/worktree.hpp"

#include <iostream>
#include <vector>

using namespace std;
using namespace std::filesystem;
using minigit::FilePath;

static string pathArgument(const vector<string>& arguments) {
    if (arguments.size() == 1 && !arguments[0].empty() && arguments[0][0] != '-') return arguments[0];
    if (arguments.size() == 2 && arguments[0] == "--") return arguments[1];
    throw minigit::Error("Expected one path. Use '--' before a leading-dash filename.");
}

static void printHelp() {
    cout << "MiniGit: an educational local version control system\n"
              << "  minigit init\n"
              << "  minigit add [--] <path>\n"
              << "  minigit status\n"
              << "  minigit commit -m <message>\n"
              << "  minigit log\n"
              << "  minigit diff [--staged]\n"
              << "  minigit restore [--source <commit-or-branch>] [--] <path>\n"
              << "  minigit branch [<name> | -d <name>]\n"
              << "  minigit switch <branch>\n"
              << "  minigit merge-base <branch-a> <branch-b>\n"
              << "  minigit merge <branch> | --abort\n"
              << "  minigit recover\n"
              << "Restore discards working changes to the selected tracked file.\n"
              << "Merge exit 0 can leave unresolved conflicts. Check 'minigit status', stage resolutions, then commit.\n";
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            printHelp();
            return 1;
        }
        string command = argv[1];
        vector<string> arguments;
        for (int index = 2; index < argc; ++index) arguments.push_back(argv[index]);
        if ((command == "--help" || command == "help") && arguments.empty()) {
            printHelp();
            return 0;
        }
        if (command == "init" && arguments.empty()) {
            minigit::initializeRepository(current_path());
            return 0;
        }
        FilePath root = minigit::findRepository(current_path());
        minigit::RepositoryLock lock(root / ".minigit");
        if (command == "recover" && arguments.empty()) {
            minigit::recoverTransaction(root);
            return 0;
        }
        minigit::Repository repository(root);
        if (command == "add") repository.add(pathArgument(arguments));
        else if (command == "status" && arguments.empty()) repository.status();
        else if (command == "commit" && arguments.size() == 2 && arguments[0] == "-m") repository.commit(arguments[1]);
        else if (command == "log" && arguments.empty()) repository.log();
        else if (command == "diff" && arguments.empty()) repository.diff(false);
        else if (command == "diff" && arguments.size() == 1 && arguments[0] == "--staged") repository.diff(true);
        else if (command == "restore") {
            string source;
            if (arguments.size() >= 2 && arguments[0] == "--source") {
                source = arguments[1];
                if (source.empty()) throw minigit::Error("--source requires a nonempty commit ID or branch name.");
                arguments.erase(arguments.begin(), arguments.begin() + 2);
            }
            repository.restore(pathArgument(arguments), source);
        }
        else if (command == "branch" && arguments.empty()) repository.branches();
        else if (command == "branch" && arguments.size() == 1) repository.createBranch(arguments[0]);
        else if (command == "branch" && arguments.size() == 2 && arguments[0] == "-d") repository.deleteBranch(arguments[1]);
        else if (command == "switch" && arguments.size() == 1) repository.switchBranch(arguments[0]);
        else if (command == "merge-base" && arguments.size() == 2) repository.mergeBase(arguments[0], arguments[1]);
        else if (command == "merge" && arguments.size() == 1 && arguments[0] == "--abort") repository.abortMerge();
        else if (command == "merge" && arguments.size() == 1) repository.merge(arguments[0]);
        else throw minigit::Error("Unknown command or invalid arguments: " + command);
        // A successfully started conflicted merge also returns 0; status tracks resolution.
        return 0;
    } catch (const exception& error) {
        cerr << "minigit: " << error.what() << '\n';
        return 1;
    }
}
