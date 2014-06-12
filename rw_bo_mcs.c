/*
The MIT License (MIT)

Copyright (c) 2014 Erick Elejalde & Leo Ferres
{eelejalde|lferres}@udec.cl

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

// gcc -g -std=c99 -o lock numa-locks.c -pthread -lrt
#define _GNU_SOURCE

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <malloc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* helper macros */
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

/* Constants */

#define CACHE_ALIGN 64
#ifndef NUMA_NODES
#define NUMA_NODES 2
#endif

/* Numa constants */
enum numastate {GLOBAL_RELEASE = 1, LOCAL_RELEASE, BUSY};
#define MAX_VISITORS 64

/* helper functions */
static inline int getcpu(int *cpu, int *node) {
#ifdef SYS_getcpu
  int status;
  status = syscall(SYS_getcpu, cpu, node, NULL);
  *node = *cpu % 2 ? 1 : 0;
  return (status == -1) ? status : *cpu;
#else
  return -1; // unavailable
#endif
}

/* Delete this function before production */
unsigned long gettid() {
  pthread_t ptid = pthread_self();
  unsigned long threadId = 0;
  memcpy(&threadId, &ptid, min(sizeof(threadId), sizeof(ptid)));
  return threadId;
}

/******************************************************************************/
/* Backoff Lock
/******************************************************************************/
#define BACKOFF_FREE -1
#define BACKOFF_BASE 100
#define BACKOFF_CAP 1000
#define BACKOFF_FACTOR 2
#define REMOTE_BACKOFF_BASE 500
#define REMOTE_BACKOFF_CAP 5000

struct bo {
  volatile int lock;
  int state;
  int visitors;
  bool successor_exists;
  int gstate;
  char pad[47];
};

struct bo *init_bo() {
  struct bo *l = memalign(CACHE_ALIGN, sizeof(struct bo));
  l->lock = BACKOFF_FREE;
  l->state = GLOBAL_RELEASE;
  l->successor_exists = false;
  return l;
}

void destroy_bo(struct bo *l) {
  free(l);
}

bool bo_isalone(struct bo *l) {
  return !l->successor_exists;
}

bool bo_islocked(struct bo *l) {
  return l->lock != BACKOFF_FREE;
}

bool bo_maypass_local(struct bo *l) {
  return l->visitors < MAX_VISITORS;
}

void bo_backoff(int *bwait, int cap) {
  for(int i = *bwait; i; i--);
  *bwait = min(*bwait*BACKOFF_FACTOR, cap);
}

int bo_acquire(struct bo *l, int node) {
  int tmp, bwait; 
  while(true) {
    tmp = __sync_val_compare_and_swap(&l->lock, BACKOFF_FREE, node);
    if(tmp == BACKOFF_FREE) return node;
    if(tmp == node) {
      bwait = BACKOFF_BASE;
      while (true) {
	l->successor_exists = true;
	bo_backoff(&bwait, BACKOFF_CAP);
	tmp = __sync_val_compare_and_swap(&l->lock, BACKOFF_FREE, node);
	if(tmp == BACKOFF_FREE) {
	  l->successor_exists = false;
	  return node;
	}
	if(tmp != node)  {
	  bo_backoff(&bwait, BACKOFF_CAP);
	  break;
	}
      }
    } else {
      bwait = REMOTE_BACKOFF_BASE;
      while (true) {
	l->successor_exists = true;
	bo_backoff(&bwait, REMOTE_BACKOFF_CAP);
	tmp = __sync_val_compare_and_swap(&l->lock, BACKOFF_FREE, node);
	if(tmp == BACKOFF_FREE) {
	  l->successor_exists = false;
	  return node;
	}
	if(tmp == node)  {
	  bo_backoff(&bwait, REMOTE_BACKOFF_CAP);
	  break;
	}
      }
    }
  }
  return node;
}

void bo_release(struct bo *l) {
  l->lock = BACKOFF_FREE;
}

/******************************************************************************/
/* MCS Lock */
/******************************************************************************/
struct mcs {
  int state;
  unsigned long visitors;
  struct mcs *next;
  struct mcs *gstate;
#ifdef __x86_64__
  char pad[40];
#else
  char pad[48];
#endif
};

bool mcs_isalone(struct mcs *l) {
  return l == NULL || l->next == NULL; 
}

bool mcs_islocked(struct mcs *l) {
  return l != NULL;
}


bool mcs_maypass_local(struct mcs *l) {
  return l->visitors < MAX_VISITORS;
}

struct mcs *init_mcs() { 
  return NULL;
}

void destroy_mcs(struct mcs *l) { 
}

struct mcs *mcs_acquire(struct mcs **l) {
  struct mcs *i = memalign(CACHE_ALIGN, sizeof(struct mcs));
  i->next = NULL;
  i->state = GLOBAL_RELEASE;
  i->visitors = 0;
  struct mcs *predecessor = __sync_lock_test_and_set(l,i);
  if(predecessor) {
    i->state = BUSY;
    i->visitors = predecessor->visitors;
    i->gstate = predecessor->gstate;
    predecessor->next = i;
    printf("arrive: %p->%p\n",predecessor, i);
    while (i->state == BUSY);
  }
  (*l)->gstate = i;
  printf("acquire: %p->%p\n",i,i->next);
  return i;
}

struct mcs *mcs_release(struct mcs **l) {
  struct mcs *tmp = (*l)->gstate;
  if(!tmp->next) {
    if(__sync_bool_compare_and_swap(l, tmp, NULL)) {
      free(tmp);
      printf("release: %p->NULL\n",tmp);
      return NULL;
    }
    while(!tmp->next);
  }
  struct mcs *successor = tmp->next;
  free(tmp);
  printf("release: %p->%p\n",tmp, successor);
  return successor;
}

/******************************************************************************/
/* Cohort Lock (BO-MCS) */
/******************************************************************************/

struct c_bo_mcs {
  struct bo *glock;
  struct mcs **llocks;
#ifdef __x86_64__
  char pad[48];
#else
  char pad[56];
#endif
};

struct c_bo_mcs *init_c_bo_mcs() {
  struct c_bo_mcs *l = malloc(sizeof(struct c_bo_mcs));
  l->glock = init_bo();
  l->llocks = memalign(CACHE_ALIGN, sizeof(struct mcs*) * NUMA_NODES);
  for (int i = 0; i < NUMA_NODES; i++)
    l->llocks[i] = NULL;
  return l;
}

void destroy_c_bo_mcs(struct c_bo_mcs *l) {
  for (int i = 0; i < NUMA_NODES; i++)
    destroy_mcs(l->llocks[i]);
  free(l->llocks);
  destroy_bo(l->glock);
  free(l);
}

bool c_bo_mcs_islocked(struct c_bo_mcs *l) {
  return bo_islocked(l->glock);
}

void c_bo_mcs_acquire(struct c_bo_mcs *l, int *node) {
  int cpu, t;
  if (node == NULL)
    getcpu(&cpu, node);
  mcs_acquire(&l->llocks[*node]);
  if(l->llocks[*node]->state == GLOBAL_RELEASE) {
    bo_acquire(l->glock, *node);
    l->llocks[*node]->visitors = 0;
  } else l->llocks[*node]->visitors++;
}

void c_bo_mcs_release(struct c_bo_mcs *l, int node) {
  struct mcs *tmp;
  if(mcs_isalone(l->llocks[node]) || !mcs_maypass_local(l->llocks[node])) {
    bo_release(l->glock);
    if(tmp = mcs_release(&l->llocks[node])) 
      tmp->state = GLOBAL_RELEASE;
  } else
    if(tmp = mcs_release(&l->llocks[node]))
      tmp->state = LOCAL_RELEASE;
}


/******************************************************************************/
/* RW Lock */
/******************************************************************************/

#define PATIENCE 1000

typedef enum _mode {R, W} mode;
typedef enum _pref {NEUTRAL, READER, WRITER, READER_OPT} preference;

struct readindr {
  unsigned arrive;
  unsigned depart;
  char pad[56];
};

struct rw {
  struct readindr *indicators;
  struct c_bo_mcs *writers;
  preference pref;
  int rbarrier;
  int wbarrier;
  bool wactive;
#ifdef __x86_64__
  char pad[28];
#else
  char pad[40];
#endif
};

void readindr_waitempty(struct readindr *indr) {
  unsigned tmp;
  for(int i = 0; i < NUMA_NODES; i++)
    do {
      tmp = indr[i].depart;
    } while(tmp != indr[i].arrive);
}

bool readindr_isempty(struct readindr *indr) {
  unsigned tmp;
  for(int i = 0; i < NUMA_NODES; i++) {
    tmp = indr[i].depart;
    if (tmp != indr[i].arrive)
      return false;
  }
  return true;
}

struct rw *init_rw(preference p) {
  struct rw *l = malloc(sizeof(struct rw));
  l->writers = init_c_bo_mcs();
  l->pref = p;
  l->wactive = false;
  l->indicators = malloc(NUMA_NODES * sizeof(struct readindr));
  for(int i=0; i<NUMA_NODES; i++)
    l->indicators[i].arrive = l->indicators[i].depart = 0;

  return l;
}

void destroy_rw(struct rw *l) {
  destroy_c_bo_mcs(l->writers);
  free(l->indicators);
  free(l);
}

void rw_acquire(struct rw *l, mode m, int *node) {
  int cpu, patience = 0;

  bool braised;

  getcpu(&cpu, node);


  if(m == R) {     /* readers */
    switch (l->pref) {
    case NEUTRAL:
      c_bo_mcs_acquire(l->writers, node);
      __sync_fetch_and_add(&l->indicators[*node].arrive, 1);
      c_bo_mcs_release(l->writers, *node);
      break;

    case READER:
      while(l->rbarrier); /* If we need less granularity, we can change later*/
      __sync_fetch_and_add(&l->indicators[*node].arrive, 1);
      while(c_bo_mcs_islocked(l->writers));

      break;

    case READER_OPT:
      while(l->rbarrier); /* If we need less granularity, we can change later*/
      __sync_fetch_and_add(&l->indicators[*node].arrive, 1);
      while(l->wactive);
      break;

    case WRITER:
      braised = false;

      while (true) {
	__sync_fetch_and_add(&l->indicators[*node].arrive, 1);
	if(c_bo_mcs_islocked(l->writers)) {
	  __sync_fetch_and_add(&l->indicators[*node].depart, 1);
	  while (c_bo_mcs_islocked(l->writers)) {
	    patience++;
    
	    if (patience > PATIENCE && !braised) {
	      __sync_fetch_and_add(&l->wbarrier, 1);	    
	      braised = true;
	    }
	  }
	  continue;
	}

	if(braised)
	  __sync_fetch_and_sub(&l->wbarrier, 1);
	break;
      }
    }
  } else {     /* writers */
    switch (l->pref) {
    case NEUTRAL:
      c_bo_mcs_acquire(l->writers, node);
      readindr_waitempty(l->indicators);
      break;

    case READER:
      braised = false;

      while (true) {
	c_bo_mcs_acquire(l->writers, node);

	if(!readindr_isempty(l->indicators)) {
	  c_bo_mcs_release(l->writers, *node);

	  while (!readindr_isempty(l->indicators)) {
	    patience++;

	    if (patience > PATIENCE && !braised) {
	      /* erect barrier */
	      __sync_fetch_and_add(&l->rbarrier, 1);
	      braised = true;
	    }
	  }
	  continue;
	}

	if(braised)
	  __sync_fetch_and_sub(&l->rbarrier, 1);
	break;
      }

      break;

    case READER_OPT:
      braised = false;
      c_bo_mcs_acquire(l->writers, node);      
      while (true) {
	while(!readindr_isempty(l->indicators)) {
	  patience++;
	  if (patience > PATIENCE && !braised) {
	    __sync_fetch_and_add(&l->rbarrier, 1);
	    braised = true;
	  }
	}

	l->wactive = true;

	if(!readindr_isempty(l->indicators)) {
	  l->wactive = false;
	  continue;
	}

	if(braised)
	  __sync_fetch_and_sub(&l->rbarrier, 1);
	break;
      }
      break;

    case WRITER:
      while(l->wbarrier);

      c_bo_mcs_acquire(l->writers, node);
      readindr_waitempty(l->indicators);
    }
  }
}

void rw_release(struct rw *l, mode m, int node) {
  if(m == R)
    __sync_fetch_and_add(&l->indicators[node].depart, 1);
  else {
    l->wactive = false;
    c_bo_mcs_release(l->writers, node);
  }
}

/******************************************************************************/
/* RWBench Benchmark */
/******************************************************************************/
/*
  wprob: probability to enter the CS in read-write mode
  shared_array: 64 elements integer array shared between all threads
  l: ReadWrite Lock
  WCSLen: time to be elapsed in the critical section when in read-write mode
  RCSLen: time to be elapsed in the critical section when in read-only mode
  RCSLen: time to be elapsed in the non-critical section
*/

double mtimediff(struct timeval te, struct timeval ts) {
 return ((te.tv_sec - ts.tv_sec) * 1000000L +
        (te.tv_usec - ts.tv_usec)) / 1000;
}

struct opts {
  int wcslen;
  int rcslen;
  int ncslen;
  int prob;
  int *array;
  struct rw *lock;
  unsigned msecs;
};

void *rw_bnchmrk(void *arg) {

  /* struct unpack */
  struct opts o = *(struct opts*)arg;
  int wprob = o.prob;
  int *shared_array  = o.array;
  struct rw *l = o.lock;
  int wcslen = o.wcslen;
  int rcslen = o.rcslen;
  int ncslen = o.ncslen;
  unsigned msecs = o.msecs;

  int node, private_array[64];
  long i = 0; /* result */

  struct timeval tsmain, tfmain, tsncs, tfncs, tswcs, tfwcs, tsrcs, tfrcs; 

  int seed = time(NULL);

  gettimeofday(&tsmain, NULL);
  gettimeofday(&tfmain, NULL);
  while (mtimediff(tfmain, tsmain) <= msecs) {
    i++;

    gettimeofday(&tsncs, NULL);
    gettimeofday(&tfncs, NULL);
    while (mtimediff(tfncs, tsncs) <= ncslen) {
      int r = rand_r(&seed) % 500;
      private_array[rand_r(&seed) % 64] += r;
      private_array[rand_r(&seed) % 64] -= r;
      gettimeofday(&tfncs, NULL);      
    }

    int p = rand_r(&seed) % 100;

    if( p < wprob) {

      gettimeofday(&tswcs, NULL);
      rw_acquire(l, W, &node); /*entering the critical section*/


      gettimeofday(&tfwcs, NULL);
      while (mtimediff(tfwcs, tswcs) <= wcslen) {
	int r = rand_r(&seed) % 500;
	shared_array[rand_r(&seed) % 64] += r;
	shared_array[rand_r(&seed) % 64] -= r;
	gettimeofday(&tfwcs, NULL);
      }

      rw_release(l, W, node);
    } else {
      gettimeofday(&tsrcs, NULL);
      rw_acquire(l, R, &node); /*entering the critical section*/
      gettimeofday(&tfrcs, NULL);
      while (mtimediff(tfrcs,tsrcs) <= rcslen) {
	volatile int i1 = shared_array[rand_r(&seed) % 64];
	volatile int i2 = shared_array[rand_r(&seed) % 64];
	gettimeofday(&tfrcs, NULL);
      }

      rw_release(l, R, node);
    }
    gettimeofday(&tfmain, NULL);
  }

  return (void *)i;
}

/* main */
int main(int argc, char *argv[])
{
  int threads = atoi(argv[5]);

  /* initialize rw lock */
  struct rw *l = NULL;
  preference pref;

  /* populate struct */
  struct opts *o = malloc(sizeof(struct opts));
  o->wcslen = atoi(argv[1]);
  o->rcslen = atoi(argv[2]);
  o->ncslen = atoi(argv[3]);
  o->prob = atoi(argv[4]);

  o->array = calloc(64, sizeof(int));
  o->msecs = atoi(argv[7]);

  switch (atoi(argv[6])) {
  case 0:
    pref = NEUTRAL;
    break;

  case 1:
    pref = READER;
    break;

  case 2:
    pref = WRITER;
    break;

  case 3:
    pref = READER_OPT;
    break;
  }

  o->lock = init_rw(pref);

  pthread_t *t = malloc(sizeof(pthread_t) * threads);
  for (int i = 0; i < threads; i++)
    pthread_create(&t[i], NULL, rw_bnchmrk, (void *)o);

  long ac = 0;
  void *status;

  for (int i = 0; i < threads; i++) {
    pthread_join(t[i], &status);
    ac += (long)status;
  }

  printf ("%ld\n",ac);

  destroy_rw(o->lock);
  free(t);
  free(o->array);
  free(o);
  
  return 0;
}
