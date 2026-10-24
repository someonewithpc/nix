// Parse every file in a list and report lex+parse and bindVars time.
// usage: parse-bench <file-list> [reps] [--dump <out-file>]
#include "nix/expr/eval.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-settings.hh"
#if __has_include("nix/expr/parser.hh")
#  include "nix/expr/parser.hh"
#else
#  include "nix/parser-tab.hh"
#endif
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/store-open.hh"
#include "nix/store/globals.hh"
#include "nix/util/file-system.hh"
#include "nix/util/terminal.hh"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace nix;
using Clock = std::chrono::steady_clock;

int main(int argc, char ** argv)
{
    if (argc < 2) {
        std::cerr << "usage: parse-bench <file-list> [reps] [--dump <out-file>]\n";
        return 2;
    }
    int reps = 1;
    std::string dumpPath;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dump" && i + 1 < argc)
            dumpPath = argv[++i];
        else
            reps = std::stoi(a);
    }

    initLibStore(false);
    initGC();

    bool readOnlyMode = true;
    fetchers::Settings fetchSettings{};
    EvalSettings evalSettings{readOnlyMode};
    evalSettings.nixPath = {};
    EvalState state({}, openStore("dummy://"), fetchSettings, evalSettings, nullptr);

    std::vector<std::string> paths, sources;
    {
        std::ifstream list(argv[1]);
        for (std::string p; std::getline(list, p);)
            if (!p.empty()) {
                paths.push_back(p);
                sources.push_back(readFile(p));
            }
    }
    size_t bytes = 0;
    for (auto & s : sources)
        bytes += s.size();

    std::ofstream dump;
    if (!dumpPath.empty())
        dump.open(dumpPath);

    double parseSecs = 0, bindSecs = 0;
    size_t failures = 0;
    for (int r = 0; r < reps; r++) {
        for (size_t i = 0; i < paths.size(); i++) {
            // The parser writes into its buffer, so each run gets a fresh copy
            std::string buf = sources[i];
            buf.append("\0\0", 2);
            auto path = state.rootPath(CanonPath(paths[i]));
            DocCommentMap docComments;
            try {
                auto t0 = Clock::now();
                Expr * e = parseExprFromBuf(
                    buf.data(),
                    buf.size(),
                    Pos::Origin(path),
                    path.parent(),
                    state.mem.exprs,
                    state.symbols,
                    evalSettings,
                    state.positions,
                    docComments,
                    state.rootFS);
                auto t1 = Clock::now();
                e->bindVars(state, state.staticBaseEnv);
                auto t2 = Clock::now();
                parseSecs += std::chrono::duration<double>(t1 - t0).count();
                bindSecs += std::chrono::duration<double>(t2 - t1).count();
                if (r == 0 && dump.is_open()) {
                    std::ostringstream out;
                    e->show(state.symbols, out);
                    dump << "=== " << paths[i] << "\n" << out.str() << "\n";
                }
            } catch (std::exception & ex) {
                if (r == 0) {
                    failures++;
                    if (dump.is_open())
                        dump << "=== " << paths[i] << "\nERROR " << filterANSIEscapes(ex.what(), true) << "\n";
                }
            }
        }
    }

    double mb = bytes / 1e6;
    std::cout << paths.size() << " files, " << mb << " MB, " << failures << " failed\n"
              << "lex+parse " << parseSecs / reps << " s/rep (" << mb * reps / parseSecs << " MB/s)\n"
              << "bindVars  " << bindSecs / reps << " s/rep\n";
}
