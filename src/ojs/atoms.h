// atoms.h — property names the engine uses itself, interned once per
// runtime. A(name) is the pkey of the atom.
#ifndef OJS_ATOMS_H
#define OJS_ATOMS_H

#define ATOM_LIST(X) \
    X(empty, "") X(out_of_memory, "out of memory") X(length, "length") X(prototype, "prototype") X(constructor, "constructor") \
    X(name, "name") X(message, "message") X(toString, "toString") X(valueOf, "valueOf") \
    X(value, "value") X(writable, "writable") X(enumerable, "enumerable") X(configurable, "configurable") \
    X(get, "get") X(set, "set") X(done, "done") X(next, "next") X(return_, "return") X(throw_, "throw") \
    X(then, "then") X(callee, "callee") X(caller, "caller") X(arguments, "arguments") X(proto, "__proto__") \
    X(lastIndex, "lastIndex") X(index, "index") X(input, "input") X(groups, "groups") X(indices, "indices") \
    X(source, "source") X(flags, "flags") X(global, "global") X(ignoreCase, "ignoreCase") \
    X(multiline, "multiline") X(dotAll, "dotAll") X(unicode, "unicode") X(unicodeSets, "unicodeSets") \
    X(sticky, "sticky") X(hasIndices, "hasIndices") X(raw, "raw") X(join, "join") X(default_, "default") \
    X(cause, "cause") X(errors, "errors") X(stack, "stack") X(toJSON, "toJSON") X(size, "size") \
    X(add, "add") X(has, "has") X(delete_, "delete") X(clear, "clear") X(keys, "keys") X(values, "values") \
    X(entries, "entries") X(undefined, "undefined") X(null_, "null") X(true_, "true") X(false_, "false") \
    X(NaN, "NaN") X(Infinity, "Infinity") X(number, "number") X(string, "string") X(boolean, "boolean") \
    X(object, "object") X(function, "function") X(symbol, "symbol") X(bigint, "bigint") \
    X(globalThis, "globalThis") X(eval, "eval") X(this_, "this") X(target, "target") X(meta, "meta") \
    X(new_, "new") X(async, "async") X(await, "await") X(yield, "yield") X(let, "let") X(static_, "static") \
    X(of, "of") X(from, "from") X(as, "as") X(anonymous, "anonymous") X(bound_, "bound ") X(get_, "get ") \
    X(set_, "set ") X(Symbol_iterator, "[Symbol.iterator]") X(resolve, "resolve") X(reject, "reject") \
    X(status, "status") X(reason, "reason") X(fulfilled, "fulfilled") X(rejected, "rejected") \
    X(Object, "Object") X(Array, "Array") X(Function, "Function") X(Error, "Error") X(String, "String") \
    X(Number, "Number") X(Boolean, "Boolean") X(Symbol, "Symbol") X(Promise, "Promise") X(RegExp, "RegExp") \
    X(Date, "Date") X(Map, "Map") X(Set, "Set") X(Math, "Math") X(JSON, "JSON") X(Reflect, "Reflect") \
    X(Proxy, "Proxy") X(BigInt, "BigInt") X(lineNumber, "lineNumber") X(columnNumber, "columnNumber") \
    X(fileName, "fileName") X(proxy, "proxy") X(revoke, "revoke") X(apply, "apply") X(construct, "construct") \
    X(defineProperty, "defineProperty") X(deleteProperty, "deleteProperty") \
    X(getOwnPropertyDescriptor, "getOwnPropertyDescriptor") X(getPrototypeOf, "getPrototypeOf") \
    X(setPrototypeOf, "setPrototypeOf") X(isExtensible, "isExtensible") X(preventExtensions, "preventExtensions") \
    X(ownKeys, "ownKeys") X(byteLength, "byteLength") X(byteOffset, "byteOffset") X(buffer, "buffer") \
    X(maxByteLength, "maxByteLength") X(toISOString, "toISOString") X(lastIndexOf, "lastIndexOf") \
    X(push, "push") X(callee_strict, "callee") X(fill, "fill") X(at, "at") X(str_0, "0") X(str_1, "1") \
    X(toLocaleString, "toLocaleString") X(description, "description") X(indexOf, "indexOf") \
    X(any, "any") X(all, "all") X(race, "race") X(allSettled, "allSettled") X(exec, "exec") \
    X(match, "match") X(replace, "replace") X(search, "search") X(split, "split") X(matchAll, "matchAll") \
    X(species, "species") X(hint_default, "default") X(Generator, "Generator") X(AsyncGenerator, "AsyncGenerator") \
    X(AsyncFunction, "AsyncFunction") X(GeneratorFunction, "GeneratorFunction") \
    X(AsyncGeneratorFunction, "AsyncGeneratorFunction") X(Arguments, "Arguments") X(star_default, "*default*") \
    X(ns_star, "*namespace*") X(unscopables, "unscopables") X(toPrimitive, "toPrimitive") X(iterator, "iterator") \
    X(asyncIterator, "asyncIterator") X(hasInstance, "hasInstance") X(isConcatSpreadable, "isConcatSpreadable") \
    X(toStringTag, "toStringTag") X(getYear, "getYear") X(cookie, "cookie") X(timeout, "timeout") \
    X(kind, "kind") X(grow, "grow") X(resize, "resize") X(detached, "detached") X(resizable, "resizable") \
    X(growable, "growable") X(WeakRef, "WeakRef") X(cleanupSome, "cleanupSome") X(unregister, "unregister") \
    X(register_, "register")

struct atoms_common {
#define ATOM_FIELD(id, s) struct str* id;
    ATOM_LIST(ATOM_FIELD)
#undef ATOM_FIELD
};

#define A(id) (pk_from_atom(J->A->id))

#endif
