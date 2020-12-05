#ifndef MASTER_H
#define MASTER_H

#include <unistd.h>
#include "memory.h"

#define MAX_USERS 18

struct vclock {
  unsigned int sec;
	unsigned int ns;
};

//helper functions for virtual clock
#define VCLOCK_COPY(x,y) x.sec = y.sec; x.ns = y.ns;
#define VCLOCK_AVE(x,count) x.sec /= count; x.ns /= count;

enum status_type { READY=1, IOBLK, TERMINATE, DECISON_COUNT};
enum vclock_type { BLOCKED_TIME=0, VCLOCK_COUNT};

// entry in the process control table
struct process {
	int	pid;
	int id;
	enum status_type state;

	struct vclock	vclk[VCLOCK_COUNT];
  struct page   pages[USER_PT_SIZE];		//page table
	double        weights[USER_PT_SIZE];	//weight for each page in child page table
};

//The variables shared between master and palin processes
struct shared {
	struct vclock vclk;
	struct process procs[MAX_USERS];
  int term_flag;

  struct frame  frames[FT_SIZE];	//frame table
};


//shared memory constants
#define FTOK_Q_PATH "/tmp"
#define FTOK_SHM_PATH "/tmp"
#define FTOK_Q_KEY 6776
#define FTOK_SHM_KEY 7667

struct msgbuf {
	long mtype;
	pid_t from;

  int id;
  int rw;	      //receive or return
	int addr;      //value
	int result;
};

#define MSG_SIZE sizeof(pid_t) + (4*sizeof(int))

#endif
