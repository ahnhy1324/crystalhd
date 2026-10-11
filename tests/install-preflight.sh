#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-install-preflight.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT HUP INT TERM
mkdir "$test_dir/bin" "$test_dir/root"
export TEST_ROOT=$test_dir/root
export TEST_PREFLIGHT=$repo_dir/scripts/crystalhd-install-preflight
export TEST_ACTION_LOG=$test_dir/actions
export MOCK_DKMS_STATUS=
export MOCK_DKMS_RESULT=0

cat >"$test_dir/bin/dkms" <<'EOF'
#!/bin/sh
test "$*" = 'status -m crystalhd' || exit 99
[ -z "$MOCK_DKMS_STATUS" ] || printf '%s\n' "$MOCK_DKMS_STATUS"
exit "$MOCK_DKMS_RESULT"
EOF
cat >"$test_dir/bin/mock-make" <<'EOF'
#!/bin/sh
case "$*" in
    *install-preflight*) sh "$TEST_PREFLIGHT" test-kernel "$TEST_ROOT" ;;
    *) printf '%s\n' "$*" >>"$TEST_ACTION_LOG" ;;
esac
EOF
chmod 0755 "$test_dir/bin/dkms" "$test_dir/bin/mock-make"
export PATH=$test_dir/bin:$PATH

expect_refused()
{
    if sh "$TEST_PREFLIGHT" test-kernel "$TEST_ROOT" >"$test_dir/output" 2>&1; then
        echo 'Direct install accepted conflicting or unknown DKMS ownership' >&2
        exit 1
    fi
    grep -q 'README.dkms' "$test_dir/output"
}

sh "$TEST_PREFLIGHT" test-kernel "$TEST_ROOT"
MOCK_DKMS_STATUS='crystalhd/3.10.0, test-kernel, x86_64: installed'
expect_refused

# An already registered version for another kernel still owns future updates.
MOCK_DKMS_STATUS='crystalhd/3.10.1, other-kernel, x86_64: added'
expect_refused

# Under parallel make, reject before either module or runtime installation.
if make -s -C "$repo_dir" -j2 install \
    MAKE="$test_dir/bin/mock-make" >"$test_dir/output" 2>&1; then
    echo 'Top-level install ignored preflight failure' >&2
    exit 1
fi
test ! -e "$TEST_ACTION_LOG"

MOCK_DKMS_STATUS=
MOCK_DKMS_RESULT=2
expect_refused
MOCK_DKMS_RESULT=0

# Orphaned state and compressed module files still block direct ownership.
mkdir -p "$TEST_ROOT/var/lib/dkms/crystalhd/3.10.0"
expect_refused
rmdir "$TEST_ROOT/var/lib/dkms/crystalhd/3.10.0"
mkdir -p "$TEST_ROOT/lib/modules/test-kernel/updates/dkms"
for suffix in '' .gz .xz .zst; do
    module=$TEST_ROOT/lib/modules/test-kernel/updates/dkms/crystalhd.ko$suffix
    : >"$module"
    expect_refused
    rm -- "$module"
done

# DESTDIR stages must remain usable on a development host managed by DKMS.
MOCK_DKMS_STATUS='crystalhd/3.10.0, test-kernel, x86_64: installed'
make -s -C "$repo_dir/driver/linux" -f Makefile.in \
    DESTDIR="$test_dir/stage" install-preflight
test ! -e "$test_dir/stage"

printf '%s\n' 'direct-install ownership preflight: PASS'
