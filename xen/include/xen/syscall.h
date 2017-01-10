/******************************************************************************
 * syscall.h
 */

#ifndef __XEN_SYSCALL_H__
#define __XEN_SYSCALL_H__

#include <xen/types.h>
#include <xen/time.h>
#include <public/xen.h>
#include <public/domctl.h>
#include <public/sysctl.h>
#include <public/platform.h>
#include <public/event_channel.h>
#include <public/tmem.h>
#include <public/version.h>
#include <public/pmu.h>
#include <asm/hypercall.h>
#include <xsm/xsm.h>

#define __SYSCALL_app_exit	0
#define __SYSCALL_app_console	1

long do_app_exit(uint32_t exit_code);

long do_app_console(XEN_GUEST_HANDLE_PARAM(char) ptr, size_t size);

#endif
