/*
 * include/linux/topology.h
 *
 * Written by: Matthew Dobson, IBM Corporation
 *
 * Copyright (C) 2002, IBM Corp.
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Send feedback to <colpatch@us.ibm.com>
 */
#ifndef _LINUX_TOPOLOGY_H
#define _LINUX_TOPOLOGY_H

#include <linux/cpumask.h>
#include <linux/bitops.h>
#include <linux/mmzone.h>
#include <linux/smp.h>
#include <asm/topology.h>

#ifndef node_has_online_mem
#define node_has_online_mem(nid) (1)
#endif

#ifndef nr_cpus_node
#define nr_cpus_node(node)				\
	({						\
		node_to_cpumask_ptr(__tmp__, node);	\
		cpus_weight(*__tmp__);			\
	})
#endif

#define for_each_node_with_cpus(node)			\
	for_each_online_node(node)			\
		if (nr_cpus_node(node))

int arch_update_cpu_topology(void);

/* Conform to ACPI 2.0 SLIT distance definitions */
#define LOCAL_DISTANCE		10
#define REMOTE_DISTANCE		20
#ifndef node_distance
#define node_distance(from,to)	((from) == (to) ? LOCAL_DISTANCE : REMOTE_DISTANCE)
#endif
#ifndef RECLAIM_DISTANCE
/*
 * If the distance between nodes in a system is larger than RECLAIM_DISTANCE
 * (in whatever arch specific measurement units returned by node_distance())
 * then switch on zone reclaim on boot.
 */
#define RECLAIM_DISTANCE 20
#endif
#ifndef PENALTY_FOR_NODE_WITH_CPUS
#define PENALTY_FOR_NODE_WITH_CPUS	(1)
#endif

/*
 * Below are the 3 major initializers used in building sched_domains:
 * SD_SIBLING_INIT, for SMT domains
 * SD_CPU_INIT, for SMP domains
 * SD_NODE_INIT, for NUMA domains
 *
 * Any architecture that cares to do any tuning to these values should do so
 * by defining their own arch-specific initializer in include/asm/topology.h.
 * A definition there will automagically override these default initializers
 * and allow arch-specific performance tuning of sched_domains.
 * (Only non-zero and non-null fields need be specified.)
 */

#ifdef CONFIG_SCHED_SMT
/* MCD - Do we really need this?  It is always on if CONFIG_SCHED_SMT is,
 * so can't we drop this in favor of CONFIG_SCHED_SMT?
 */
#define ARCH_HAS_SCHED_WAKE_IDLE
/* Common values for SMT siblings */
#ifndef SD_SIBLING_INIT
#define SD_SIBLING_INIT (struct sched_domain) {		\
	.min_interval		= 1,			\
	.max_interval		= 2,			\
	.busy_factor		= 64,			\
	.imbalance_pct		= 110,			\
	.flags			= SD_LOAD_BALANCE	\
				| SD_BALANCE_NEWIDLE	\
				| SD_BALANCE_FORK	\
				| SD_BALANCE_EXEC	\
				| SD_WAKE_AFFINE	\
				| SD_WAKE_BALANCE	\
				| SD_SHARE_CPUPOWER,	\
	.last_balance		= jiffies,		\
	.balance_interval	= 1,			\
}
#endif
#endif /* CONFIG_SCHED_SMT */

#ifdef CONFIG_SCHED_MC
/* Common values for MC siblings. for now mostly derived from SD_CPU_INIT */
#ifndef SD_MC_INIT
#define SD_MC_INIT (struct sched_domain) {		\
	.min_interval		= 1,			\
	.max_interval		= 4,			\
	.busy_factor		= 64,			\
	.imbalance_pct		= 125,			\
	.cache_nice_tries	= 1,			\
	.busy_idx		= 2,			\
	.wake_idx		= 1,			\
	.forkexec_idx		= 1,			\
	.flags			= SD_LOAD_BALANCE	\
				| SD_BALANCE_FORK	\
				| SD_BALANCE_EXEC	\
				| SD_WAKE_AFFINE	\
				| SD_WAKE_BALANCE	\
				| SD_SHARE_PKG_RESOURCES\
				| BALANCE_FOR_MC_POWER,	\
	.last_balance		= jiffies,		\
	.balance_interval	= 1,			\
}
#endif
#endif /* CONFIG_SCHED_MC */

/* Common values for CPUs */
#ifndef SD_CPU_INIT
#define SD_CPU_INIT (struct sched_domain) {		\
	.min_interval		= 1,			\
	.max_interval		= 4,			\
	.busy_factor		= 64,			\
	.imbalance_pct		= 125,			\
	.cache_nice_tries	= 1,			\
	.busy_idx		= 2,			\
	.idle_idx		= 1,			\
	.newidle_idx		= 2,			\
	.wake_idx		= 1,			\
	.forkexec_idx		= 1,			\
	.flags			= SD_LOAD_BALANCE	\
				| SD_BALANCE_EXEC	\
				| SD_BALANCE_FORK	\
				| SD_WAKE_AFFINE	\
				| SD_WAKE_BALANCE	\
				| BALANCE_FOR_PKG_POWER,\
	.last_balance		= jiffies,		\
	.balance_interval	= 1,			\
}
#endif

/* sched_domains SD_ALLNODES_INIT for NUMA machines */
#define SD_ALLNODES_INIT (struct sched_domain) {	\
	.min_interval		= 64,			\
	.max_interval		= 64*num_online_cpus(),	\
	.busy_factor		= 128,			\
	.imbalance_pct		= 133,			\
	.cache_nice_tries	= 1,			\
	.busy_idx		= 3,			\
	.idle_idx		= 3,			\
	.flags			= SD_LOAD_BALANCE	\
				| SD_BALANCE_NEWIDLE	\
				| SD_WAKE_AFFINE	\
				| SD_SERIALIZE,		\
	.last_balance		= jiffies,		\
	.balance_interval	= 64,			\
}

#ifdef CONFIG_NUMA
#ifndef SD_NODE_INIT
#error Please define an appropriate SD_NODE_INIT in include/asm/topology.h!!!
#endif
#endif /* CONFIG_NUMA */

#ifndef topology_physical_package_id
#define topology_physical_package_id(cpu)	((void)(cpu), -1)
#endif
#ifndef topology_core_id
#define topology_core_id(cpu)			((void)(cpu), 0)
#endif
#ifndef topology_thread_siblings
#define topology_thread_siblings(cpu)		cpumask_of_cpu(cpu)
#endif
#ifndef topology_core_siblings
#define topology_core_siblings(cpu)		cpumask_of_cpu(cpu)
#endif

#endif /* _LINUX_TOPOLOGY_H */
