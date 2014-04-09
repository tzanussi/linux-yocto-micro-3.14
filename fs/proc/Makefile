#
# Makefile for the Linux proc filesystem routines.
#

obj-y   += proc.o

proc-y			:= nommu.o task_nommu.o
proc-$(CONFIG_MMU)	:= task_mmu.o

proc-y       += inode.o root.o base.o generic.o array.o \
		fd.o
proc-$(CONFIG_TTY)      += proc_tty.o
proc-y	+= cmdline.o
proc-$(CONFIG_PROC_FS)	+= consoles.o
proc-y	+= cpuinfo.o
proc-$(CONFIG_PROC_FS)	+= devices.o
proc-$(CONFIG_PROC_FS)	+= interrupts.o
proc-$(CONFIG_PROC_FS)	+= loadavg.o
proc-y	+= meminfo.o
proc-$(CONFIG_PROC_FS)	+= stat.o
proc-$(CONFIG_PROC_FS)	+= uptime.o
proc-y	+= version.o
proc-$(CONFIG_PROC_FS)	+= softirqs.o
proc-y	+= namespaces.o
proc-y	+= self.o
proc-$(CONFIG_PROC_SYSCTL)	+= proc_sysctl.o
proc-$(CONFIG_NET)		+= proc_net.o
proc-$(CONFIG_PROC_KCORE)	+= kcore.o
proc-$(CONFIG_PROC_VMCORE)	+= vmcore.o
proc-$(CONFIG_PROC_DEVICETREE)	+= proc_devtree.o
proc-$(CONFIG_PRINTK)	+= kmsg.o
proc-$(CONFIG_PROC_PAGE_MONITOR)	+= page.o
