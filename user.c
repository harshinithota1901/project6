#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include "master.h"

static int shmid = -1, msgid = -1;  //semaphore identifier
static struct shared * shmp = NULL;

static struct process *pcb = NULL;
static int my_index = 0;

//Initialize the shared memory pointer
static int shared_initialize()
{
	key_t key = ftok(FTOK_SHM_PATH, FTOK_SHM_KEY);  //get a key for the shared memory
	if(key == -1){
		perror("ftok");
		return -1;
	}

	shmid = shmget(key, 0, 0);
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

	msgid = msgget(key, 0);
	if(msgid == -1){
		perror("msgget");
		return EXIT_FAILURE;
	}

	return 0;
}

static int send_msg(const int msgid, struct msgbuf *m)
{
	m->mtype = getppid();	//send to parent
	m->from = getpid();	//mark who is sending the message
	m->id = pcb->id;
	if(msgsnd(msgid, m, MSG_SIZE, 0) == -1){
		perror("msgsnd");
		return -1;
	}
	return 0;
}

static int get_msg(const int msgid, struct msgbuf *m)
{
	if(msgrcv(msgid, (void*)m, MSG_SIZE, getpid(), 0) == -1){
		perror("msgrcv");
		return -1;
	}
	return 0;
}

static int decide_action()
{
	int action;
	const int number = rand() % 100;
	if(number < 75){
		action = READ;	//READ
	}else{
		action = WRITE;	//WRITE
	}

	return action;
}

static void msg_reference(struct msgbuf *msg, enum request d, const int arg_m){
	int i;

	msg->rw = d;

	if(arg_m == 0){
		//random address
		msg->addr = rand() % (USER_PT_SIZE*1024);
	}else{

		//find the page index using weights
		const double n = fmod(drand48(), pcb->weights[USER_PT_SIZE-1]);
		//search for n in weights[]
		for(i=0; i < USER_PT_SIZE; i++){
			if(n <= pcb->weights[i]){
				break;	//i is our page index
			}
		}

		//calculate page and generate random offset
		msg->addr = (i*1024) + (rand() % 1024);

		//update the weights
		for(i=1; i < USER_PT_SIZE; i++){
			pcb->weights[i] += pcb->weights[i-1];
		}
	}

	msg->result = -1;
}

int main(const int argc, char * const argv[]){
	int i;
	struct msgbuf msg;

	if(shared_initialize() < 0){
		return EXIT_FAILURE;
	}

	my_index = atoi(argv[1]);
	const int arg_m		 = atoi(argv[2]);
  pcb = &shmp->procs[my_index];

	//initialize the rand functions
	srand(getpid());
	srand48(getpid());

	//set the weights
	for(i=0; i < USER_PT_SIZE; i++){
		pcb->weights[i] = 1.0 / (i+1);
	}

	int terminate_me = 0;
	while(terminate_me == 0){

		const int nref_count = 9 + (rand() % 2);	//1000 +/- 100
		for(i=0; i < nref_count; i++){
			int d = decide_action();
			msg_reference(&msg, d,	 arg_m);

			//send request to enter critical section to master
			if(	(send_msg(msgid, &msg) == -1) ||	//lock shared oss clock
					(get_msg( msgid, &msg) == -1) ){
				break;
			}
		}

		//check for termination, by reading invalid address
		msg_reference(&msg, CHECK_TERM, -1);
		//send request to enter critical section to master
		if(	(send_msg(msgid, &msg) == -1) ||	//lock shared oss clock
				(get_msg( msgid, &msg) == -1) ){
			break;
		}

		//if terminal flag is raised
		if(msg.result == DENY){
			terminate_me = 1;
			break;
		}
	}

	//terminate by writing to invalid address
	msg_reference(&msg, TERM, -1);
	send_msg(msgid, &msg);

	printf("USER: Done\n");

	shmdt(shmp);
	return EXIT_SUCCESS;
}
