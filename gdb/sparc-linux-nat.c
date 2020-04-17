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

/*Added by David Wilkins----->*/

struct sparc_linux_hw_breakpoint{
	//add GDB high level number for reference?
	unsigned int address;
	enum target_hw_bp_type type;
	int enabled;
	int hw_slot;
	int inserted;		//can be ignored, remove
};
static int
hw_breakpoint_equal(struct sparc_linux_hw_breakpoint bp1, struct sparc_linux_hw_breakpoint bp2){
	return bp1.address == bp2.address && bp1.type == bp2.type;
}
#define SPARC_MAX_HW_BPS 4
/*stores information about the hardware breakpoints associated with a certain inferior
 * shall contain inserted breakpoints only*/
typedef struct sparc_linux_inferior_bps{
	ptid_t id;						//the inferior id that this structure stores information for (contains pid, lwp, tid)
	struct sparc_linux_hw_breakpoint *bps[SPARC_MAX_HW_BPS];
	int num;
} *inf_bp_list;
//define vector type for inf_bp_list pointer
DEF_VEC_P(inf_bp_list);
//construct and empty inf_bp_list vector
VEC(inf_bp_list) *inf_list = NULL;
/*returns the breakpoint that equals bp in the inferior list. If none exist, return null*/
static struct sparc_linux_hw_breakpoint *
get_breakpoint_in_inf_list(struct sparc_linux_inferior_bps * list, struct sparc_linux_hw_breakpoint bp){
	int i;
	for(i=0; i<SPARC_MAX_HW_BPS; i++){
		if(list->bps[i] != NULL){
			if(hw_breakpoint_equal(*list->bps[i], bp))
				return list->bps[i];
		}
	}
	return NULL;
}
/*return 0 on succes*/
static int
insert_breakpoint_in_inf_list(struct sparc_linux_inferior_bps * list, struct sparc_linux_hw_breakpoint *bp){
	int i;
	for(i = 0; SPARC_MAX_HW_BPS; i++){
		if(list->bps[i] == NULL){
			list->bps[i] = bp;
			return 0;
		}
	}
	return -1;
}
static int
remove_breakpoint_in_inf_list(struct sparc_linux_inferior_bps * list, struct sparc_linux_hw_breakpoint *bp){
	int i;
	for(i = 0; SPARC_MAX_HW_BPS; i++){
		if(list->bps[i] == bp){
			list->bps[i] = NULL;
			return 0;
		}
	}
	return -1;
}
static int sparc_linux_can_use_hw_breakpoint (struct target_ops *,
				     enum bptype, int, int);
static 
int sparc_linux_can_use_hw_breakpoint (struct target_ops *t,
			enum bptype type, int cnt, int othertype)
{
	
	/*check if cnt exceeds max and check hardware registers
	 * cnt = how many hardware breakpoints are used, othertype is unused (=0)*/
	
	printf("can use\n");
		return 1;				 
}
/*place address correctly*/
static CORE_ADDR sparc_place_addr(CORE_ADDR addr){
	int int_addr = ((unsigned int) addr);
	//clear two LSBs
	return (CORE_ADDR)(int_addr & 0xfffffffc);
}

/*
static int
update_bp_registers (struct lwp_info *lwp, void *arg){
	int i = 0;
	if(lwp->arch_private == NULL){
		lwp->arch_private = XCNEW(struct arch_lwp_info);
	}
	return 0;
}
 * */
/*return sparc_linux_inferior_bps associated with id. If none exist create a new one*/
static struct sparc_linux_inferior_bps*
sparc_linux_get_inferior_bps (ptid_t id){
  int i;
  struct sparc_linux_inferior_bps *inf_bps;
  //iterate through inf_list
  for (i = 0; VEC_iterate(inf_bp_list, inf_list, i, inf_bps); i++){
	  if(ptid_equal(inf_bps->id, id))
		return inf_bps;
  }
  //if none exists create a new bp list for the inferior
  inf_bps = (struct sparc_linux_inferior_bps *) xcalloc (1,sizeof (struct sparc_linux_inferior_bps));
  inf_bps->id = id; 
  inf_bps->num = 0;
  //push to vector
  VEC_safe_push(inf_bp_list, inf_list , inf_bps);
  return inf_bps;
}
static int sparc_linux_insert_hw_breakpoint (struct target_ops *,
				     struct gdbarch *, struct bp_target_info *);
static 
int sparc_linux_insert_hw_breakpoint (struct target_ops *ops,
				     struct gdbarch *arch, struct bp_target_info *info)
{
  struct sparc_linux_hw_breakpoint *bp;
  struct sparc_linux_inferior_bps *inf_bps;		//breakpoints list for this inferior
  CORE_ADDR address;
  int inf_pid;
  int r = 1;
  int i;
  //allocate the new breakpoint
  bp = (struct sparc_linux_hw_breakpoint *) xcalloc (1, sizeof (struct sparc_linux_hw_breakpoint));
  //get bps list for this inferior
  inf_bps = sparc_linux_get_inferior_bps(inferior_ptid);
  //place address so it aligns
  address = sparc_place_addr(info->reqstd_address);
  info->placed_address = address;
  //build bp structure
  bp->address = (unsigned int)address;
  bp->type = hw_execute;
  bp->enabled = 1;					//assuming is enabled for now
  inf_pid = ptid_get_pid(inferior_ptid);
  //check if breakpoint already exists
  if(get_breakpoint_in_inf_list(inf_bps, *bp) != NULL)
	return r;
  printf("insert (%x) pid: %d\n", bp->address, inf_pid);
  //use address br_address and type as data
  r = ptrace(PTRACE_SETHBREGS, inf_pid, bp->address, hw_execute);
  bp->hw_slot = r;
  //if insertion unsuccessful, deallocate. Else place bp in list
  if(r < 0){
	  xfree(bp);
	  return -1;
  }
   insert_breakpoint_in_inf_list(inf_bps, bp);
   return 0;
}

static int sparc_linux_remove_hw_breakpoint (struct target_ops *,
				     struct gdbarch *, struct bp_target_info *);
static 
int sparc_linux_remove_hw_breakpoint (struct target_ops *ops,
				     struct gdbarch *arch, struct bp_target_info *info)
{
	struct sparc_linux_hw_breakpoint comp_bp; 			//comparator breakpoint
	struct sparc_linux_hw_breakpoint *bp;
	struct sparc_linux_inferior_bps *inf_bps;		//breakpoints list for this inferior

	int inf_pid;
	unsigned int address = (unsigned int) info->placed_address;
	//get bps list for this inferior
	inf_bps = sparc_linux_get_inferior_bps(inferior_ptid);
	comp_bp.address = address;
	comp_bp.type = hw_execute;
	printf("remove (%x) pid: %d\n", address, inf_pid);
	inf_pid = ptid_get_pid(inferior_ptid);
	bp = get_breakpoint_in_inf_list(inf_bps, comp_bp);
	if(bp == NULL){
		printf("could not remove: breakpoint not found\n");
		return 1;
	}
	ptrace(PTRACE_SETHBREGS, inf_pid, bp->hw_slot , 4);
	remove_breakpoint_in_inf_list(inf_bps, bp);
	xfree(bp);	
	return 0;				 
}

static void
sparc_linux_prepare_to_resume (struct lwp_info *lwp){
	/*Om det finns någon referens-koppling mellan min låg-nivå lista av hw bps (i GDB) och hög-nivå data strukturen för breakpoints (struct breakpoint) kanske delete remove problemet kan lösas
genom att i "sparc_linux_prepare_to_resume" kolla om hög-nivå breakpointen finns kvar och om inte, ta bort dess låg-nivå respresentation. */
	printf("prepare to resume\n");
}
/*remove all breakpoints in inferior (pid is pid_t of inferior)*/
static void
sparc_linux_forget_process (pid_t pid){
	
	struct sparc_linux_inferior_bps *inf_bps, *p;
	int i, j;
	for (i = 0; VEC_iterate(inf_bp_list, inf_list, i, p); i++){
	  if(p->id.pid == pid){
		  inf_bps = p;
		  break;
	  }
		
	}
	//deallocate all the breakpoints
	for(j = 0; j <SPARC_MAX_HW_BPS; j++){
		if(inf_bps->bps[j] != NULL){
			ptrace(PTRACE_SETHBREGS, getpid(), inf_bps->bps[j]->hw_slot , 4);	//current pid is used since inferior is dead
			xfree(inf_bps->bps[j]);
			inf_bps->bps[j] = NULL;
		}
	}
	//deallocate array
	//xfree(inf_bps->bps);
	//deallocate bp_list
	VEC_ordered_remove(inf_bp_list, inf_list,  i);
	printf("forget process\n");
}

/*
static void
sparc_linux_new_thread (struct lwp_info *lp){
	/ Handle thread creation.  We need to copy the breakpoints and watchpoints
   in the parent thread to the child thread. /
}
*/
/*
static void
arm_linux_new_fork (struct lwp_info *parent, pid_t child_pid){
	/ linux_nat_new_fork hook.  /
}
*/
/*<-----added by david wilkins*/
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
  
  
  //linux_nat_set_new_thread (t, sparc_linux_new_thread);					//added by David WIlkins
  linux_nat_set_prepare_to_resume (t, sparc_linux_prepare_to_resume);	//added by David Wilkins
  
 // linux_nat_set_new_fork (t, sparc_linux_new_fork);						//added by David WIlkins
  linux_nat_set_forget_process (t, sparc_linux_forget_process);			//added by David Wilkins
}
