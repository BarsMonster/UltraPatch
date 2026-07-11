#!/usr/bin/env bash
# Copyright (c) 2026 Mikhail Svarichevsky <mikhail@zeptobars.com>
# SPDX-License-Identifier: MIT

set -u

MAKE_CMD="${MAKE:-make}"
: "${HOST_TOOL:?run_gate.sh: HOST_TOOL not set by make gate}"
: "${DECODER_CANONICAL_HDR:?run_gate.sh: DECODER_CANONICAL_HDR not set by make gate}"
: "${RELEASE_PROFILE:?run_gate.sh: RELEASE_PROFILE not set by make gate}"
[ -x "$HOST_TOOL" ] || { echo "run_gate.sh: host tool is not executable: $HOST_TOOL" >&2; exit 1; }
[ -r "$DECODER_CANONICAL_HDR" ] || {
  echo "run_gate.sh: canonical decoder header is not readable: $DECODER_CANONICAL_HDR" >&2
  exit 1
}
. "$(dirname "$0")/tempdir.sh"
rc=0

# The corpus encoder saturates every core by itself, while several other gate legs also encode
# nontrivial fixtures.  Leave one quarter of the machine to those concurrent legs; otherwise a
# 32-worker corpus pool oversubscribes the 32-core reference host and needlessly reduces the
# headroom under the 80 s cap.
# Most auxiliary legs run at lower CPU priority so they use the reserved cores without preempting
# the matrix's slow cross-major worker. The two long, mostly single-worker synthetic legs retain
# normal priority so the matrix cannot starve them. Explicit job overrides remain authoritative.
if [ -n "${JOBS:-}" ]; then
  corpus_jobs=$JOBS
else
  cores=$(nproc 2>/dev/null || echo 4)
  corpus_jobs=$((cores * 3 / 4))
  [ "$corpus_jobs" -gt 0 ] || corpus_jobs=1
fi
ab_jobs="${AB_MATRIX_TEST_JOBS:-8}"

echo "running gate (all legs concurrent; corpus jobs=$corpus_jobs; A-B jobs=$ab_jobs): check-release-inventory + check-pack-corpus + check-assets + check + check-malformed + check-edge + check-degrade + check-golden + check-decoder-contract + check-models + check-wire-config + check-arm + check-stack + check-ab-matrix + check-release-gate-contract + check-corpus..."

LEGS="check-release-inventory-internal:inventory.txt:check-release-inventory check-pack-corpus-internal:package.txt:check-pack-corpus check-assets-internal:assets.txt:check-assets check-internal:c.txt:check check-malformed-internal:malformed.txt:check-malformed check-edge-internal:e.txt:check-edge check-degrade-internal:dg.txt:check-degrade check-golden-internal:g.txt:check-golden check-decoder-contract-internal:dec_contract.txt:check-decoder-contract check-models-internal:models.txt:check-models check-wire-config-internal:wire_config.txt:check-wire-config check-arm-internal:a.txt:check-arm check-stack-internal:st.txt:check-stack check-ab-matrix-internal:ab.txt:check-ab-matrix check-release-gate-contract-internal:release_gate.txt:check-release-gate-contract check-corpus-matrix-internal:m.txt:check-corpus"

pids=""
for spec in $LEGS; do
  IFS=: read -r target file _ <<<"$spec"
  # gate-internal already published the exact profile-specific host tool and canonical generated
  # decoder header before this fork. Pass both with `-o` (assume-old), so every leg consumes the
  # same pre-fork artifacts even if an input mtime changes while the gate is running.
  if [ "$target" = check-corpus-matrix-internal ]; then
    "$MAKE_CMD" --no-print-directory -o "$HOST_TOOL" -o "$DECODER_CANONICAL_HDR" \
      JOBS="$corpus_jobs" "$target" >"$tmp/$file" 2>&1 &
  elif [ "$target" = check-degrade-internal ] || [ "$target" = check-edge-internal ]; then
    "$MAKE_CMD" --no-print-directory -o "$HOST_TOOL" -o "$DECODER_CANONICAL_HDR" \
      "$target" >"$tmp/$file" 2>&1 &
  elif [ "$target" = check-ab-matrix-internal ]; then
    nice -n 5 "$MAKE_CMD" --no-print-directory -o "$HOST_TOOL" -o "$DECODER_CANONICAL_HDR" \
      AB_MATRIX_TEST_JOBS="$ab_jobs" "$target" >"$tmp/$file" 2>&1 &
  else
    nice -n 10 "$MAKE_CMD" --no-print-directory -o "$HOST_TOOL" -o "$DECODER_CANONICAL_HDR" \
      "$target" >"$tmp/$file" 2>&1 &
  fi
  pids="$pids $!"
done
for p in $pids; do
  wait "$p" || rc=1
done

# A zero child status is necessary but not sufficient: the release gate must receive the complete
# proof it claims to summarize. Require each authoritative marker exactly once, validate fixed
# cardinalities, and independently check numeric ratchets before allowing PASS. This catches an
# accidentally empty/no-op leg as well as truncated or structurally ambiguous output.
METRIC_VALUE=
evidence_error() {
  printf '%s\n' "$*" >&2
  rc=1
}
load_metric() {
  local file=$1 key=$2 count
  count=$(grep -c "^${key}=" "$tmp/$file" 2>/dev/null || :)
  if [ "$count" -ne 1 ]; then
    evidence_error "missing or duplicate gate evidence: $file:$key (count=$count)"
    METRIC_VALUE=
    return 1
  fi
  METRIC_VALUE=$(sed -n "s/^${key}=//p" "$tmp/$file")
  return 0
}
require_exact() {
  local file=$1 key=$2 expected=$3
  if load_metric "$file" "$key" && [ "$METRIC_VALUE" != "$expected" ]; then
    evidence_error "invalid gate evidence: $file:$key='$METRIC_VALUE' (expected '$expected')"
  fi
}
require_prefix() {
  local file=$1 key=$2 prefix=$3
  if load_metric "$file" "$key"; then
    case "$METRIC_VALUE" in
      "$prefix"*) ;;
      *) evidence_error "invalid gate evidence: $file:$key='$METRIC_VALUE' (expected prefix '$prefix')" ;;
    esac
  fi
}
require_nonempty() {
  local file=$1 key=$2
  if load_metric "$file" "$key" && [ -z "$METRIC_VALUE" ]; then
    evidence_error "invalid gate evidence: $file:$key is empty"
  fi
}
require_uint() {
  local file=$1 key=$2
  if load_metric "$file" "$key"; then
    case "$METRIC_VALUE" in
      ''|*[!0-9]*) evidence_error "invalid gate evidence: $file:$key='$METRIC_VALUE' (expected unsigned integer)" ;;
    esac
  fi
}
require_uint_le() {
  local file=$1 key=$2 limit=$3
  if load_metric "$file" "$key"; then
    case "$METRIC_VALUE" in
      ''|*[!0-9]*) evidence_error "invalid gate evidence: $file:$key='$METRIC_VALUE' (expected unsigned integer)" ;;
      *) [ "$METRIC_VALUE" -le "$limit" ] || evidence_error "invalid gate evidence: $file:$key=$METRIC_VALUE exceeds $limit" ;;
    esac
  fi
}

profile=${RELEASE_PROFILE#release_profile=}
case "$RELEASE_PROFILE" in
  release_profile=*) ;;
  *) evidence_error "invalid gate evidence: release profile marker is malformed" ;;
esac
case "$profile" in
  *[!0-9a-f]*|'') evidence_error "invalid gate evidence: release profile hash is malformed" ;;
esac
[ "${#profile}" -eq 64 ] || evidence_error "invalid gate evidence: release profile hash length is ${#profile}, expected 64"

release_fixtures=${BASE_RELEASE_FIXTURES:?}
release_home_images=${BASE_RELEASE_HOME_IMAGES:?}
release_foreign_images=${BASE_RELEASE_FOREIGN_IMAGES:?}
release_golden_blobs=${BASE_RELEASE_GOLDEN_BLOBS:?}
release_home_pairs=$((release_home_images * release_home_images))
release_foreign_pairs=$(((release_foreign_images - 1) * 2))
release_wire_pairs=$((release_home_pairs + release_foreign_pairs))
release_corpus_assets=$((2 * (release_fixtures + release_home_images)))

require_prefix inventory.txt release_inventory "OK ("
require_exact inventory.txt release_fixture_count "$release_fixtures"
require_exact inventory.txt release_home_images "$release_home_images"
require_exact inventory.txt release_foreign_images "$release_foreign_images"
require_exact inventory.txt release_home_pairs "$release_home_pairs"
require_exact inventory.txt release_foreign_pairs "$release_foreign_pairs"
require_exact inventory.txt release_wire_pairs "$release_wire_pairs"
require_exact inventory.txt release_golden_blobs "$release_golden_blobs"
require_exact inventory.txt release_home_total "${BASE_FULL_TOTAL:?}"
require_exact inventory.txt release_oneface_grow "${BASE_ONEFACE_GROW:?}"
require_exact inventory.txt release_oneface_revert "${BASE_ONEFACE_REVERT:?}"
require_prefix package.txt corpus_package "OK ("
require_exact assets.txt corpus_assets "verified $release_corpus_assets files via test-bench/corpus.sha256"
require_exact assets.txt foreign_assets "verified $release_foreign_images files via test-bench/foreign.sha256"
require_exact malformed.txt malformed_rejects 29
require_exact e.txt edge_cases 16
require_exact e.txt edge_roundtrips 15
require_exact e.txt edge_refusals 1
require_exact e.txt edge_failures 0
require_uint e.txt edge_alt_diff_16k_encode_cpu_ms
require_uint e.txt edge_alt_diff_32k_encode_cpu_ms
require_uint_le e.txt edge_alt_diff_64k_encode_cpu_ms 4999
require_uint_le e.txt edge_alt_diff_256k_encode_cpu_ms 19999
require_uint e.txt edge_alt_diff_16k_encode_wall_ms
require_uint e.txt edge_alt_diff_32k_encode_wall_ms
require_uint e.txt edge_alt_diff_64k_encode_wall_ms
require_uint e.txt edge_alt_diff_256k_encode_wall_ms
require_exact g.txt golden_wire "OK ($release_golden_blobs blobs)"
require_prefix g.txt wire_baseline_update_contract "OK ("
require_exact dec_contract.txt decoder_contract OK
require_prefix dec_contract.txt decoder_portable "OK ("
require_prefix dec_contract.txt decoder_address_contract "OK ("
require_prefix dec_contract.txt decoder_resource_contract "OK ("
require_exact models.txt model_contract OK
require_prefix wire_config.txt wire_config_override "OK ("
require_prefix ab.txt ab_wire_change "OK ("
require_prefix release_gate.txt release_gate_contract "OK ("

for key in degrade_journal_peak degrade_opc_splits degrade_direction degrade_rowwindow \
           degrade_bigspan; do
  require_nonempty dg.txt "$key"
  [ "$METRIC_VALUE" != NA ] || evidence_error "invalid gate evidence: dg.txt:$key=NA"
done
require_exact dg.txt degrade_packed_preserve OK
require_exact dg.txt degrade_packed_correction OK
require_exact dg.txt split_run_budget OK
require_exact dg.txt degrade_cases 7
require_exact dg.txt degrade_fail 0

require_exact a.txt arm_size_integration "static PatchApply wrapper"
require_uint_le a.txt arm_object_text "${BASE_ARM_TEXT:?}"
require_uint_le a.txt arm_object_data "${BASE_ARM_DATA:?}"
require_uint_le a.txt arm_object_bss "${BASE_ARM_BSS:?}"
require_exact a.txt arm_linked_integration "no-startup static PatchApply wrapper + minimal flash stubs"
require_uint_le a.txt arm_linked_text "${BASE_ARM_LINKED_TEXT:?}"
require_uint_le a.txt arm_linked_data "${BASE_ARM_LINKED_DATA:?}"
require_uint_le a.txt arm_linked_bss "${BASE_ARM_LINKED_BSS:?}"
load_metric a.txt arm_linked_runtime_helpers || :
require_exact a.txt soft_div_calls "${BASE_ARM_SOFT_DIV:?}"
require_exact a.txt arm_bss_hard_cap_overrides "REJECTED (object + linked)"
require_exact a.txt arm_package_parity "OK (source + canonical single header; object + linked)"

require_exact st.txt stack_static_integration "static PatchApply wrapper"
require_uint_le st.txt stack_static_bound_bytes "${BASE_STACK_STATIC_CEIL_O2:?}"
require_exact st.txt stack_static_ceiling_o2 "${BASE_STACK_STATIC_CEIL_O2:?}"
require_exact st.txt stack_generic_integration "caller-owned PatchApply * wrapper"
require_uint_le st.txt stack_generic_bound_bytes "${BASE_STACK_GENERIC_CEIL_O2:?}"
require_exact st.txt stack_generic_ceiling_o2 "${BASE_STACK_GENERIC_CEIL_O2:?}"
require_exact st.txt stack_package_parity "OK (source + canonical single header; static + generic)"

require_exact m.txt matrix_ok "$release_home_pairs/$release_home_pairs"
require_exact m.txt full_total "${BASE_FULL_TOTAL:?}"
require_exact m.txt home_size_better 0
require_exact m.txt home_size_worse 0
require_exact m.txt home_size_equal "$release_home_pairs"
require_exact m.txt foreign_ok "$release_foreign_pairs/$release_foreign_pairs"
require_exact m.txt foreign_total "${BASE_FOREIGN_TOTAL:?}"
require_exact m.txt wire_identity "$release_wire_pairs/$release_wire_pairs"
require_uint m.txt max_journal
require_exact m.txt max_amplified 0
require_uint_le m.txt max_maxpageerase 1
require_exact m.txt max_inversions 0
require_exact m.txt max_unaligned 0
require_exact m.txt max_oob_page_writes 0
require_exact m.txt max_canary_corrupt 0
require_exact c.txt oneface_grow "${BASE_ONEFACE_GROW:?}"
require_exact c.txt oneface_revert "${BASE_ONEFACE_REVERT:?}"

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
printf 'release_profile        : %s\n' "${RELEASE_PROFILE#release_profile=}"
kvs 'inventory.txt|release_inventory|release inventory        : '
kvs 'package.txt|corpus_package|corpus package          : '
kvs 'assets.txt|corpus_assets|corpus assets          : ' 'assets.txt|foreign_assets|foreign assets         : ' 'malformed.txt|malformed_rejects|malformed rejects      : '
awk -F= '/^edge_cases=/{c=$2}/^edge_roundtrips=/{r=$2}/^edge_refusals=/{f=$2}END{if(c!="")printf "edge inputs             : %s round-trip + %s refused of %s\n",r,f,c}' "$tmp/e.txt"
awk -F= '/^edge_alt_diff_16k_encode_cpu_ms=/{a=$2}/^edge_alt_diff_32k_encode_cpu_ms=/{b=$2}/^edge_alt_diff_64k_encode_cpu_ms=/{c=$2}/^edge_alt_diff_256k_encode_cpu_ms=/{d=$2}END{if(a!="")printf "alternating-diff CPU    : %s / %s / %s / %s ms  (16/32/64/256 KiB)\n",a,b,c,d}' "$tmp/e.txt"
awk -F= '/^edge_alt_diff_16k_encode_wall_ms=/{a=$2}/^edge_alt_diff_32k_encode_wall_ms=/{b=$2}/^edge_alt_diff_64k_encode_wall_ms=/{c=$2}/^edge_alt_diff_256k_encode_wall_ms=/{d=$2}END{if(a!="")printf "alternating-diff wall   : %s / %s / %s / %s ms  (16/32/64/256 KiB)\n",a,b,c,d}' "$tmp/e.txt"
kvs 'g.txt|golden_wire|golden wire             : ' 'g.txt|wire_baseline_update_contract|baseline update        : ' 'dec_contract.txt|decoder_contract|decoder contract        : ' 'dec_contract.txt|decoder_portable|decoder portability     : ' 'models.txt|model_contract|model contract          : ' 'wire_config.txt|wire_config_override|wire config override    : '
kvs 'ab.txt|ab_wire_change|wire-change A-B check    : '
kvs 'release_gate.txt|release_gate_contract|release gate contract   : '
awk -F= '/^degrade_journal_peak=/{j=$2}/^degrade_opc_splits=/{o=$2}/^degrade_direction=/{d=$2}/^degrade_rowwindow=/{w=$2}/^degrade_bigspan=/{f=$2}/^degrade_packed_preserve=/{p=$2}/^degrade_packed_correction=/{x=$2}/^degrade_cases=/{c=$2}END{if(c!="")printf "degradation paths       : journal_peak=%s opc_splits=%s dir=%s rowwin=%s bigspan=%s packed=%s/%s (%s cases)\n",j,o,d,w,f,p,x,c}' "$tmp/dg.txt"
kvs 'a.txt|arm_size_integration|ARM object integration  : '
awk -F= -v bt="${BASE_ARM_TEXT:?}" -v bd="${BASE_ARM_DATA:?}" -v bb="${BASE_ARM_BSS:?}" \
  '/^arm_object_text=/{t=$2}/^arm_object_data=/{d=$2}/^arm_object_bss=/{b=$2}END{if(t!="")printf "ARM object text/data/bss : %s / %s / %s   (ratchet %s/%s/%s, .bss cap 12288)\n",t,d,b,bt,bd,bb}' "$tmp/a.txt"
kvs 'a.txt|arm_linked_integration|ARM linked integration  : '
awk -F= -v bt="${BASE_ARM_LINKED_TEXT:?}" -v bd="${BASE_ARM_LINKED_DATA:?}" -v bb="${BASE_ARM_LINKED_BSS:?}" '/^arm_linked_text=/{t=$2}/^arm_linked_data=/{d=$2}/^arm_linked_bss=/{b=$2}END{if(t!="")printf "ARM linked text/data/bss : %s / %s / %s   (ratchet %s/%s/%s, .bss cap 12288)\n",t,d,b,bt,bd,bb}' "$tmp/a.txt"
kvs 'a.txt|arm_linked_runtime_helpers|ARM linked helpers      : '
kvs 'a.txt|soft_div_calls|ARM   soft-divide calls  : '
kvs 'a.txt|arm_package_parity|ARM packaging parity   : '
awk -F= '/^stack_static_bound_bytes=/{b=$2}/^stack_static_ceiling_o2=/{c=$2}END{if(b!="")printf "stack, static wrapper   : %s B  (gcc -O2, ceiling %s, excl. externs)\n",b,c}' "$tmp/st.txt"
awk -F= '/^stack_generic_bound_bytes=/{b=$2}/^stack_generic_ceiling_o2=/{c=$2}END{if(b!="")printf "stack, generic pointer  : %s B  (gcc -O2, ceiling %s, excl. externs)\n",b,c}' "$tmp/st.txt"
kvs 'st.txt|stack_package_parity|stack packaging parity : '
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
