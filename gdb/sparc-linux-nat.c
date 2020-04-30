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
#ifndef PTRACE_GETHBREGS
#define PTRACE_GETHBREGS 28
#endif /*PTRACE_GETHBREGS*/


static int sparc_linux_remove_hw_breakpoint (struct target_ops *,
				     struct gdbarch *, struct bp_target_info *);

static int sparc_linux_insert_hw_breakpoint (struct target_ops *,
				     struct gdbarch *, struct bp_target_info *);
					
static int sparc_linux_remove_watchpoint (struct target_ops *ops, CORE_ADDR addr, int len,
				 enum target_hw_bp_type type, struct expression *cond);
				 
static int sparc_linux_insert_watchpoint (struct target_ops *ops, CORE_ADDR addr, int len,
				 enum target_hw_bp_type type, struct expression *cond);
				 
static int sparc_linux_insert_mask_watchpoint (struct target_ops *ops, CORE_ADDR addr, 
				  CORE_ADDR mask, enum target_hw_bp_type type);
				  
static int sparc_linux_insert_mask_watchpoint (struct target_ops *ops, CORE_ADDR addr, 
				  CORE_ADDR mask, enum target_hw_bp_type type);
				  
static int to_region_ok_for_hw_watchpoint (struct target_ops *ops,
					   CORE_ADDR addr, int len);
static int sparc_linux_watchpoint_addr_within_range (struct target_ops *ops,
					    CORE_ADDR addr, CORE_ADDR start, int len);
						
static int sparc_linux_stopped_by_watchpoint (struct target_ops *ops);

int sparc_linux_stopped_data_address (struct target_ops *ops, CORE_ADDR *addr_p);

				 

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
	unsigned int mask;
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
static int sparc_linux_can_use_hw_breakpoint (struct target_ops *,
				     enum bptype, int, int);
static 
int sparc_linux_can_use_hw_breakpoint (struct target_ops *t,
			enum bptype type, int cnt, int othertype)
{
	/*can not check registers since child isn't created yet*/
	if(cnt >= SPARC_MAX_HW_BPS)
		return -1;
	return 1;			 
}
/*if mask equals 0 the default mask will be used*/
static 
int sparc_linux_insert_hw_breakpoint_1 (CORE_ADDR address, 
					  enum target_hw_bp_type type, unsigned int mask)
{
  struct sparc_linux_hw_breakpoint *bp;
  struct sparc_linux_inferior_bps *inf_bps;		//breakpoints list for this inferior
  int inf_pid;
  int r;
  int r2;
  int i;
  
   //get bps list for this inferior
  inf_bps = sparc_linux_get_inferior_bps(inferior_ptid);
  //allocate the new breakpoint
  bp = (struct sparc_linux_hw_breakpoint *) xcalloc (1, sizeof (struct sparc_linux_hw_breakpoint));
  //build bp structure
  bp->address = (unsigned int)address;
  bp->type = type;
  bp->enabled = 1;					//assuming is enabled for now
  inf_pid = ptid_get_pid(inferior_ptid);
  //check if breakpoint already exists
  if(get_breakpoint_in_inf_list(inf_bps, *bp) != NULL){
    printf("could not insert: breakpoint already exists\n");
	goto dealloc;
  }
  
  if(insert_breakpoint_in_inf_list(inf_bps, bp) != 0){
	  printf("could not insert: breakpoint list full\n");
	  goto dealloc;
  }
  printf("insert (%x) pid: %d\n", bp->address, inf_pid);
  //use address br_address and type as data
  r = ptrace(PTRACE_SETHBREGS, inf_pid, bp->address, bp->type);
  bp->hw_slot = r;
  //if insertion unsuccessful, deallocate. Else place bp in list
  if(r < 0)
	goto dealloc;
	
  /*change mask*/
  if(mask != 0){
	r2 = ptrace(PTRACE_SETHBREGS, inf_pid, bp->mask, 4 + r);
	bp->mask = mask;
  }
	
  return 0;
  
  dealloc: xfree(bp);
  return -1;
  
}
static 
int sparc_linux_remove_hw_breakpoint_1(struct sparc_linux_hw_breakpoint comp_bp)
{
	struct sparc_linux_hw_breakpoint *bp;
	struct sparc_linux_inferior_bps *inf_bps;		//breakpoints list for this inferior
	int r = 0;
	int inf_pid;
	
	//get bps list for this inferior
	inf_bps = sparc_linux_get_inferior_bps(inferior_ptid);	
	inf_pid = ptid_get_pid(inferior_ptid);
	printf("remove (%x) pid: %d\n", comp_bp.address, inf_pid);
	bp = get_breakpoint_in_inf_list(inf_bps, comp_bp);
	if(bp == NULL){
		printf("could not remove: breakpoint not found\n");
		return -1;
	}
	r = ptrace(PTRACE_SETHBREGS, inf_pid, bp->hw_slot , 8);
	remove_breakpoint_in_inf_list(inf_bps, bp);
	if(r < 0)
		return -1;
	xfree(bp);	
	return 0;				 
}


static 
int sparc_linux_remove_hw_breakpoint (struct target_ops *ops,
				     struct gdbarch *arch, struct bp_target_info *info)
{
	struct sparc_linux_hw_breakpoint comp_bp; 			//comparator breakpoint
	comp_bp.address = (unsigned int) info->placed_address;
	comp_bp.type = hw_execute;
	return sparc_linux_remove_hw_breakpoint_1(comp_bp);
}

static 
int sparc_linux_insert_hw_breakpoint (struct target_ops *ops,
				     struct gdbarch *arch, struct bp_target_info *info)
{
	CORE_ADDR address = sparc_place_addr(info->reqstd_address);
    info->placed_address = address;
	return sparc_linux_insert_hw_breakpoint_1(address, hw_execute, 0);
}

/* Set/clear a hardware watchpoint starting at ADDR, for LEN bytes.
   TYPE is 0 for write, 1 for read, and 2 for read/write accesses.
   COND is the expression for its condition, or NULL if there's none.
   Returns 0 for success, 1 if the watchpoint type is not supported,
   -1 for failure.  */
static
int sparc_linux_remove_watchpoint (struct target_ops *ops, CORE_ADDR addr, int len,
				 enum target_hw_bp_type type, struct expression *cond)
{
  struct sparc_linux_hw_breakpoint comp_bp; 			//comparator breakpoint
  comp_bp.address = (unsigned int)addr;
  comp_bp.type = type;
  return sparc_linux_remove_hw_breakpoint_1(comp_bp);
}

static			 
int sparc_linux_insert_watchpoint (struct target_ops *ops, CORE_ADDR addr, int len,
				 enum target_hw_bp_type type, struct expression *cond)
{
  /*TODO: use length to create a mask*/
  return sparc_linux_insert_hw_breakpoint_1(sparc_place_addr(addr), type, 0);
}

/* Insert a new masked watchpoint at ADDR using the mask MASK.
   RW may be hw_read for a read watchpoint, hw_write for a write watchpoint
   or hw_access for an access watchpoint.  Returns 0 for success, 1 if
   masked watchpoints are not supported, -1 for failure.  */
static
int sparc_linux_insert_mask_watchpoint (struct target_ops *ops, CORE_ADDR addr, 
				  CORE_ADDR mask, enum target_hw_bp_type type)
{
  return sparc_linux_insert_hw_breakpoint_1(sparc_place_addr(addr), type, (unsigned int)mask);
}
	  
	/* Remove a masked watchpoint at ADDR with the mask MASK.
   RW may be hw_read for a read watchpoint, hw_write for a write watchpoint
   or hw_access for an access watchpoint.  Returns 0 for success, non-zero
   for failure.  */
static
int sparc_linux_remove_mask_watchpoint (struct target_ops *ops, CORE_ADDR addr, 
				  CORE_ADDR mask, enum target_hw_bp_type type)
{
  struct sparc_linux_hw_breakpoint comp_bp; 			//comparator breakpoint
  comp_bp.address = (unsigned int)addr;
  comp_bp.type = type;
  return sparc_linux_remove_hw_breakpoint_1(comp_bp);
}

/* Returns the number of debug registers needed to watch the given
   memory region, or zero if not supported.  */
static
int sparc_linux_region_ok_for_hw_watchpoint (struct target_ops *ops,
					   CORE_ADDR addr, int len)
{
	   return 1;
}
  
/* Return non-zero if ADDR is within the range of a watchpoint spanning
   LENGTH bytes beginning at START.  */
static
int sparc_linux_watchpoint_addr_within_range (struct target_ops *ops,
					    CORE_ADDR addr, CORE_ADDR start, int len)
{
			return 1;				
}
	  
/* Returns non-zero if we were stopped by a hardware watchpoint (memory read or
write).  Only the INFERIOR_PTID task is being queried. */
static
int sparc_linux_stopped_by_watchpoint (struct target_ops *ops){
	CORE_ADDR *addr;
	return sparc_linux_stopped_data_address(ops, addr);
}
	  
/* Return non-zero if target knows the data address which triggered this
   target_stopped_by_watchpoint, in such case place it to *ADDR_P.  Only the
   INFERIOR_PTID task is being queried.  */
int sparc_linux_stopped_data_address (struct target_ops *ops, CORE_ADDR *addr_p){
	siginfo_t siginfo;
	struct sparc_linux_hw_breakpoint comp_bp;
	struct sparc_linux_hw_breakpoint *bp;
	struct sparc_linux_inferior_bps *inf_bps;
	//get siginfo
	int inf_pid = ptid_get_pid(inferior_ptid);
	ptrace(PTRACE_GETSIGINFO, inf_pid, 0,&siginfo);
	//get list if breakpoints 
	inf_bps = sparc_linux_get_inferior_bps(inferior_ptid);
	//get get breakpoint that triggered trap
	comp_bp.address = *((unsigned int *)siginfo.si_addr);
	comp_bp.type = 0;		//only write watchpoint supported
	bp = get_breakpoint_in_inf_list(inf_bps, comp_bp);
	
	if(bp == NULL)
		return 0;
		
	*addr_p = *((CORE_ADDR *)siginfo.si_addr);
	return 1;	
}
	   
    
static void
sparc_linux_prepare_to_resume (struct lwp_info *lwp){
	/*Om det finns någon referens-koppling mellan min låg-nivå lista av hw bps (i GDB) och hög-nivå data strukturen för breakpoints (struct breakpoint) kanske delete remove problemet kan lösas
genom att i "sparc_linux_prepare_to_resume" kolla om hög-nivå breakpointen finns kvar och om inte, ta bort dess låg-nivå respresentation. */
	//printf("prepare to resume\n");
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
	if(inf_bps == NULL)
		return;
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
  t->to_insert_watchpoint = sparc_linux_insert_watchpoint; 				//added by David Wilkins
  t->to_remove_watchpoint = sparc_linux_remove_watchpoint; 				//added by David Wilkins
  t->to_insert_mask_watchpoint = sparc_linux_insert_mask_watchpoint; 	//added by David Wilkins
  t->to_remove_mask_watchpoint = sparc_linux_remove_mask_watchpoint; 	//added by David Wilkins
  t->to_watchpoint_addr_within_range = sparc_linux_watchpoint_addr_within_range;
  t->to_region_ok_for_hw_watchpoint = sparc_linux_region_ok_for_hw_watchpoint;
  /* Register the target.  */
  linux_nat_add_target (t);
  /*override linux implemementation of these operations*/
  t->to_stopped_data_address = sparc_linux_stopped_data_address;
  t->to_stopped_by_watchpoint = sparc_linux_stopped_by_watchpoint;
  
  //linux_nat_set_new_thread (t, sparc_linux_new_thread);					//added by David WIlkins
  //observer_attach_thread_exit(sparc_linux_forget_process);
  linux_nat_set_prepare_to_resume (t, sparc_linux_prepare_to_resume);		//added by David Wilkins
  
  //linux_nat_set_new_fork (t, sparc_linux_new_fork);						//added by David WIlkins
  linux_nat_set_forget_process (t, sparc_linux_forget_process);				//added by David Wilkins
}
