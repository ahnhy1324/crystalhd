#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-rx-receipt.XXXXXX")
cleanup()
{
	rm -f "$test_dir/check" "$test_dir/rx-receipt-functions.h"
	rmdir "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

awk '
	/^struct crystalhd_rx_receipt_store[[:space:]]*\{/ { copying = 1; found++ }
	copying { print }
	END { if (found != 1) exit 1 }
' "$repo_dir/driver/linux/crystalhd_rx_receipt.c" > \
	"$test_dir/rx-receipt-functions.h"

# The production/default build has no receipt object.  Every core callsite is
# therefore required to compile entirely through the header's inline no-ops.
"${CC:-cc}" ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror \
	-I"$repo_dir/driver/linux" -fsyntax-only -x c - <<'EOF'
#include <stdbool.h>
#include <stdint.h>
typedef int BC_STATUS;
#include "crystalhd_rx_receipt.h"
void check_default_noops(struct crystalhd_adp *adp, struct crystalhd_hw *hw,
	struct crystalhd_rx_dma_pkt *pkt)
{
	crystalhd_rx_receipt_cleanup(adp);
	crystalhd_rx_receipt_descriptors(pkt, 0);
	crystalhd_rx_receipt_reserve(hw, pkt, 0, 0, 0, 0);
	crystalhd_rx_receipt_fire_return(hw, 0);
	(void)crystalhd_rx_receipt_irq_entry(hw);
	crystalhd_rx_receipt_irq(hw, 0, 0, 0);
	crystalhd_rx_receipt_complete(hw, pkt, 0);
	crystalhd_rx_receipt_parse(hw, pkt, false);
	crystalhd_rx_receipt_disposition(hw, pkt, 0);
}
EOF

for sanitize in no yes; do
	extra=
	if [ "$sanitize" = yes ]; then
		extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
	fi
	# CFLAGS and sanitizer flags intentionally expand as arguments.
	"${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
		-pthread $extra -I"$repo_dir/include" -I"$test_dir" \
		"$repo_dir/tests/rx-receipt.c" -o "$test_dir/check"
	printf 'RX receipt: sanitizers=%s\n' "$sanitize"
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$test_dir/check"
done
