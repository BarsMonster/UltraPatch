# Shared temporary directory setup for shell gates. Sets $tmp and cleans it on exit.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
