/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <apr_atomic.h>
#include <apr_thread_mutex.h>
#include <apr_thread_cond.h>

#include <mpm_common.h>
#include <httpd.h>
#include <http_core.h>
#include <http_log.h>

#include "h2_private.h"
#include "h2_mplx.h"
#include "h2_request.h"
#include "h2_task_queue.h"
#include "h2_worker.h"
#include "h2_workers.h"


static int in_list(h2_workers *workers, h2_mplx *m)
{
    h2_mplx *e;
    for (e = H2_MPLX_LIST_FIRST(&workers->mplxs); 
         e != H2_MPLX_LIST_SENTINEL(&workers->mplxs);
         e = H2_MPLX_NEXT(e)) {
        if (e == m) {
            return 1;
        }
    }
    return 0;
}

static void cleanup_zombies(h2_workers *workers, int lock)
{
    if (lock) {
        apr_thread_mutex_lock(workers->lock);
    }
    while (!H2_WORKER_LIST_EMPTY(&workers->zombies)) {
        h2_worker *zombie = H2_WORKER_LIST_FIRST(&workers->zombies);
        H2_WORKER_REMOVE(zombie);
        ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, workers->s,
                      "h2_workers: cleanup zombie %d", zombie->id);
        h2_worker_destroy(zombie);
    }
    if (lock) {
        apr_thread_mutex_unlock(workers->lock);
    }
}

/**
 * Get the next task for the given worker. Will block until a task arrives
 * or the max_wait timer expires and more than min workers exist.
 * The previous h2_mplx instance might be passed in and will be served
 * with preference, since we can ask it for the next task without aquiring
 * the h2_workers lock.
 */
static apr_status_t get_mplx_next(h2_worker *worker, h2_mplx **pm, 
                                  const h2_request **preq, void *ctx)
{
    apr_status_t status;
    apr_time_t max_wait, start_wait;
    h2_workers *workers = (h2_workers *)ctx;
    
    max_wait = apr_time_from_sec(apr_atomic_read32(&workers->max_idle_secs));
    start_wait = apr_time_now();
    
    status = apr_thread_mutex_lock(workers->lock);
    if (status == APR_SUCCESS) {
        const h2_request *req = NULL;
        h2_mplx *m = NULL;
        int has_more = 0;

        ++workers->idle_worker_count;
        ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, workers->s,
                     "h2_worker(%d): looking for work", h2_worker_get_id(worker));
        
        while (!req && !h2_worker_is_aborted(worker) && !workers->aborted) {
            
            /* Get the next h2_mplx to process that has a task to hand out.
             * If it does, place it at the end of the queu and return the
             * task to the worker.
             * If it (currently) has no tasks, remove it so that it needs
             * to register again for scheduling.
             * If we run out of h2_mplx in the queue, we need to wait for
             * new mplx to arrive. Depending on how many workers do exist,
             * we do a timed wait or block indefinitely.
             */
            m = NULL;
            while (!req && !H2_MPLX_LIST_EMPTY(&workers->mplxs)) {
                m = H2_MPLX_LIST_FIRST(&workers->mplxs);
                H2_MPLX_REMOVE(m);
                
                req = h2_mplx_pop_request(m, &has_more);
                if (req) {
                    if (has_more) {
                        H2_MPLX_LIST_INSERT_TAIL(&workers->mplxs, m);
                    }
                    else {
                        has_more = !H2_MPLX_LIST_EMPTY(&workers->mplxs);
                    }
                    break;
                }
            }
            
            if (!req) {
                /* Need to wait for a new mplx to arrive.
                 */
                cleanup_zombies(workers, 0);
                
                if (workers->worker_count > workers->min_size) {
                    apr_time_t now = apr_time_now();
                    if (now >= (start_wait + max_wait)) {
                        /* waited long enough without getting a task. */
                        if (workers->worker_count > workers->min_size) {
                            ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, 
                                         workers->s,
                                         "h2_workers: aborting idle worker");
                            h2_worker_abort(worker);
                            break;
                        }
                    }
                    ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, workers->s,
                                 "h2_worker(%d): waiting signal, "
                                 "worker_count=%d", worker->id, 
                                 (int)workers->worker_count);
                    apr_thread_cond_timedwait(workers->mplx_added,
                                              workers->lock, max_wait);
                }
                else {
                    ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, workers->s,
                                 "h2_worker(%d): waiting signal (eternal), "
                                 "worker_count=%d", worker->id, 
                                 (int)workers->worker_count);
                    apr_thread_cond_wait(workers->mplx_added, workers->lock);
                }
            }
        }
        
        /* Here, we either have gotten task and mplx for the worker or
         * needed to give up with more than enough workers.
         */
        if (req) {
            ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, workers->s,
                         "h2_worker(%d): start request(%ld-%d)",
                         h2_worker_get_id(worker), m->id, req->id);
            *pm = m;
            *preq = req;
            
            if (has_more && workers->idle_worker_count > 1) {
                apr_thread_cond_signal(workers->mplx_added);
            }
            status = APR_SUCCESS;
        }
        else {
            status = APR_EOF;
        }
        
        --workers->idle_worker_count;
        apr_thread_mutex_unlock(workers->lock);
    }
    
    return status;
}

static void worker_done(h2_worker *worker, void *ctx)
{
    h2_workers *workers = (h2_workers *)ctx;
    apr_status_t status = apr_thread_mutex_lock(workers->lock);
    if (status == APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, workers->s,
                     "h2_worker(%d): done", h2_worker_get_id(worker));
        H2_WORKER_REMOVE(worker);
        --workers->worker_count;
        H2_WORKER_LIST_INSERT_TAIL(&workers->zombies, worker);
        
        apr_thread_mutex_unlock(workers->lock);
    }
}

static apr_status_t add_worker(h2_workers *workers)
{
    h2_worker *w = h2_worker_create(workers->next_worker_id++,
                                    workers->pool, workers->thread_attr,
                                    get_mplx_next, worker_done, workers);
    if (!w) {
        return APR_ENOMEM;
    }
    ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, workers->s,
                 "h2_workers: adding worker(%d)", h2_worker_get_id(w));
    ++workers->worker_count;
    H2_WORKER_LIST_INSERT_TAIL(&workers->workers, w);
    return APR_SUCCESS;
}

static apr_status_t h2_workers_start(h2_workers *workers)
{
    apr_status_t status = apr_thread_mutex_lock(workers->lock);
    if (status == APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, workers->s,
                      "h2_workers: starting");

        while (workers->worker_count < workers->min_size
               && status == APR_SUCCESS) {
            status = add_worker(workers);
        }
        apr_thread_mutex_unlock(workers->lock);
    }
    return status;
}

h2_workers *h2_workers_create(server_rec *s, apr_pool_t *server_pool,
                              int min_size, int max_size,
                              apr_size_t max_tx_handles)
{
    apr_status_t status;
    h2_workers *workers;
    apr_pool_t *pool;

    AP_DEBUG_ASSERT(s);
    AP_DEBUG_ASSERT(server_pool);

    /* let's have our own pool that will be parent to all h2_worker
     * instances we create. This happens in various threads, but always
     * guarded by our lock. Without this pool, all subpool creations would
     * happen on the pool handed to us, which we do not guard.
     */
    apr_pool_create(&pool, server_pool);
    workers = apr_pcalloc(pool, sizeof(h2_workers));
    if (workers) {
        workers->s = s;
        workers->pool = pool;
        workers->min_size = min_size;
        workers->max_size = max_size;
        apr_atomic_set32(&workers->max_idle_secs, 10);
        
        workers->max_tx_handles = max_tx_handles;
        workers->spare_tx_handles = workers->max_tx_handles;
        
        apr_threadattr_create(&workers->thread_attr, workers->pool);
        if (ap_thread_stacksize != 0) {
            apr_threadattr_stacksize_set(workers->thread_attr,
                                         ap_thread_stacksize);
            ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, s,
                         "h2_workers: using stacksize=%ld", 
                         (long)ap_thread_stacksize);
        }
        
        APR_RING_INIT(&workers->workers, h2_worker, link);
        APR_RING_INIT(&workers->zombies, h2_worker, link);
        APR_RING_INIT(&workers->mplxs, h2_mplx, link);
        
        status = apr_thread_mutex_create(&workers->lock,
                                         APR_THREAD_MUTEX_DEFAULT,
                                         workers->pool);
        if (status == APR_SUCCESS) {
            status = apr_thread_cond_create(&workers->mplx_added, workers->pool);
        }
        
        if (status == APR_SUCCESS) {
            status = apr_thread_mutex_create(&workers->tx_lock,
                                             APR_THREAD_MUTEX_DEFAULT,
                                             workers->pool);
        }
        
        if (status == APR_SUCCESS) {
            status = h2_workers_start(workers);
        }
        
        if (status != APR_SUCCESS) {
            h2_workers_destroy(workers);
            workers = NULL;
        }
    }
    return workers;
}

void h2_workers_destroy(h2_workers *workers)
{
    /* before we go, cleanup any zombie workers that may have accumulated */
    cleanup_zombies(workers, 1);
    
    if (workers->mplx_added) {
        apr_thread_cond_destroy(workers->mplx_added);
        workers->mplx_added = NULL;
    }
    if (workers->lock) {
        apr_thread_mutex_destroy(workers->lock);
        workers->lock = NULL;
    }
    while (!H2_MPLX_LIST_EMPTY(&workers->mplxs)) {
        h2_mplx *m = H2_MPLX_LIST_FIRST(&workers->mplxs);
        H2_MPLX_REMOVE(m);
    }
    while (!H2_WORKER_LIST_EMPTY(&workers->workers)) {
        h2_worker *w = H2_WORKER_LIST_FIRST(&workers->workers);
        H2_WORKER_REMOVE(w);
    }
    if (workers->pool) {
        apr_pool_destroy(workers->pool);
        /* workers is gone */
    }
}

apr_status_t h2_workers_register(h2_workers *workers, struct h2_mplx *m)
{
    apr_status_t status = apr_thread_mutex_lock(workers->lock);
    if (status == APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_TRACE3, status, workers->s,
                     "h2_workers: register mplx(%ld)", m->id);
        if (in_list(workers, m)) {
            ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, workers->s,
                         "h2_workers: already registered mplx(%ld)", m->id);
            status = APR_EAGAIN;
        }
        else {
            H2_MPLX_LIST_INSERT_TAIL(&workers->mplxs, m);
            status = APR_SUCCESS;
        }
        
        if (workers->idle_worker_count > 0) { 
            apr_thread_cond_signal(workers->mplx_added);
        }
        else if (status == APR_SUCCESS 
                 && workers->worker_count < workers->max_size) {
            ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, workers->s,
                         "h2_workers: got %d worker, adding 1", 
                         workers->worker_count);
            add_worker(workers);
        }
        apr_thread_mutex_unlock(workers->lock);
    }
    return status;
}

apr_status_t h2_workers_unregister(h2_workers *workers, struct h2_mplx *m)
{
    apr_status_t status = apr_thread_mutex_lock(workers->lock);
    if (status == APR_SUCCESS) {
        status = APR_EAGAIN;
        if (in_list(workers, m)) {
            H2_MPLX_REMOVE(m);
            status = APR_SUCCESS;
        }
        apr_thread_mutex_unlock(workers->lock);
    }
    return status;
}

void h2_workers_set_max_idle_secs(h2_workers *workers, int idle_secs)
{
    if (idle_secs <= 0) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, workers->s,
                     APLOGNO(02962) "h2_workers: max_worker_idle_sec value of %d"
                     " is not valid, ignored.", idle_secs);
        return;
    }
    apr_atomic_set32(&workers->max_idle_secs, idle_secs);
}

apr_size_t h2_workers_tx_reserve(h2_workers *workers, apr_size_t count)
{
    apr_status_t status = apr_thread_mutex_lock(workers->tx_lock);
    if (status == APR_SUCCESS) {
        count = H2MIN(workers->spare_tx_handles, count);
        workers->spare_tx_handles -= count;
        ap_log_error(APLOG_MARK, APLOG_TRACE2, 0, workers->s,
                     "h2_workers: reserved %d tx handles, %d/%d left", 
                     (int)count, (int)workers->spare_tx_handles,
                     (int)workers->max_tx_handles);
        apr_thread_mutex_unlock(workers->tx_lock);
        return count;
    }
    return 0;
}

void h2_workers_tx_free(h2_workers *workers, apr_size_t count)
{
    apr_status_t status = apr_thread_mutex_lock(workers->tx_lock);
    if (status == APR_SUCCESS) {
        workers->spare_tx_handles += count;
        ap_log_error(APLOG_MARK, APLOG_TRACE2, 0, workers->s,
                     "h2_workers: freed %d tx handles, %d/%d left", 
                     (int)count, (int)workers->spare_tx_handles,
                     (int)workers->max_tx_handles);
        apr_thread_mutex_unlock(workers->tx_lock);
    }
}

