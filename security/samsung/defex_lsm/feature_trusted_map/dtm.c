// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020-2022 Samsung Electronics Co., Ltd. All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */

#include <linux/compat.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/binfmts.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/string.h>
#include "include/defex_config.h"

#if KERNEL_VER_GTE(4, 11, 0)
#include <linux/sched/task.h>
#endif

#include "include/defex_debug.h"
#include "include/dtm.h"
#include "include/dtm_engine.h"
#include "include/dtm_log.h"
#include "include/dtm_utils.h"

#if KERNEL_VER_GTE(5, 9, 0)
struct bprm_page_dump {
	struct page *page;    /* Previously dumped page. */
	char *data;           /* Contents of "page". Size is PAGE_SIZE. */
};

bool bprm_dump_page(struct linux_binprm *bprm, unsigned long pos,
		      struct bprm_page_dump *dump)
{
	struct page *page;
#ifdef CONFIG_MMU
	int ret;
#endif

	/* dump->data is released by tomoyo_find_next_domain(). */
	if (!dump->data) {
		dump->data = kzalloc(PAGE_SIZE, GFP_NOFS);
		if (!dump->data)
			return false;
	}
	/* Same with get_arg_page(bprm, pos, 0) in fs/exec.c */
#ifdef CONFIG_MMU
	/*
	 * This is called at execve() time in order to dig around
	 * in the argv/environment of the new proceess
	 * (represented by bprm).
	 */
	mmap_read_lock(bprm->mm);
	ret = get_user_pages_remote(bprm->mm, pos, 1,
				    FOLL_FORCE, &page, NULL
#if KERNEL_VER_LESS(6, 5, 0)
					, NULL
#endif
					);
	mmap_read_unlock(bprm->mm);
	if (ret <= 0)
		return false;
#else
	page = bprm->page[pos / PAGE_SIZE];
#endif
	if (page != dump->page) {
		const unsigned int offset = pos % PAGE_SIZE;
/*
 * Maybe kmap()/kunmap() should be used here.
 * But remove_arg_zero() uses kmap_atomic()/kunmap_atomic() or kmap_local_page()/kunmap_local()
 * So do I.
 */
#if KERNEL_VER_LESS(5, 11, 0)
		char *kaddr = kmap_atomic(page);
#else
		char *kaddr = kmap_local_page(page);
#endif

		dump->page = page;
		memcpy(dump->data + offset, kaddr + offset,
		       PAGE_SIZE - offset);
#if KERNEL_VER_LESS(5, 11, 0)
		kunmap_atomic(kaddr);
#else
		kunmap_local(kaddr);
#endif
	}
	/* Same with put_arg_page(page) in fs/exec.c */
#ifdef CONFIG_MMU
	put_page(page);
#endif
	return true;
}

static char *bprm_print_bprm(struct linux_binprm *bprm,
			       struct bprm_page_dump *dump)
{
	static const int bprm_buffer_len = 4096 * 2;
	char *buffer = kzalloc(bprm_buffer_len, GFP_NOFS);
	char *cp;
	unsigned long pos = bprm->p;
	int offset = pos % PAGE_SIZE;
	int argv_count = bprm->argc;

	if (!buffer)
		return NULL;
	cp = buffer;
	if (!argv_count)
		return NULL;
	while (argv_count) {
		if (!bprm_dump_page(bprm, pos, dump))
			goto out;
		pos += PAGE_SIZE - offset;
		/* Read. */
		while (offset < PAGE_SIZE) {
			const char *kaddr = dump->data;
			const unsigned char c = kaddr[offset++];

			if (cp - buffer > bprm_buffer_len - 10) { // silently truncate
				*cp++ = ' ';
				goto end_buffer;
			}
			if (c == '\\') {
				*cp++ = '\\';
				*cp++ = '\\';
			} else if (c > ' ' && c < 127) {
				*cp++ = c;
			} else if (!c) {
				*cp++ = ' ';
			} else {
				*cp++ = '\\';
				*cp++ = (c >> 6) + '0';
				*cp++ = ((c >> 3) & 7) + '0';
				*cp++ = (c & 7) + '0';
			}
			if (c)
				continue;
			if (argv_count)
				--argv_count;
			if (!argv_count)
				break;
		}
		offset = 0;
	}
end_buffer:
	*cp = '\0';
	return buffer;
out:
	return NULL;
}
#else
/* From fs/exec.c: SM8150_Q, SM8250_Q, SM8250_R, exynos9820, exynos9830 */
struct user_arg_ptr {
#ifdef CONFIG_COMPAT
	bool is_compat;
#endif
	union {
		const char __user *const __user *native;
#ifdef CONFIG_COMPAT
		const compat_uptr_t __user *compat;
#endif
	} ptr;
};

/* From fs/exec.c: SM8150_Q, SM8250_Q, SM8250_R, exynos9820, exynos9830 */
static const char __user *get_user_arg_ptr(struct user_arg_ptr argv, int nr)
{
	const char __user *native;

#ifdef CONFIG_COMPAT
	if (unlikely(argv.is_compat)) {
		compat_uptr_t compat;

		if (get_user(compat, argv.ptr.compat + nr))
			return ERR_PTR(-EFAULT);

		return compat_ptr(compat);
	}
#endif

	if (get_user(native, argv.ptr.native + nr))
		return ERR_PTR(-EFAULT);

	return native;
}
#endif

static void dtm_kfree_args(struct dtm_context *context)
{
	const char **argv;
	int arg, to_free;

	if (unlikely(!is_dtm_context_valid(context)))
		return;
	argv = context->callee_argv;
	arg = min_t(int, context->callee_argc, DTM_MAX_ARGC);
	to_free = context->callee_copied_argc;
	context->callee_copied_argc = 0;
	while (--arg >= 0 && to_free > 0) {
		if (!argv[arg])
			continue;
		kfree(argv[arg]);
		to_free--;
	}
}

#if KERNEL_VER_GTE(5, 9, 0)
/* Creates unescaped duplicate of the next command-line argument.
 * Essentially reverses bprm_print_bprm().
 * Returns NULL if out of memory or out of arguments. Otherwise, updates
 * parameters to point to next argument and store argument length
 */
static char *argv_dup(const char **argvp, int *arg_len_p)
{
	const char *p = *argvp, *after_arg_p;
	int arg_len;
	char *arg_copy;

	while (*p == ' ')  // skip eventual leading space (just in case)
		++p;
	if (!*p) // just for safety
		return 0;
	// Compute required size of duplicate (offsets 1, 3 explained in next loop)
	for (arg_len = 0; *p && *p != ' '; ++p, ++arg_len)
		if (*p == '\\') {
			if (p[1] == '\\')
				++p;
			else
				p += 3;
		}
	arg_copy = kmalloc(arg_len + 1, GFP_KERNEL);
	if (!arg_copy) {
		defex_log_err("DTM::%s: out of memory", __func__);
		return 0;
	}
	/* Points to next argument (bprm_print_bprm _always_ appends a trailing
	 * space, but test for '\0' just in case)
	 */
	after_arg_p = *p ? p + 1 : p;
	// Now copy, unescaping
	for (p = *argvp, arg_len = 0; *p && *p != ' '; ++p, ++arg_len)
		if (*p == '\\') // An escape must be followed by...
			if (p[1] == '\\') { // ...either a backslash...
				arg_copy[arg_len] = '\\';
				++p;
			} else { // ...or (always 3-digit) octal number
				arg_copy[arg_len] = (char)(((p[1] - '0') << 6)
					+ ((p[2] - '0') << 3) + p[3] - '0');
				p += 3;
			}
		else // plain character
			arg_copy[arg_len] = *p;
	arg_copy[arg_len] = 0;
	if (arg_len_p)
		*arg_len_p = arg_len;
	*argvp = after_arg_p; // Prepare for next argument
	return arg_copy;
}

/*
 * Gets call argument value, copying from user if needed.
 */
const char *dtm_get_callee_arg(struct dtm_context *context, int arg_index)
{
	int max_argc, i_arg, arg_len, arg_len_total;
	const char *argv_p;

	if (unlikely(!is_dtm_context_valid(context)))
		return NULL;
	max_argc = min_t(int, context->callee_argc, DTM_MAX_ARGC);
	if (unlikely((arg_index < 0) || (arg_index >= max_argc)))
		return NULL;
	if (context->callee_argv[arg_index])
		return context->callee_argv[arg_index];

	// Saves duplicates of _all_ arguments
	for (argv_p = context->callee_argv_ref, arg_len_total = 0, i_arg = 0;
		i_arg < max_argc; ++i_arg) {
		context->callee_argv[i_arg] = argv_dup(&argv_p, &arg_len);
		if (!context->callee_argv[i_arg])
			break;
		arg_len_total += arg_len;
	}
	context->callee_copied_argc = i_arg;
	context->callee_copied_args_len =
		context->callee_total_args_len = arg_len_total + i_arg;
	return context->callee_argv[arg_index];
}
#else
const char *dtm_get_callee_arg(struct dtm_context *context, int arg_index)
{
	struct user_arg_ptr argv;
	const char __user *user_str;
	char *copy;
	int max_argc, arg_len, copy_len;

	if (unlikely(!is_dtm_context_valid(context)))
		return NULL;
	max_argc = min_t(int, context->callee_argc, DTM_MAX_ARGC);
	if (unlikely((arg_index < 0) || (arg_index >= max_argc)))
		return NULL;
	if (context->callee_argv[arg_index])
		return context->callee_argv[arg_index];

	argv = *(struct user_arg_ptr *)context->callee_argv_ref;
	user_str = get_user_arg_ptr(argv, arg_index);
	if (IS_ERR(user_str))
		return NULL;

	arg_len = strnlen_user(user_str, MAX_ARG_STRLEN);
	if (unlikely(!arg_len))
		return NULL;

	copy_len = min_t(int, arg_len, DTM_MAX_ARG_STRLEN);
	copy = kzalloc(copy_len, GFP_KERNEL);
	if (unlikely(!copy))
		return NULL;

	if (unlikely(copy_from_user(copy, user_str, copy_len)))
		goto out_free_copy;
	copy[copy_len - 1] = '\0';

	context->callee_argv[arg_index] = copy;
	context->callee_copied_argc++;
	context->callee_copied_args_len += copy_len;
	context->callee_total_args_len += arg_len;
	return copy;

out_free_copy:
	kfree(copy);
	return NULL;
}
#endif

/*
 * Initializes dtm context data structure.
 */
__visible_for_testing bool dtm_context_get(struct dtm_context *context,
					   struct defex_context *defex_context,
					   int callee_argc,
					   void *callee_argv_ref)
{
	memset(context, 0, sizeof(*context));
	context->defex_context = defex_context;
	context->callee_argc = callee_argc;
	context->callee_argv_ref = callee_argv_ref;
	return true;
}

/*
 * Releases resources associated to dtm context.
 */
__visible_for_testing void dtm_context_put(struct dtm_context *context, int free_argv)
{
	if (context->program_name != DTM_UNKNOWN && // avoid free()ing static memory
		context->program_name != context->callee_argv[0]) // avoid double free
		kfree(context->program_name);
	dtm_kfree_args(context);
	if (free_argv)
		kfree(context->callee_argv_ref);
}

/*
 * Gets basename of program for current call.
 */
const char *dtm_get_program_name(struct dtm_context *context)
{
	if (unlikely(!is_dtm_context_valid(context)))
		return NULL;
	if (!context->program_name) {
		context->program_name = dtm_get_callee_arg(context, 0);
		if (!context->program_name)
			context->program_name = DTM_UNKNOWN;
	}
	/* Notice: we can't directly store the result of kbasename() otherwise
	 * dtm_context_put would deallocate incorrectly
	 */
	return kbasename(context->program_name);
}

/**
 * Gets stdin mode bit for current call.
 */
int dtm_get_stdin_mode_bit(struct dtm_context *context)
{
	if (unlikely(!context))
		return DTM_FD_MODE_ERROR;
	if (!context->stdin_mode_bit)
		context->stdin_mode_bit = dtm_get_fd_mode_bit(0);
	return context->stdin_mode_bit;
}

/**
 * Gets stdin mode for current call.
 */
const char *dtm_get_stdin_mode(struct dtm_context *context)
{
	if (unlikely(!context))
		return NULL;
	if (!context->stdin_mode)
		context->stdin_mode = dtm_get_fd_mode_bit_name(
					dtm_get_stdin_mode_bit(context));
	return context->stdin_mode;
}

#if KERNEL_VER_GTE(5, 9, 0)
int defex_trusted_map_lookup(struct defex_context *defex_context,
			     int callee_argc, struct linux_binprm *bprm)
{
	int ret = DTM_DENY;
	struct dtm_context context;
	void *callee_argv_ref;
	struct bprm_page_dump dump = {0, 0};

	callee_argv_ref = bprm_print_bprm(bprm, &dump);

	if (unlikely(!defex_context || !(defex_context->task)))
		goto out;
	if (unlikely(!dtm_context_get(&context, defex_context, callee_argc, callee_argv_ref)))
		goto out;
	ret = dtm_enforce(&context);
	// defex_log_info("DTM RET = %d", ret);
	dtm_context_put(&context, 1);
out:
	kfree(dump.data);
	return ret;
}
#else
int defex_trusted_map_lookup(struct defex_context *defex_context,
			     int callee_argc, void *callee_argv_ref)
{
	int ret = DTM_DENY;
	struct dtm_context context;

	if (unlikely(!defex_context || !(defex_context->task)))
		goto out;
	if (unlikely(!dtm_context_get(&context, defex_context, callee_argc, callee_argv_ref)))
		goto out;
	ret = dtm_enforce(&context);
	dtm_context_put(&context, 0);
out:
	return ret;
}
#endif
