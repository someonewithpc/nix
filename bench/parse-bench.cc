// Parse every file in a list and report lex+parse and bindVars time.
// usage: parse-bench <file-list> [reps] [--dump <out-file>] [--threads <n>]
//
// With --threads, each rep parses and binds every file on n threads sharing
// one EvalState, largest files first, and reports wall time
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace nix;
using Clock = std::chrono::steady_clock;

#if __has_include("nix/expr/parse-buffer.hh")
#  include "nix/expr/parse-buffer.hh"
static ParseBuffer bufferOf(const std::string & s)
{
    return ParseBuffer(s);
}
#  define BUF_ARGS(b) b
#else
static std::string bufferOf(const std::string & s)
{
    std::string b;
    b.reserve(s.size() + 2);
    b.append(s);
    b.append("\0\0", 2);
    return b;
}
#  define BUF_ARGS(b) b.data(), b.size()
#endif

int main(int argc, char ** argv)
{
    if (argc < 2) {
        std::cerr << "usage: parse-bench <file-list> [reps] [--dump <out-file>]\n";
        return 2;
    }
    int reps = 1;
    std::string dumpPath;
    unsigned threads = 0;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dump" && i + 1 < argc)
            dumpPath = argv[++i];
        else if (a == "--threads" && i + 1 < argc)
            threads = std::stoul(argv[++i]);
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

    auto parseOne = [&](size_t i, bool show) -> std::string {
        auto buf = bufferOf(sources[i]);
        auto path = state.rootPath(CanonPath(paths[i]));
        DocCommentMap docComments;
        try {
            Expr * e = parseExprFromBuf(
                BUF_ARGS(buf),
                Pos::Origin(path),
                path.parent(),
                state.mem.exprs,
                state.symbols,
                evalSettings,
                state.positions,
                docComments,
                state.rootFS);
            e->bindVars(state, state.staticBaseEnv);
            if (!show)
                return {};
            std::ostringstream out;
            e->show(state.symbols, out);
            out << "\n";
            std::vector<std::pair<Pos, DocComment>> docs;
            for (auto & [p, d] : docComments)
                docs.emplace_back(state.positions[p], d);
            std::ranges::sort(docs, [](auto & a, auto & b) {
                return std::pair(a.first.line, a.first.column) < std::pair(b.first.line, b.first.column);
            });
            for (auto & [p, d] : docs)
                out << "doc " << p.line << ":" << p.column << " " << d.getInnerText(state.positions) << "\n";
            return out.str();
        } catch (std::exception & ex) {
            return "ERROR " + filterANSIEscapes(ex.what(), true) + "\n";
        }
    };

    if (threads) {
        std::vector<size_t> order(paths.size());
        for (size_t i = 0; i < order.size(); i++)
            order[i] = i;
        std::ranges::sort(order, [&](size_t a, size_t b) { return sources[a].size() > sources[b].size(); });
        std::vector<std::string> shown(paths.size());
        auto pass = [&](bool show) {
            std::atomic<size_t> next = 0;
            std::vector<std::thread> pool;
            for (unsigned k = 0; k < threads; k++)
                pool.emplace_back([&] {
#if NIX_USE_BOEHMGC
                    GC_stack_base base;
                    GC_get_stack_base(&base);
                    auto registered = GC_register_my_thread(&base);
#endif
                    for (size_t j; (j = next.fetch_add(1)) < order.size();) {
                        auto res = parseOne(order[j], show);
                        if (show)
                            shown[order[j]] = std::move(res);
                    }
#if NIX_USE_BOEHMGC
                    if (registered == GC_SUCCESS)
                        GC_unregister_my_thread();
#endif
                });
            for (auto & t : pool)
                t.join();
        };
        /* Untimed, since printing the trees costs more than parsing them */
        pass(true);
        double secs = 0;
        for (int r = 0; r < reps; r++) {
            auto t0 = Clock::now();
            pass(false);
            secs += std::chrono::duration<double>(Clock::now() - t0).count();
        }
        size_t failures = 0;
        for (size_t i = 0; i < paths.size(); i++) {
            if (shown[i].starts_with("ERROR "))
                failures++;
            if (dump.is_open())
                dump << "=== " << paths[i] << "\n" << shown[i];
        }
        double mb = bytes / 1e6;
        std::cout << paths.size() << " files, " << mb << " MB, " << failures << " failed\n"
                  << "lex+parse+bindVars on " << threads << " threads " << secs / reps << " s/rep (" << mb * reps / secs
                  << " MB/s)\n";
        return 0;
    }

    double parseSecs = 0, bindSecs = 0;
    size_t failures = 0;
    for (int r = 0; r < reps; r++) {
        for (size_t i = 0; i < paths.size(); i++) {
            // The parser writes into its buffer, so each run gets a fresh copy
            auto buf = bufferOf(sources[i]);
            auto path = state.rootPath(CanonPath(paths[i]));
            DocCommentMap docComments;
            try {
                auto t0 = Clock::now();
                Expr * e = parseExprFromBuf(
                    BUF_ARGS(buf),
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
                    out << "\n";
                    std::vector<std::pair<Pos, DocComment>> docs;
                    for (auto & [p, d] : docComments)
                        docs.emplace_back(state.positions[p], d);
                    std::ranges::sort(docs, [](auto & a, auto & b) {
                        return std::pair(a.first.line, a.first.column) < std::pair(b.first.line, b.first.column);
                    });
                    for (auto & [p, d] : docs)
                        out << "doc " << p.line << ":" << p.column << " " << d.getInnerText(state.positions) << "\n";
                    dump << "=== " << paths[i] << "\n" << out.str();
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
