# SPDX-License-Identifier: GPL-2.0-or-later
# Shared source extraction; fixtures supply only types and device boundaries.
BEGIN { if (with_fault == "") with_fault = 1 }
/^static BC_STATUS flea_pm_fault\(/ {
    if (with_fault) { copying = 1; found++ }
}
/^(BC_STATUS crystalhd_flea_cleanup_begin|void crystalhd_flea_cleanup_end)\(/ {
    copying = 1; found++
}
copying { print }
copying && /^}/ { copying = 0 }
END { if (copying || found != 2 + with_fault) exit 1 }
