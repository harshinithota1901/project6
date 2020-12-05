#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>

#include "master.h"
#include "blockedq.h"

//maximum time to run
#define MAX_RUNTIME 2
//maximum children to create
#define MAX_CHILDREN 100

//Our program options
static unsigned int arg_c = 100;
static char * arg_l = NULL;
static unsigned int arg_t = MAX_RUNTIME;
static unsigned int arg_m = 0;

static pid_t childpids[MAX_USERS];  //array for user pids
static unsigned int C = 0, E = 0;
static int shmid = -1, msgid = -1;    //shared memory and msg queue ids
static unsigned int interrupted = 0;

static FILE * output = NULL;
static struct shared * shmp = NULL; //pointer to shared memory

static unsigned int pcb_bitmap = 0;

static struct blockedq bq;        //blocked queue

enum _mem_stats {RGRANTED=0, RBLOCKED, RDENIED, TOTAL_REF, TOTAL_RD, TOTAL_WR, TOTAL_EVICT, TOTAL_FAULT};
static unsigned int mem_stats[8];
static time_t start_t = 0;

static void output_result(){

	fprintf(output, "Simulation statistics:\n");
	fprintf(output, "Logical time: %u:%u\n", shmp->vclk.sec, shmp->vclk.ns);

	fprintf(output, "Total references: %d\n", mem_stats[TOTAL_REF]);
	fprintf(output, "Total read: %d\n", mem_stats[TOTAL_RD]);
	fprintf(output, "Total write: %d\n", mem_stats[TOTAL_RD]);

	fprintf(output, "Total evicted: %d\n", mem_stats[TOTAL_EVICT]);
	fprintf(output, "Total faults: %d\n", mem_stats[TOTAL_FAULT]);

	time_t end_t = time(NULL);
	fprintf(output, "Memory accesses/sec: %.2f\n", (float) mem_stats[TOTAL_REF] / (end_t - start_t));
	fprintf(output, "Page fauls per memory access: %.2f\n", (float)mem_stats[TOTAL_FAULT] / mem_stats[TOTAL_REF]);
}

static void list_memory(){
	int i;

	fprintf(output, "[%i:%i] Master: Current memory layout is:\n", shmp->vclk.sec, shmp->vclk.ns);
  fprintf(output, "\t\t\t\tOccupied\tDirtyBit\ttimeStamp\n");

	for(i=0; i < FT_SIZE; i++){

    const struct frame * fr = &shmp->frames[i];

    const char * is_loaded = (fr->pid >= 0) ? "Yes" : "No";
    int is_dirty = 0;
    if(fr->pid > 0){
      is_dirty = (shmp->frames[i].status == DIRTY) ? 1 : 0;
    }
    fprintf(output, "Frame %2d\t\t%s\t\t%7d\t%8d\n", i, is_loaded, is_dirty, fr->timeStamp);
	}
	fprintf(output, "\n");
}

//Called when we receive a signal
static void sign_handler(const int sig)
{
  interrupted = 1;
	fprintf(output, "[%u:%u] Signal %i received\n", shmp->vclk.sec, shmp->vclk.ns, sig);
}

//return 0 or 1 bot bit n from pcb bitmap
static int bit_status(const int n){
  return ((pcb_bitmap & (1 << n)) >> n);
}

//find first available pcb
static int unused_pcb(){
	int i;
  for(i=0; i < MAX_USERS; i++){
  	if(bit_status(i) == 0){
			pcb_bitmap ^= (1 << i);	//raise the bit
      return i;
    }
  }
  return -1;
}

//mark a pcb as unused
void pcb_release(struct process * procs, const unsigned int pi){
  int i;
  struct process * pcb = &shmp->procs[pi];

  //remove proc from blocked q
  for(i=0; i < blockedq_size(&bq); i++){
    if(bq.queue[i] == pi){
      blockedq_deq(&bq, i);
    }
  }

  //clear frames, used by that user
  clear_pt(pcb->pages, shmp->frames);

  pcb_bitmap ^= (1 << pi); //switch bit
  //NOTE: don't bzero, because it breaks p->fid !
  //bzero(&shmp->procs[pi], sizeof(struct process));
  pcb->pid = 0;
  pcb->id = 0;
  pcb->state = READY;
  bzero(pcb->vclk, sizeof(struct vclock)*VCLOCK_COUNT);
  bzero(pcb->weights, sizeof(double)*USER_PT_SIZE);
}


struct process * pcb_get(){
	const int i = unused_pcb();
	if(i == -1){
		return NULL;
	}

  shmp->procs[i].id	= C;
  shmp->procs[i].state = READY;
	return &shmp->procs[i];
}

//Create a child process
static pid_t master_fork(const char *prog)
{

  struct process *pcb = pcb_get();
  if(pcb == NULL){
    fprintf(output, "Warning: No pcb available\n");
    return 0; //no free processes
  }

  const int pcb_index = pcb - shmp->procs; //process index

	const pid_t pid = fork();  //create process
	if(pid < 0){
		perror("fork");
		return -1;

	}else if(pid == 0){
    char buf[10], buf2[10];
		snprintf(buf, sizeof(buf), "%d", pcb_index);
    snprintf(buf2, sizeof(buf2), "%d", arg_m);

    //run the specified program
		execl(prog, prog, buf, buf2, NULL);
		perror("execl");
		exit(1);

	}else{
    pcb->pid = pid;
    //save child pid
		childpids[C++] = pid;
	}
	return pid;
}

//Wait for all processes to exit
static void master_waitall()
{
  int i;
  for(i=0; i < C; ++i){ //for each process
    if(childpids[i] == 0){  //if pid is zero, process doesn't exist
      continue;
    }

    int status;
    if(waitpid(childpids[i], &status, WNOHANG) > 0){

      if (WIFEXITED(status)) {  //if process exited

        fprintf(output,"Master: Child %u terminated with %i at %u:%u\n",
          childpids[i], WEXITSTATUS(status), shmp->vclk.sec, shmp->vclk.ns);

      }else if(WIFSIGNALED(status)){  //if process was signalled
        fprintf(output,"Master: Child %u killed with signal %d at system time at %u:%u\n",
          childpids[i], WTERMSIG(status), shmp->vclk.sec, shmp->vclk.ns);
      }
      childpids[i] = 0;
    }
  }
}

//Called at end to cleanup all resources and exit
static void master_exit(const int ret)
{
  //tell all users to terminate
  int i;
  for(i=0; i < C; i++){
    if(childpids[i] <= 0){
      continue;
    }
  	kill(childpids[i], SIGTERM);
  }
  master_waitall();

  list_memory();
  output_result();

  if(shmp){
    shmdt(shmp);
    shmctl(shmid, IPC_RMID, NULL);
  }

  if(msgid > 0){
    msgctl(msgid, IPC_RMID, NULL);
  }

  fclose(output);
	exit(ret);
}

static void vclock_increment(struct vclock * x, struct vclock * inc){
  x->sec += inc->sec;
  x->ns += inc->ns;
	if(x->ns > 1000000000){
		x->sec++;
		x->ns = 0;
	}
}

//Move time forward
static int update_timer(struct shared *shmp, struct vclock * fork_vclock)
{
  static const int maxTimeBetweenNewProcsSecs = 1;
  static const int maxTimeBetweenNewProcsNS = 500000;

  struct vclock inc = {0, 100};

  vclock_increment(&shmp->vclk, &inc);
  //usleep(10);
  //fprintf(output, "[%u:%u] Master: Incremented system time with 100 ns\n", shmp->vclk.sec, shmp->vclk.ns);

  //if its time to fork
  if(  (shmp->vclk.sec  > fork_vclock->sec) ||
      ((shmp->vclk.sec == fork_vclock->sec) && (shmp->vclk.ns > fork_vclock->ns))){

    *fork_vclock = shmp->vclk;
    inc.sec = (rand() % maxTimeBetweenNewProcsSecs);
    inc.ns  = (rand() % maxTimeBetweenNewProcsNS);
    vclock_increment(fork_vclock, &inc);

    return 1;
  }

  return 0;
}

//Process program options
static int update_options(const int argc, char * const argv[])
{

  int opt;
	while((opt=getopt(argc, argv, "hc:l:t:m:")) != -1){
		switch(opt){
			case 'h':
				fprintf(output,"Usage: master [-h]\n");
        fprintf(output,"Usage: master [-n x] [-s x] [-t time] infile\n");
				fprintf(output," -h Describe program options\n");
				fprintf(output," -c x Total of child processes (Default is 100)\n");
        fprintf(output," -l filename Log filename (Default is log.txt)\n");
        fprintf(output," -t x Maximum runtime (Default is 2)\n");
        fprintf(output," -m x Request scheme - 0 for random, 1 for weights (Default is 0)\n");
				return 1;

      case 'c':
        arg_c	= atoi(optarg); //convert value -n from string to int
        break;

      case 't':
        arg_t	= atoi(optarg);
        break;

      case 'l':
				arg_l = strdup(optarg);
				break;

      case 'm':
				arg_m = atoi(optarg);
				break;

			default:
				fprintf(output, "Error: Invalid option '%c'\n", opt);
				return -1;
		}
	}

	if(arg_l == NULL){
		arg_l = strdup("log.txt");
	}

  if((arg_m < 0) || (arg_m > 1)){
    fprintf(stderr, "Error: Invalid m\n");
    return -1;
  }
  return 0;
}

//Initialize the shared memory
static int shared_initialize()
{
  key_t key = ftok(FTOK_SHM_PATH, FTOK_SHM_KEY);  //get a key for the shared memory
	if(key == -1){
		perror("ftok");
		return -1;
	}

  const long shared_size = sizeof(struct shared);

	shmid = shmget(key, shared_size, IPC_CREAT | IPC_EXCL | S_IRWXU);
	if(shmid == -1){
		perror("shmget");
		return -1;
	}

  shmp = (struct shared*) shmat(shmid, NULL, 0); //attach it
  if(shmp == NULL){
		perror("shmat");
		return -1;
	}

	key = ftok(FTOK_Q_PATH, FTOK_Q_KEY);
	if(key == -1){
		perror("ftok");
		return -1;
	}

	msgid = msgget(key, IPC_CREAT | IPC_EXCL | 0666);
	if(msgid == -1){
		perror("msgget");
		return -1;
	}
  return 0;
}

//Initialize the master process
static int master_initialize()
{
  int i;

  if(shared_initialize() < 0){
    return -1;
  }

  //zero pids
  bzero(childpids, sizeof(pid_t)*MAX_USERS);

  //zero the shared clock
  shmp->vclk.sec	= 0;
	shmp->vclk.ns	= 0;

  //zero the processes
  bzero(shmp, sizeof(struct shared));
	bzero(mem_stats, sizeof(mem_stats));

  //initialize queues
  blockedq_init(&bq);

	memory_initialize();

  //initialize the page and frame tables
	for(i=0; i < MAX_USERS; i++){
		pcb_release(&shmp->procs[i], i);
  }
  pcb_bitmap = 0;

	//clear frame table
	for(i=0; i < FT_SIZE; i++){
		clear_frame(&shmp->frames[i], i);
  }

	//save start time, so we can calculate memory speed
	start_t = time(NULL);

  return 0;
}

//Send a message to user process. Buffer must be filled!
static int send_msg(struct msgbuf *m)
{
	m->from = getpid();	//mark who is sending the message
	if(msgsnd(msgid, m, MSG_SIZE, 0) == -1){
		perror("msgsnd");
		return -1;
	}
  return 0;
}

static int get_msg(struct msgbuf *m)
{
	if(msgrcv(msgid, (void*)m, MSG_SIZE, getpid(), 0) == -1){
		perror("msgrcv");
		return -1;
	}
	return 0;
}

static int memory_fault(struct msgbuf *mb, struct process * pcb){

	mem_stats[TOTAL_FAULT]++;

	//get page index
  const int pid = ADDR_TO_PID(mb->addr);

	//get a free frame index
  int fid = unused_frame();
  if(fid >= 0){

    if(shmp->frames[fid].pid != -1){
			fprintf(output, "Error: frame bitmap is wrong. Frame %d is used\n", fid);
      master_exit(EXIT_FAILURE);
		}

		fprintf(output, "[%u:%u] Master: Using free frame %d for P%d page %d\n",
      shmp->vclk.sec, shmp->vclk.ns, fid, pcb->id, pid);

  }else{	//no frame is free

		mem_stats[TOTAL_EVICT]++;
	  fid = lru_evict(shmp->frames);

	  struct frame * fr = &shmp->frames[fid];
	  struct page * p = &shmp->procs[fr->upid].pages[fr->pid];

	  fprintf(output, "[%u:%u] Master: Evicting page %d of P%d\n",
	      shmp->vclk.sec, shmp->vclk.ns, fr->pid, fr->upid);

	  if(p->fid < 0){	//if page is empty
		  fprintf(output, "Error: Eviction of empty page\n");
	    master_exit(1);
	  }

	  //page in frame gets excluded
	  p->referenced = 0;

	  fprintf(output, "[%u:%u] Master: Clearing frame %d and swapping in P%d page %d\n",
	    shmp->vclk.sec, shmp->vclk.ns, p->fid, pcb->id, pid);

		if(fr->status != DIRTY){
			struct vclock tinc;
			tinc.sec = 0; tinc.ns = 14;	//disk write takes time
			vclock_increment(&pcb->vclk[BLOCKED_TIME], &tinc);

			fprintf(output, "[%u:%u] Master: Dirty bit of frame %d set, adding additional time to the clock\n",
				shmp->vclk.sec, shmp->vclk.ns, p->fid);

		}

	  fid = p->fid;
	  clear_frame(&shmp->frames[p->fid], p->fid);
	  p->fid = -1;
	}

	//return index of freed/free frame
  return fid;
}

static int memory_load(struct msgbuf *mb, struct process * pcb){
  int rv;
  struct vclock tinc;

  mem_stats[TOTAL_REF]++;

  const int pid = ADDR_TO_PID(mb->addr);
	//check if user is accessing beyond page table
  if(pid >= USER_PT_SIZE){
		fprintf(stderr, "Error: Address beyond page table\n");
		mem_stats[RDENIED]++;
		return DENY;
	}

	struct page * p = &pcb->pages[pid];

	//if page has no frame (not in memory)
  if(p->fid < 0){

    fprintf(output, "[%u:%u] Master: Address %d is not in a frame, pagefault\n",
      shmp->vclk.sec, shmp->vclk.ns, mb->addr);

		//save frame index in page
  	p->fid = memory_fault(mb, pcb);
  	p->referenced = 1;

		//mark frame as used, and save page/user details
    used_frame(&shmp->frames[p->fid], p->fid);
  	shmp->frames[p->fid].pid = pid;
  	shmp->frames[p->fid].upid    = mb->id;

    //14 ms load time for device
    VCLOCK_COPY(pcb->vclk[BLOCKED_TIME], shmp->vclk);
    tinc.sec = 0; tinc.ns = 14;
    vclock_increment(&pcb->vclk[BLOCKED_TIME], &tinc);

		//add to blocked queue
    const int pcb_index = pcb - shmp->procs;
    blockedq_enq(&bq, pcb_index, mb->addr);

		//won't send reply yet
  	rv = DELAY;

  }else{
    tinc.sec = 0; tinc.ns = 10;
    vclock_increment(&shmp->vclk, &tinc);

    lru_update(shmp->frames, p->fid);
    rv = ALLOW;
  }

  return rv;
}

static int dispatch_request(struct msgbuf *mb, struct process * pcb){
  int unblock = 0;

  switch(mb->rw){
    case READ:
      mem_stats[TOTAL_RD]++;
      fprintf(output,"[%u:%u] Master: P%d requesting read of address %d\n",
        shmp->vclk.sec, shmp->vclk.ns, pcb->id, mb->addr);

      mb->result = memory_load(mb, pcb);
      if(mb->result == ALLOW){
        const int pid = ADDR_TO_PID(mb->addr);
        struct page * p = &pcb->pages[pid];

        fprintf(output, "[%u:%u] Master: Address %d in frame %d, giving data to P%d\n", shmp->vclk.sec, shmp->vclk.ns,
    			mb->addr, p->fid, pcb->id);

        unblock = 1;
      }else if(mb->result == DENY){
        fprintf(output, "[%u:%u] Master: Address %d denied to P%d\n", shmp->vclk.sec, shmp->vclk.ns,
    			mb->addr, pcb->id);
        unblock = 1;
      }

      break;

    case WRITE:
      mem_stats[TOTAL_WR]++;
      fprintf(output,"[%u:%u] Master: P%d requesting write of address %d\n",
        shmp->vclk.sec, shmp->vclk.ns, pcb->id, mb->addr);

      mb->result = memory_load(mb, pcb);
      if(mb->result == ALLOW){
          const int pid = ADDR_TO_PID(mb->addr);
          struct page * p = &pcb->pages[pid];

          fprintf(output, "[%u:%u] Master: Address %d in frame %d, writing data to frame\n",
      			shmp->vclk.sec, shmp->vclk.ns, mb->addr, p->fid);

      		struct frame * fr = &shmp->frames[p->fid];
      		fr->status = DIRTY;	//we have written to frame, mark it dirty

          unblock = 1;
      }else if(mb->result == DENY){
        fprintf(output, "[%u:%u] Master: Address %d denied to P%d\n", shmp->vclk.sec, shmp->vclk.ns,
    			mb->addr, pcb->id);
        unblock = 1;
      }
      break;

    case TERM:
      fprintf(output,"[%u:%u] Master: P%d terminates\n", shmp->vclk.sec, shmp->vclk.ns, pcb->id);
      mb->result = 0;
      unblock = 1;
      break;

    case CHECK_TERM:
      fprintf(output,"[%u:%u] Master: P%d checks terminate flag\n",
        shmp->vclk.sec, shmp->vclk.ns, pcb->id);

      mb->result = (shmp->term_flag == 1) ? DENY : ALLOW; //return terminate flag as result
      unblock = 1;
      break;

    default:
    fprintf(output,"[%u:%u] Master: P%d send invalid request %d\n",
      shmp->vclk.sec, shmp->vclk.ns, pcb->id, mb->rw);
      mb->result = DENY;
      unblock = 1;
      break;
  }
  return unblock;
}

static int accept_request(){
  int rv = 0;
  struct msgbuf mb;

  //tell process he can run and get his decision
  if(get_msg(&mb) == -1){
    return -1;
  }

  const int pcb_index = mb.id;
  struct process * pcb = &shmp->procs[pcb_index];

  const int unblock = dispatch_request(&mb, pcb);
  if(unblock){ //if request wasn't blocked
		mb.mtype = pcb->pid;	//send to user
    send_msg(&mb); //unblock waiting user by reply

    if(mb.rw == TERM){
      pcb_release(pcb, pcb_index);
      ++E;
    }
    rv = 1;
  }

  //calculate dispatch time
  struct vclock temp;
  temp.sec = 0;
  temp.ns = rand() % 100;
  fprintf(output,"[%u:%u] Master: total time this dispatching was %d nanoseconds\n", shmp->vclk.sec, shmp->vclk.ns, temp.ns);
  vclock_increment(&shmp->vclk, &temp);

  return rv;
}

static int dispatch_bq(){
  int unblocked = 0;
  struct msgbuf mb;

  while(1){
    const int pos = blockedq_ready(&bq, &shmp->vclk, shmp->procs);
    if(pos == -1){
      break;
    }

    mb.id   = bq.queue[pos];
    mb.addr = bq.addr[pos];
    blockedq_deq(&bq, pos);

    struct process * pcb = &shmp->procs[mb.id];

    //TODO: setup memory page/frame
    const int pid = ADDR_TO_PID(mb.addr);
    struct page * p = &pcb->pages[pid];

    lru_update(shmp->frames, p->fid);

    //change process pcb to ready, and reset timers
    pcb->state = READY;
    //clear blocked time
    pcb->vclk[BLOCKED_TIME].sec = pcb->vclk[BLOCKED_TIME].ns = 0;

    mb.mtype = pcb->pid;	//send to user
    send_msg(&mb);        //unblock waiting user by reply

    unblocked++;
  }

  return unblocked;
}

int main(const int argc, char * const argv[])
{

  if(update_options(argc, argv) < 0){
    master_exit(1);
  }

  output = fopen(arg_l, "w");
  if(output == NULL){
    perror("fopen");
    return 1;
  }

  //signal(SIGCHLD, master_waitall);
  signal(SIGTERM, sign_handler);
  signal(SIGALRM, sign_handler);
  alarm(arg_t);

  if(master_initialize() < 0){
    master_exit(1);
  }


  int list_sec = time(NULL);
  struct vclock fork_vclock = {0,0};

  //run until interrupted
  while(!interrupted){

    if(update_timer(shmp, &fork_vclock) > 0){
      if(C < arg_c){
        const pid_t pid = master_fork("./user");
        fprintf(output,"[%u:%u] Master: Creating new child pid %i\n", shmp->vclk.sec, shmp->vclk.ns, pid);
      }else if(E >= arg_c){  //we have generated all of the children
        interrupted = 1;  //stop master loop
        break;
      }
    }

    const int nref = accept_request() + dispatch_bq();
    if(nref <= 0){
      if(blockedq_size(&bq) > 0){

        struct process * pcb = &shmp->procs[blockedq_top(&bq)];
        fprintf(output,"[%u:%u] Master: No process ready. Setting time to first unblock at %u:%u.\n",
          shmp->vclk.sec, shmp->vclk.ns, pcb->vclk[BLOCKED_TIME].sec, pcb->vclk[BLOCKED_TIME].ns);

        VCLOCK_COPY(shmp->vclk, pcb->vclk[BLOCKED_TIME]);

        dispatch_bq();
      }else{
        //jump to next fork time
        fprintf(output,"[%u:%u] Master: No process ready. Setting time to next fork at %u:%u.\n",
          shmp->vclk.sec, shmp->vclk.ns, fork_vclock.sec, fork_vclock.ns);
        shmp->vclk = fork_vclock;
      }
    }

    //list memory every second
    if((time(NULL) - list_sec) >= 1){
      list_memory();
      list_sec = time(NULL);
    }
	}

  fprintf(output,"[%u:%u] Master exit\n", shmp->vclk.sec, shmp->vclk.ns);
	master_exit(0);

	return 0;
}
