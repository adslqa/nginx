
/*
 * Copyright (C) 2002-2004 Igor Sysoev, http://sysoev.ru/en/
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


/*
 * On Linux up to 2.4.21 sendfile() (syscall #187) works with 32-bit
 * offsets only and the including <sys/sendfile.h> breaks the compiling
 * if off_t is 64 bit wide.  So we use own sendfile() definition where offset
 * parameter is int32_t and use sendfile() with the file parts below 2G.
 *
 * Linux 2.4.21 has a new sendfile64() syscall #239.
 */


ngx_chain_t *ngx_linux_sendfile_chain(ngx_connection_t *c, ngx_chain_t *in)
{
    int              rc;
    u_char          *prev;
    off_t            fprev;
    size_t           size, fsize, sent;
    ngx_int_t        eintr;
    struct iovec    *iov;
    ngx_err_t        err;
    ngx_hunk_t      *file;
    ngx_array_t      header;
    ngx_event_t     *wev;
    ngx_chain_t     *cl, *tail;
#if (HAVE_SENDFILE64)
    off_t            offset;
#else
    int32_t          offset;
#endif

    wev = c->write;

    if (!wev->ready) {
        return in;
    }

    do {
        file = NULL;
        fsize = 0;
        eintr = 0;

        ngx_init_array(header, c->pool, 10, sizeof(struct iovec),
                       NGX_CHAIN_ERROR);

        prev = NULL;
        iov = NULL;

        /* create the iovec and coalesce the neighbouring hunks */

        for (cl = in; cl && header.nelts < IOV_MAX; cl = cl->next) {
            if (ngx_hunk_special(cl->hunk)) {
                continue;
            }

            if (!ngx_hunk_in_memory_only(cl->hunk)) {
                break;
            }

            if (prev == cl->hunk->pos) {
                iov->iov_len += cl->hunk->last - cl->hunk->pos;

            } else {
                ngx_test_null(iov, ngx_push_array(&header), NGX_CHAIN_ERROR);
                iov->iov_base = (void *) cl->hunk->pos;
                iov->iov_len = cl->hunk->last - cl->hunk->pos;
            }

            prev = cl->hunk->last;
        }

        /* set TCP_CORK if there is a header before a file */

        if (!c->tcp_nopush == 0
            && header.nelts != 0
            && cl
            && cl->hunk->type & NGX_HUNK_FILE)
        {
            if (ngx_tcp_nopush(c->fd) == NGX_ERROR) {
                err = ngx_errno;

                /*
                 * there is a tiny chance to be interrupted, however
                 * we continue a processing without the TCP_CORK
                 */

                if (err != NGX_EINTR) { 
                    wev->error = 1;
                    ngx_connection_error(c, err, ngx_tcp_nopush_n " failed");
                    return NGX_CHAIN_ERROR;
                }

            } else {
                c->tcp_nopush = 1;
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, 0,
                               "tcp_nopush");
            }
        }

        if (header.nelts == 0 && cl && cl->hunk->type & NGX_HUNK_FILE) {

            /* get the file hunk */

            file = cl->hunk;
            fsize = (size_t) (file->file_last - file->file_pos);
            fprev = file->file_last;
            cl = cl->next; 

            /* coalesce the neighbouring file hunks */

            while (cl && (cl->hunk->type & NGX_HUNK_FILE)) {
                if (file->file->fd != cl->hunk->file->fd
                    || fprev != cl->hunk->file_pos)
                {
                    break;
                }

                fsize += (size_t) (cl->hunk->file_last - cl->hunk->file_pos);
                fprev = cl->hunk->file_last;
                cl = cl->next;
            }
        }

        /* 
         * the tail is the rest of the chain that exceeded
         * a single sendfile() capability
         */

        tail = cl;

        if (fsize) {
#if (HAVE_SENDFILE64)
            offset = file->file_pos;
#else
            offset = (int32_t) file->file_pos;
#endif
            rc = sendfile(c->fd, file->file->fd, &offset, fsize);

            if (rc == -1) {
                err = ngx_errno;

                if (err == NGX_EAGAIN || err == NGX_EINTR) {
                    if (err == NGX_EINTR) {
                        eintr = 1;
                    }

                    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                                   "sendfile() is not ready");

                } else {
                    wev->error = 1;
                    ngx_connection_error(c, err, "sendfile() failed");
                    return NGX_CHAIN_ERROR;
                }
            }

            sent = rc > 0 ? rc : 0;

            ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                           "sendfile: %d, @" OFF_T_FMT " %d:%d",
                           rc, file->file_pos, sent, fsize);

        } else {
            rc = writev(c->fd, header.elts, header.nelts);

            if (rc == -1) {
                err = ngx_errno;

                if (err == NGX_EAGAIN || err == NGX_EINTR) {
                    if (err == NGX_EINTR) {
                        eintr = 1;
                    }

                    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, c->log, err,
                                   "writev() not ready");

                } else {
                    wev->error = 1;
                    ngx_connection_error(c, err, "writev() failed");
                    return NGX_CHAIN_ERROR; 
                }
            }

            sent = rc > 0 ? rc : 0;

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, c->log, 0, "writev: %d", sent);
        }

        c->sent += sent;

        for (cl = in; cl; cl = cl->next) {

            if (ngx_hunk_special(cl->hunk)) {
                continue;
            }

            if (sent == 0) {
                break;
            }

            size = ngx_hunk_size(cl->hunk);

            if (sent >= size) {
                sent -= size;

                if (cl->hunk->type & NGX_HUNK_IN_MEMORY) {
                    cl->hunk->pos = cl->hunk->last;
                }

                if (cl->hunk->type & NGX_HUNK_FILE) {
                    cl->hunk->file_pos = cl->hunk->file_last;
                }

                continue;
            }

            if (cl->hunk->type & NGX_HUNK_IN_MEMORY) {
                cl->hunk->pos += sent;
            }

            if (cl->hunk->type & NGX_HUNK_FILE) {
                cl->hunk->file_pos += sent;
            }

            break;
        }

        in = cl;

        /* "tail == in" means that a single sendfile() is complete */

    } while ((tail && tail == in) || eintr);

    if (in) {
        wev->ready = 0;
    }

    return in;
}
