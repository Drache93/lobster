// Copyright 2026 Wouter van Oortmerssen. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// lobsterfmt: formats Lobster programs, like gofmt.
//
//     lobsterfmt [-l] [-w] program.lobster ...
//     lobsterfmt [-l] [-w] -program program.lobster file.lobster ...
//
// What names mean is what the parser reads code by, so it takes a whole program. The first formats
// the main files given and the files of those programs in their directory or under it. The second
// formats only the files given, which are read as part of the program. By default the main files or
// given files are printed:
//     -l  list the files whose formatting differs instead
//     -w  write the result to the files instead
// Exits with 2 if a program can't be formatted safely.

#include "format.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using namespace lobster;

extern vector<string> data_dirs;

// Written next to it first, for a failure not to leave half a file. What the name refers to keeps
// its permissions, and a file that can't be written stays as it is.
static bool WriteFormatted(const string &file, string_view result) {
    std::error_code ec;
    auto target = fs::canonical(file, ec);
    if (ec) return false;
    auto perms = fs::status(target, ec).permissions();
    if (ec || (perms & fs::perms::owner_write) == fs::perms::none) return false;
    auto temp = target.string() + ".lobsterfmt";
    {
        std::ofstream outf(temp, std::ios::binary);
        outf << result;
        if (!outf) return false;
    }
    fs::permissions(temp, perms, ec);
    if (!ec) fs::rename(temp, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temp, ignored);
    }
    return !ec;
}

// Formats the program as the flags say, returning whether that was possible: only `given` of it if
// there are any. A file that's in several programs has to come out the same in each, in `seen`.
static bool Program(const char *exe, const string &program, const vector<string> &given, bool list,
                    bool write, map<string, string> &seen) {
    if (!fs::is_regular_file(program)) {
        std::cerr << program << ": no such file\n";
        return false;
    }
    data_dirs.clear();
    if (!InitPlatform(GetMainDirFromExePath(exe), program, false, lobsterfmt::LoadSource)) {
        std::cerr << program << ": can't find the location of the modules\n";
        return false;
    }
    auto maindir = fs::absolute(program).lexically_normal().parent_path().generic_string() + "/";
    auto canonical = [](const string &path) { return fs::weakly_canonical(path).string(); };
    // Which path the compiler loads each from, by what it is on disk.
    lobsterfmt::Recorder recorder;
    auto main = StripDirPart(program);
    set<string> only;
    if (!given.empty()) {
        auto errors = lobsterfmt::ReadProgram(main, recorder);
        if (!errors.empty()) {
            std::cerr << errors << "\n";
            return false;
        }
        auto ok = true;
        for (auto &file : given) {
            auto found = false;
            for (auto &f : recorder.files) {
                if (f && canonical(f->path) == canonical(file)) {
                    only.insert(f->path);
                    found = true;
                }
            }
            if (!found) {
                std::cerr << file << ": not part of " << program << "\n";
                ok = false;
            }
        }
        if (!ok) return false;
    }
    vector<lobsterfmt::FormattedFile> files;
    auto error = lobsterfmt::FormatProgram(main, maindir, files, given.empty() ? nullptr : &only);
    if (!error.empty()) {
        std::cerr << error << "\n";
        return false;
    }
    auto ok = true;
    for (auto &f : files) {
        auto it = seen.find(f.path);
        if (it != seen.end()) {
            if (it->second != f.result) {
                std::cerr << f.path << ": formats differently in " << program << "\n";
                ok = false;
            }
            continue;
        }
        seen[f.path] = f.result;
        std::ifstream in(f.path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        auto differs = ss.str() != f.result;
        if (list && differs) std::cout << f.path << "\n";
        if (write && differs && !WriteFormatted(f.path, f.result)) {
            std::cerr << f.path << ": can't write\n";
            ok = false;
        }
        auto printed = !given.empty() || f.path == StripFilePart(program) + main;
        if (!list && !write && printed) std::cout << f.result;
    }
    return ok;
}

int main(int argc, char **argv) {
    min_output_level = OUTPUT_ERROR;
    auto list = false, write = false;
    string program;
    vector<string> paths;
    for (int i = 1; i < argc; i++) {
        string_view arg = argv[i];
        if (arg == "-l") list = true;
        else if (arg == "-w") write = true;
        else if (arg == "-program" && i + 1 < argc) program = argv[++i];
        else if (arg.starts_with("-")) {
            std::cerr << "unknown flag " << arg << "\n";
            return 2;
        } else paths.push_back(argv[i]);
    }
    if (paths.empty()) {
        std::cerr << "usage: lobsterfmt [-l] [-w] [-program program.lobster] file.lobster ...\n";
        return 2;
    }
    auto status = 0;
    map<string, string> seen;
    try {
        if (!program.empty()) {
            if (!Program(argv[0], program, paths, list, write, seen)) status = 2;
        } else {
            for (auto &path : paths) {
                if (!Program(argv[0], path, {}, list, write, seen)) status = 2;
            }
        }
    } catch (const fs::filesystem_error &e) {
        std::cerr << e.what() << "\n";
        status = 2;
    }
    return status;
}
