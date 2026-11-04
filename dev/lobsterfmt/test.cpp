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

// Tests for lobsterfmt:
//
//     lobsterfmt_test [-seed N] [-rounds N] [-compiler LOBSTER] path ...
//
// The rules of the layout, what's refused. Then, for each .lobster file under the paths that can
// be formatted as a program (many are parts of one):
// - the result formats to itself;
// - with its whitespace scrambled (but not what it means) it formats the same;
// - broken (lines and characters deleted, duplicated, joined, split, spliced with another file) it
//   is refused, or formats to something that formats to itself, and never hits a check for a bug;
// - with -compiler, in a copy with all of it scrambled and then formatted, that compiler's parse
//   dump of each file that compiles stays the same, and the errors of those that don't, and none
//   that compiles is refused. A dump only has code that is used.

#include "format.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using namespace lobsterfmt;

extern vector<string> data_dirs;

// What the goldens use, imported by each.
static const string_view prelude = R"(
var a = 1
var b = 2
var c = 3
var d = 4
var i = 5
var n = 6
var xs = [ 1 ]
class P:
    x = 1
    y = 2
class Q:
    x = 1
    y = [ 1 ]
    def f(v): return v
    def m(v, blk): return blk(v)
class Two<K, V>:
    k:K
    v:V
let q = Q {}
def f(v): return v
def f(v, w): return v + w
def g(v): return v
def h(v, blk): return blk(v)
def t(v, blk): return blk(v)
def two(v, w): return v + w
enum C:
    red
    size
def red(v): return v
)";

// Inputs, and the output each rule of the layout gives them. Each starts with an import of prelude.
static const pair<string_view, string_view> goldens[] = {
    // Spaces around binary operators, after commas, none inside ( ) or in xs[i].
    { "let x=a+b*c\nlet y = xs[ 0 ]\nf( x ,y )\n", "let x = a + b * c\nlet y = xs[0]\nf(x, y)\n" },
    // Inside [ ] of a vector, { } of a constructor.
    { "let v=[1,2]\nlet e=[]::int\nlet p=P{1,2}\nlet r=P{}\n",
      "let v = [ 1, 2 ]\nlet e = []::int\nlet p = P { 1, 2 }\nlet r = P {}\n" },
    // Types: a:int, [int], ->, ::, string?, generics.
    { "def f2( v : int, w:[ int ], y :: P )->string?:\n    nil\n",
      "def f2(v:int, w:[int], y::P) -> string?:\n    nil\n" },
    { "let t1 = Two<string,int> { \"a\", 1 }\nvar t2 = []::resource<texture>\n",
      "let t1 = Two<string, int> { \"a\", 1 }\nvar t2 = []::resource<texture>\n" },
    // Fields, and inheritance.
    { "class A : P\n  z : int = 1\n  w:float\n", "class A : P\n    z:int = 1\n    w:float\n" },
    // Blocks: 4 spaces, a space after the : of a block, none before.
    { "if a :b\nelse :\n\tc\n", "if a: b\nelse:\n    c\n" },
    // Unary operators, not, and keywords before (.
    { "a = - 1\nlet o = not  a\nc = ~ a\nfor (xs) x: x\nassert(a)\n",
      "a = -1\nlet o = not a\nc = ~a\nfor(xs) x: x\nassert (a)\n" },
    // What it's for that the parser knows: a call or not, an operator or not.
    { "f (a)\nf(a)\nf -1\nwhile a<b: a++\n", "f (a)\nf(a)\nf -1\nwhile a < b: a++\n" },
    { "let u = a -1\nlet w = xs [0]\nf(a<b, c > -1)\n", "let u = a - 1\nlet w = xs [0]\nf(a < b, c > -1)\n" },
    { "q  .x\nq.  y\nq.x\n", "q.x\nq.y\nq.x\n" },
    // Imports.
    { "import  .prelude\nimport from \"x/\"\n", "import .prelude\nimport from \"x/\"\n" },
    // Continuation lines: once more, or twice when a block follows.
    { "let s = a +\nb\nif a and\n   b:\n    c\n",
      "let s = a +\n    b\nif a and\n        b:\n    c\n" },
    // A comment between continuation lines, indented as they are.
    { "if a and\n// c\n   b:\n    c\n", "if a and\n        // c\n        b:\n    c\n" },
    // A block that starts in a continuation line.
    { "let w =\n  if a:\n    1\n  else:\n    2\n",
      "let w =\n    if a:\n        1\n    else:\n        2\n" },
    // Brackets: lined up after the bracket, or once more when it ends the line.
    { "two(a,\nb)\nlet ys = [\n1,\n2\n]\n", "two(a,\n    b)\nlet ys = [\n    1,\n    2\n]\n" },
    // Comments: two spaces before one at the end of a line, lined up on consecutive lines.
    { "a = 1 // one\nb = b + 2   // two\n", "a = 1      // one\nb = b + 2  // two\n" },
    // A comment on its own line goes with the code after it.
    { "def e():\n        // c\n    g(1)\n", "def e():\n    // c\n    g(1)\n" },
    // Blank lines: at most one, none at the start of a block or the file, none at the end.
    { "a = 1\n\n\n\nb = 2\nif c:\n\n    d = 1\n\n\n", "a = 1\n\nb = 2\nif c:\n    d = 1\n" },
    // Strings, multi-line ones and interpolations are left alone.
    { "let s =  \"a  {b+c}  \"\nlet r = \"\"\"\n  x  \n\"\"\"\n",
      "let s = \"a  {b+c}  \"\nlet r = \"\"\"\n  x  \n\"\"\"\n" },
    // CRLF becomes LF; trailing whitespace goes.
    { "a = 1   \r\nb = 2\r\n", "a = 1\nb = 2\n" },
    // Operators defined, and comments in a line.
    { "def operator+= (o:P): o\ndef operator[](o:P, v:int): v\n",
      "def operator+=(o:P): o\ndef operator[](o:P, v:int): v\n" },
    { "a=b/* c */+d\n", "a = b /* c */ + d\n" },
    // A function's name and its ( (where the space doesn't matter), vectors in type arguments.
    { "def e (v): v\nlet p = Two<string,[ int ]> { \"\", [] }\n",
      "def e(v): v\nlet p = Two<string, [int]> { \"\", [] }\n" },
    { "def e() -> int, [ string ]: return 1, []\n", "def e() -> int, [string]: return 1, []\n" },
    // g on the line before (5) is called with it.
    { "print(g\n(5))\nprint(g\n  (5))\n", "print(g\n(5))\nprint(g\n      (5))\n" },
    { "print(a \nor b)\n", "print(a\n      or b)\n" },
    { "print(g\n// c\n(5))\nprint(g\n  // c\n(5))\n", "print(g\n// c\n(5))\nprint(g\n      // c\n      (5))\n" },
    // A line break in a string's interpolation, in brackets.
    { "print \"{two(1,\n2)}\"\n", "print \"{two(1,\n2)}\"\n" },
    // A name that's both an enum value and a function: a space after it calls it.
    { "print(red+1)\nprint(red-1)\nprint(red -1)\nprint(a-1)\n",
      "print(red+1)\nprint(red-1)\nprint(red -1)\nprint(a - 1)\n" },
    // Conditions after what the parser knows isn't a call.
    { "if a -1: b = 1\nif q.y [0]: c = 1\nif q.f (1): d = 1\nwhile i -1: i--\n",
      "if a - 1: b = 1\nif q.y [0]: c = 1\nif q.f (1): d = 1\nwhile i - 1: i--\n" },
    // A body is indented from where its header starts.
    { "let x =\n  h(1) y:\n    y + 1\n", "let x =\n    h(1) y:\n        y + 1\n" },
    { "let x = 1 +\n  2 +\n    h(1) y:\n      y\n",
      "let x = 1 +\n    2 +\n        h(1) y:\n            y\n" },
    { "let z = q\n  .m(1) x:\n    x\n", "let z = q\n    .m(1) x:\n        x\n" },
    { "print(if a:\n1\nelse:\n2\n)\n", "print(if a:\n          1\n      else:\n          2\n)\n" },
    // Comments.
    { "let e = 1 /*/ x */ + 2\n", "let e = 1 /*/ x */ + 2\n" },
    { "print(g/* c */(1))\n", "print(g/* c */(1))\n" },
    { "print(\n  /* c */ 1)\n", "print(\n    /* c */ 1)\n" },
    { "two(16 /* h */, 0)\nf(/* c */ 1)\nf(1 /* d */)\n",
      "two(16 /* h */, 0)\nf(/* c */ 1)\nf(1 /* d */)\n" },
    { "def e():\n  // h\n\n  g(1)\n", "def e():\n    // h\n\n    g(1)\n" },
    { "let z = q\n\n  // c\n  .f(1)\n", "let z = q\n\n    // c\n    .f(1)\n" },
    { "let z = q\n// c\n// e\n  .f(1)\n", "let z = q\n    // c\n    // e\n    .f(1)\n" },
    // Whitespace before a string with interpolations, for a call without parentheses.
    { "print /* c */ \"{a}\"\n", "print /* c */ \"{a}\"\n" },
    { "print(g\n    \"{a}\")\n", "print(g\n      \"{a}\")\n" },
    // Only an operator stays tight after such a name.
    { "print((red) + 1)\nlet v=red\nif red: 1 else: 2\n", "print((red) + 1)\nlet v = red\nif red: 1 else: 2\n" },
    // Type arguments glued to a comment, and a variable named like a namespace.
    { "def p<T>(v): return v\np/* c */<int>(1)\n", "def p<T>(v): return v\np/* c */<int>(1)\n" },
    { "let tcp = q\nprint(tcp .x)\n", "let tcp = q\nprint(tcp .x)\n" },
    // The parameters of a function value.
    { "t(1)(x:int): x\nfor([ 1 ])(e:int, j:int): e\n", "t(1) (x:int): x\nfor([ 1 ]) (e:int, j:int): e\n" },
};

static const string_view refusals[] = {
    "if a:\n    b = 1\n  c = 1\n",
    "a = 1\nb = \"c\n",
    "let x = 1;\n",
    "f(a\n",
    "if a:\n\tb = 1\n    c = 1\n",
    "a /* no end",
    "let = 1\n",
    "/* c */ a = 1\n",
    "print(no_such_name)\n",
    string_view("a = 1\0b\n", 8),
};

static int failures = 0;

static void Failed(string_view what, string_view input) {
    failures++;
    std::cout << what << ": " << input.substr(0, 2000) << "\n";
}

static bool IsBug(string_view error) { return error.find("formatter bug") != string_view::npos; }

static string exe;

// The program with main file `path` is what gets read from now on.
static void SetProgram(const string &path) {
    data_dirs.clear();
    InitPlatform(GetMainDirFromExePath(exe), path, false, LoadSource);
}

// The result of formatting `text` as `path`, the main file of the program.
static string FormatText(const string &path, string_view text, string &error) {
    SetProgram(path);
    auto dir = StripFilePart(path);
    auto name = StripDirPart(path);
    Overlay()[path] = string(text);
    vector<FormattedFile> files;
    error = FormatProgram(name, dir, files);
    Overlay().erase(path);
    if (!error.empty()) return {};
    for (auto &f : files) if (f.path == path) return f.result;
    error = "no result";
    return {};
}

// Whether it can be formatted, to something that formats to itself.
static bool FormatsToItself(const string &path, string_view text, string &once) {
    string error;
    once = FormatText(path, text, error);
    if (!error.empty()) {
        if (IsBug(error)) Failed(error, text);
        return false;
    }
    auto again = FormatText(path, once, error);
    if (!error.empty() || again != once) Failed(cat(path, ": formatting it twice changes it"), text);
    return true;
}

// Reproducible on every platform.
struct Random {
    uint64_t state;
    int operator()(int n) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return int((state >> 33) % uint64_t(n));
    }
};

// Other whitespace, the same meaning. Keeps whether there is any where the parser looks at that.
static string Scramble(const Syntax &syntax, Random &rnd) {
    string_view src = syntax.src;
    auto factor = 1 + rnd(3);
    string out(rnd(3), '\n');
    int depth = 0;
    const Token *prev = nullptr, *prevcode = nullptr, *beforeprev = nullptr;
    for (auto &t : syntax.tokens) {
        if (prev) {
            auto between = src.substr(prev->end, t.start - prev->end);
            auto newlines = (int)std::count(between.begin(), between.end(), '\n');
            if (newlines) {
                if (!rnd(3)) out.append(1 + rnd(3), ' ');
                out.append(newlines > 1 ? newlines + rnd(3) : 1, '\n');
                auto orig = (int)(between.size() - between.rfind('\n') - 1);
                // In brackets indentation doesn't matter, but whether there is any does.
                out.append(depth && orig ? 1 + rnd(12) : orig * factor, ' ');
            } else if (t.role == TR_TIGHT || prev->role == TR_TIGHT) {
                out += between;
            } else if ((prev->t != T_NONE && IsName(*prev)) ||
                       (prev->t == T_MINUS && beforeprev && IsName(*beforeprev))) {
                if (!between.empty()) out.append(1 + rnd(3), ' ');
            } else {
                auto n = rnd(4);
                if (prev->t == T_NONE || t.t == T_NONE) n = max(n, 1);
                else if (!n && Merges(*prev, t)) n = 1;
                out.append(n, ' ');
            }
        }
        out += t.text;
        if (t.t != T_NONE) {
            if (IsOpener(t.t)) depth++;
            if (IsCloser(t.t)) depth--;
            beforeprev = prevcode;
            prevcode = &t;
        }
        prev = &t;
    }
    out.append(1 + rnd(3), '\n');
    if (rnd(5)) return out;
    string crlf;
    for (auto c : out) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    return crlf;
}

static vector<string> Lines(string_view s) {
    vector<string> lines;
    for (;;) {
        auto n = s.find('\n');
        lines.push_back(string(s.substr(0, n)));
        if (n == string_view::npos) return lines;
        s.remove_prefix(n + 1);
    }
}

static string Joined(const vector<string> &lines, size_t from = 0) {
    string out;
    for (auto i = from; i < lines.size(); i++) append(out, i > from ? "\n" : "", lines[i]);
    return out;
}

// The code damaged somewhere.
static string Mutate(string_view src, string_view other, Random &rnd) {
    static const string_view snippets[] = {
        "(", ")", "[", "]", "{", "}", ":", ",", "-", "<", ">", "=", ".", "::", "if", "x", "1",
        "\"s\"", "//", "/*", "*/", "\"\"\"", "\t", "  ", "\n", "\n    ", "def f():", "fn x:", "->",
    };
    auto lines = Lines(src);
    auto l = rnd((int)lines.size());
    switch (rnd(10)) {
        case 0:
            lines.erase(lines.begin() + l);
            return Joined(lines);
        case 1:
            lines.insert(lines.begin() + l, lines[l]);
            return Joined(lines);
        case 2:
            lines[l] = string(1 + rnd(4), ' ') + lines[l];
            return Joined(lines);
        case 3:
            lines[l].erase(0, min(lines[l].find_first_not_of(' '), size_t(4)));
            return Joined(lines);
        case 4:
            if (l + 1 < (int)lines.size()) {
                lines[l] += " " + lines[l + 1];
                lines.erase(lines.begin() + l + 1);
            }
            return Joined(lines);
        case 5: {
            // Spliced: the start of this, the rest of another file.
            auto others = Lines(other);
            lines.resize(l);
            return cat(Joined(lines), "\n", Joined(others, rnd((int)others.size())));
        }
        default: {
            auto at = (size_t)rnd(int(src.size()) + 1);
            if (rnd(2)) {
                auto snippet = snippets[rnd((int)std::size(snippets))];
                return cat(src.substr(0, at), snippet, src.substr(at));
            }
            return cat(src.substr(0, at), src.substr(min(at + 1 + rnd(3), src.size())));
        }
    }
}

static string ReadAll(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void WriteAll(const fs::path &path, string_view s) {
    std::ofstream out(path, std::ios::binary);
    out << s;
}

// What `compiler` makes of `file`, run from the current directory: its parse dump, or its errors
// without where they are, which formatting can change. The dump is of what gets typechecked, so has
// nothing of code that isn't used.
static string ParseDump(const string &compiler, const string &file) {
    auto dump = fs::path(file).parent_path() / "parsedump.txt";
    fs::remove(dump);
    fs::remove("lobsterfmt_output.txt");
    auto command = cat("\"", compiler, "\" --silent --compile-only --parsedump \"", file,
                       "\" > lobsterfmt_output.txt 2>&1");
    if (!std::system(command.c_str())) return ReadAll(dump);
    string errors = "errors:\n";
    for (auto &line : Lines(ReadAll("lobsterfmt_output.txt"))) {
        auto at = line.find(": error: ");
        if (at != string::npos) append(errors, line.substr(at), "\n");
    }
    return errors;
}

// With everything under the current directory scrambled and then formatted, compares what compiles.
static void CompareWithCompiler(const string &compiler, const string &path, Random &rnd, int &same,
                                int &errors) {
    vector<string> programs;
    for (auto &e : fs::recursive_directory_iterator(".")) {
        if (e.path().extension() != ".lobster") continue;
        programs.push_back(fs::absolute(e.path()).lexically_normal().generic_string());
    }
    sort(programs.begin(), programs.end());
    vector<string> before;
    for (auto &file : programs) before.push_back(ParseDump(compiler, file));
    auto root = fs::current_path().generic_string() + "/";
    // Scrambled first, since most code is formatted already.
    set<string> scrambled;
    vector<pair<string, string>> writes;
    for (auto &file : programs) {
        SetProgram(file);
        Recorder recorder;
        if (!ReadProgram(StripDirPart(file), recorder).empty()) continue;
        for (auto &f : recorder.files) {
            if (!f || !string_view(f->path).starts_with(root)) continue;
            if (scrambled.insert(f->path).second) writes.push_back({ f->path, Scramble(*f, rnd) });
        }
    }
    for (auto &[file, text] : writes) WriteAll(file, text);
    for (auto [i, file] : enumerate(programs)) {
        SetProgram(file);
        vector<FormattedFile> files;
        auto error = FormatProgram(StripDirPart(file), StripFilePart(file), files);
        if (!error.empty()) {
            // Part of a program, or it doesn't compile to begin with.
            if (!before[i].starts_with("errors:") || IsBug(error)) {
                Failed(cat(path, "/", file, ": refused: ", error), "");
            }
            continue;
        }
        for (auto &f : files) WriteAll(f.path, f.result);
    }
    for (auto [i, file] : enumerate(programs)) {
        if (before[i].starts_with("errors:")) errors++;
        else same++;
        if (ParseDump(compiler, file) != before[i]) {
            Failed(cat(path, "/", file, ": the parse dump or errors change"), "");
        }
    }
}

int main(int argc, char **argv) {
    min_output_level = OUTPUT_ERROR;
    exe = argv[0];
    uint64_t seed = 1;
    int rounds = 3;
    string compiler;
    vector<string> paths;
    for (int i = 1; i < argc; i++) {
        string_view arg = argv[i];
        if (arg == "-seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
        else if (arg == "-rounds" && i + 1 < argc) rounds = std::stoi(argv[++i]);
        else if (arg == "-compiler" && i + 1 < argc) compiler = fs::absolute(argv[++i]).string();
        else paths.push_back(argv[i]);
    }
    auto dir = (fs::temp_directory_path() / "lobsterfmt_golden/").generic_string();
    auto golden = dir + "golden.lobster";
    Overlay()[dir + "prelude.lobster"] = string(prelude);
    const string_view header = "import .prelude\n";
    for (auto [input, want] : goldens) {
        string error, once;
        auto got = FormatText(golden, cat(header, input), error);
        if (!error.empty()) Failed(error, input);
        else if (got != cat(header, want)) Failed(cat("got: ", got, "\nwanted: ", want), input);
        FormatsToItself(golden, cat(header, want), once);
    }
    for (auto src : refusals) {
        string error;
        FormatText(golden, cat(header, src), error);
        if (error.empty()) Failed("should have been refused", src);
        else if (IsBug(error)) Failed(error, src);
    }
    // Not the compiler's own modules, also from a main file given without a directory.
    {
        auto start = fs::current_path();
        fs::create_directories(dir);
        fs::current_path(dir);
        Overlay()["golden.lobster"] = "import std\nprint(1)\n";
        SetProgram("golden.lobster");
        vector<FormattedFile> formatted;
        auto error = FormatProgram("golden.lobster", fs::current_path().generic_string() + "/",
                                   formatted);
        if (!error.empty() || formatted.size() != 1) {
            Failed(cat("a program's imports from elsewhere: ", error, " ", formatted.size()), "");
        }
        Overlay().erase("golden.lobster");
        fs::current_path(start);
    }
    // Not the compiler's own modules when the program's folder holds them.
    {
        auto root = fs::absolute(MainDir()).lexically_normal().generic_string();
        auto inroot = root + "lobsterfmt_test_main.lobster";
        Overlay()[inroot] = "import std\nprint(1)\n";
        data_dirs.clear();
        InitPlatform(GetMainDirFromExePath(exe), inroot, false, LoadSource);
        vector<FormattedFile> formatted;
        auto error = FormatProgram("lobsterfmt_test_main.lobster", root, formatted);
        if (!error.empty() || formatted.size() != 1) {
            Failed(cat("a program next to the modules: ", error, " ", formatted.size()), "");
        }
        Overlay().erase(inroot);
    }
    // Only the file asked for, read as part of the program.
    {
        auto part = dir + "part.lobster";
        auto main = cat(header, "import .part\nlet  z=1\n");
        Overlay()[part] = "let  y=a\n";
        Overlay()[golden] = main;
        SetProgram(golden);
        set<string> only { part };
        vector<FormattedFile> formatted;
        auto error = FormatProgram("golden.lobster", dir, formatted, &only);
        if (!error.empty() || formatted.size() != 1 || formatted[0].result != "let y = a\n") {
            Failed(cat("part of a program: ", error), main);
        }
        // Alone it doesn't know `a`.
        Overlay()[golden] = Overlay()[part];
        SetProgram(golden);
        formatted.clear();
        if (FormatProgram("golden.lobster", dir, formatted).empty()) {
            Failed("should have been refused alone", part);
        }
    }
    Overlay().clear();
    vector<fs::path> files;
    for (auto &path : paths) {
        for (auto &e : fs::recursive_directory_iterator(path)) {
            if (e.path().extension() != ".lobster") continue;
            files.push_back(fs::absolute(e.path()).lexically_normal());
        }
    }
    sort(files.begin(), files.end());
    vector<string> sources;
    for (auto &file : files) sources.push_back(ReadAll(file));
    Random rnd { seed };
    int formatted = 0, scrambled = 0, broken = 0, refused = 0, same = 0, errors = 0;
    for (auto [i, src] : enumerate(sources)) {
        auto path = files[i].generic_string();
        string result;
        if (!FormatsToItself(path, src, result)) continue;
        formatted++;
        for (int r = 0; r < rounds; r++) {
            SetProgram(path);
            Overlay()[path] = result;
            Recorder recorder;
            ReadProgram(StripDirPart(path), recorder);
            Overlay().clear();
            auto s = Scramble(*recorder.files[0], rnd);
            string error;
            auto got = FormatText(path, s, error);
            scrambled++;
            if (!error.empty() || got != result) Failed(cat(path, ": scrambled, ", error), s);
            auto m = src;
            for (int k = 1 + rnd(3); k; k--) {
                m = Mutate(m, sources[rnd((int)sources.size())], rnd);
            }
            string mresult;
            broken++;
            if (!FormatsToItself(path, m, mresult)) refused++;
        }
    }
    if (!compiler.empty()) {
        // In a copy, for what it imports to be next to it.
        auto start = fs::current_path();
        for (auto &path : paths) {
            auto copy = fs::temp_directory_path() / cat("lobsterfmt_test_", seed);
            fs::remove_all(copy);
            fs::copy(path, copy, fs::copy_options::recursive);
            fs::current_path(copy);
            CompareWithCompiler(compiler, path, rnd, same, errors);
            fs::current_path(start);
            fs::remove_all(copy);
        }
    }
    std::cout << std::size(goldens) << " golden, " << std::size(refusals) << " refused, "
              << files.size() << " files (" << formatted << " formatted as programs), " << scrambled
              << " scrambled, " << broken << " broken (" << refused << " refused), " << same
              << " parse dumps the same, " << errors << " the same errors: " << failures
              << " failures\n";
    return failures ? 1 : 0;
}
