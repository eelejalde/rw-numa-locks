rw-numa-locks
=============

NUMA-Aware Reader-Writer Locks

This work is based on  I. Calciu et. al. where the authors introduce a family of reader-writer lock algorithms intended to take advantage of NUMA architectures. These algorithms use what they call a cohort-lock to coordinate writers, and a set of distributed reader indicators to keep track of readers. These locks can be initialized with a certain "preference" to readers, writers or with a neutral preference. If preference is given to a certain group, the lock will grant "privileged" access to members of that group as soon as possible, executing threads in batches. Following the paper, we have implemented four preferences: Neutral (NEUTRAL-pref), Reader (READERS-pref), Writer (WRITERS-pref) and an optimized variation of the Reader-Preference (READER-OPT-pref).

Cohort-locks are NUMA-aware spin locks that use two levels of locks organized hierarchically: a global lock, and a set of local locks (one for each NUMA node) in the second level. This allows nodes to exploit cache locality, giving priority to processes running on the same NUMA-node before releasing the global lock. This cohorting technique can use any lock for both the global lock and the local locks, including spinlocks, or combinations of them. The notation  we use here is putting the global lock first, followed by a dash, followed by the local lock type: global_lock_type-local_locks_types; e.g. tkt-bo  uses a ticket lock for the global lock and backoff locks for the local locks).

Locks Abbreviations:
tkt : Ticket Lock
bo : Backoff Lock
mcs : MCS List-Based Queuing Lock
ptkt : Partitioned Ticket Lock

WARNING! The code is in a state that allows us to do testing, and it obviously can be used (we think the locks are correct), but it has no intention of being "release" code. Our intention was to put the code online and get comments on it while we work on a library with much more encapsulation and less code duplication. This, however, will take a little time (unless someone on the internet would like to help us!). The code for the locks is quite clean, so it would not take much time to bring yourself up to speed. Shoot us a message ({eelejalde | lferres}@udec.cl) and fork us if you'd like to help out!
