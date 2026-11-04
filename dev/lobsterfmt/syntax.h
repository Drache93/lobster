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

#ifndef LOBSTERFMT_SYNTAX
#define LOBSTERFMT_SYNTAX

// What the compiler's parser finds out about each token of a program, see TokenObserver. It knows
// what names mean only from the whole program, so that's what gets parsed.

#include "lobster/stdafx.h"
#include "lobster/vmdata.h"
#include "lobster/lex.h"
#include "lobster/compiler.h"

namespace lobsterfmt {

using namespace lobster;

// A comment has type T_NONE. A string with interpolations is one T_STR.
struct Token {
    TType t = T_NONE;
    string_view text;
    int start = 0;
    int end = 0;
    // Lex::whitespacebefore, which some parses depend on.
    int ws = 0;
    // Lex::cont: whether a line break after it continues the line, as the parser left it.
    bool cont = false;
    TokenRole role = TR_NONE;
    // Where it is among the tokens that aren't comments, and how many brackets it's in.
    int i = -1;
    int depth = 0;
    // The T_LINEFEED, T_INDENT and T_DEDENT before it.
    string blocks;
    // If it's the first token of a block's body, the index of the token its header starts with.
    int owner = -1;
    // Whether it's in the header of an indented block, other than the token that starts that.
    bool inheader = false;
    // A . with whitespace before it after a namespace's name: without any it'd be one token.
    bool keepspace = false;
};

// A keyword, identifier, or inf or nan, which lex as floats.
inline bool IsWord(const Token &t) { return t.t != T_NONE && Lex::IsIdentStart(t.text[0]); }
inline bool IsIdent(const Token &t) { return t.t == T_IDENT || (t.t == T_FLOAT && IsWord(t)); }
inline bool IsString(const Token &t) { return t.t == T_STR || (t.t == T_INT && t.text[0] == '\''); }
inline bool IsNumber(const Token &t) {
    return (t.t == T_INT || t.t == T_FLOAT) && !IsWord(t) && !IsString(t);
}
inline bool IsOpener(TType t) { return t == T_LEFTPAREN || t == T_LEFTBRACKET || t == T_LEFTCURLY; }
inline bool IsCloser(TType t) {
    return t == T_RIGHTPAREN || t == T_RIGHTBRACKET || t == T_RIGHTCURLY;
}

// Keywords that start statements or are operators. The rest (`int`, `resource`, `super`, ...) the
// parser reads much like identifiers.
inline bool IsStatementKeyword(TType t) {
    switch (t) {
        case T_CLASS: case T_FUN: case T_LAMBDA: case T_RETURN: case T_IS: case T_STRUCT:
        case T_INCLUDE: case T_FROM: case T_PROGRAM: case T_PRIVATE: case T_ENUM: case T_ENUM_FLAGS:
        case T_VAR: case T_CONST: case T_PAKFILE: case T_IF: case T_ELSE: case T_ELIF: case T_WHILE:
        case T_FOR: case T_SWITCH: case T_CASE: case T_DEFAULT: case T_NAMESPACE: case T_BREAK:
        case T_CONTINUE: case T_OPERATOR: case T_ATTRIBUTE: case T_MEMBER: case T_MEMBER_FRAME:
        case T_STATIC: case T_STATIC_FRAME: case T_ABSTRACT: case T_GUARD: case T_ASSERT:
        case T_PREFERFREE: case T_AND: case T_OR: case T_NOT:
            return true;
        default:
            return false;
    }
}

// Whether a space after it can matter to the parser: before a (, or an argument.
inline bool IsName(const Token &t) { return IsWord(t) && !IsStatementKeyword(t.t); }

// Spaces and comments, from `at` up to `end`, as tokens.
inline void AddComments(string_view src, int at, int end, vector<Token> &tokens) {
    while (at < end) {
        auto rest = src.substr(at, end - at);
        if (rest.starts_with("//") || rest.starts_with("/*")) {
            auto n = rest[1] == '/' ? rest.find('\n') : rest.find("*/", 2);
            n = n == string_view::npos ? rest.size() : rest[1] == '/' ? n : n + 2;
            Token t;
            t.text = rest.substr(0, n);
            t.start = at;
            t.end = at + int(n);
            tokens.push_back(t);
            at += int(n);
        } else {
            at++;
        }
    }
}

// The tokens of a source file, and why it can't be formatted.
struct Syntax {
    string filename;
    // What the compiler loaded it from.
    string path;
    string src;
    vector<Token> tokens;
    // The tokens that aren't comments.
    vector<Token *> code;
    string error;
    // While reading it: the last token that isn't a comment, in `tokens`, and where that ends.
    int last = -1;
    int end = 0;
    int codetokens = 0;
    int depth = 0;
    // How many interpolations are open, and where the outermost string started.
    int interp = 0;
    const char *interpstart = nullptr;
    size_t interpws = 0;
    string blocks;
    int pendingowner = -1;

    bool NewlineBefore(int i) const {
        if (i <= 0 || i >= (int)code.size()) return false;
        auto gap = string_view(src).substr(code[i - 1]->end, code[i]->start - code[i - 1]->end);
        return gap.find('\n') != string_view::npos;
    }

    void Refuse(string_view message, int offset) {
        if (!error.empty()) return;
        int line = 1;
        for (auto c : string_view(src).substr(0, offset)) line += c == '\n';
        error = cat(filename, "(", line, "): error: ", message);
    }

    // After the whole program has been read.
    void Finish() {
        AddComments(src, end, (int)src.size(), tokens);
        for (auto &t : tokens) if (t.t != T_NONE) code.push_back(&t);
    }
};

// Keeps what the compiler's lexer and parser find, per file of the program.
struct Recorder : TokenObserver {
    vector<unique_ptr<Syntax>> files;

    Syntax &File(Lex &lex) {
        auto idx = (size_t)lex.fileidx;
        if (files.size() <= idx) files.resize(idx + 1);
        if (!files[idx]) {
            files[idx] = std::make_unique<Syntax>();
            auto &f = *files[idx];
            f.filename = lex.filenames[idx].first;
            f.path = lex.filenames[idx].second;
            f.src = *lex.source.get();
            // Which Lex skips in the main file, before there's a token.
            if (f.src.starts_with("#!")) {
                auto n = min(f.src.find('\n'), f.src.size());
                Token t;
                t.text = string_view(f.src).substr(0, n);
                t.end = int(n);
                f.tokens.push_back(t);
                f.end = int(n);
            }
        }
        return *files[idx];
    }

    void Add(Lex &lex, Syntax &f, TType t, const char *from, const char *to) {
        auto begin = lex.source.get()->c_str();
        int start = int(from - begin), stop = int(to - begin);
        // Another read of the current token, which the parser put back.
        if (f.last >= 0 && start < f.tokens[f.last].end) return;
        AddComments(f.src, f.end, start, f.tokens);
        Token tok;
        tok.t = t;
        tok.text = string_view(f.src).substr(start, stop - start);
        tok.start = start;
        tok.end = stop;
        tok.ws = int(t == T_STR && f.interpstart == from ? f.interpws : lex.whitespacebefore);
        tok.keepspace = t == T_DOT && tok.ws && f.last >= 0 &&
                        lex.namespaces.count(f.tokens[f.last].text);
        tok.cont = lex.cont;
        tok.i = f.codetokens++;
        tok.depth = f.depth;
        tok.owner = f.pendingowner;
        f.pendingowner = -1;
        if (IsOpener(t)) f.depth++;
        if (IsCloser(t) && f.depth) f.depth--;
        // A line break before a dedent the parser takes as none.
        for (size_t k = 0; k < f.blocks.size(); k++) {
            if (f.blocks[k] != 'l' || k + 1 == f.blocks.size() || f.blocks[k + 1] != 'd') {
                tok.blocks += f.blocks[k];
            }
        }
        f.blocks.clear();
        f.tokens.push_back(tok);
        f.last = (int)f.tokens.size() - 1;
        f.end = stop;
    }

    void Read(Lex &lex) override {
        auto &f = File(lex);
        switch (lex.token) {
            case T_ENDOFFILE: case T_ENDOFINCLUDE: return;
            case T_LINEFEED: f.blocks += 'l'; return;
            case T_INDENT: f.blocks += 'i'; return;
            case T_DEDENT: f.blocks += 'd'; return;
            case T_STR_INT_START:
                if (!f.interp++) {
                    f.interpstart = lex.tokenstart;
                    f.interpws = lex.whitespacebefore;
                }
                return;
            case T_STR_INT_MIDDLE: return;
            case T_STR_INT_END:
                if (f.interp && !--f.interp) Add(lex, f, T_STR, f.interpstart, lex.p);
                return;
            default:
                if (!f.interp) Add(lex, f, lex.token, lex.tokenstart, lex.p);
                return;
        }
    }

    static int Previous(const Syntax &f) {
        for (int k = f.last - 1; k >= 0; k--) if (f.tokens[k].t != T_NONE) return k;
        return -1;
    }

    void Mark(Lex &lex, TokenRole role, bool previous) override {
        auto &f = File(lex);
        if (f.interp) return;
        auto k = previous ? Previous(f) : f.last;
        if (k >= 0) f.tokens[k].role = role;
    }

    void NoContinuation(Lex &lex) override {
        auto &f = File(lex);
        if (!f.interp && f.last >= 0) f.tokens[f.last].cont = false;
    }

    int Here(Lex &lex) override {
        auto &f = File(lex);
        return f.interp ? -1 : f.last;
    }

    void Block(Lex &lex, int header) override {
        auto &f = File(lex);
        if (header < 0 || f.interp) return;
        if (lex.token == T_INDENT) {
            for (int k = header + 1; k <= f.last; k++) f.tokens[k].inheader = true;
            f.pendingowner = header;
        } else if (f.last > header) {
            f.tokens[f.last].owner = header;
        }
    }
};

}  // namespace lobsterfmt

#endif  // LOBSTERFMT_SYNTAX
