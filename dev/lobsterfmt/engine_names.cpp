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

// The parser only needs to know that the builtins of the engine are there, and how many arguments
// they take, which saves this from linking the engine and what it depends on. The compiler built
// next to this says what they are, see --gen-builtins-table.

#include "format.h"
#include "lobster/tonative.h"

namespace lobster {
// What these two link against when the compiler is built with a JIT.
bool RunTCC(const char *, const char *, string &error, const void **, const char **,
            const JitOptions &, function<bool(void **)>) {
    error = "lobsterfmt doesn't run code";
    return false;
}
bool RunMIR(const char *, const char *, string &error, const void **, const char **,
            const JitOptions &, function<bool(void **)>) {
    error = "lobsterfmt doesn't run code";
    return false;
}
}

namespace lobsterfmt {

struct BuiltinName {
    const char *ns;
    const char *name;
    int nargs;
};

#include "builtin_names.h"

void RegisterEngineNames(NativeRegistry &nfr) {
    // What's linked here is registered already, each with all of its overloads.
    set<string> linked;
    auto fullname = [](const BuiltinName &b) {
        return *b.ns ? cat(b.ns, ".", b.name) : string(b.name);
    };
    for (auto &b : builtin_names) if (nfr.FindNative(fullname(b))) linked.insert(fullname(b));
    // The registry keeps pointing to these.
    static map<string, BuiltinGroup> groups;
    for (auto &b : builtin_names) {
        if (linked.count(fullname(b))) continue;
        string ids, typeids;
        for (int i = 0; i < b.nargs; i++) {
            append(ids, i ? "," : "", "a");
            typeids += 'A';
        }
        auto symbol = cat("builtin_", *b.ns ? cat(b.ns, "_") : string(), b.name);
        new BuiltinDef(groups[b.ns], (new string(symbol))->c_str(), b.name,
                       (new string(ids))->c_str(), (new string(typeids))->c_str(), "", "", false,
                       BCG_NONE, nullptr);
    }
    for (auto &[ns, group] : groups) RegisterBuiltin(nfr, ns.c_str(), "engine", group);
    for (auto name : resource_names) {
        if (!LookupResourceType(name)) new ResourceType { name };
    }
}

}  // namespace lobsterfmt
