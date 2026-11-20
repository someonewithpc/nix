#!/usr/bin/env bash

# A client of the daemon queues its derivation writes. When the daemon
# fails to write one, the command must fail with the daemon's error and
# print nothing the unqueued write would not have let it print

source common.sh

TODO_NixOS

clearStore
startDaemon

restoreStore() {
    chmod u+w "$NIX_STORE_DIR"
    killDaemon
}
trap restoreStore EXIT

drvExpr() {
    echo "derivation { name = \"write-error-$1\"; builder = \"/bin/sh\"; system = \"x\"; }"
}

# Writes still work while the store is writable
nix-instantiate --expr "$(drvExpr ok)" | grepQuiet write-error-ok

chmod u-w "$NIX_STORE_DIR"

expectWriteError() {
    local out="$TEST_ROOT/write-error.out" err="$TEST_ROOT/write-error.err" res=0
    "$@" > "$out" 2> "$err" || res=$?
    if [[ $res -ne 1 ]]; then
        cat "$err" >&2
        fail "expected exit status 1, got $res from ${*@Q}"
    fi
    grepQuiet "Permission denied" "$err" || { cat "$err" >&2; fail "no write error from ${*@Q}"; }
    grepQuietInverse "later" "$err" || fail "an error after the write was reported instead of the write's"
    [[ ! -s $out ]] || { cat "$out" >&2; fail "${*@Q} printed output"; }
}

expectWriteError nix-instantiate --expr "$(drvExpr a)"
expectWriteError nix-instantiate --eval --read-write-mode --strict --expr "(($(drvExpr b))).drvPath"
expectWriteError nix-instantiate --eval --read-write-mode --json --strict --expr "{ p = ($(drvExpr c)).drvPath; }"
expectWriteError nix eval --raw --expr "($(drvExpr d)).drvPath"
expectWriteError nix eval --json --expr "{ p = ($(drvExpr e)).drvPath; }"
expectWriteError nix eval --expr "($(drvExpr f)).drvPath"

# An error raised after the write was queued: unqueued, the write would
# have failed first
expectWriteError nix-instantiate --eval --read-write-mode --expr "builtins.seq ($(drvExpr g)).drvPath (throw \"later\")"
expectWriteError nix eval --expr "builtins.seq ($(drvExpr h)).drvPath (throw \"later\")"

# A legacy command that builds what it evaluated
echo "[ ($(drvExpr i)) ]" > "$TEST_ROOT/write-error-env.nix"
expectWriteError nix-env -p "$TEST_ROOT/write-error-profile" -f "$TEST_ROOT/write-error-env.nix" -i write-error-i
