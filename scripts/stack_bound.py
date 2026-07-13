#!/usr/bin/env python3
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT
#
# stack_bound.py -- static worst-case caller-stack bound for the decoder.
#
# patch_apply_run() runs the entire decode synchronously on the caller's stack. The decoder has
# no recursion and no indirect calls except the integrator's byte callback (blx through g_pull_fn) and
# the two flash primitives (flash_read/flash_write_page, direct bl to extern symbols), so an
# exact static worst-case stack depth is derivable from per-function frame sizes plus the
# call graph.
#
# METHOD
#   frames : arm-none-eabi-gcc -fstack-usage emits a .su file, one line per emitted
#            function: "<file>:<line>:<col>:<name>\t<bytes>\t<qualifier>". The <bytes>
#            value is the full prologue SP decrement of that function INCLUDING every
#            pushed register and the saved LR (verified empirically: up_rc_decode pushes
#            {r4-r7,lr}=20B + 3 saved regs=12B + 8B locals => .su=40). So a function's saved return
#            address is already counted inside ITS OWN frame; a plain `bl` pushes nothing
#            (it writes LR), and the callee's push{...,lr} that spills LR is in the
#            callee's .su. Summing frames along a call chain therefore already includes
#            every on-stack return address -- no separate per-call addend is needed.
#            (Inlined callees contribute no .su line; their locals are folded into the
#            surviving caller's frame, which is exactly why we read the call graph from
#            THIS object's disassembly rather than from source.)
#   graph  : arm-none-eabi-objdump -d. Nodes = defined function symbols. Edges = `bl`
#            to a defined internal symbol. We FAIL LOUDLY (invalidating the static method)
#            if any of these appear:
#              - recursion (a cycle in the internal call graph),
#              - an internal function whose address is TAKEN (any absolute relocation against
#                .text / a named function; see check_no_code_address_taken) -- that would let an
#                indirect call (`blx`) reach code the bl-graph does not track. With no such
#                pointer, every blx provably targets g_pull_fn (the external byte callback),
#                so the bl-derived graph is the complete internal call graph,
#              - a `bl` to an unexpected external symbol (anything not in the flash-primitive
#                set or the toolchain-runtime allowlist),
#              - a dynamic/VLA stack frame (.su qualifier != "static").
#
# BOUND = longest root-to-leaf weighted path from the selected harness entry (the repository
#         uses rcv3_run wrappers for both static PatchApply storage and a caller-owned
#         PatchApply pointer), summing internal frames only. Externs are EXCLUDED and reported
#         separately:
#           - integrator externs  : flash_read, flash_write_page, and the byte callback. Their
#                                   stack is the integrator's own cost, by contract.
#           - toolchain externs   : bounded compiler helpers such as memmove / memset.
#                                   Leaves, small bounded non-recursive frames; not in the .su,
#                                   so excluded here and absorbed by the gate ceiling headroom.
#                                   The separate ARM gate rejects divide helpers.
#
# Deterministic: no wall-clock, no randomness; output is a pure function of the object.

import argparse
import os
import re
import subprocess
import sys

INTEGRATOR_EXTERNS = {"flash_read", "flash_write_page"}
# Compiler-runtime helpers the codegen may emit (libgcc / freestanding builtins). Leaves,
# not defined in the TU. Matched by exact name or these prefixes.
TOOLCHAIN_EXTERN_RE = re.compile(
    r"^(__aeabi_[a-z0-9]+|__udivsi3|__divsi3|__umodsi3|__modsi3|"
    r"memcpy|memmove|memset|__gnu_thumb1_case_[a-z0-9]+)$"
)

HDR_RE = re.compile(r"^[0-9a-fA-F]+ <([^>]+)>:")
# a disassembled instruction line: "   92:\tf7ff ffb5 \tbl\t0 <up_rc_decode>"
BL_RE = re.compile(r"\bbl\b\s+[0-9a-fA-F]+ <([^>]+)>")
BLX_RE = re.compile(r"\bblx\b")
# an objdump -r record: "00000032 R_ARM_THM_CALL   flash_read"
RELOC_RE = re.compile(r"^([0-9a-fA-F]+)\s+(R_\S+)\s+(\S+)")


def die(msg):
    sys.stderr.write("stack_bound.py: FATAL: " + msg + "\n")
    sys.exit(2)


def parse_su(path):
    """Return {name: bytes}. Fail loudly on any non-static (dynamic/VLA) frame."""
    sizes = {}
    with open(path) as f:
        for ln in f:
            ln = ln.rstrip("\n")
            if not ln:
                continue
            parts = ln.split("\t")
            if len(parts) < 3:
                die("cannot parse .su line: %r" % ln)
            ident, nbytes, qual = parts[0], parts[1], parts[2]
            # ident is file:line:col:name -> name is the last colon field
            name = ident.rsplit(":", 1)[-1]
            if qual.strip() != "static":
                die("non-static stack frame for %s (qualifier=%r): a dynamic/VLA frame "
                    "invalidates the static bound" % (name, qual))
            sizes[name] = int(nbytes)
    if not sizes:
        die("empty .su: %s" % path)
    return sizes


def parse_objdump(objdump, obj):
    """Return (order, edges, indirect) where
       order    = list of internal function names in address order
       edges    = {func: [bl-target, ...]}  (targets may be internal or extern)
       indirect = {func: count of blx sites}"""
    try:
        out = subprocess.check_output(
            [objdump, "-d", obj], universal_newlines=True)
    except (OSError, subprocess.CalledProcessError) as e:
        die("objdump failed: %s" % e)
    order, edges, indirect = [], {}, {}
    cur = None
    for ln in out.splitlines():
        m = HDR_RE.match(ln)
        if m:
            cur = m.group(1)
            order.append(cur)
            edges.setdefault(cur, [])
            indirect.setdefault(cur, 0)
            continue
        if cur is None:
            continue
        mb = BL_RE.search(ln)
        if mb:
            edges[cur].append(mb.group(1))
        elif BLX_RE.search(ln):
            indirect[cur] += 1
    return order, edges, indirect


def check_no_code_address_taken(objdump, obj, internal):
    """Prove no internal function's address is ever materialised as data: FAIL if any
    ABSOLUTE-address relocation targets .text or a named internal function. If none do, no
    code pointer to an internal function exists, so every indirect call (blx) can only reach
    g_pull_fn (the integrator's byte callback, an external leaf) -- which means the bl-derived
    call graph is the COMPLETE internal call graph. PC-relative branch relocs (R_ARM_*CALL /
    *JUMP) are direct calls already in the graph; R_ARM_PREL31 into .text is .ARM.exidx unwind
    metadata (never a callable pointer); ABS32 into .bss/.rodata/.data are data pointers."""
    try:
        out = subprocess.check_output([objdump, "-r", obj], universal_newlines=True)
    except (OSError, subprocess.CalledProcessError) as e:
        die("objdump -r failed: %s" % e)
    for ln in out.splitlines():
        m = RELOC_RE.match(ln.strip())
        if not m:
            continue
        rtype, sym = m.group(2), m.group(3)
        base = sym.split("+", 1)[0]
        absolute = ("ABS" in rtype) or ("TARGET1" in rtype) or ("GOT" in rtype)
        if absolute and (base == ".text" or base in internal):
            die("absolute relocation %s against code symbol %s: an internal function address is "
                "taken, so an indirect call could reach untracked internal code; the static "
                "call graph is not provably complete" % (rtype, sym))


def frame_for(name, sizes):
    """Map a disassembly symbol to its .su frame size, tolerating the clone-suffix
    discrepancy where -fstack-usage drops a trailing '.<n>' (e.g. disasm
    'foo.constprop.0' vs .su 'foo.constprop')."""
    if name in sizes:
        return sizes[name]
    stripped = re.sub(r"\.\d+$", "", name)
    if stripped in sizes:
        return sizes[stripped]
    return None


def main():
    ap = argparse.ArgumentParser(description="static worst-case caller-stack bound")
    ap.add_argument("obj", help="the decoder harness object built with -fstack-usage")
    ap.add_argument("--su", help="the .su file (default: <obj> with .su extension)")
    ap.add_argument("--objdump", default=os.environ.get(
        "OBJDUMP", "arm-none-eabi-objdump"))
    ap.add_argument("--entry", default="rcv3_run",
                    help="entry symbol (default rcv3_run == patch_apply_run)")
    ap.add_argument("--quiet", action="store_true",
                    help="print only 'stack_bound_bytes=N'")
    args = ap.parse_args()

    su = args.su or os.path.splitext(args.obj)[0] + ".su"
    if not os.path.exists(args.obj):
        die("object not found: %s" % args.obj)
    if not os.path.exists(su):
        die(".su not found: %s (build with -fstack-usage)" % su)

    sizes = parse_su(su)
    order, edges, indirect = parse_objdump(args.objdump, args.obj)
    internal = set(order)

    if args.entry not in internal:
        die("entry %s not found in object" % args.entry)

    # --- validation pass: classify every call, fail on anything that breaks the method ---
    # A bl whose target carries a '+0xNN' offset is NOT a call: it is GCC using bl as a
    # long-range branch to an interior label of the SAME function (the block does work and
    # b.n's back; it never establishes a frame or returns via bx lr, so it adds no stack).
    # Real calls always target a bare function ENTRY. We assert every interior bl stays
    # inside its own function (a bl into another function's interior would be pathological).
    integ_ext, tool_ext = set(), set()
    for func, targs in edges.items():
        for t in targs:
            if "+" in t:
                base = t.split("+", 1)[0]
                if base == func:
                    continue  # intra-function long branch: no stack effect, no edge
                die("bl into interior of a different function: %s -> %s; static bound "
                    "cannot be trusted" % (func, t))
            if t in internal:
                continue
            if t in INTEGRATOR_EXTERNS:
                integ_ext.add(t)
            elif TOOLCHAIN_EXTERN_RE.match(t):
                tool_ext.add(t)
            else:
                die("unexpected external call: %s -> %s (not a flash primitive, callback, "
                    "or known toolchain helper); static bound cannot be trusted" % (func, t))

    # indirect calls (blx): prove they can only reach the external byte callback, never
    # internal code, by showing no internal function address is ever taken. Robust across
    # toolchains/opt-levels (the compiler inlines the byte-source read, spreading the callback blx
    # across its callers).
    check_no_code_address_taken(args.objdump, args.obj, internal)
    callback_sites = sum(indirect.values())

    # every internal node must have a frame size
    frames = {}
    for name in internal:
        fr = frame_for(name, sizes)
        if fr is None:
            die("no .su frame for internal symbol %s" % name)
        frames[name] = fr

    # build internal-only adjacency (dedup, preserve first-seen order)
    adj = {}
    for func in internal:
        seen, lst = set(), []
        for t in edges.get(func, []):
            if t in internal and t not in seen:
                seen.add(t)
                lst.append(t)
        adj[func] = lst

    # --- recursion / cycle detection (DFS colouring) over the reachable subgraph ---
    WHITE, GREY, BLACK = 0, 1, 2
    color = {n: WHITE for n in internal}

    def find_cycle(u, stack):
        color[u] = GREY
        stack.append(u)
        for v in adj[u]:
            if color[v] == GREY:
                i = stack.index(v)
                die("recursion detected: %s (a cycle invalidates the static bound)"
                    % " -> ".join(stack[i:] + [v]))
            if color[v] == WHITE:
                find_cycle(v, stack)
        stack.pop()
        color[u] = BLACK

    find_cycle(args.entry, [])

    # --- longest weighted path from entry (DAG, memoised) ---
    best = {}
    succ = {}

    def depth(u):
        if u in best:
            return best[u]
        bd, bs = 0, None
        for v in adj[u]:
            d = depth(v)
            if d > bd:
                bd, bs = d, v
        best[u] = frames[u] + bd
        succ[u] = bs
        return best[u]

    total = depth(args.entry)

    # reconstruct worst path
    path = []
    n = args.entry
    while n is not None:
        path.append(n)
        n = succ.get(n)

    if args.quiet:
        print("stack_bound_bytes=%d" % total)
        return

    print("stack_bound_bytes=%d" % total)
    print("stack_bound_entry=%s" % args.entry)
    print("stack_bound_reachable_funcs=%d" % len(best))
    print("stack_bound_callback_sites=%d" % callback_sites)
    print("stack_bound_integrator_externs=%s" % ",".join(sorted(integ_ext)))
    print("stack_bound_toolchain_externs=%s" % ",".join(sorted(tool_ext)))
    print("stack_bound_excludes=integrator(flash_read,flash_write_page,byte-callback)+toolchain-leaves")
    print("stack_bound_includes=all-first-party-frames-incl-saved-LR/regs")
    print("# worst-case path (frame bytes per function; sum == bound):")
    run = 0
    for f in path:
        run += frames[f]
        print("#   %-34s %4d  (cumulative %d)" % (f, frames[f], run))


if __name__ == "__main__":
    main()
