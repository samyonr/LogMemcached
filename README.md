# LogMemcached

A source code of the paper LogMemcached - An RDMA based Continuous Cache Replication, that was released as part of the KBNets '17 Proceedings of the Workshop on Kernel-Bypass Networks in SIGCOMM.

The original paper can be found here: http://dl.acm.org/citation.cfm?id=3098584 and a free copy can be found here: https://conferences.sigcomm.org/sigcomm/2017/workshop-kbnets.html

An extended version of the paper can be found here: http://arad.mscc.huji.ac.il/dissertations/W/JMC/9920573069103701.pdf

The paper authors are: Samyon Ristov from The Hebrew University of Jerusalem, Yaron Weinsberg from Microsoft, Danny Dolev from The Hebrew University of Jerusalem and Tal Anker from Mellanox Technologies.

The code's purpose is described in the abstract below. To understand it further it's advised to read the original paper or the extended version, linked above. The paper and especially its extended version describe the system's flow, design, data structures, pseudocode, evaluation and performance analysis. This readme does not focus on these aspects. Instead, it's focuses on the technical and research aspects that are missing in this code and work.

## Abstract

One of the advantages of cloud computing is its ability to quickly scale out services to meet demand. A common technique to mitigate the increasing load in these services is to deploy a cache.

Although it seems natural that the caching layer would also deal with availability and fault tolerance, these issues are nevertheless often ignored, as the cache has only recently begun to be considered a critical system component. A cache may evict items at any moment, and so a failing cache node can simply be treated as if the set of items stored on that node have already been evicted. However, setting up a cache instance is a time-consuming operation that could inadvertently affect the service’s operation.

This work addresses this limitation by introducing cache replication at the server side by expanding Memcached (which currently provides availability via client side replication). This work presents the design and implementation of LogMemcached, a modification of Memcached’s internal data structures to include state replication via RDMA to provide an increased system availability, improved failure resilience and enhanced load balancing capabilities without compromising performance and with introducing a very low CPU load, while keeping the main principles of Memcached’s Design Philosophy.

##  Environment

### Hardware

Mellanox Technologies MT27520 Family. ConnectX-3 Pro.

### Linux

We ran under the code with Ubuntu 14.10, kernel 3.16.0-43 with OFED version 3.18. It has not been tested under any other OS, although there should not be any problems with most of the other Debian distributions.

## Known issues, bugs and missing features

What is needed to improve LogMemcached, and make it production ready:

* prepend and append command's items does not replicate well - the items are replicated fine, but they are allocated twice. First, there is an allocation in "process_update_command" in memcached.c, see "FIXME" comment near "item_alloc". Then, they are alloceted again in "do_store_item" in memcached.c. The allocated item in process_update_command will stay "ITEM_DIRTY" forever, crushing the replication process. Without replication, the system will work fine (one prepend/append item will stay "ITEM_DIRTY" and the other one will be stored normally).
* during replication, the systems waits for "ITEM_DIRTY" flag to disappear - it make sense, but what if because of some problem, an item stayed in "ITEM_DIRTY" state forever? Like in the bug above? This will cause the replication process to crush. The system should wait some time until the "ITEM_DIRTY" flag is removed, and if it's not removed, to keep further if possible, or to ping the master that some problem occurred - causing the master to mark the item with "ITEM_CORRUPTED" flag. Beware of consistency problems here.
* memory ghost - sometimes, during replications, weird things are happening. One such place is "do_store_replication" in memcached.c, see "FIXME" comment there. safe_buf (and buf) sometimes, every hundreds of thousands of reads is modified during the run. For example, there is an 'if' statement (if ((flags & ITEM_STORED) && !(flags & ITEM_DIRTY) && !(flags & ITEM_CYCLE))) that shows that it->it_data_flags is not ITEM_STORED, but then, during the run, in the last 'else' statement in that 'if' statement, it->it_data_flags is ITEM_STORED - hence, someone modified it. As a patch for now, the flag to the stack before doing any comparison (i.e. there is no direct comparison to safe_buf (or buf). Probably the problem is caused because the NIC, or some CPU reordering. Not sure. The bug is reproduces easily if the comparison is done to it->it_data_flags directly (i.e. to safe_buf or buf), and there is a client that store big items (32KB or above) nonstop.
* memory barriers - the code uses plenty memory barriers (asm volatile("": : :"memory")) - they are placed before critical places, but there is no proof that they really works as expected, and that they all in the right places. Need to recheck each one of them.
* sleep for synchronization - in backup_rdma.c a usleep(1000) statement appears for a few times, which is clearly slow down the system. The reason they are there is that without them the replication just fails, especially when it occurs fast enough. To reproduce the problem, remove the usleep(1000) statements, start a LogMemcached master and a slave, and start a client (like memaslap) that will set (without get) items to the master. The replication will fail right away.
* binary API - currently, LogMemcached supports only the ASCII API, while Memcached supports a binary API too, that behaves a bit differently than the ASCII API.
* multiple slaves - currently, Logmemcached support only one replication slave, but it should support at least 3 slaves, and it's better to support any arbitrary number of slaves (of course, there is a performance costs to replicate via multiple slaves).
* read only mode - replication slaves should support a read only mode, in which, while they perform replication (i.e. while they act as slaves), they should allow incoming "get" commands, and forbid all others. Currently, there is no mechanism that forbids other commands. Be careful with "get" commands too. In LRU or LFU modes a get command can "issue" a "set" command. See more details in the paper.
* slave connection in the middle of the process - slaves that connect not at the beginning may suffer from problems. Log's tail can be not in the beginning of the log, or the replication may be too slow, and the master will delete an item that is currently replicated. The slave is responsible for the replication, but it should be a bit smarter to avoid not stable state. Currently, the replication works well in "get" intensive workflows (and for a limited time in a "set" intensive workflows) when both the master and the slave started in the same time, and the replication speed is not much slower than the "set" speed. Also, see a "FIXME" comment in backup_rdma.c that describes a scenario where the system clearly fails. The scenario is fictional, but may represent a possible similar scenario.
* reconnection mechanism - if the slave's connection is lost, it will not try to reconnect.
* replication handler should stop running - it should stop its run in case the connection was lost, or a call to stop the replication where received.
* the log is initialized as a one big chunk. If it's not possible, the system fails. Instead, it's possible to allocate many smaller chunks, and "notify" the slaves about that (write the information in an available to RDMA data structure).
* item by item replication - LogMemcached replicate 2MB at a time (max. item's size is 1MB). Instead, it could replicate bigger chunks, making the replication faster. The size of the replicated item should be adjusted to the network and system's requirements in real time.
* improving the replication benchmark - the benchmark, defined by "REPLICATION_BENCHMARK", can be improved, to give better results.
* hard coded stuff and consts - many stuff, like some addresses, are hard coded and not configurable. Other stuff should use consts (or #define) instead of being a magic number (like item's min. size and log's remaining size calculation.

## Extending the research

Ideas to extend the research:

* system's performance on bigger load - the evaluation was performed when the master's CPU is not bounded. Need to run all the tests with higher load. The client's limitations (when it's CPU and network are bounded) should be detected, and the behavior of the system when the server is CPU bounded should be tested.
* test the system in bigger scale, with more slaves and more clients.
* need to understand the causes and the mechanics of the CPU increase in the user mode. What exactly causes the increased CPU in the user mode, by how much, when it's stable (under what loads) and how it will behave with more slaves.
* multiple logs can be used for items with different sizes. That may increase the replication speed because the items' sizes will be known in advance, and they should not be checked after each replication.
* a scatter/gather techniques may be used to increase the replication process.
* optimized log management - log compaction algorithms may be considered to increase the replication or to enable adding slaves on-the-fly.
* stronger consistency - test a system where slaves can signal the master, indicating which items where already copied. The master can acknowledge a "set" command only after a predefined number of slaves copied the item.
* strong consistency - test the system with a strong consistency by using leader election.
* RDMA WRITE - test a system which uses RDMA WRITE instead of RDMA READ.
