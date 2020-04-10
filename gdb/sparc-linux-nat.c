/* Native-dependent code for GNU/Linux SPARC.
   Copyright (C) 2005-2016 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "regcache.h"

#include <sys/procfs.h>
#include "gregset.h"

#include "sparc-tdep.h"
#include "sparc-nat.h"
#include "inferior.h"
#include "target.h"
#include "linux-nat.h"
//added by David Wilkins
#include "break-common.h"
#include "nat/gdb_ptrace.h"
#include "nat/linux-ptrace.h"

#ifndef PTRACE_SETHBREGS
#define PTRACE_SETHBREGS 27
#endif /*PTRACE_SETHBREGS*/

void
supply_gregset (struct regcache *regcache, const prgregset_t *gregs)
{
  sparc32_supply_gregset (sparc_gregmap, regcache, -1, gregs);
}

void
supply_fpregset (struct regcache *regcache, const prfpregset_t *fpregs)
{
  sparc32_supply_fpregset (sparc_fpregmap, regcache, -1, fpregs);
}

void
fill_gregset (const struct regcache *regcache, prgregset_t *gregs, int regnum)
{
  sparc32_collect_gregset (sparc_gregmap, regcache, regnum, gregs);
}

void
fill_fpregset (const struct regcache *regcache,
	       prfpregset_t *fpregs, int regnum)
{
  sparc32_collect_fpregset (sparc_fpregmap, regcache, regnum, fpregs);
}

/*Added by David Wilkins*/

struct sparc_linux_hw_breakpoint{
	unsigned int address;
	enum target_hw_bp_type type;
	//add control bits
};

static int sparc_linux_can_use_hw_breakpoint (struct target_ops *,
				     enum bptype, int, int);
static 
int sparc_linux_can_use_hw_breakpoint (struct target_ops *t,
			enum bptype arg1, int arg2, int arg3)
{
	printf("can use\n");
		return 1;				 
}
/*place address correctly*/
static CORE_ADDR sparc_place_addr(CORE_ADDR addr){
	int int_addr = ((unsigned int) addr);
	//clear two LSBs
	return (CORE_ADDR)(int_addr & 0xfffffffc);
}
static int sparc_linux_insert_hw_breakpoint (struct target_ops *,
				     struct gdbarch *, struct bp_target_info *);
static 
int sparc_linux_insert_hw_breakpoint (struct target_ops *ops,
				     struct gdbarch *arch, struct bp_target_info *info)
{
	
  struct sparc_linux_hw_breakpoint bp;
  int inf_pid;
  int r = 1;
  printf("insert1\n");
  CORE_ADDR address = sparc_place_addr(info->reqstd_address);
  info->placed_address = address;
  bp.address = (unsigned int) address;
  bp.type = hw_execute;
  
  inf_pid = ptid_get_pid(inferior_ptid);
  //use address br_address and type as data
  printf("insert2\n");
  r = ptrace(PTRACE_SETHBREGS, inf_pid, bp.address, hw_execute);
  printf("insert3\n");
  return r;
}

static int sparc_linux_remove_hw_breakpoint (struct target_ops *,
				     struct gdbarch *, struct bp_target_info *);
static 
int sparc_linux_remove_hw_breakpoint (struct target_ops *ops,
				     struct gdbarch *arch, struct bp_target_info *info)
{
		return 1;				 
}

void _initialize_sparc_linux_nat (void);

void
_initialize_sparc_linux_nat (void)
{
  struct target_ops *t;

  /* Fill in the generic GNU/Linux methods.  */
  t = linux_target ();

  sparc_fpregmap = &sparc32_bsd_fpregmap;

  /* Add our register access methods.  */
  t->to_fetch_registers = sparc_fetch_inferior_registers;
  t->to_store_registers = sparc_store_inferior_registers;
  t->to_can_use_hw_breakpoint = sparc_linux_can_use_hw_breakpoint; 		//added by David Wilkins
  t->to_insert_hw_breakpoint = sparc_linux_insert_hw_breakpoint; 		//added by David Wilkins
  t->to_remove_hw_breakpoint = sparc_linux_remove_hw_breakpoint; 		//added by David Wilkins
  /* Register the target.  */
  linux_nat_add_target (t);
}
