# Shared temporary directory setup for shell gates. Sets $tmp and cleans it up on
# normal exit AND on the public-target time-cap kill path. The Makefile cap SIGTERMs the whole
# process group on overrun; dash does NOT run an EXIT trap for an untrapped fatal
# signal, so a bare `trap ... EXIT` leaks $tmp in the /bin/sh gate legs. On TERM/INT
# we clean up, restore the default disposition, and re-raise the same signal so the
# shell dies by the signal (status semantics preserved); `timeout` still reports 124
# at the cap boundary because it decides 124 from its own timer, not the child status.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
trap 'rm -rf "$tmp"; trap - TERM INT EXIT; kill -s TERM "$$"' TERM
trap 'rm -rf "$tmp"; trap - TERM INT EXIT; kill -s INT "$$"' INT
