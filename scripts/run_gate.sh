#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -u

MAKE_CMD="${MAKE:-make}"
. "$(dirname "$0")/tempdir.sh"
rc=0

echo "running gate (all legs concurrent): check-assets + check + check-malformed + check-edge + check-degrade + check-golden + check-decoder-contract + check-models + check-arm + check-stack + check-corpus..."

LEGS="check-assets-internal:assets.txt:check-assets check-internal:c.txt:check check-malformed-internal:malformed.txt:check-malformed check-edge-internal:e.txt:check-edge check-degrade-internal:dg.txt:check-degrade check-golden-internal:g.txt:check-golden check-decoder-contract-internal:dec_contract.txt:check-decoder-contract check-models-internal:models.txt:check-models check-arm-internal:a.txt:check-arm check-stack-internal:st.txt:check-stack check-corpus-internal:m.txt:check-corpus"

pids=""
for spec in $LEGS; do
  IFS=: read -r target file _ <<<"$spec"
  "$MAKE_CMD" --no-print-directory "$target" >"$tmp/$file" 2>&1 &
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
kvs 'g.txt|golden_wire|golden wire             : ' 'dec_contract.txt|decoder_contract|decoder contract        : ' 'models.txt|model_contract|model contract          : '
awk -F= '/^degrade_journal_peak=/{j=$2}/^degrade_opc_splits=/{o=$2}/^degrade_direction=/{d=$2}/^degrade_rowwindow=/{w=$2}/^degrade_bigspan=/{f=$2}/^degrade_cases=/{c=$2}END{if(c!="")printf "degradation paths       : journal_peak=%s opc_splits=%s dir=%s rowwin=%s bigspan=%s (%s cases)\n",j,o,d,w,f,c}' "$tmp/dg.txt"
kvs 'a.txt|arm_size_integration|ARM   integration       : '
awk -v bt="${BASE_ARM_TEXT:?}" -v bd="${BASE_ARM_DATA:?}" -v bb="${BASE_ARM_BSS:?}" 'NR==2{printf "ARM   text / data / bss  : %s / %s / %s   (ratchet %s/%s/%s, .bss cap 12288)\n",$1,$2,$3,bt,bd,bb}' "$tmp/a.txt"
kvs 'a.txt|soft_div_calls|ARM   soft-divide calls  : '
awk -F= '/^stack_bound_bytes=/{b=$2}/^stack_ceiling_o2=/{c=$2}END{if(b!="")printf "caller-stack bound       : %s B  (gcc -O2, ceiling %s, excl. externs)\n",b,c}' "$tmp/st.txt"
kvs 'm.txt|matrix_ok|matrix round-trips      : ' 'm.txt|full_total|corpus full_total       : '
awk -F= '/^home_size_better=/{b=$2}/^home_size_worse=/{w=$2}/^home_size_equal=/{e=$2}END{if(b!="")printf "home size split         : %s better / %s worse / %s equal\n",b,w,e}' "$tmp/m.txt"
kvs 'm.txt|foreign_ok|foreign round-trips     : ' 'm.txt|foreign_total|foreign full_total      : ' 'c.txt|oneface_grow|one-face grow            : ' 'c.txt|oneface_revert|one-face revert          : ' 'm.txt|max_amplified|NVM rows amplified       : ' 'm.txt|max_maxrowerase|NVM max erases-per-row   : ' 'm.txt|max_inversions|NVM frontier inversions  : ' 'm.txt|max_journal|journal peak slots      : '
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
