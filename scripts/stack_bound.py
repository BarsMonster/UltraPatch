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
#   graph  : arm-none-eabi-gcc -fcallgraph-info=su emits a .ci file for the final
#            optimized translation unit. Its function nodes contain complete stack-frame
#            sizes (including saved LR/registers) and its edges include direct and indirect
#            calls. Inlined callees are already folded into their surviving caller.
#            We FAIL LOUDLY (invalidating the static method) if any of these appear:
#              - recursion (a cycle in the internal call graph),
#              - an internal function whose address is TAKEN (any absolute relocation against
#                .text / a named function; see check_no_code_address_taken) -- that would let an
#                indirect call reach internal code. With no such pointer, every indirect edge
#                targets g_pull_fn (the external byte callback),
#              - a call to an unexpected external symbol (anything not in the flash-primitive
#                set or the toolchain-runtime allowlist),
#              - a dynamic/VLA stack frame (node qualifier != "static").
#
# BOUND = longest root-to-leaf weighted path from the selected harness entry (the repository
#         uses decoder_run wrappers for both static PatchApply storage and a caller-owned
#         PatchApply pointer), summing internal frames only. Externs are EXCLUDED and reported
#         separately:
#           - integrator externs  : flash_read, flash_write_page, and the byte callback. Their
#                                   stack is the integrator's own cost, by contract.
#           - toolchain externs   : bounded compiler helpers such as memmove / memset.
#                                   Leaves, small bounded non-recursive frames; not in the graph,
#                                   so excluded here and absorbed by the gate ceiling headroom.
#
# Deterministic: no wall-clock or randomness; output depends only on the object and its .ci.

import argparse
import os
import re
import shlex
import subprocess
import sys

INTEGRATOR_EXTERNS = {"flash_read", "flash_write_page"}
# Intentional libc leaves; every other external helper invalidates the decoder contract.
TOOLCHAIN_EXTERNS = {"memmove", "memset"}

# GCC's VCG output keeps every node/edge on one line. Quoted fields may contain escaped
# characters, hence the (?:\\.|[^"\\])* form rather than a plain [^"]*.
QSTR = r'((?:\\.|[^"\\])*)'
NODE_RE = re.compile(r'^node:\s*\{\s*title:\s*"' + QSTR +
                     r'"\s+label:\s*"' + QSTR + r'".*\}\s*$')
EDGE_RE = re.compile(r'^edge:\s*\{\s*sourcename:\s*"' + QSTR +
                     r'"\s+targetname:\s*"' + QSTR +
                     r'"(?:\s+label:\s*"' + QSTR + r'")?\s*\}\s*$')
STACK_RE = re.compile(r'\\n([0-9]+) bytes \(([^)]+)\)$')
# an objdump -r record: "00000032 R_ARM_THM_CALL   flash_read"
RELOC_RE = re.compile(r"^([0-9a-fA-F]+)\s+(R_\S+)\s+(\S+)")


def die(msg):
    sys.stderr.write("stack_bound.py: FATAL: " + msg + "\n")
    sys.exit(2)


def parse_ci(path):
    """Return (frames, call edges, indirect-call counts, indirect source sites)."""
    nodes, raw_edges = {}, []
    with open(path) as f:
        for ln in f:
            ln = ln.rstrip("\n")
            m = NODE_RE.match(ln)
            if m:
                ident, label = m.groups()
                if ident in nodes:
                    # GCC can emit multiple declarations for one external builtin (for
                    # example both the builtin and header declarations of memset).
                    if STACK_RE.search(nodes[ident]) or STACK_RE.search(label):
                        die("duplicate internal callgraph node: %s" % ident)
                    continue
                nodes[ident] = label
                continue
            m = EDGE_RE.match(ln)
            if m:
                raw_edges.append(m.groups())
            elif ln.lstrip().startswith(("node:", "edge:")):
                die("cannot parse .ci line: %r" % ln)

    frames, internal_ids = {}, {}
    for ident, label in nodes.items():
        m = STACK_RE.search(label)
        if not m:
            continue
        nbytes, qual = m.groups()
        name = ident.rsplit(":", 1)[-1]
        if qual != "static":
            die("non-static stack frame for %s (qualifier=%r): a dynamic/VLA frame "
                "invalidates the static bound" % (name, qual))
        if name in frames:
            die("duplicate internal function name: %s" % name)
        frames[name] = int(nbytes)
        internal_ids[ident] = name
    if not frames:
        die("no stack-annotated nodes in %s (build with -fcallgraph-info=su)" % path)

    edges = {name: [] for name in frames}
    indirect = {name: 0 for name in frames}
    indirect_sites = set()
    for source_id, target_id, callsite in raw_edges:
        if source_id not in nodes or target_id not in nodes:
            die("callgraph edge refers to an unknown node: %s -> %s" %
                (source_id, target_id))
        if source_id not in internal_ids:
            die("callgraph edge has a non-internal source: %s" % source_id)
        source = internal_ids[source_id]
        if target_id in internal_ids:
            edges[source].append(internal_ids[target_id])
        elif target_id == "__indirect_call":
            if callsite is None:
                die("indirect call has no source site: %s" % source)
            indirect[source] += 1
            indirect_sites.add(callsite)
        else:
            edges[source].append(target_id)
    return frames, edges, indirect, indirect_sites


def check_no_code_address_taken(objdump, obj, internal):
    """Prove no internal function's address is ever materialised as data: FAIL if any
    ABSOLUTE-address relocation targets .text or a named internal function. If none do, no
    code pointer to an internal function exists, so every indirect call can only reach
    g_pull_fn (the integrator's byte callback, an external leaf). PC-relative branch relocs
    (R_ARM_*CALL /
    *JUMP) are direct calls already in the graph; R_ARM_PREL31 into .text is .ARM.exidx unwind
    metadata (never a callable pointer); ABS32 into .bss/.rodata/.data are data pointers."""
    try:
        out = subprocess.check_output(
            shlex.split(objdump) + ["-r", obj], universal_newlines=True)
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


def main():
    ap = argparse.ArgumentParser(description="static worst-case caller-stack bound")
    ap.add_argument("obj", help="decoder harness object built with -fcallgraph-info=su")
    ap.add_argument("--ci", help="the callgraph file (default: <obj> with .ci extension)")
    ap.add_argument("--objdump", default=os.environ.get(
        "OBJDUMP", "arm-none-eabi-objdump"))
    ap.add_argument("--entry", default="decoder_run",
                    help="entry symbol (default decoder_run == patch_apply_run)")
    ap.add_argument("--quiet", action="store_true",
                    help="print only 'stack_bound_bytes=N'")
    args = ap.parse_args()

    ci = args.ci or os.path.splitext(args.obj)[0] + ".ci"
    if not os.path.exists(args.obj):
        die("object not found: %s" % args.obj)
    if not os.path.exists(ci):
        die(".ci not found: %s (build with -fcallgraph-info=su)" % ci)

    frames, edges, indirect, indirect_sites = parse_ci(ci)
    internal = set(frames)

    if args.entry not in internal:
        die("entry %s not found in object" % args.entry)

    # --- validation pass: classify every call, fail on anything that breaks the method ---
    integ_ext, tool_ext = set(), set()
    for func, targs in edges.items():
        for t in targs:
            if t in internal:
                continue
            if t in INTEGRATOR_EXTERNS:
                integ_ext.add(t)
            elif t in TOOLCHAIN_EXTERNS:
                tool_ext.add(t)
            else:
                die("unexpected external call: %s -> %s (not a flash primitive, callback, "
                    "or known toolchain helper); static bound cannot be trusted" % (func, t))

    # Indirect calls: prove they can only reach the external byte callback, never
    # internal code, by showing no internal function address is ever taken. Robust across
    # toolchains/opt-levels (the compiler inlines the byte-source read, spreading the callback
    # across its callers).
    if len(indirect_sites) != 1:
        die("expected one source-level byte-callback site, found %d: %s" %
            (len(indirect_sites), ",".join(sorted(indirect_sites))))
    check_no_code_address_taken(args.objdump, args.obj, internal)
    callback_sites = sum(indirect.values())

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
