#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -u

MAKE_CMD="${MAKE:-make}"
: "${HOST_TOOL:?run_gate.sh: HOST_TOOL not set by make gate}"
: "${CORPUS_ASSET_STAMP:?run_gate.sh: CORPUS_ASSET_STAMP not set by make gate}"
: "${RELEASE_PROFILE:?run_gate.sh: RELEASE_PROFILE not set by make gate}"
[ -x "$HOST_TOOL" ] || { echo "run_gate.sh: host tool is not executable: $HOST_TOOL" >&2; exit 1; }
[ -r "$CORPUS_ASSET_STAMP" ] || {
  echo "run_gate.sh: generated corpus is not ready: $CORPUS_ASSET_STAMP" >&2
  exit 1
}
. "$(dirname "$0")/tempdir.sh"
rc=0

# The corpus encoder saturates every core by itself, while several other gate legs also encode
# nontrivial fixtures. Leave one quarter of the machine to those concurrent legs; otherwise a
# 32-worker corpus pool oversubscribes the 32-core reference host and reduces the 80 s headroom.
if [ -n "${JOBS:-}" ]; then
  corpus_jobs=$JOBS
else
  cores=$(nproc 2>/dev/null || echo 4)
  corpus_jobs=$((cores * 3 / 4))
  [ "$corpus_jobs" -gt 0 ] || corpus_jobs=1
fi
ab_jobs="${AB_MATRIX_TEST_JOBS:-8}"

echo "running gate (all legs concurrent; corpus jobs=$corpus_jobs; A-B jobs=$ab_jobs): check-release-inventory + check-assets + check + check-malformed + check-edge + check-degrade + check-golden + check-decoder-contract + check-models + check-arm + check-stack + check-ab-matrix + check-corpus..."

LEGS="check-release-inventory-internal:inventory.txt:check-release-inventory check-assets-internal:assets.txt:check-assets check-internal:c.txt:check check-malformed-internal:malformed.txt:check-malformed check-edge-internal:e.txt:check-edge check-degrade-internal:dg.txt:check-degrade check-golden-internal:g.txt:check-golden check-decoder-contract-internal:dec_contract.txt:check-decoder-contract check-models-internal:models.txt:check-models check-arm-internal:a.txt:check-arm check-stack-internal:st.txt:check-stack check-ab-matrix-internal:ab.txt:check-ab-matrix check-corpus-internal:m.txt:check-corpus"

pids=""
for spec in $LEGS; do
  IFS=: read -r target file _ <<<"$spec"
  # gate-internal published this exact profile-specific tool before forking. `-o` prevents a
  # concurrent leg from rebuilding it after an input mtime changes during the gate.
  if [ "$target" = check-corpus-internal ]; then
    "$MAKE_CMD" --no-print-directory -o "$HOST_TOOL" -o "$CORPUS_ASSET_STAMP" \
      JOBS="$corpus_jobs" "$target" >"$tmp/$file" 2>&1 &
  elif [ "$target" = check-degrade-internal ] || [ "$target" = check-edge-internal ]; then
    "$MAKE_CMD" --no-print-directory -o "$HOST_TOOL" -o "$CORPUS_ASSET_STAMP" \
      "$target" >"$tmp/$file" 2>&1 &
  elif [ "$target" = check-ab-matrix-internal ]; then
    nice -n 5 "$MAKE_CMD" --no-print-directory -o "$HOST_TOOL" -o "$CORPUS_ASSET_STAMP" \
      AB_MATRIX_TEST_JOBS="$ab_jobs" "$target" >"$tmp/$file" 2>&1 &
  else
    nice -n 10 "$MAKE_CMD" --no-print-directory -o "$HOST_TOOL" -o "$CORPUS_ASSET_STAMP" \
      "$target" >"$tmp/$file" 2>&1 &
  fi
  pids="$pids $!"
done
for p in $pids; do
  wait "$p" || rc=1
done

# Each child owns its policy and ratchets. The coordinator only requires the complete,
# unambiguous metric schema it summarizes, catching empty/no-op or truncated legs without
# duplicating every child's assertions here.
require_metrics() {
  local spec file keys key count
  for spec in "$@"; do
    IFS=: read -r file keys <<<"$spec"
    for key in $keys; do
      count=$(grep -c "^${key}=" "$tmp/$file" 2>/dev/null || :)
      if [ "$count" -ne 1 ]; then
        echo "missing or duplicate gate metric: $file:$key (count=$count)" >&2
        rc=1
      fi
    done
  done
}
require_metrics \
  'inventory.txt:corpus_topology_contract release_inventory release_fixture_count release_home_images release_foreign_images release_home_pairs release_foreign_edges release_foreign_pairs release_wire_pairs release_golden_blobs release_home_total release_oneface_grow release_oneface_revert' \
  'assets.txt:corpus_assets foreign_assets' \
  'malformed.txt:malformed_rejects' \
  'e.txt:edge_cases edge_roundtrips edge_refusals edge_failures edge_alt_diff_16k_encode_cpu_ms edge_alt_diff_32k_encode_cpu_ms edge_alt_diff_64k_encode_cpu_ms edge_alt_diff_256k_encode_cpu_ms edge_alt_diff_16k_encode_wall_ms edge_alt_diff_32k_encode_wall_ms edge_alt_diff_64k_encode_wall_ms edge_alt_diff_256k_encode_wall_ms' \
  'g.txt:golden_wire' \
  'dec_contract.txt:decoder_contract decoder_portable decoder_address_contract decoder_resource_contract decoder_linkage_contract' \
  'models.txt:model_contract' \
  'ab.txt:ab_wire_change' \
  'dg.txt:degrade_journal_peak degrade_opc_splits degrade_direction degrade_rowwindow degrade_bigspan degrade_packed_preserve degrade_packed_correction split_run_budget degrade_cases degrade_fail' \
  'a.txt:arm_size_integration arm_object_text arm_object_data arm_object_bss arm_linked_integration arm_linked_text arm_linked_data arm_linked_bss arm_linked_runtime_helpers soft_div_calls arm_decoder_build' \
  'st.txt:stack_static_integration stack_static_bound_bytes stack_static_ceiling_o2 stack_generic_integration stack_generic_bound_bytes stack_generic_ceiling_o2 stack_decoder_build' \
  'm.txt:matrix_ok full_total home_size_better home_size_worse home_size_equal foreign_ok foreign_total wire_identity max_amplified max_maxpageerase max_inversions max_unaligned max_oob_page_writes max_canary_corrupt max_journal' \
  'c.txt:oneface_grow oneface_revert'

kv() {
  sed -n "s/^$2=/$3/p" "$tmp/$1"
}
kvs() {
  for spec in "$@"; do
    IFS='|' read -r file key label <<<"$spec"
    kv "$file" "$key" "$label"
  done
}

echo "================ ULTRAPATCH GATE ====================="
printf 'release_profile        : %s\n' "${RELEASE_PROFILE#release_profile=}"
kvs 'inventory.txt|release_inventory|release inventory        : '
kvs 'assets.txt|corpus_assets|corpus assets          : ' 'assets.txt|foreign_assets|foreign assets         : ' 'malformed.txt|malformed_rejects|malformed rejects      : '
awk -F= '/^edge_cases=/{c=$2}/^edge_roundtrips=/{r=$2}/^edge_refusals=/{f=$2}END{if(c!="")printf "edge inputs             : %s round-trip + %s refused of %s\n",r,f,c}' "$tmp/e.txt"
awk -F= '/^edge_alt_diff_16k_encode_cpu_ms=/{a=$2}/^edge_alt_diff_32k_encode_cpu_ms=/{b=$2}/^edge_alt_diff_64k_encode_cpu_ms=/{c=$2}/^edge_alt_diff_256k_encode_cpu_ms=/{d=$2}END{if(a!="")printf "alternating-diff CPU    : %s / %s / %s / %s ms  (16/32/64/256 KiB)\n",a,b,c,d}' "$tmp/e.txt"
awk -F= '/^edge_alt_diff_16k_encode_wall_ms=/{a=$2}/^edge_alt_diff_32k_encode_wall_ms=/{b=$2}/^edge_alt_diff_64k_encode_wall_ms=/{c=$2}/^edge_alt_diff_256k_encode_wall_ms=/{d=$2}END{if(a!="")printf "alternating-diff wall   : %s / %s / %s / %s ms  (16/32/64/256 KiB)\n",a,b,c,d}' "$tmp/e.txt"
kvs 'g.txt|golden_wire|golden wire             : ' 'dec_contract.txt|decoder_contract|decoder contract        : ' 'dec_contract.txt|decoder_linkage_contract|decoder linkage policy: ' 'dec_contract.txt|decoder_portable|decoder portability     : ' 'models.txt|model_contract|model contract          : '
kvs 'ab.txt|ab_wire_change|wire-change A-B check    : '
awk -F= '/^degrade_journal_peak=/{j=$2}/^degrade_opc_splits=/{o=$2}/^degrade_direction=/{d=$2}/^degrade_rowwindow=/{w=$2}/^degrade_bigspan=/{f=$2}/^degrade_packed_preserve=/{p=$2}/^degrade_packed_correction=/{x=$2}/^degrade_cases=/{c=$2}END{if(c!="")printf "degradation paths       : journal_peak=%s opc_splits=%s dir=%s rowwin=%s bigspan=%s packed=%s/%s (%s cases)\n",j,o,d,w,f,p,x,c}' "$tmp/dg.txt"
kvs 'a.txt|arm_size_integration|ARM object integration  : '
awk -F= -v bt="${BASE_ARM_TEXT:?}" -v bd="${BASE_ARM_DATA:?}" -v bb="${BASE_ARM_BSS:?}" '/^arm_object_text=/{t=$2}/^arm_object_data=/{d=$2}/^arm_object_bss=/{b=$2}END{if(t!="")printf "ARM object text/data/bss : %s / %s / %s   (ratchet %s/%s/%s, .bss cap 12288)\n",t,d,b,bt,bd,bb}' "$tmp/a.txt"
kvs 'a.txt|arm_linked_integration|ARM linked integration  : '
awk -F= -v bt="${BASE_ARM_LINKED_TEXT:?}" -v bd="${BASE_ARM_LINKED_DATA:?}" -v bb="${BASE_ARM_LINKED_BSS:?}" '/^arm_linked_text=/{t=$2}/^arm_linked_data=/{d=$2}/^arm_linked_bss=/{b=$2}END{if(t!="")printf "ARM linked text/data/bss : %s / %s / %s   (ratchet %s/%s/%s, .bss cap 12288)\n",t,d,b,bt,bd,bb}' "$tmp/a.txt"
kvs 'a.txt|arm_linked_runtime_helpers|ARM linked helpers      : ' 'a.txt|soft_div_calls|ARM   soft-divide calls  : ' 'a.txt|arm_decoder_build|ARM decoder build      : '
awk -F= '/^stack_static_bound_bytes=/{b=$2}/^stack_static_ceiling_o2=/{c=$2}END{if(b!="")printf "stack, static wrapper   : %s B  (gcc -O2, ceiling %s, excl. externs)\n",b,c}' "$tmp/st.txt"
awk -F= '/^stack_generic_bound_bytes=/{b=$2}/^stack_generic_ceiling_o2=/{c=$2}END{if(b!="")printf "stack, generic pointer  : %s B  (gcc -O2, ceiling %s, excl. externs)\n",b,c}' "$tmp/st.txt"
kvs 'st.txt|stack_decoder_build|stack decoder build    : '
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
