// Copyright (c) 2014 Baidu.com, Inc. All Rights Reserved
// Author: Ge,Jun (gejun@baidu.com)
// Date: Sun Jul 13 15:04:18 CST 2014

#include <map>
#include <gtest/gtest.h>
#include "base/atomicops.h"
#include "base/time.h"
#include "base/macros.h"
#include "base/scoped_lock.h"
#include "bthread/bthread.h"
#include "bthread/condition_variable.h"
#include "bthread/stack.h"

namespace {
struct Arg {
    bthread_mutex_t m;
    bthread_cond_t c;
};

pthread_mutex_t wake_mutex = PTHREAD_MUTEX_INITIALIZER;
long signal_start_time = 0;
std::vector<bthread_t> wake_tid;
std::vector<long> wake_time;
volatile bool stop = false;
const long SIGNAL_INTERVAL_US = 10000;

std::ostream& operator<<(std::ostream& os,
                         const std::vector<bthread_t>& wake_tid) {
    for (size_t i = 0; i < wake_tid.size(); ++i) {
        os << i << ":" << wake_tid[i] << ' ';
    }
    return os;
}


void* signaler(void* void_arg) {
    Arg* a = (Arg*)void_arg;
    signal_start_time = base::gettimeofday_us();
    while (!stop) {
        bthread_usleep(SIGNAL_INTERVAL_US);
        bthread_cond_signal(&a->c);
    }
    return NULL;
}

void* waiter(void* void_arg) {
    Arg* a = (Arg*)void_arg;
    bthread_mutex_lock(&a->m);
    while (!stop) {
        bthread_cond_wait(&a->c, &a->m);
        
        BAIDU_SCOPED_LOCK(wake_mutex);
        wake_tid.push_back(bthread_self());
        wake_time.push_back(base::gettimeofday_us());
    }
    bthread_mutex_unlock(&a->m);
    return NULL;
}

TEST(CondTest, sanity) {
    Arg a;
    ASSERT_EQ(0, bthread_mutex_init(&a.m, NULL));
    ASSERT_EQ(0, bthread_cond_init(&a.c, NULL));
    // has no effect
    ASSERT_EQ(0, bthread_cond_signal(&a.c));

    stop = false;
    wake_tid.resize(1024);
    wake_tid.clear();
    wake_time.resize(1024);
    wake_time.clear();
    
    bthread_t wth[8];
    const size_t NW = ARRAY_SIZE(wth);
    for (size_t i = 0; i < NW; ++i) {
        ASSERT_EQ(0, bthread_start_urgent(&wth[i], NULL, waiter, &a));
    }
    
    bthread_t sth;
    ASSERT_EQ(0, bthread_start_urgent(&sth, NULL, signaler, &a));

    bthread_usleep(SIGNAL_INTERVAL_US * 300);

    pthread_mutex_lock(&wake_mutex);
    const size_t nbeforestop = wake_time.size();
    pthread_mutex_unlock(&wake_mutex);
    
    stop = true;
    for (size_t i = 0; i < NW; ++i) {
        bthread_cond_signal(&a.c);
    }
    
    bthread_join(sth, NULL);
    for (size_t i = 0; i < NW; ++i) {
        bthread_join(wth[i], NULL);
    }

    printf("wake up for %lu times\n", wake_tid.size());

    // Check timing
    long square_sum = 0;
    for (size_t i = 0; i < nbeforestop; ++i) {
        long last_time = (i ? wake_time[i-1] : signal_start_time);
        long delta = wake_time[i] - last_time - SIGNAL_INTERVAL_US;
        EXPECT_GT(wake_time[i], last_time);
        square_sum += delta * delta;
        EXPECT_LT(abs(delta), 1000L) << "error[" << i << "]=" << delta << "="
            << wake_time[i] << " - " << last_time;
    }
    printf("Average error is %fus\n", sqrt(square_sum / std::max(nbeforestop, 1UL)));

    // Check fairness
    std::map<bthread_t, int> count;
    for (size_t i = 0; i < wake_tid.size(); ++i) {
        ++count[wake_tid[i]];
    }
    EXPECT_EQ(NW, count.size());
    for (size_t i = 0; i < NW; ++i) {
    }
    size_t avg_count = wake_tid.size() / count.size();
    for (std::map<bthread_t, int>::iterator
             it = count.begin(); it != count.end(); ++it) {
        ASSERT_LE(abs(it->second - avg_count), 1)
            << "bthread=" << it->first
            << " count=" << it->second
            << " avg=" << avg_count;
        printf("%lu wakes up %d times\n", it->first, it->second);
    }

    bthread_cond_destroy(&a.c);
    bthread_mutex_destroy(&a.m);
}

struct WrapperArg {
    bthread::Mutex mutex;
    bthread::ConditionVariable cond;
};

void* cv_signaler(void* void_arg) {
    WrapperArg* a = (WrapperArg*)void_arg;
    signal_start_time = base::gettimeofday_us();
    while (!stop) {
        bthread_usleep(SIGNAL_INTERVAL_US);
        a->cond.notify_one();
    }
    return NULL;
}

void* cv_bmutex_waiter(void* void_arg) {
    WrapperArg* a = (WrapperArg*)void_arg;
    std::unique_lock<bthread_mutex_t> lck(*a->mutex.native_handler());
    while (!stop) {
        a->cond.wait(lck);
    }
    return NULL;
}

void* cv_mutex_waiter(void* void_arg) {
    WrapperArg* a = (WrapperArg*)void_arg;
    std::unique_lock<bthread::Mutex> lck(a->mutex);
    while (!stop) {
        a->cond.wait(lck);
    }
    return NULL;
}

#define COND_IN_PTHREAD

#ifndef COND_IN_PTHREAD
#define pthread_join bthread_join
#define pthread_create bthread_start_urgent
#endif

TEST(CondTest, cpp_wrapper) {
    stop = false;
    bthread::ConditionVariable cond;
    pthread_t bmutex_waiter_threads[8];
    pthread_t mutex_waiter_threads[8];
    pthread_t signal_thread;
    WrapperArg a;
    for (size_t i = 0; i < ARRAY_SIZE(bmutex_waiter_threads); ++i) {
        ASSERT_EQ(0, pthread_create(&bmutex_waiter_threads[i], NULL,
                                    cv_bmutex_waiter, &a));
        ASSERT_EQ(0, pthread_create(&mutex_waiter_threads[i], NULL,
                                    cv_mutex_waiter, &a));
    }
    ASSERT_EQ(0, pthread_create(&signal_thread, NULL, cv_signaler, &a));
    LOG(INFO) << "Start to sleep";
    bthread_usleep(100L * 1000);
    {
        BAIDU_SCOPED_LOCK(a.mutex);
        stop = true;
    }
    LOG(INFO) << "Stopped, join signal_thread";
    pthread_join(signal_thread, NULL);
    LOG(INFO) << "signal_thread quit, join waiter_threads";
    a.cond.notify_all();
    for (size_t i = 0; i < ARRAY_SIZE(bmutex_waiter_threads); ++i) {
        pthread_join(bmutex_waiter_threads[i], NULL);
        pthread_join(mutex_waiter_threads[i], NULL);
    }
}

#ifndef COND_IN_PTHREAD
#undef pthread_join
#undef pthread_create
#endif

#define ENABLE_PROFILE
#ifdef ENABLE_PROFILE
# include <google/profiler.h>
#else
# define ProfilerStart(a)
# define ProfilerStop()
#endif

class Signal {
protected:
    Signal() : _signal(0) {}
    void notify() {
        BAIDU_SCOPED_LOCK(_m);
        ++_signal;
        _c.notify_one();
    }

    int wait(int old_signal) {
        std::unique_lock<bthread::Mutex> lck(_m);
        while (_signal == old_signal) {
            _c.wait(lck);
        }
        return _signal;
    }

private:
    bthread::Mutex _m;
    bthread::ConditionVariable _c;
    int _signal;
};

struct PingPongArg {
    bool stopped;
    Signal sig1;
    Signal sig2;
    base::atomic<int> nthread;
    base::atomic<long> total_count;
};

void *ping_pong_thread(void* arg) {
    PingPongArg* a = (PingPongArg*)arg;
    long local_count = 0;
    bool odd = (a->nthread.fetch_add(1)) % 2;
    int old_signal = 0;
    while (!a->stopped) {
        if (odd) {
            a->sig1.notify();
            old_signal = a->sig2.wait(old_signal);
        } else {
            old_signal = a->sig1.wait(old_signal);
            a->sig2.notify();
        }
        ++local_count;
    }
    a->total_count.fetch_add(local_count);
    return NULL;
}

TEST(CondTest, ping_pong) {
    PingPongArg arg;
    arg.stopped = false;
    arg.nthread = 0;
    bthread_t threads[2];
    ProfilerStart("cond.prof");
    for (int i = 0; i < 2; ++i) {
        ASSERT_EQ(0, bthread_start_urgent(&threads[i], NULL, ping_pong_thread, &arg));
    }
    usleep(1000 * 1000);
    arg.stopped = true;
    arg.sig1.notify();
    arg.sig2.notify();
    for (int i = 0; i < 2; ++i) {
        ASSERT_EQ(0, bthread_join(threads[i], NULL));
    }
    ProfilerStop();
    LOG(INFO) << "total_count=" << arg.total_count.load();
}

struct BroadcastArg {
    bthread::ConditionVariable wait_cond;
    bthread::ConditionVariable broadcast_cond;
    bthread::Mutex mutex;
    int nwaiter;
    int cur_waiter;
    int rounds;
    int sig;
};

void* wait_thread(void* arg) {
    BroadcastArg* ba = (BroadcastArg*)arg;
    std::unique_lock<bthread::Mutex> lck(ba->mutex);
    while (ba->rounds > 0) {
        const int saved_round = ba->rounds;
        ++ba->cur_waiter;
        while (saved_round == ba->rounds) {
            if (ba->cur_waiter >= ba->nwaiter) {
                ba->broadcast_cond.notify_one();
            }
            ba->wait_cond.wait(lck);
        }
    }
    return NULL;
}

void* broadcast_thread(void* arg) {
    BroadcastArg* ba = (BroadcastArg*)arg;
    //int local_round = 0;
    while (ba->rounds > 0) {
        std::unique_lock<bthread::Mutex> lck(ba->mutex);
        while (ba->cur_waiter < ba->nwaiter) {
            ba->broadcast_cond.wait(lck);
        }
        ba->cur_waiter = 0;
        --ba->rounds;
        ba->wait_cond.notify_all();
    }
    return NULL;
}

void* disturb_thread(void* arg) {
    BroadcastArg* ba = (BroadcastArg*)arg;
    std::unique_lock<bthread::Mutex> lck(ba->mutex);
    while (ba->rounds > 0) {
        lck.unlock();
        lck.lock();
    }
    return NULL;
}

TEST(CondTest, mix_usage) {
    BroadcastArg ba;
    ba.nwaiter = 0;
    ba.cur_waiter = 0;
    ba.rounds = 100000;
    const int NTHREADS = 10;
    ba.nwaiter = NTHREADS * 2;

    bthread_t normal_threads[NTHREADS];
    for (int i = 0; i < NTHREADS; ++i) {
        ASSERT_EQ(0, bthread_start_urgent(&normal_threads[i], NULL, wait_thread, &ba));
    }
    pthread_t pthreads[NTHREADS];
    for (int i = 0; i < NTHREADS; ++i) {
        ASSERT_EQ(0, pthread_create(&pthreads[i], NULL,
                                    wait_thread, &ba));
    }
    pthread_t broadcast;
    pthread_t disturb;
    ASSERT_EQ(0, pthread_create(&broadcast, NULL, broadcast_thread, &ba));
    ASSERT_EQ(0, pthread_create(&disturb, NULL, disturb_thread, &ba));
    for (int i = 0; i < NTHREADS; ++i) {
        bthread_join(normal_threads[i], NULL);
        pthread_join(pthreads[i], NULL);
    }
    pthread_join(broadcast, NULL);
    pthread_join(disturb, NULL);
}

class BthreadCond {
public:
    BthreadCond() {
        bthread_cond_init(&_cond, NULL);
        bthread_mutex_init(&_mutex, NULL);
        _count = 1;
    }
    ~BthreadCond() {
        bthread_mutex_destroy(&_mutex);
        bthread_cond_destroy(&_cond);
    }

    void Init(int count = 1) {
        _count = count;
    }

    int Signal() {
        int ret = 0;
        bthread_mutex_lock(&_mutex);
        _count --;
        bthread_cond_signal(&_cond);
        bthread_mutex_unlock(&_mutex);
        return ret;
    }

    int Wait() {
        int ret = 0;
        bthread_mutex_lock(&_mutex);
        while (_count > 0) {
            ret = bthread_cond_wait(&_cond, &_mutex);
        }
        bthread_mutex_unlock(&_mutex);
        return ret;
    }
private:
    int _count;
    bthread_cond_t _cond;
    bthread_mutex_t _mutex;
};

volatile bool g_stop = false;
bool started_wait = false;
bool ended_wait = false;

void* usleep_thread(void *) {
    while (!g_stop) {
        bthread_usleep(1000L * 1000L);
    }
    return NULL;
}

void* wait_cond_thread(void* arg) {
    BthreadCond* c = (BthreadCond*)arg;
    started_wait = true;
    c->Wait();
    ended_wait = true;
    return NULL;
}

TEST(CondTest, too_many_bthread) {
    std::vector<bthread_t> th;
    th.resize(32768);
    BthreadCond c;
    c.Init();
    bthread_t tid;
    bthread_start_urgent(&tid, &BTHREAD_ATTR_PTHREAD, wait_cond_thread, &c);
    for (size_t i = 0; i < th.size(); ++i) {
        bthread_start_background(&th[i], NULL, usleep_thread, NULL);
    }
    c.Signal();
    usleep(1 * 1000 * 1000L);
    g_stop = true;
    bthread_join(tid, NULL);
    ASSERT_TRUE(started_wait);
    ASSERT_TRUE(ended_wait);
    for (size_t i = 0; i < th.size(); ++i) {
        bthread_join(th[i], NULL);
    }
}
} // namespace