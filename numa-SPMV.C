/*
 * This code is part of the project "NUMA-aware Graph-structured Analytics"
 *
 *
 * Copyright (C) 2014 Institute of Parallel And Distributed Systems (IPADS), Shanghai Jiao Tong University
 *     All rights reserved
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * For more about this software, visit:
 *
 *     http://ipads.se.sjtu.edu.cn/projects/polymer.html
 *
 */

#include "polymer-wgh.h"
#include "gettime.h"
#include "math.h"

#include <pthread.h>
#include <sys/mman.h>
#include <numa.h>
using namespace std;

#define PAGE_SIZE (4096)

int CORES_PER_NODE = 6;

volatile int shouldStart = 0;

double *p_curr_global = NULL;
double *p_next_global = NULL;

double *p_ans = NULL;
int vPerNode = 0;
int numOfNode = 0;

bool needResult = false;

pthread_barrier_t barr;
pthread_barrier_t global_barr;
pthread_mutex_t mut;

vertices *All;

template <class vertex>
struct SPMV_F {
    double* p_curr, *p_next;
    vertex* V;
    int rangeLow;
    int rangeHi;
    SPMV_F(double* _p_curr, double* _p_next, vertex* _V, int _rangeLow, int _rangeHi) :
        p_curr(_p_curr), p_next(_p_next), V(_V), rangeLow(_rangeLow), rangeHi(_rangeHi) {
        printf("SPMV - struct SPMV_F\n");
    }

    inline void *nextPrefetchAddr(intT index) {
        return &p_curr[index];
    }
    inline bool update(intT s, intT d, int edgeLen) { //update function applies PageRank equation
        p_next[d] += p_curr[s] * edgeLen;
        return 1;
    }
    inline bool updateAtomic (intT s, intT d, int edgeLen) { //atomic Update
        writeAdd(&p_next[d], p_curr[s] * edgeLen);
        /*
        if (d == 110101) {
            cout << "Update from " << s << "\t" << std::scientific << std::setprecision(9) << p_curr[s]/V[s].getOutDegree() << " -- " << p_next[d] << "\n";
        }
        */
        return 1;
    }

    inline void initFunc(void *dataPtr, intT d) {
        *(double *)dataPtr = 0.0;
    }

    inline bool reduceFunc(void *dataPtr, intT s, intT edgeW) {
        *(double *)dataPtr += p_curr[s] * edgeW;
        return true;
    }

    inline bool combineFunc(void *dataPtr, intT d) {
        writeAdd(&p_next[d], *(double *)dataPtr);
        return true;
    }

    inline bool cond (intT d) {
        return true;    //does nothing
    }
};

//resets p
struct SPMV_Vertex_Reset {
    double* p_curr;
    SPMV_Vertex_Reset(double* _p_curr) :
        p_curr(_p_curr) {
        printf("SPMV - struct SPMV_Vertex_Reset\n");
    }
    inline bool operator () (intT i) {
        p_curr[i] = 0.0;
        return 1;
    }
};

struct SPMV_worker_arg {
    void *GA;
    int maxIter;
    int tid;
    int numOfNode;
    int rangeLow;
    int rangeHi;
};

struct SPMV_subworker_arg {
    void *GA;
    int maxIter;
    int tid;
    int subTid;
    int startPos;
    int endPos;
    int rangeLow;
    int rangeHi;
    double **p_curr_ptr;
    double **p_next_ptr;
    pthread_barrier_t *node_barr;
    LocalFrontier *localFrontier;
};

template <class vertex>
void *SPMVSubWorker(void *arg) {
    printf("SPMV - SPMVSubWorker\n");

    SPMV_subworker_arg *my_arg = (SPMV_subworker_arg *)arg;
    wghGraph<vertex> &GA = *(wghGraph<vertex> *)my_arg->GA;
    const intT n = GA.n;
    int maxIter = my_arg->maxIter;
    int tid = my_arg->tid;
    int subTid = my_arg->subTid;
    pthread_barrier_t *local_barr = my_arg->node_barr;
    LocalFrontier *output = my_arg->localFrontier;

    double *p_curr = *(my_arg->p_curr_ptr);
    double *p_next = *(my_arg->p_next_ptr);

    int currIter = 0;
    int rangeLow = my_arg->rangeLow;
    int rangeHi = my_arg->rangeHi;

    int start = my_arg->startPos;
    int end = my_arg->endPos;

    Subworker_Partitioner subworker(CORES_PER_NODE);
    subworker.tid = tid;
    subworker.subTid = subTid;
    subworker.dense_start = start;
    subworker.dense_end = end;
    subworker.global_barr = &global_barr;

    pthread_barrier_wait(local_barr);
    if (subworker.isMaster()) {
        printf("started\n");
    }
    pthread_barrier_wait(&global_barr);
    All->m = GA.m;
    while(1) {
        if (maxIter > 0 && currIter >= maxIter)
            break;
        currIter++;
        if (subTid == 0) {
            {
                parallel_for(long i=output->startID; i<output->endID; i++) output->setBit(i, false);
            }
        }

        pthread_barrier_wait(&global_barr);
        edgeMapDenseForward(GA, All, SPMV_F<vertex>(p_curr, p_next, GA.V, rangeLow, rangeHi), output, true, start, end);
        //edgeMapDenseForwardDynamic(GA, All, SPMV_F<vertex>(p_curr, p_next, GA.V, rangeLow, rangeHi), output, subworker);
        //edgeMapDenseReduce(GA, All, SPMV_F<vertex>(p_curr, p_next, GA.V, rangeLow, rangeHi),output,false,subworker);
        //edgeMap(GA, All, SPMV_F<vertex>(p_curr,p_next,GA.V,rangeLow,rangeHi),output,0,DENSE_FORWARD, false, true, subworker);

        pthread_barrier_wait(&global_barr);
        //pthread_barrier_wait(local_barr);

        vertexMap(All,SPMV_Vertex_Reset(p_curr), tid, subTid, CORES_PER_NODE);
        pthread_barrier_wait(&global_barr);
        //pthread_barrier_wait(local_barr);
        swap(p_curr, p_next);

        pthread_barrier_wait(&global_barr);

        //pthread_barrier_wait(local_barr);
    }
    if (subworker.isMaster()) {
        p_ans = p_curr;
    }
    pthread_barrier_wait(local_barr);
    return NULL;
}

pthread_barrier_t timerBarr;

template <class vertex>
void *SPMVThread(void *arg) {
    printf("SPMV - SPMVThread\n");

    SPMV_worker_arg *my_arg = (SPMV_worker_arg *)arg;
    wghGraph<vertex> &GA = *(wghGraph<vertex> *)my_arg->GA;
    int maxIter = my_arg->maxIter;
    int tid = my_arg->tid;

    char nodeString[10];
    sprintf(nodeString, "%d", tid);
    struct bitmask *nodemask = numa_parse_nodestring(nodeString);
    numa_bind(nodemask);

    int rangeLow = my_arg->rangeLow;
    int rangeHi = my_arg->rangeHi;
    printf("%d before partition\n", tid);
    //wghGraph<vertex> localGraph = graphFilter(GA, rangeLow, rangeHi);
    wghGraph<vertex> localGraph = graphFilter2Direction(GA, rangeLow, rangeHi);

    pthread_barrier_wait(&barr);
    if (tid == 0)
        GA.del();
    pthread_barrier_wait(&barr);

    printf("%d after partition\n", tid);

    int sizeOfShards[CORES_PER_NODE];
    subPartitionByDegree(localGraph, CORES_PER_NODE, sizeOfShards, sizeof(double), true, true);
    for (int i = 0; i < CORES_PER_NODE; i++) {
        //printf("subPartition: %d %d: %d\n", tid, i, sizeOfShards[i]);
    }

    while (shouldStart == 0) ;
    pthread_barrier_wait(&timerBarr);
    printf("over filtering\n");
    /*
    if (0 != __cilkrts_set_param("nworkers","1")) {
    printf("set failed: %d\n", tid);
    }
    */

    const intT n = GA.n;
    int numOfT = my_arg->numOfNode;

    int blockSize = rangeHi - rangeLow;

    //printf("blockSizeof %d: %d low: %d high: %d\n", tid, blockSize, rangeLow, rangeHi);

    double one_over_n = 1/(double)n;


    double* p_curr = p_curr_global;
    double* p_next = p_next_global;
    bool* frontier = (bool *)numa_alloc_local(sizeof(bool) * blockSize);

    /*
    double* p_curr = (double *)malloc(sizeof(double) * blockSize);
    double* p_next = (double *)malloc(sizeof(double) * blockSize);
    bool* frontier = (bool *)malloc(sizeof(bool) * blockSize);
    */

    /*
    if (tid == 0)
    startTime();
    */
    double mapTime = 0.0;
    struct timeval start, end;
    struct timezone tz = {0, 0};

    for(intT i=rangeLow; i<rangeHi; i++) p_curr[i] = one_over_n;
    for(intT i=rangeLow; i<rangeHi; i++) p_next[i] = 0; //0 if unchanged
    for(intT i=0; i<blockSize; i++) frontier[i] = true;
    if (tid == 0)
        All = new vertices(numOfT);

    //printf("register %d: %p\n", tid, frontier);

    LocalFrontier *current = new LocalFrontier(frontier, rangeLow, rangeHi);

    bool* next = (bool *)numa_alloc_local(sizeof(bool) * blockSize);
    for(intT i=0; i<blockSize; i++) next[i] = false;
    LocalFrontier *output = new LocalFrontier(next, rangeLow, rangeHi);

    pthread_barrier_wait(&barr);

    All->registerFrontier(tid, current);

    pthread_barrier_wait(&barr);

    if (tid == 0)
        All->calculateOffsets();

    pthread_barrier_t localBarr;
    pthread_barrier_init(&localBarr, NULL, CORES_PER_NODE+1);

    int startPos = 0;

    pthread_t subTids[CORES_PER_NODE];

    for (int i = 0; i < CORES_PER_NODE; i++) {
        SPMV_subworker_arg *arg = (SPMV_subworker_arg *)malloc(sizeof(SPMV_subworker_arg));
        arg->GA = (void *)(&localGraph);
        arg->maxIter = maxIter;
        arg->tid = tid;
        arg->subTid = i;
        arg->rangeLow = rangeLow;
        arg->rangeHi = rangeHi;
        arg->p_curr_ptr = &p_curr;
        arg->p_next_ptr = &p_next;
        arg->node_barr = &localBarr;
        arg->localFrontier = output;

        arg->startPos = startPos;
        arg->endPos = startPos + sizeOfShards[i];
        startPos = arg->endPos;
        pthread_create(&subTids[i], NULL, SPMVSubWorker<vertex>, (void *)arg);
    }

    pthread_barrier_wait(&barr);

    pthread_barrier_wait(&localBarr);

    pthread_barrier_wait(&localBarr);

    pthread_barrier_wait(&barr);
    return NULL;
}

struct SPMV_Hash_F {
    int shardNum;
    int vertPerShard;
    int n;
    SPMV_Hash_F(int _n, int _shardNum):n(_n), shardNum(_shardNum), vertPerShard(_n / _shardNum) {
        printf("SPMV - struct SPMV_Hash_F\n");
    }

    inline int hashFunc(int index) {
        if (index >= shardNum * vertPerShard) {
            return index;
        }
        int idxOfShard = index % shardNum;
        int idxInShard = index / shardNum;
        return (idxOfShard * vertPerShard + idxInShard);
    }

    inline int hashBackFunc(int index) {
        if (index >= shardNum * vertPerShard) {
            return index;
        }
        int idxOfShard = index / vertPerShard;
        int idxInShard = index % vertPerShard;
        return (idxOfShard + idxInShard * shardNum);
    }
};

template <class vertex>
void SPMV_main(wghGraph<vertex> &GA, int maxIter) {
    printf("SPMV - SPMV_main\n");

    numOfNode = numa_num_configured_nodes();
    vPerNode = GA.n / numOfNode;
    CORES_PER_NODE = numa_num_configured_cpus() / numOfNode;
    pthread_barrier_init(&barr, NULL, numOfNode);
    pthread_barrier_init(&timerBarr, NULL, numOfNode+1);
    pthread_barrier_init(&global_barr, NULL, CORES_PER_NODE * numOfNode);
    pthread_mutex_init(&mut, NULL);
    int sizeArr[numOfNode];
    SPMV_Hash_F hasher(GA.n, numOfNode);
    graphHasher(GA, hasher);
    partitionByDegree(GA, numOfNode, sizeArr, sizeof(double));
    /*
    intT vertPerPage = PAGESIZE / sizeof(double);
    intT subShardSize = ((GA.n / numOfNode) / vertPerPage) * vertPerPage;
    for (int i = 0; i < numOfNode - 1; i++) {
    sizeArr[i] = subShardSize;
    }
    sizeArr[numOfNode - 1] = GA.n - subShardSize * (numOfNode - 1);
    */
    p_curr_global = (double *)mapDataArray(numOfNode, sizeArr, sizeof(double));
    p_next_global = (double *)mapDataArray(numOfNode, sizeArr, sizeof(double));

    printf("start create %d threads\n", numOfNode);
    pthread_t tids[numOfNode];
    int prev = 0;
    for (int i = 0; i < numOfNode; i++) {
        SPMV_worker_arg *arg = (SPMV_worker_arg *)malloc(sizeof(SPMV_worker_arg));
        arg->GA = (void *)(&GA);
        arg->maxIter = maxIter;
        arg->tid = i;
        arg->numOfNode = numOfNode;
        arg->rangeLow = prev;
        arg->rangeHi = prev + sizeArr[i];
        prev = prev + sizeArr[i];
        pthread_create(&tids[i], NULL, SPMVThread<vertex>, (void *)arg);
    }
    shouldStart = 1;
    pthread_barrier_wait(&timerBarr);
    //nextTime("Graph Partition");
    startTime();
    printf("all created\n");
    for (int i = 0; i < numOfNode; i++) {
        pthread_join(tids[i], NULL);
    }
    nextTime("SPMV");
    if (needResult) {
        for (intT i = 0; i < GA.n; i++) {
            cout << i << "\t" << std::scientific << std::setprecision(9) << p_ans[hasher.hashFunc(i)] << "\n";
        }
    }
}

int parallel_main(int argc, char* argv[]) {
    printf("SPMV - parallel_main\n");

    char* iFile;
    bool binary = false;
    bool symmetric = false;
    int maxIter = -1;
    needResult = false;
    if(argc > 1) iFile = argv[1];
    if(argc > 2) maxIter = atoi(argv[2]);
    if(argc > 3) if((string) argv[3] == (string) "-result") needResult = true;
    if(argc > 4) if((string) argv[4] == (string) "-s") symmetric = true;
    if(argc > 5) if((string) argv[5] == (string) "-b") binary = true;
    numa_set_interleave_mask(numa_all_nodes_ptr);
    if(symmetric) {
        wghGraph<symmetricWghVertex> WG =
            readWghGraph<symmetricWghVertex>(iFile,symmetric,binary);
        SPMV_main(WG, maxIter);
        //WG.del();
    } else {
        wghGraph<asymmetricWghVertex> WG =
            readWghGraph<asymmetricWghVertex>(iFile,symmetric,binary);
        SPMV_main(WG, maxIter);
        //WG.del();
    }
    return 0;
}