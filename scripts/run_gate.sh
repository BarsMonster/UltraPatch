#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -u

MAKE_CMD="${MAKE:-make}"
. "$(dirname "$0")/tempdir.sh"
rc=0

# The corpus encoder saturates every core by itself, while several other gate legs also encode
# nontrivial fixtures.  Leave one quarter of the machine to those concurrent legs; otherwise a
# 32-worker corpus pool oversubscribes the 32-core reference host and approaches the 60 s cap.
# An explicit `make gate JOBS=N` remains authoritative.  Standalone check-corpus still uses nproc.
if [ -n "${JOBS:-}" ]; then
  corpus_jobs=$JOBS
else
  cores=$(nproc 2>/dev/null || echo 4)
  corpus_jobs=$((cores * 3 / 4))
  [ "$corpus_jobs" -gt 0 ] || corpus_jobs=1
fi

echo "running gate (all legs concurrent; corpus jobs=$corpus_jobs): check-assets + check + check-malformed + check-edge + check-degrade + check-golden + check-decoder-contract + check-models + check-wire-config + check-arm + check-stack + check-corpus..."

LEGS="check-assets-internal:assets.txt:check-assets check-internal:c.txt:check check-malformed-internal:malformed.txt:check-malformed check-edge-internal:e.txt:check-edge check-degrade-internal:dg.txt:check-degrade check-golden-internal:g.txt:check-golden check-decoder-contract-internal:dec_contract.txt:check-decoder-contract check-models-internal:models.txt:check-models check-wire-config-internal:wire_config.txt:check-wire-config check-arm-internal:a.txt:check-arm check-stack-internal:st.txt:check-stack check-corpus-internal:m.txt:check-corpus"

pids=""
for spec in $LEGS; do
  IFS=: read -r target file _ <<<"$spec"
  # gate-internal already linked ./ultrapatch before this fork. Pass `-o ultrapatch`
  # (assume-old) so each forked leg treats the prebuilt binary as up-to-date and never
  # relinks it, even if a source mtime is newer at sub-make startup (an edit landing in
  # the seconds between the pre-fork build and this loop). Without it, several legs that
  # list ultrapatch as a prerequisite could race concurrent `-o ultrapatch` links on the
  # same path while other legs exec it (ETXTBSY / half-written exec).
  if [ "$target" = check-corpus-internal ]; then
    "$MAKE_CMD" --no-print-directory -o ultrapatch JOBS="$corpus_jobs" "$target" >"$tmp/$file" 2>&1 &
  else
    "$MAKE_CMD" --no-print-directory -o ultrapatch "$target" >"$tmp/$file" 2>&1 &
  fi
  pids="$pids $!"
done
for p in $pids; do
  wait "$p" || rc=1
done

kv() {
  sed -n "s/^$2=/$3/p" "$tmp/$1"
}
kvs() {
  for spec in "$@"; do
    IFS='|' read -r file key label <<<"$spec"
    kv "$file" "$key" "$label"
  done
}

echo "==================== A1 GATE ========================="
kvs 'assets.txt|corpus_assets|corpus assets          : ' 'assets.txt|foreign_assets|foreign assets         : ' 'malformed.txt|malformed_rejects|malformed rejects      : '
awk -F= '/^edge_cases=/{c=$2}/^edge_roundtrips=/{r=$2}/^edge_refusals=/{f=$2}END{if(c!="")printf "edge inputs             : %s round-trip + %s refused of %s\n",r,f,c}' "$tmp/e.txt"
kvs 'g.txt|golden_wire|golden wire             : ' 'dec_contract.txt|decoder_contract|decoder contract        : ' 'dec_contract.txt|decoder_portable|decoder portability     : ' 'models.txt|model_contract|model contract          : ' 'wire_config.txt|wire_config_override|wire config override    : '
awk -F= '/^degrade_journal_peak=/{j=$2}/^degrade_opc_splits=/{o=$2}/^degrade_direction=/{d=$2}/^degrade_rowwindow=/{w=$2}/^degrade_bigspan=/{f=$2}/^degrade_packed_preserve=/{p=$2}/^degrade_packed_correction=/{x=$2}/^degrade_cases=/{c=$2}END{if(c!="")printf "degradation paths       : journal_peak=%s opc_splits=%s dir=%s rowwin=%s bigspan=%s packed=%s/%s (%s cases)\n",j,o,d,w,f,p,x,c}' "$tmp/dg.txt"
kvs 'a.txt|arm_size_integration|ARM object integration  : '
awk -v bt="${BASE_ARM_TEXT:?}" -v bd="${BASE_ARM_DATA:?}" -v bb="${BASE_ARM_BSS:?}" 'NR==2{printf "ARM object text/data/bss : %s / %s / %s   (ratchet %s/%s/%s, .bss cap 12288)\n",$1,$2,$3,bt,bd,bb}' "$tmp/a.txt"
kvs 'a.txt|arm_linked_integration|ARM linked integration  : '
awk -F= -v bt="${BASE_ARM_LINKED_TEXT:?}" -v bd="${BASE_ARM_LINKED_DATA:?}" -v bb="${BASE_ARM_LINKED_BSS:?}" '/^arm_linked_text=/{t=$2}/^arm_linked_data=/{d=$2}/^arm_linked_bss=/{b=$2}END{if(t!="")printf "ARM linked text/data/bss : %s / %s / %s   (ratchet %s/%s/%s, .bss cap 12288)\n",t,d,b,bt,bd,bb}' "$tmp/a.txt"
kvs 'a.txt|arm_linked_runtime_helpers|ARM linked helpers      : '
kvs 'a.txt|soft_div_calls|ARM   soft-divide calls  : '
awk -F= '/^stack_static_bound_bytes=/{b=$2}/^stack_static_ceiling_o2=/{c=$2}END{if(b!="")printf "stack, static wrapper   : %s B  (gcc -O2, ceiling %s, excl. externs)\n",b,c}' "$tmp/st.txt"
awk -F= '/^stack_generic_bound_bytes=/{b=$2}/^stack_generic_ceiling_o2=/{c=$2}END{if(b!="")printf "stack, generic pointer  : %s B  (gcc -O2, ceiling %s, excl. externs)\n",b,c}' "$tmp/st.txt"
kvs 'm.txt|matrix_ok|matrix round-trips      : ' 'm.txt|full_total|corpus full_total       : '
awk -F= '/^home_size_better=/{b=$2}/^home_size_worse=/{w=$2}/^home_size_equal=/{e=$2}END{if(b!="")printf "home size split         : %s better / %s worse / %s equal\n",b,w,e}' "$tmp/m.txt"
kvs 'm.txt|foreign_ok|foreign round-trips     : ' 'm.txt|foreign_total|foreign full_total      : ' 'm.txt|wire_identity|corpus wire identity    : ' 'c.txt|oneface_grow|one-face grow            : ' 'c.txt|oneface_revert|one-face revert          : ' 'm.txt|max_amplified|NVM pages amplified      : ' 'm.txt|max_maxpageerase|NVM max erases-per-page  : ' 'm.txt|max_inversions|NVM frontier inversions  : ' 'm.txt|max_unaligned|NVM unaligned calls      : ' 'm.txt|max_oob_page_writes|NVM out-of-range calls   : ' 'm.txt|max_canary_corrupt|NVM canary corruptions  : ' 'm.txt|max_journal|journal peak slots      : '
if [ "$rc" = 0 ]; then
  echo "robustness check         : PASS (round-trip both dirs + corrupt/truncated/CRC rejects)"
  echo "RESULT                   : ALL GATES PASS"
else
  echo "RESULT                   : *** GATE FAILED (rc=$rc) ***"
  for spec in $LEGS; do
    IFS=: read -r _ file label <<<"$spec"
    [ -s "$tmp/$file" ] || continue
    echo "------------------ $label ------------------"
    cat "$tmp/$file"
  done
fi
echo "====================================================="
exit "$rc"
