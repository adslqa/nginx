#ifndef _NGX_EVENT_TIMER_H_INCLUDED_
#define _NGX_EVENT_TIMER_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#define NGX_TIMER_ERROR  (ngx_msec_t) -1

/*
 * 32 bit timer key value resolution
 *
 * 1 msec - 49 days
 * 10 msec - 1 years 4 months
 * 50 msec - 6 years 10 months
 * 100 msec - 13 years 8 months
 */

#define NGX_TIMER_RESOLUTION  1


ngx_int_t ngx_event_timer_init(ngx_log_t *log);
ngx_msec_t ngx_event_find_timer(void);
void ngx_event_expire_timers(ngx_msec_t timer);


#if (NGX_THREADS)
extern ngx_mutex_t  *ngx_event_timer_mutex;
#endif


extern volatile ngx_rbtree_t  *ngx_event_timer_rbtree;
extern ngx_rbtree_t            ngx_event_timer_sentinel;


ngx_inline static void ngx_event_del_timer(ngx_event_t *ev)
{
    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "event timer del: %d: %d",
                    ngx_event_ident(ev->data), ev->rbtree_key);

#if (NGX_THREADS)
    if (ngx_mutex_lock(ngx_event_timer_mutex) == NGX_ERROR) {
        return;
    }
#endif

    ngx_rbtree_delete((ngx_rbtree_t **) &ngx_event_timer_rbtree,
                      &ngx_event_timer_sentinel,
                      (ngx_rbtree_t *) &ev->rbtree_key);

#if (NGX_THREADS)
    ngx_mutex_unlock(ngx_event_timer_mutex);
#endif

#if (NGX_DEBUG)
    ev->rbtree_left = NULL;
    ev->rbtree_right = NULL;
    ev->rbtree_parent = NULL;
#endif

    ev->timer_set = 0;
}


ngx_inline static void ngx_event_add_timer(ngx_event_t *ev, ngx_msec_t timer)
{
    if (ev->timer_set) {
        ngx_del_timer(ev);
    }

    ev->rbtree_key = (ngx_int_t)
              (ngx_elapsed_msec / NGX_TIMER_RESOLUTION * NGX_TIMER_RESOLUTION
                                              + timer) / NGX_TIMER_RESOLUTION;
#if 0
                             (ngx_elapsed_msec + timer) / NGX_TIMER_RESOLUTION;
#endif

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, ev->log, 0,
                   "event timer add: %d: %d",
                    ngx_event_ident(ev->data), ev->rbtree_key);

#if (NGX_THREADS)
    if (ngx_mutex_lock(ngx_event_timer_mutex) == NGX_ERROR) {
        return;
    }
#endif

    ngx_rbtree_insert((ngx_rbtree_t **) &ngx_event_timer_rbtree,
                      &ngx_event_timer_sentinel,
                      (ngx_rbtree_t *) &ev->rbtree_key);

#if (NGX_THREADS)
    ngx_mutex_unlock(ngx_event_timer_mutex);
#endif

    ev->timer_set = 1;
}


#endif /* _NGX_EVENT_TIMER_H_INCLUDED_ */
