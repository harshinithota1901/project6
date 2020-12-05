//page table size in Kb
#define PAGE_SIZE	1
//pages in user process
#define USER_PT_SIZE	32
//frames in memory
#define FT_SIZE 256

//convert address to page index
#define ADDR_TO_PID(addr) addr / (PAGE_SIZE*1024)

//requests user sends to master
enum request {READ=0, WRITE, TERM, CHECK_TERM};
//replies master send to user
enum reply {ALLOW=0, DELAY, DENY};

enum frame_status {FREE=0, DIRTY, USED};

struct page {
	int fid;	//frame id
	unsigned char referenced;		//reference bit
};

struct frame {
	int pid;					//index of page, using the frame
	int upid;
	unsigned int timeStamp;	//for LRU policy
	enum frame_status status;
};

void memory_initialize();
int unused_frame();

void clear_frame(struct frame *frame, const int f);
void used_frame(struct frame *frame, const int f);

void clear_pt(struct page *page_tbl, struct frame * frame_tbl);


int lru_evict(struct frame * frame_tbl);
void lru_update(struct frame * frame_tbl, const int f);
