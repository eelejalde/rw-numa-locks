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
/* Ticket Lock */
/******************************************************************************/
struct tkt {
  unsigned long request;
  unsigned long grant;
  unsigned long visitors;
  int state;
  int gstate;
#ifdef __x86_64__
  char pad[32];
#else
  char pad[44];
#endif
};

struct tkt *init_tkt() {
  struct tkt *l = malloc(sizeof(struct tkt));
  l->request = 0;
  l->grant = 0;
  l->state = GLOBAL_RELEASE;
  l->visitors = 0;
  return l;
}

void destroy_tkt(struct tkt *l) {
  free(l);
}

bool tkt_isalone(struct tkt *l) {
  return l->request - 1 == l->grant;
}

bool tkt_islocked(struct tkt *l) {
  return l->request != l->grant;
}

bool tkt_maypass_local(struct tkt *l) {
  return l->visitors < MAX_VISITORS;
}

int tkt_acquire(struct tkt *l) {
  int val = __sync_fetch_and_add(&l->request, 1);
  while (l->grant != val);
  return val;
}

void tkt_release(struct tkt *l) {
  __sync_fetch_and_add(&l->grant, 1);
}

/******************************************************************************/
/* Cohort Lock (TKT-TKT) */
/******************************************************************************/

struct c_tkt_tkt {
  struct tkt *glock;
  struct tkt **llocks;
#ifdef __x86_64__
  char pad[48];
#else
  char pad[56];
#endif
};

struct c_tkt_tkt *init_c_tkt_tkt() {
  struct c_tkt_tkt *l = malloc(sizeof(struct c_tkt_tkt));
  l->glock = init_tkt();
  l->llocks = memalign(CACHE_ALIGN, sizeof(struct tkt) * NUMA_NODES);
  for (int i = 0; i < NUMA_NODES; i++)
    l->llocks[i] = init_tkt();
  return l;
}

void destroy_c_tkt_tkt(struct c_tkt_tkt *l) {
  for (int i = 0; i < NUMA_NODES; i++)
    destroy_tkt(l->llocks[i]);
  free(l->llocks);
  destroy_tkt(l->glock);
  free(l);
}

bool c_tkt_tkt_islocked(struct c_tkt_tkt *l) {
  return tkt_islocked(l->glock);
}

void c_tkt_tkt_acquire(struct c_tkt_tkt *l, int *node) {
  int cpu, t;
  if (node == NULL)
    getcpu(&cpu, node);
  tkt_acquire(l->llocks[*node]);
  if(l->llocks[*node]->state == GLOBAL_RELEASE) {
    l->llocks[*node]->gstate = tkt_acquire(l->glock);
    l->llocks[*node]->visitors = 0;
  } else l->llocks[*node]->visitors++;
  l->llocks[*node]->state = BUSY;
}

void c_tkt_tkt_release(struct c_tkt_tkt *l, int node) {
  if(tkt_isalone(l->llocks[node]) || !tkt_maypass_local(l->llocks[node])) {
    l->llocks[node]->state = GLOBAL_RELEASE;
    tkt_release(l->glock);
    tkt_release(l->llocks[node]);
  } else {
    l->llocks[node]->state = LOCAL_RELEASE;
    tkt_release(l->llocks[node]);
  }
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
  struct c_tkt_tkt *writers;
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
  l->writers = init_c_tkt_tkt();
  l->pref = p;
  l->wactive = false;
  l->indicators = malloc(NUMA_NODES * sizeof(struct readindr));
  for(int i=0; i<NUMA_NODES; i++)
    l->indicators[i].arrive = l->indicators[i].depart = 0;

  return l;
}

void destroy_rw(struct rw *l) {
  destroy_c_tkt_tkt(l->writers);
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
      c_tkt_tkt_acquire(l->writers, node);
      __sync_fetch_and_add(&l->indicators[*node].arrive, 1);
      c_tkt_tkt_release(l->writers, *node);
      break;

    case READER:
      while(l->rbarrier); /* If we need less granularity, we can change later*/
      __sync_fetch_and_add(&l->indicators[*node].arrive, 1);
      while(c_tkt_tkt_islocked(l->writers));

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
	if(c_tkt_tkt_islocked(l->writers)) {
	  __sync_fetch_and_add(&l->indicators[*node].depart, 1);
	  while (c_tkt_tkt_islocked(l->writers)) {
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
      c_tkt_tkt_acquire(l->writers, node);
      readindr_waitempty(l->indicators);
      break;

    case READER:
      braised = false;

      while (true) {
	c_tkt_tkt_acquire(l->writers, node);

	if(!readindr_isempty(l->indicators)) {
	  c_tkt_tkt_release(l->writers, *node);

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
      c_tkt_tkt_acquire(l->writers, node);      
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

      c_tkt_tkt_acquire(l->writers, node);
      readindr_waitempty(l->indicators);
    }
  }
}

void rw_release(struct rw *l, mode m, int node) {
  if(m == R)
    __sync_fetch_and_add(&l->indicators[node].depart, 1);
  else {
    l->wactive = false;
    c_tkt_tkt_release(l->writers, node);
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
