#include <strings.h>
#include "memory.h"

static unsigned char frame_bitmap[(FT_SIZE / 8) + 1];

//return 0 or 1 bot bit n from pcb bitmap
static int bit_status(const int byte, const int n){

  return ((frame_bitmap[byte] & (1 << n)) >> n);
}

//find first available pcb
int unused_frame(){
	int i;
  for(i=0; i < FT_SIZE; i++){
    int byte = i / 8;
    int bit = i % 8;
  	if(bit_status(byte, bit) == 0){
      return i;
    }
  }
  return -1;
}

void memory_initialize(){
  bzero(frame_bitmap, sizeof(frame_bitmap));
}

void clear_frame(struct frame *frame, const int f){
  frame->status = FREE;
  frame->pid  = -1;
  frame->upid = -1;

  frame_bitmap[f / 8] &= ~(1 << (f % 8));  //unset the bit
}

void used_frame(struct frame *frame, const int f){

  frame_bitmap[f / 8] |= (1 << (f % 8));  //set the bit
  frame->status = USED;
}

void clear_pt(struct page *pt, struct frame * ft){
  int i;
	for(i=0; i < USER_PT_SIZE; i++){

		struct page *p = &pt[i];

    if(p->fid >= 0){
			clear_frame(&ft[p->fid], p->fid);
			p->fid = -1;
		}
		p->referenced = 0;
	}
}

int lru_evict(struct frame * frame_tbl){

  int i;
  int lru = 0;
  //find the least recently used frame
  for(i=0; i < FT_SIZE; i++){
    if(frame_tbl[i].timeStamp > frame_tbl[lru].timeStamp){
      lru = i;
    }
  }

  return lru; //return frame holding evicted page

}

void lru_update(struct frame * frame_tbl, const int f){
  int i;
  for(i=0; i < FT_SIZE; i++){
    frame_tbl[i].timeStamp++;
  }
  frame_tbl[f].timeStamp--;  //reduce age of frame that was just used
}
