/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual module entry/exit control flow; every registration is a host stub. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define __init
#define __exit
#define KERN_DEBUG ""
#define KERN_ERR ""
#define CRYSTALHD_PACKAGE_VERSION "test"
#define printk(...) ((void)0)
#define pr_warn(...) ((void)0)
static bool force_l0s_off, session_l0s_off;
static int bc_chd_driver;
static int v4l2_result, research_result, register_result;
static unsigned v4l2_inits, research_inits, registrations, unregisters;
static unsigned research_cleanups, v4l2_cleanups;
static int crystalhd_v4l2_init(void) { v4l2_inits++; return v4l2_result; }
static int crystalhd_pm_research_init(void) { research_inits++; return research_result; }
static void crystalhd_v4l2_cleanup(void) { v4l2_cleanups++; }
static void crystalhd_pm_research_cleanup(void)
{
    assert(register_result < 0 || unregisters == 1);
    research_cleanups++;
}
static int pci_register_driver(void *driver)
{
    assert(driver == &bc_chd_driver && research_inits == 1 && !research_result);
    registrations++;
    return register_result;
}
static void pci_unregister_driver(void *driver)
{
    assert(driver == &bc_chd_driver && registrations == 1 && !research_cleanups);
    unregisters++;
}
#include "pm-module-lifecycle.h"

static void reset(void)
{
    v4l2_result = research_result = register_result = 0;
    v4l2_inits = research_inits = registrations = unregisters = 0;
    research_cleanups = v4l2_cleanups = 0;
}
int main(void)
{
    reset(); v4l2_result = -1;
    assert(chd_dec_module_init() == -1 && v4l2_inits == 1 && !research_inits && !registrations);
    reset(); research_result = -2;
    assert(chd_dec_module_init() == -2 && !registrations && !research_cleanups && v4l2_cleanups == 1);
    reset(); register_result = -3;
    assert(chd_dec_module_init() == -3 && registrations == 1 && research_cleanups == 1 && v4l2_cleanups == 1);
    reset();
    assert(chd_dec_module_init() == 0 && registrations == 1 && !research_cleanups);
    chd_dec_module_cleanup();
    assert(unregisters == 1 && research_cleanups == 1 && v4l2_cleanups == 1);
    puts("PASS: actual PM module init/registration-failure/exit ordering, 4 cases, no registration executed");
    return 0;
}
