/* This file contains the scheduling policy for SCHED
 *
 * The entry points are:
 *   do_noquantum:        Called on behalf of process' that run out of quantum
 *   do_start_scheduling  Request to start scheduling a proc
 *   do_stop_scheduling   Request to stop scheduling a proc
 *   do_nice		  Request to change the nice level on a proc
 *   init_scheduling      Called from main.c to set up/prepare scheduling
 */
#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
#include "kernel/proc.h" /* for queue constants */

PRIVATE timer_t sched_timer;
PRIVATE unsigned balance_timeout;

#define BALANCE_TIMEOUT	5 /* how often to balance queues in seconds */

FORWARD _PROTOTYPE( int schedule_process, (struct schedproc * rmp)	);
FORWARD _PROTOTYPE( void balance_queues, (struct timer *tp)		);

#define DEFAULT_USER_TIME_SLICE 200

#define DYNAMIC 0

/*===========================================================================*
 *				lottery				     *
 *===========================================================================*/
 PUBLIC int lottery()
 {
	struct schedproc *rmp;
	int proc_nr;
	int rv;
	int winner;
	int total_tickets = 0;
	
	/* Add each participants tickets to the total pile of tickets*/
	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if ((rmp->flags & IN_USE && rmp->is_sys_proc != 1) && rmp->priority == 15) {
			total_tickets += rmp->tickets;
		}
	}
	
	/* This is the basic loop logic for picking a winning process as desc. in our design doc. */
	if (total_tickets > 0) {
		winner = rand() % total_tickets + 1; /* min of 1 ticket */
		for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
			if ((rmp->flags & IN_USE && rmp->is_sys_proc != 1) && rmp->priority == 15) {
				if (winner > 0) {
					winner -= rmp->tickets;
					/*printf("Winners previous priority: %d\n", rmp->priority);*/
					if (winner <=0 && rmp->priority == 15) {
						/*printf("Winners previous priority: %d\n", rmp->priority);*/
						rmp->priority--;
						schedule_process(rmp);
					}
				}
				
			}
		}
	}
	return OK;
 }
 
 
 
/*===========================================================================*
 *				do_noquantum				     *
 *===========================================================================*/

PUBLIC int do_noquantum(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;
	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",
		m_ptr->m_source);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	if (rmp->is_sys_proc == 1 && rmp->priority < (MAX_USER_Q - 1)) {
		rmp->priority += 1; /* lower priority */
	}

	if (rmp->is_sys_proc == 1 && (rv = schedule_process(rmp)) != OK) {
		return rv;
	}
	
	if(DYNAMIC){
		/* This part of our code decreases the number of tickets if a process uses all of its quantum */
		if(rmp->is_sys_proc != 1 && rmp->priority == MAX_USER_Q){
			if(rmp->tickets > 1) rmp->tickets--;
		}
		if(rmp->is_sys_proc != 1 && rmp->priority == MIN_USER_Q){
			/*increase winner's number of tickets*/
		}
	}
	/* Move user-proc back down a priority level */
	if(rmp->is_sys_proc != 1 && rmp->priority == MAX_USER_Q){
		/*m_ptr->SCHEDULING_MAXPRIO = -1;*/
		rmp->priority = MIN_USER_Q;
		schedule_process(rmp);
	}
	
	
	/* run lotery */
	if (rmp->is_sys_proc != 1 && (rv = lottery()) != OK) {
		return rv;
	}
	
	return OK;
}

/*===========================================================================*
 *				do_stop_scheduling			     *
 *===========================================================================*/
PUBLIC int do_stop_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	rmp->flags = 0; /*&= ~IN_USE;*/

	
	/*if(rmp->priority == MAX_USER_Q){
		rmp->priority = MIN_USER_Q;
		schedule_process(rmp);
	}*/
	if (rmp->is_sys_proc != 1 && rmp->priority >= MAX_USER_Q) {
		/*m_ptr->SCHEDULING_MAXPRIO = 1; Adjust tickets*/
		/*printf("I'm in the Kernel panic!!!!!!!!\n");
		printf("I'm in the Kernel panic!!!!!!!!\n");
		printf("I'm in the Kernel panic!!!!!!!!\n");*/
		lottery();
	}
	/*lottery();		<----- KERNEL PANIC */
	return OK;
}

/*===========================================================================*
 *				do_start_scheduling			     *
 *===========================================================================*/
PUBLIC int do_start_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n, parent_nr_n, nice;
	
	/* we can handle two kinds of messages here */
	assert(m_ptr->m_type == SCHEDULING_START || 
		m_ptr->m_type == SCHEDULING_INHERIT);

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	/* Resolve endpoint to proc slot. */
	if ((rv = sched_isemtyendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n))
			!= OK) {
		return rv;
	}
	rmp = &schedproc[proc_nr_n];

	/* Populate process slot */
	rmp->endpoint     = m_ptr->SCHEDULING_ENDPOINT;
	rmp->parent       = m_ptr->SCHEDULING_PARENT;
	rmp->max_priority = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
	if (rmp->max_priority >= NR_SCHED_QUEUES) {
		return EINVAL;
	}
	
	/* Give 20 tickets */
	rmp->tickets = 20;
	rmp->is_sys_proc = 0;
	switch (m_ptr->m_type) {

	case SCHEDULING_START:
		/* We have a special case here for system processes, for which
		 * quanum and priority are set explicitly rather than inherited 
		 * from the parent */
		/*printf("Setting priority to %d.\n", rmp->max_priority);*/
		rmp->priority   = rmp->max_priority;
		rmp->time_slice = (unsigned) m_ptr->SCHEDULING_QUANTUM;
		rmp->is_sys_proc = 1;
		break;
		
	case SCHEDULING_INHERIT:
		/* Inherit current priority and time slice from parent. Since there
		 * is currently only one scheduler scheduling the whole system, this
		 * value is local and we assert that the parent endpoint is valid */
		if ((rv = sched_isokendpt(m_ptr->SCHEDULING_PARENT,
				&parent_nr_n)) != OK)
			return rv;

		/* New user-proc get set to priority 15*/
		rmp->priority = 15;
		rmp->time_slice = schedproc[parent_nr_n].time_slice;
		break;
		
	default: 
		/* not reachable */
		assert(0);
	}

	/* Take over scheduling the process. The kernel reply message populates
	 * the processes current priority and its time slice */
	if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0)) != OK) {
		printf("Sched: Error taking over scheduling for %d, kernel said %d\n",
			rmp->endpoint, rv);
		return rv;
	}
	rmp->flags = IN_USE;

	/* Schedule the process, giving it some quantum */
	if ((rv = schedule_process(rmp)) != OK) {
		printf("Sched: Error while scheduling process, kernel replied %d\n",
			rv);
		return rv;
	}

	/* Mark ourselves as the new scheduler.
	 * By default, processes are scheduled by the parents scheduler. In case
	 * this scheduler would want to delegate scheduling to another
	 * scheduler, it could do so and then write the endpoint of that
	 * scheduler into SCHEDULING_SCHEDULER
	 */

	m_ptr->SCHEDULING_SCHEDULER = SCHED_PROC_NR;

	return OK;
}

/*===========================================================================*
 *				do_nice					     *
 *===========================================================================*/
PUBLIC int do_nice(message *m_ptr)
{
	struct schedproc *rmp;
	int rv;
	int proc_nr_n;
	unsigned new_q, old_q, old_max_q;
	int t_tickets;
	int nice;
	
	
	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	
	/* Store old values, in case we need to roll back the changes */
	old_q     = rmp->priority;
	old_max_q = rmp->max_priority;
	
	/* Increase tickets by nice */
	nice = m_ptr->SCHEDULING_MAXPRIO;
	rmp->tickets += nice;
	/*
	t_tickets = m_ptr->SCHEDULING_MAXPRIO;
	if (rmp->is_sys_proc != 1) {
		new_q = 15;
		rmp->max_priority = 14;
		rmp->priority = new_q;
		
		 Update the proc entry and reschedule the process 
		rmp->tickets += t_tickets;
	} else {
		new_q = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
		rmp->max_priority = rmp->priority = new_q;
	}
	
	if (new_q >= NR_SCHED_QUEUES) {
		return EINVAL;
	}
	*/
	

	if ((rv = schedule_process(rmp)) != OK) {
		/* Something went wrong when rescheduling the process, roll
		 * back the changes to proc struct */
		rmp->priority     = old_q;
		rmp->max_priority = old_max_q;
	}

	return rv;
}

/*===========================================================================*
 *				schedule_process			     *
 *===========================================================================*/
PRIVATE int schedule_process(struct schedproc * rmp)
{
	int rv;

	if ((rv = sys_schedule(rmp->endpoint, rmp->priority,
			rmp->time_slice)) != OK) {
		printf("SCHED: An error occurred when trying to schedule %d: %d\n",
		rmp->endpoint, rv);
	}

	return rv;
}


/*===========================================================================*
 *				init_scheduling			     *
 *===========================================================================*/

PUBLIC void init_scheduling(void)
{
	u64_t s;
	balance_timeout = BALANCE_TIMEOUT * sys_hz();
	init_timer(&sched_timer);
	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
	read_tsc_64(&s);
	srand((unsigned)s.lo);
}

/*===========================================================================*
 *				balance_queues				     *
 *===========================================================================*/

/* This function in called every 100 ticks to rebalance the queues. The current
 * scheduler bumps processes down one priority when ever they run out of
 * quantum. This function will find all proccesses that have been bumped down,
 * and pulls them back up. This default policy will soon be changed.
 */
PRIVATE void balance_queues(struct timer *tp)
{
	struct schedproc *rmp;
	int proc_nr;
	int rv;

	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
			if (rmp->priority > rmp->max_priority &&
				rmp->priority < 14) {
				rmp->priority -= 1; /* increase priority */
				schedule_process(rmp);
			}
		}
	}
	/*lottery();*/
	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}