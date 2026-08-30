/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTALHD_COMPAT_H_
#define _CRYSTALHD_COMPAT_H_

#include <linux/device.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
#error "CrystalHD requires Linux 6.1 or newer"
#endif

static inline struct class *crystalhd_class_create(const char *name)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
	return class_create(THIS_MODULE, name);
#else
	return class_create(name);
#endif
}

static inline long crystalhd_get_user_pages_remote(struct mm_struct *mm,
						     unsigned long start,
						     unsigned long nr_pages,
						     unsigned int gup_flags,
						     struct page **pages)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
	return get_user_pages_remote(mm, start, nr_pages, gup_flags, pages,
				     NULL, NULL);
#else
	return get_user_pages_remote(mm, start, nr_pages, gup_flags, pages,
				     NULL);
#endif
}

#endif
