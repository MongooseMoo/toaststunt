/* ********************************
 * Author:       Johan Hanssen Seferidis
 * License:      MIT
 * Description:  Library providing a threading pool where you can add
 *               work. For usage, check the thpool.h file or README.md
 *
 * C++ conversion for Windows MSVC compatibility - uses std::thread/mutex/condition_variable
 * instead of pthreads.
 *
 ********************************/

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#include "thpool.h"

#ifdef THPOOL_DEBUG
#define THPOOL_DEBUG 1
#else
#define THPOOL_DEBUG 0
#endif

#if !defined(DISABLE_PRINT) || defined(THPOOL_DEBUG)
#define err(str) fprintf(stderr, str)
#else
#define err(str)
#endif

static std::atomic<int> threads_keepalive{1};
static std::atomic<int> threads_on_hold{0};


/* ========================== STRUCTURES ============================ */

/* Binary semaphore */
struct bsem {
    std::mutex mtx;
    std::condition_variable cond;
    int v;
};

/* Job */
struct job {
    struct job* prev;
    void (*function)(void* arg);
    void* arg;
};

/* Job queue */
struct jobqueue {
    std::mutex rwmutex;
    job* front;
    job* rear;
    bsem* has_jobs;
    int len;
};

/* Thread */
struct thread_wrapper {
    int id;
    std::thread* pthread;
    struct thpool_* thpool_p;
};

/* Threadpool */
struct thpool_ {
    thread_wrapper** threads;
    std::atomic<int> num_threads_alive;
    std::atomic<int> num_threads_working;
    std::mutex thcount_lock;
    std::condition_variable threads_all_idle;
    jobqueue jobqueue;
};


/* ========================== PROTOTYPES ============================ */

static int thread_init(thpool_* thpool_p, thread_wrapper** thread_p, int id);
static void thread_do(thread_wrapper* thread_p);
static void thread_destroy(thread_wrapper* thread_p);

static int jobqueue_init(jobqueue* jobqueue_p);
static void jobqueue_clear(jobqueue* jobqueue_p);
static void jobqueue_push(jobqueue* jobqueue_p, job* newjob_p);
static job* jobqueue_pull(jobqueue* jobqueue_p);
static void jobqueue_destroy(jobqueue* jobqueue_p);

static void bsem_init(bsem* bsem_p, int value);
static void bsem_reset(bsem* bsem_p);
static void bsem_post(bsem* bsem_p);
static void bsem_post_all(bsem* bsem_p);
static void bsem_wait(bsem* bsem_p);


/* ========================== THREADPOOL ============================ */

/* Initialise thread pool */
extern "C" thpool_* thpool_init(int num_threads) {
    threads_on_hold = 0;
    threads_keepalive = 1;

    if (num_threads < 0) {
        num_threads = 0;
    }

    /* Make new thread pool */
    thpool_* thpool_p = new(std::nothrow) thpool_;
    if (thpool_p == nullptr) {
        err("thpool_init(): Could not allocate memory for thread pool\n");
        return nullptr;
    }
    thpool_p->num_threads_alive = 0;
    thpool_p->num_threads_working = 0;

    /* Initialise the job queue */
    if (jobqueue_init(&thpool_p->jobqueue) == -1) {
        err("thpool_init(): Could not allocate memory for job queue\n");
        delete thpool_p;
        return nullptr;
    }

    /* Make threads in pool */
    thpool_p->threads = new(std::nothrow) thread_wrapper*[num_threads];
    if (thpool_p->threads == nullptr) {
        err("thpool_init(): Could not allocate memory for threads\n");
        jobqueue_destroy(&thpool_p->jobqueue);
        delete thpool_p;
        return nullptr;
    }

    /* Thread init */
    for (int n = 0; n < num_threads; n++) {
        thread_init(thpool_p, &thpool_p->threads[n], n);
#if THPOOL_DEBUG
        printf("THPOOL_DEBUG: Created thread %d in pool \n", n);
#endif
    }

    /* Wait for threads to initialize */
    while (thpool_p->num_threads_alive != num_threads) {
        std::this_thread::yield();
    }

    return thpool_p;
}


/* Add work to the thread pool */
extern "C" int thpool_add_work(thpool_* thpool_p, void (*function_p)(void*), void* arg_p) {
    job* newjob = new(std::nothrow) job;
    if (newjob == nullptr) {
        err("thpool_add_work(): Could not allocate memory for new job\n");
        return -1;
    }

    /* add function and argument */
    newjob->function = function_p;
    newjob->arg = arg_p;

    /* add job to queue */
    jobqueue_push(&thpool_p->jobqueue, newjob);

    return 0;
}


/* Wait until all jobs have finished */
extern "C" void thpool_wait(thpool_* thpool_p) {
    std::unique_lock<std::mutex> lock(thpool_p->thcount_lock);
    while (thpool_p->jobqueue.len || thpool_p->num_threads_working) {
        thpool_p->threads_all_idle.wait(lock);
    }
}


/* Destroy the threadpool */
extern "C" void thpool_destroy(thpool_* thpool_p) {
    /* No need to destroy if it's NULL */
    if (thpool_p == nullptr) return;

    int threads_total = thpool_p->num_threads_alive.load();

    /* End each thread's infinite loop */
    threads_keepalive = 0;

    /* Give one second to kill idle threads */
    double TIMEOUT = 1.0;
    time_t start, end;
    double tpassed = 0.0;
    time(&start);
    while (tpassed < TIMEOUT && thpool_p->num_threads_alive) {
        bsem_post_all(thpool_p->jobqueue.has_jobs);
        time(&end);
        tpassed = difftime(end, start);
    }

    /* Poll remaining threads */
    while (thpool_p->num_threads_alive) {
        bsem_post_all(thpool_p->jobqueue.has_jobs);
        SLEEP_MS(1000);
    }

    /* Job queue cleanup */
    jobqueue_destroy(&thpool_p->jobqueue);

    /* Deallocs */
    for (int n = 0; n < threads_total; n++) {
        thread_destroy(thpool_p->threads[n]);
    }
    delete[] thpool_p->threads;
    delete thpool_p;
}


/* Pause all threads in threadpool */
extern "C" void thpool_pause(thpool_* thpool_p) {
    (void)thpool_p;
    threads_on_hold = 1;
}


/* Resume all threads in threadpool */
extern "C" void thpool_resume(thpool_* thpool_p) {
    (void)thpool_p;
    threads_on_hold = 0;
}


extern "C" int thpool_num_threads_working(thpool_* thpool_p) {
    return thpool_p->num_threads_working;
}


/* ============================ THREAD ============================== */

static int thread_init(thpool_* thpool_p, thread_wrapper** thread_p, int id) {
    *thread_p = new(std::nothrow) thread_wrapper;
    if (*thread_p == nullptr) {
        err("thread_init(): Could not allocate memory for thread\n");
        return -1;
    }

    (*thread_p)->thpool_p = thpool_p;
    (*thread_p)->id = id;

    /* Create and detach thread */
    (*thread_p)->pthread = new std::thread(thread_do, *thread_p);
    (*thread_p)->pthread->detach();

    return 0;
}


/* What each thread is doing */
static void thread_do(thread_wrapper* thread_p) {
    /* Assure all threads have been created before starting serving */
    thpool_* thpool_p = thread_p->thpool_p;

    /* Mark thread as alive (initialized) */
    {
        std::lock_guard<std::mutex> lock(thpool_p->thcount_lock);
        thpool_p->num_threads_alive++;
    }

    while (threads_keepalive) {
        bsem_wait(thpool_p->jobqueue.has_jobs);

        if (threads_keepalive) {
            {
                std::lock_guard<std::mutex> lock(thpool_p->thcount_lock);
                thpool_p->num_threads_working++;
            }

            /* Read job from queue and execute it */
            job* job_p = jobqueue_pull(&thpool_p->jobqueue);
            if (job_p) {
                job_p->function(job_p->arg);
                delete job_p;
            }

            {
                std::lock_guard<std::mutex> lock(thpool_p->thcount_lock);
                thpool_p->num_threads_working--;
                if (!thpool_p->num_threads_working) {
                    thpool_p->threads_all_idle.notify_one();
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(thpool_p->thcount_lock);
        thpool_p->num_threads_alive--;
    }
}


/* Frees a thread */
static void thread_destroy(thread_wrapper* thread_p) {
    if (thread_p) {
        delete thread_p->pthread;
        delete thread_p;
    }
}


/* ============================ JOB QUEUE =========================== */

static int jobqueue_init(jobqueue* jobqueue_p) {
    jobqueue_p->len = 0;
    jobqueue_p->front = nullptr;
    jobqueue_p->rear = nullptr;

    jobqueue_p->has_jobs = new(std::nothrow) bsem;
    if (jobqueue_p->has_jobs == nullptr) {
        return -1;
    }

    bsem_init(jobqueue_p->has_jobs, 0);
    return 0;
}


static void jobqueue_clear(jobqueue* jobqueue_p) {
    while (jobqueue_p->len) {
        delete jobqueue_pull(jobqueue_p);
    }

    jobqueue_p->front = nullptr;
    jobqueue_p->rear = nullptr;
    bsem_reset(jobqueue_p->has_jobs);
    jobqueue_p->len = 0;
}


static void jobqueue_push(jobqueue* jobqueue_p, job* newjob) {
    std::lock_guard<std::mutex> lock(jobqueue_p->rwmutex);
    newjob->prev = nullptr;

    switch (jobqueue_p->len) {
        case 0:  /* if no jobs in queue */
            jobqueue_p->front = newjob;
            jobqueue_p->rear = newjob;
            break;

        default: /* if jobs in queue */
            jobqueue_p->rear->prev = newjob;
            jobqueue_p->rear = newjob;
    }
    jobqueue_p->len++;

    bsem_post(jobqueue_p->has_jobs);
}


static job* jobqueue_pull(jobqueue* jobqueue_p) {
    std::lock_guard<std::mutex> lock(jobqueue_p->rwmutex);
    job* job_p = jobqueue_p->front;

    switch (jobqueue_p->len) {
        case 0:  /* if no jobs in queue */
            break;

        case 1:  /* if one job in queue */
            jobqueue_p->front = nullptr;
            jobqueue_p->rear = nullptr;
            jobqueue_p->len = 0;
            break;

        default: /* if >1 jobs in queue */
            jobqueue_p->front = job_p->prev;
            jobqueue_p->len--;
            /* more than one job in queue -> post it */
            bsem_post(jobqueue_p->has_jobs);
    }

    return job_p;
}


static void jobqueue_destroy(jobqueue* jobqueue_p) {
    jobqueue_clear(jobqueue_p);
    delete jobqueue_p->has_jobs;
}


/* ======================== SYNCHRONISATION ========================= */

static void bsem_init(bsem* bsem_p, int value) {
    if (value < 0 || value > 1) {
        err("bsem_init(): Binary semaphore can take only values 1 or 0");
        exit(1);
    }
    bsem_p->v = value;
}


static void bsem_reset(bsem* bsem_p) {
    bsem_p->v = 0;
}


static void bsem_post(bsem* bsem_p) {
    std::lock_guard<std::mutex> lock(bsem_p->mtx);
    bsem_p->v = 1;
    bsem_p->cond.notify_one();
}


static void bsem_post_all(bsem* bsem_p) {
    std::lock_guard<std::mutex> lock(bsem_p->mtx);
    bsem_p->v = 1;
    bsem_p->cond.notify_all();
}


static void bsem_wait(bsem* bsem_p) {
    std::unique_lock<std::mutex> lock(bsem_p->mtx);
    while (bsem_p->v != 1) {
        bsem_p->cond.wait(lock);
    }
    bsem_p->v = 0;
}
