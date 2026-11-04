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

#ifndef LOBSTERFMT_FORMAT
#define LOBSTERFMT_FORMAT

// Formats Lobster source in the layout of docs/source/style_guide.md, like gofmt: no options.
// Only whitespace between tokens changes, and never which line a token is on (other than blank
// lines). What it knows of the code comes from the compiler's own parser, see syntax.h, so it takes
// a whole program, and formats the files of it that are next to or under its main file. The result
// is read again and refused unless it reads the same, see Check.
//
// Refused, other than what doesn't parse: code after a comment at the start of a line outside
// brackets, since the lexer measures that line's indentation from the end of the comment, and
// what's nested too deeply for this to recurse on, see Reader::Nested.

#include "syntax.h"

#include <filesystem>

namespace lobsterfmt {

// Whether this can start an argument to a call without (): `f x`, `f -1`.
inline bool CanStartArgument(const Token &t) {
    switch (t.t) {
        case T_LEFTPAREN: case T_LEFTBRACKET: case T_LEFTCURLY: case T_MINUS: case T_NEG:
        case T_INCR: case T_DECR:
            return true;
        default:
            return t.t != T_NONE && (IsWord(t) || IsNumber(t) || IsString(t));
    }
}

// Whether it ends a value, which a ( or [ after it calls or indexes.
inline bool EndsValue(const Token &t) {
    return IsNumber(t) || IsString(t) || IsIdent(t) || t.t == T_NIL || t.t == T_SUPER ||
           IsCloser(t.t) || (t.role == TR_GENERIC && t.t != T_LT) || t.role == TR_POSTFIX;
}

// Whether two tokens written together would lex as something else.
inline bool Merges(const Token &a, const Token &b) {
    if (IsOpener(a.t) || IsOpener(b.t) || IsCloser(a.t) || IsCloser(b.t)) return false;
    if (IsString(a) || IsString(b)) return false;
    // Only the sides that touch: a namespaced name is one token here, to the parser.
    auto left = a.text, right = b.text;
    if (IsWord(a)) left = left.substr(left.rfind('.') + 1);
    if (IsWord(b)) right = right.substr(0, right.find('.'));
    vector<pair<string, string>> filenames;
    auto both = cat(left, right);
    Lex lex("", filenames, {}, both, 1);
    auto first = lex.p - lex.source.get()->c_str();
    if (lex.num_errors || first != (ptrdiff_t)left.size()) return true;
    lex.Next();
    return lex.num_errors || lex.token == T_ENDOFFILE || *lex.p;
}

// The text of a token, without trailing whitespace on a // comment.
inline string_view TokenText(const Token &t) {
    if (!t.text.starts_with("//")) return t.text;
    auto text = t.text;
    while (!text.empty() && strchr(" \t\r\f", text.back())) text.remove_suffix(1);
    return text;
}

// The width of what follows the last line break, in characters.
inline int LastLineWidth(string_view s) {
    int n = 0;
    for (auto i = s.size(); i > 0 && s[i - 1] != '\n'; i--) {
        if ((unsigned char)s[i - 1] < 0x80 || (unsigned char)s[i - 1] >= 0xC0) n++;
    }
    return n;
}

const int indentwidth = 4;
// Of a line continuing the one before when a block follows that's indented once from that line,
// which the two need telling apart by.
const int contwidth = 8;
// Before a comment at the end of a line.
const int commentgap = 2;

inline bool Spaced(const Token &a, const Token &b) {
    auto ra = a.role, rb = b.role;
    if (b.t == T_COMMA) return false;
    if (a.t == T_COMMA) return true;
    if (rb == TR_PARAMS) return true;
    if (ra == TR_TIGHT) return false;
    if (rb == TR_POSTFIX || ra == TR_UNARY || ra == TR_OPNAME || a.t == T_OPERATOR) return false;
    if (a.t == T_INCLUDE && b.t == T_DOT) return true;  // import .file
    if (a.t == T_DOT || a.t == T_DOTDOT || a.t == T_TYPEIN || b.t == T_DOT || b.t == T_DOTDOT ||
        b.t == T_TYPEIN) return false;
    if (b.t == T_QUESTIONMARK) return false;
    if ((ra == TR_GENERIC && a.t == T_LT) || rb == TR_GENERIC) return false;
    if (b.t == T_COLON) return rb == TR_INHERIT;
    if (a.t == T_COLON) return ra != TR_TYPECOLON;
    if (a.t == T_LEFTPAREN || b.t == T_RIGHTPAREN) return false;
    if (a.t == T_LEFTBRACKET) return ra == TR_LIST && b.t != T_RIGHTBRACKET;
    if (b.t == T_RIGHTBRACKET) return rb == TR_LIST && a.t != T_LEFTBRACKET;
    if (a.t == T_LEFTCURLY) return b.t != T_RIGHTCURLY;
    if (b.t == T_RIGHTCURLY) return true;
    if (b.t == T_LEFTPAREN) {
        return !(EndsValue(a) || a.t == T_FOR || a.t == T_WHILE || a.t == T_LAMBDA);
    }
    if (b.t == T_LEFTBRACKET) return rb != TR_INDEX;
    return true;
}

// Whether the parser reads b differently with or without whitespace between a and it: `f(x)` and
// `f (x)`.
inline bool WsDecides(const Token &a, const Token &b) {
    if (b.role == TR_TIGHT) return true;
    return IsName(a) && a.role != TR_DEFNAME && b.t == T_LEFTPAREN;
}

// Whether the parser can read b differently without whitespace between a and it: also `f x` and
// `f -1` (if f is a function).
inline bool WsMatters(const Token &a, const Token &b) {
    return WsDecides(a, b) || (IsName(a) && a.role != TR_DEFNAME && CanStartArgument(b));
}

inline bool Separated(const Token &a, const Token &b) {
    // Keep what's there where the parser looks.
    if (WsDecides(a, b) || (b.ws && WsMatters(a, b))) return b.ws != 0;
    // `a<b<c>>`, which lexes as two tokens in a type only.
    if (a.role == TR_GENERIC && b.role == TR_GENERIC) return Spaced(a, b);
    return Spaced(a, b) || Merges(a, b);
}

// Writes the formatted source of a Syntax.
struct Layout {
    // A level of indentation, as the compiler's lexer keeps them (Lex::indentstack).
    struct Level {
        int orig;
        // Of the lines at this level.
        int indent;
        // A line continuing the one before (after an operator), indented more.
        bool cont;
    };

    struct Bracket {
        int indent;
        bool hanging;
        int align = -1;
    };

    // What a line is to the lexer: one that starts a statement, continues the one before after an
    // operator, or after a ., or is in brackets (where its indentation doesn't matter), or a
    // comment.
    enum RowKind { ROW_COMMENT, ROW_BLOCK, ROW_CONT, ROW_DOT, ROW_BRACKET };

    struct Row {
        vector<Token *> tokens;
        int orig;
        int blanks;
        RowKind kind = ROW_COMMENT;
    };

    struct Trailing {
        int line;
        string code;
        string_view comment;
    };

    Syntax &syntax;
    vector<Row> rows;
    vector<Level> levels { { 0, 0, false } };
    vector<Bracket> brackets;
    vector<string> out;
    // The code and comment of each line of `out` that ends in one, to line them up.
    vector<Trailing> trailing;
    Token *prevcode = nullptr;
    // Whether the last line ended with the : of a block.
    bool opensblock = false;
    bool seencode = false;
    // Of each line, the next one that starts a statement, or -1.
    vector<int> nextblock;
    // Of each line so far, its indentation.
    vector<int> rowindent;
    // Of each token, its line.
    vector<int> tokenrow;

    explicit Layout(Syntax &syntax) : syntax(syntax) {}

    static Token *FirstCode(const Row &r) {
        for (auto t : r.tokens) if (t->t != T_NONE) return t;
        return nullptr;
    }

    void MakeRows() {
        auto src = syntax.src;
        int end = 0;
        for (auto &t : syntax.tokens) {
            int newlines = 0;
            for (auto c : src.substr(end, t.start - end)) newlines += c == '\n';
            if (newlines || rows.empty()) {
                auto linestart = src.substr(0, t.start).rfind('\n');
                linestart = linestart == string_view::npos ? 0 : linestart + 1;
                rows.push_back({ { &t }, t.start - int(linestart), max(0, newlines - 1) });
            } else {
                rows.back().tokens.push_back(&t);
            }
            tokenrow.push_back((int)rows.size() - 1);
            end = t.end;
        }
        for (auto &r : rows) {
            auto first = FirstCode(r);
            if (!first) continue;
            if (first->depth) r.kind = ROW_BRACKET;
            else if (first->t == T_DOT) r.kind = ROW_DOT;
            else if (first->i > 0 && syntax.code[first->i - 1]->cont) r.kind = ROW_CONT;
            else r.kind = ROW_BLOCK;
        }
        nextblock.resize(rows.size());
        int upcoming = -1;
        for (int k = (int)rows.size() - 1; k >= 0; k--) {
            nextblock[k] = upcoming;
            if (rows[k].kind == ROW_BLOCK) upcoming = k;
        }
    }

    // Puts a line that starts a statement at `orig` on `ls`, as Lex::Next does, at `indent` if it
    // starts a block. False if it's at no level.
    static bool Place(vector<Level> &ls, int orig, int indent) {
        for (;;) {
            if (orig > ls.back().orig) {
                ls.push_back({ orig, indent, false });
            } else if (orig < ls.back().orig) {
                auto cont = false;
                while (orig < ls.back().orig) {
                    cont = ls.back().cont;
                    ls.pop_back();
                }
                if (cont) continue;
                return orig == ls.back().orig;
            }
            return true;
        }
    }

    // Of the line that `first` starts if it starts a block: once from where its header starts.
    int BlockIndentOf(const Token *first) const {
        // A comment can get here for a header on a line that is still to come.
        if (first && first->owner >= 0 && tokenrow[first->owner] < (int)rowindent.size()) {
            return rowindent[tokenrow[first->owner]] + indentwidth;
        }
        return levels.back().indent + indentwidth;
    }

    int BracketIndent(const Token *first) const {
        auto &b = brackets.back();
        if (first && IsCloser(first->t)) return b.indent;
        return b.hanging || b.align < 0 ? b.indent + indentwidth : b.align;
    }

    // Once from the line it continues, or twice if it's in the header of a block, whose body is
    // indented once from where the header starts, which the two need telling apart by.
    int ContIndent(int r, const Token &first) {
        auto &line = rows[r];
        auto p = levels.back();
        if (line.orig < p.orig) {
            syntax.Refuse("line continuation can't indent less than the previous line",
                          first.start);
        }
        if (line.orig <= p.orig && p.cont) return p.indent;
        auto indent = p.indent + (first.inheader ? contwidth : indentwidth);
        if (line.orig > p.orig) levels.push_back({ line.orig, indent, true });
        return indent;
    }

    int BlockIndent(int r, const Token &first) {
        auto &line = rows[r];
        // The lexer doesn't look at the indentation of the first line, unless it follows another.
        if (!seencode) return 0;
        if (!Place(levels, line.orig, BlockIndentOf(&first))) {
            syntax.Refuse("inconsistent dedent", first.start);
        }
        return levels.back().indent;
    }

    // Of a comment on a line of its own: it goes with the code around it, at the deepest level (of
    // the one it's in, or the block the next line starts) not deeper than it was.
    int CommentIndent(int r) const {
        auto orig = rows[r].orig;
        int best = 0, bestorig = -1;
        auto consider = [&](const Level &l) {
            if (l.orig <= orig && l.orig >= bestorig) {
                bestorig = l.orig;
                best = l.indent;
            }
        };
        for (auto &l : levels) consider(l);
        auto after = levels;
        if (auto n = nextblock[r]; n >= 0) {
            auto indent = BlockIndentOf(FirstCode(rows[n]));
            if (Place(after, rows[n].orig, indent)) consider(after.back());
        }
        return best;
    }

    // Whether the line is in brackets after a name, where the next code has no whitespace before
    // it, which comment lines between would add to: `g\n(5)` calls g, `g\n (5)` (or `g \n(5)`)
    // calls it with (5).
    bool Unindented() const {
        auto p = prevcode;
        if (!p || p->i + 1 >= (int)syntax.code.size()) return false;
        return !syntax.code[p->i + 1]->ws && IsName(*p);
    }

    // Whether whitespace before the next code is up to what's before it now.
    bool Glued() const {
        auto p = prevcode;
        if (!p || p->i + 1 >= (int)syntax.code.size()) return false;
        auto &n = *syntax.code[p->i + 1];
        return !n.ws && WsMatters(*p, n);
    }

    int IndentOf(int r) {
        auto &line = rows[r];
        auto &first = *line.tokens[0];
        auto fc = FirstCode(line);
        if (first.t == T_NONE && fc && line.kind != ROW_BRACKET) {
            syntax.Refuse("code after a comment at the start of a line", fc->start);
        }
        switch (line.kind) {
            case ROW_BRACKET:
                if (Unindented()) return 0;
                // A body on a line of its own, where the lexer has no idea of blocks.
                if (fc->owner >= 0 && tokenrow[fc->owner] != r) {
                    auto &header = rows[tokenrow[fc->owner]];
                    auto startsline = FirstCode(header) == &syntax.tokens[fc->owner];
                    return startsline ? BlockIndentOf(fc) : BracketIndent(nullptr) + indentwidth;
                }
                return BracketIndent(fc);
            case ROW_DOT: return levels.back().indent + indentwidth;
            case ROW_CONT: return ContIndent(r, *fc);
            case ROW_BLOCK: return BlockIndent(r, *fc);
            default:
                if (!brackets.empty()) return Unindented() ? 0 : BracketIndent(nullptr);
                if (prevcode && prevcode->cont) return levels.back().indent + indentwidth;
                return CommentIndent(r);
        }
    }

    // The line rows[r], indented by `indent`, and whether it ends in a comment, in `comment`.
    string WriteRow(int r, int indent, Trailing &comment, bool &hascomment) {
        auto &tokens = rows[r].tokens;
        string line(indent, ' ');
        Token *prev = nullptr;
        int align = -1;
        hascomment = false;
        for (auto [i, t] : enumerate(tokens)) {
            // Whitespace next to a comment that would be some before the code after it.
            auto glued = (t->t == T_NONE || (prev && prev->t == T_NONE)) && Glued();
            if (t->t == T_NONE && t->text.starts_with("//") && prev && !glued) {
                comment = { 0, line, TokenText(*t) };
                hascomment = true;
            }
            if (hascomment && t->t == T_NONE) {
                line.append(commentgap, ' ');
            } else if (prev && !glued) {
                if (t->t == T_NONE) {
                    if (prev->t != T_LEFTPAREN) line += ' ';
                } else if (prev->t == T_NONE) {
                    auto tight = (t->t == T_COMMA || IsCloser(t->t)) && prevcode &&
                                 !Spaced(*prevcode, *t);
                    if (!tight) line += ' ';
                } else if (Separated(*prev, *t)) {
                    line += ' ';
                }
            }
            if (t->t != T_NONE && align >= 0) {
                brackets[align].align = LastLineWidth(line);
                align = -1;
            }
            line += TokenText(*t);
            if (t->t != T_NONE) {
                if (IsOpener(t->t)) {
                    auto hanging = true;
                    for (size_t k = i + 1; k < tokens.size(); k++) {
                        if (tokens[k]->t != T_NONE) hanging = false;
                    }
                    brackets.push_back({ indent, hanging });
                    if (!hanging) align = int(brackets.size()) - 1;
                } else if (IsCloser(t->t)) {
                    brackets.pop_back();
                }
                prevcode = t;
            }
            prev = t;
        }
        return line;
    }

    string Run() {
        MakeRows();
        // Comments on lines of their own between continuation lines, which go with the line after.
        vector<int> pending;
        for (int r = 0; r < (int)rows.size(); r++) {
            auto pc = prevcode;
            auto indent = IndentOf(r);
            Trailing comment;
            bool hascomment;
            auto line = WriteRow(r, indent, comment, hascomment);
            auto fc = FirstCode(rows[r]);
            rowindent.push_back(indent);
            if (!out.empty() && !opensblock && rows[r].blanks) out.push_back("");
            auto next = r + 1;
            while (next < (int)rows.size() && rows[next].kind == ROW_COMMENT) next++;
            auto beforedot = next < (int)rows.size() && rows[next].kind == ROW_DOT;
            if (!fc && brackets.empty() && ((pc && pc->cont) || !pending.empty() || beforedot)) {
                pending.push_back((int)out.size());
            } else if (fc) {
                for (auto at : pending) {
                    auto n = out[at].find_first_not_of(' ');
                    out[at] = string(indent, ' ') + out[at].substr(n);
                }
                pending.clear();
            }
            if (hascomment && comment.code.find('\n') == string::npos) {
                comment.line = (int)out.size();
                trailing.push_back(comment);
            }
            out.push_back(line);
            opensblock = false;
            if (fc) {
                seencode = true;
                auto last = prevcode;
                opensblock = last && brackets.empty() &&
                             ((last->t == T_COLON && last->role == TR_NONE) ||
                              last->role == TR_BLOCK);
            }
        }
        // Comments at the end of consecutive lines line up, as gofmt does.
        for (size_t i = 0; i < trailing.size();) {
            auto j = i;
            while (j + 1 < trailing.size() && trailing[j + 1].line == trailing[j].line + 1) j++;
            int width = 0;
            for (auto k = i; k <= j; k++) width = max(width, LastLineWidth(trailing[k].code));
            for (auto k = i; k <= j; k++) {
                auto &c = trailing[k];
                auto gap = width - LastLineWidth(c.code) + commentgap;
                out[c.line] = cat(c.code, string(gap, ' '), c.comment);
            }
            i = j + 1;
        }
        string result;
        for (auto &l : out) append(result, l, "\n");
        return result;
    }
};

// A last resort for text between tokens, which comments are the only thing allowed in.
inline string NonSpace(string_view s) {
    string r;
    for (auto c : s) if (!strchr(" \t\r\f\n", c)) r += c;
    return r;
}

// Why `after` doesn't read as `syntax` does: its text but for whitespace, tokens, blocks and the
// whitespace the parser looks at, or nothing. All of which would be a bug in this formatter.
inline string Check(const Syntax &syntax, const Syntax &after) {
    auto bug = [&](string_view what) {
        return cat(syntax.filename, ": error: formatting would change ", what,
                   " (this is a formatter bug)");
    };
    if (NonSpace(syntax.src) != NonSpace(after.src)) return bug("more than whitespace");
    if (after.tokens.size() != syntax.tokens.size()) return bug("the tokens");
    for (auto [i, t] : enumerate(syntax.tokens)) {
        if (TokenText(t) != TokenText(after.tokens[i])) return bug("the tokens");
        if (t.role != after.tokens[i].role) return bug("how the parser reads it");
    }
    for (int i = 1; i < (int)syntax.code.size(); i++) {
        auto &a = *syntax.code[i - 1];
        auto &t = *syntax.code[i];
        if (t.blocks != after.code[i]->blocks) return bug("the indentation");
        // A line break outside brackets is a token of its own.
        if (!t.depth && syntax.NewlineBefore(i)) continue;
        auto ws = after.code[i]->ws;
        if (WsDecides(a, t) ? (t.ws > 0) != (ws > 0) : t.ws && !ws && WsMatters(a, t)) {
            return bug("whitespace the parser uses");
        }
    }
    return {};
}

// The text of files by the path they load from, instead of what's on disk.
inline map<string, string> &Overlay() {
    static map<string, string> overlay;
    return overlay;
}

// Lines end in LF as far as anything here knows.
inline int64_t LoadSource(string_view_nt path, string *dest, int64_t start, int64_t len) {
    auto it = Overlay().find(string(path.sv));
    if (it != Overlay().end()) {
        *dest = it->second;
    } else {
        auto size = DefaultLoadFile(path, dest, start, len);
        if (size < 0) return size;
    }
    string src;
    for (size_t i = 0; i < dest->size(); i++) {
        if ((*dest)[i] != '\r' || i + 1 == dest->size() || (*dest)[i + 1] != '\n') {
            src += (*dest)[i];
        }
    }
    dest->swap(src);
    return (int64_t)dest->size();
}

// What it needs of the builtins it doesn't link, see engine_names.h.
extern void RegisterEngineNames(NativeRegistry &nfr);

inline NativeRegistry &Natives() {
    static NativeRegistry *nfr = nullptr;
    if (!nfr) {
        nfr = new NativeRegistry();
        RegisterCoreLanguageBuiltins(*nfr);
        RegisterEngineNames(*nfr);
        nfr->DoneRegistering();
    }
    return *nfr;
}

// The tokens of the program with main file `fn`, returning the errors.
inline string ReadProgram(string_view fn, Recorder &recorder) {
    string source;
    if (LoadFile(fn, &source) < 0) return cat("can't open file: ", fn);
    vector<pair<string, string>> filenames;
    auto errors = ParseProgram(Natives(), fn, source, recorder, filenames);
    for (auto &f : recorder.files) if (f) f->Finish();
    return errors;
}

struct FormattedFile {
    string path;
    string filename;
    string result;
};

// Formats the program with main file `fn`, in the absolute path `maindir`. On return the files of
// it in `maindir` or under it are in `files`, unless it returns why it can't be formatted safely.
// With `only`, just those of it by the path they load from.
inline string FormatProgram(string_view fn, string_view maindir, vector<FormattedFile> &files,
                            const set<string> *only = nullptr) {
    Recorder before;
    auto errors = ReadProgram(fn, before);
    if (!errors.empty()) return errors;
    map<string, string> changed;
    for (auto &f : before.files) {
        if (!f) continue;
        // Never the compiler's own modules, which it finds elsewhere.
        auto path = std::filesystem::absolute(f->path).lexically_normal().generic_string();
        if (!(only ? only->count(f->path) != 0 : string_view(path).starts_with(maindir))) continue;
        if (auto nul = f->src.find('\0'); nul != string::npos) {
            f->Refuse("a NUL character", (int)nul);
        }
        auto result = Layout(*f).Run();
        if (!f->error.empty()) return f->error;
        if (result != f->src) changed[f->path] = result;
        files.push_back({ f->path, f->filename, result });
    }
    if (changed.empty()) return {};
    auto kept = Overlay();
    for (auto &[path, text] : changed) Overlay()[path] = text;
    Recorder after;
    errors = ReadProgram(fn, after);
    Overlay() = kept;
    if (!errors.empty()) {
        return cat(fn, ": error: formatting would break it (this is a formatter bug):\n", errors);
    }
    if (after.files.size() != before.files.size()) {
        return cat(fn, ": error: this is a formatter bug");
    }
    for (auto [i, f] : enumerate(before.files)) {
        if (!f || !changed.count(f->path)) continue;
        auto error = Check(*f, *after.files[i]);
        if (!error.empty()) return error;
    }
    return {};
}

}  // namespace lobsterfmt

#endif  // LOBSTERFMT_FORMAT
