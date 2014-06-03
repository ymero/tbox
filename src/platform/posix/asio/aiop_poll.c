/*!The Treasure Box Library
 * 
 * TBox is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * 
 * TBox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with TBox; 
 * If not, see <a href="http://www.gnu.org/licenses/"> http://www.gnu.org/licenses/</a>
 * 
 * Copyright (C) 2009 - 2015, ruki All rights reserved.
 *
 * @author      ruki
 * @file        aiop_poll.c
 *
 */
/* //////////////////////////////////////////////////////////////////////////////////////
 * includes
 */
#include "prefix.h"
#include <sys/poll.h>
#include "../../spinlock.h"
#include "../../../algorithm/algorithm.h"

/* //////////////////////////////////////////////////////////////////////////////////////
 * types
 */

// the poll lock type
typedef struct __tb_poll_lock_t
{
    // the pfds
    tb_spinlock_t           pfds;

    // the hash
    tb_spinlock_t           hash;

}tb_poll_lock_t;

// the poll reactor type
typedef struct __tb_aiop_reactor_poll_t
{
    // the reactor base
    tb_aiop_reactor_t       base;

    // the poll fds
    tb_vector_t*            pfds;

    // the copy fds
    tb_vector_t*            cfds;

    // the hash
    tb_hash_t*              hash;

    // the lock
    tb_poll_lock_t          lock;

}tb_aiop_reactor_poll_t;

/* //////////////////////////////////////////////////////////////////////////////////////
 * implementation
 */
static tb_bool_t tb_poll_walk_delo(tb_vector_t* vector, tb_pointer_t item, tb_bool_t* bdel, tb_cpointer_t priv)
{
    // check
    tb_assert_and_check_return_val(vector && bdel && priv, tb_false);

    // the fd
    tb_long_t fd = (tb_long_t)priv;

    // is this?
    struct pollfd* pfd = (struct pollfd*)item;
    if (pfd && pfd->fd == fd) 
    {
        // remove it
        *bdel = tb_true;

        // break
        return tb_false;
    }

    // ok
    return tb_true;
}
static tb_bool_t tb_poll_walk_sete(tb_iterator_t* iterator, tb_pointer_t item, tb_cpointer_t priv)
{
    // check
    tb_assert_and_check_return_val(iterator, tb_false);

    // the aioe
    tb_aioe_t const* aioe = (tb_aioe_t const*)priv;
    tb_assert_and_check_return_val(aioe, tb_false);

    // the aioo
    tb_aioo_t const* aioo = aioe->aioo;
    tb_assert_and_check_return_val(aioo && aioo->handle, tb_false);

    // is this?
    struct pollfd* pfd = (struct pollfd*)item;
    if (pfd && pfd->fd == ((tb_long_t)aioo->handle - 1)) 
    {
        pfd->events = 0;
        if (aioe->code & TB_AIOE_CODE_RECV || aioe->code & TB_AIOE_CODE_ACPT) pfd->events |= POLLIN;
        if (aioe->code & TB_AIOE_CODE_SEND || aioe->code & TB_AIOE_CODE_CONN) pfd->events |= POLLOUT;

        // break
        return tb_false;
    }

    // ok
    return tb_true;
}
static tb_bool_t tb_aiop_reactor_poll_addo(tb_aiop_reactor_t* reactor, tb_aioo_t const* aioo)
{
    // check
    tb_aiop_reactor_poll_t* rtor = (tb_aiop_reactor_poll_t*)reactor;
    tb_assert_and_check_return_val(rtor && rtor->pfds && rtor->cfds && aioo && aioo->handle, tb_false);

    // the aiop
    tb_aiop_t* aiop = reactor->aiop;
    tb_assert_and_check_return_val(aiop, tb_false);

    // add handle => aioo
    tb_bool_t ok = tb_false;
    tb_spinlock_enter(&rtor->lock.hash);
    if (rtor->hash) 
    {
        tb_hash_set(rtor->hash, aioo->handle, aioo);
        ok = tb_true;
    }
    tb_spinlock_leave(&rtor->lock.hash);
    tb_assert_and_check_return_val(ok, tb_false);

    // the code
    tb_size_t code = aioo->code;

    // init pfd
    struct pollfd pfd = {0};
    pfd.fd = ((tb_long_t)aioo->handle) - 1;
    if (code & TB_AIOE_CODE_RECV || code & TB_AIOE_CODE_ACPT) pfd.events |= POLLIN;
    if (code & TB_AIOE_CODE_SEND || code & TB_AIOE_CODE_CONN) pfd.events |= POLLOUT;

    // add pfd, TODO: addo by binary search
    tb_spinlock_enter(&rtor->lock.pfds);
    tb_vector_insert_tail(rtor->pfds, &pfd);
    tb_spinlock_leave(&rtor->lock.pfds);

    // spak it
    if (aiop->spak[0] && code) tb_socket_send(aiop->spak[0], (tb_byte_t const*)"p", 1);

    // ok?
    return ok;
}
static tb_bool_t tb_aiop_reactor_poll_delo(tb_aiop_reactor_t* reactor, tb_aioo_t const* aioo)
{
    // check
    tb_aiop_reactor_poll_t* rtor = (tb_aiop_reactor_poll_t*)reactor;
    tb_assert_and_check_return_val(rtor && rtor->pfds && rtor->cfds && aioo && aioo->handle, tb_false);

    // the aiop
    tb_aiop_t* aiop = reactor->aiop;
    tb_assert_and_check_return_val(aiop, tb_false);

    // delo it, TODO: delo by binary search
    tb_spinlock_enter(&rtor->lock.pfds);
    tb_vector_walk(rtor->pfds, tb_poll_walk_delo, (tb_pointer_t)(((tb_long_t)aioo->handle) - 1));
    tb_spinlock_leave(&rtor->lock.pfds);

    // del handle => aioo
    tb_spinlock_enter(&rtor->lock.hash);
    if (rtor->hash) tb_hash_del(rtor->hash, aioo->handle);
    tb_spinlock_leave(&rtor->lock.hash);

    // spak it
    if (aiop->spak[0]) tb_socket_send(aiop->spak[0], (tb_byte_t const*)"p", 1);

    // ok
    return tb_true;
}
static tb_bool_t tb_aiop_reactor_poll_post(tb_aiop_reactor_t* reactor, tb_aioe_t const* aioe)
{
    // check
    tb_aiop_reactor_poll_t* rtor = (tb_aiop_reactor_poll_t*)reactor;
    tb_assert_and_check_return_val(rtor && rtor->pfds && rtor->cfds && aioe, tb_false);

    // the aiop
    tb_aiop_t* aiop = reactor->aiop;
    tb_assert_and_check_return_val(aiop, tb_false);

    // the aioo
    tb_aioo_t* aioo = aioe->aioo;
    tb_assert_and_check_return_val(aioo, tb_false);

    // save aioo
    aioo->code = aioe->code;
    aioo->priv = aioe->priv;

    // sete it, TODO: sete by binary search
    tb_spinlock_enter(&rtor->lock.pfds);
    tb_walk_all(rtor->pfds, tb_poll_walk_sete, (tb_pointer_t)aioe);
    tb_spinlock_leave(&rtor->lock.pfds);

    // spak it
    if (aiop->spak[0]) tb_socket_send(aiop->spak[0], (tb_byte_t const*)"p", 1);

    // ok
    return tb_true;
}
static tb_long_t tb_aiop_reactor_poll_wait(tb_aiop_reactor_t* reactor, tb_aioe_t* list, tb_size_t maxn, tb_long_t timeout)
{   
    // check
    tb_aiop_reactor_poll_t* rtor = (tb_aiop_reactor_poll_t*)reactor;
    tb_assert_and_check_return_val(rtor && rtor->pfds && rtor->cfds && list && maxn, -1);

    // the aiop
    tb_aiop_t* aiop = reactor->aiop;
    tb_assert_and_check_return_val(aiop, tb_false);

    // loop
    tb_long_t wait = 0;
    tb_bool_t stop = tb_false;
    tb_hong_t time = tb_mclock();
    while (!wait && !stop && (timeout < 0 || tb_mclock() < time + timeout))
    {
        // copy pfds
        tb_spinlock_enter(&rtor->lock.pfds);
        tb_vector_copy(rtor->cfds, rtor->pfds);
        tb_spinlock_leave(&rtor->lock.pfds);

        // cfds
        struct pollfd*  cfds = (struct pollfd*)tb_vector_data(rtor->cfds);
        tb_size_t       cfdm = tb_vector_size(rtor->cfds);
        tb_assert_and_check_return_val(cfds && cfdm, -1);

        // wait
        tb_long_t cfdn = poll(cfds, cfdm, timeout);
        tb_assert_and_check_return_val(cfdn >= 0, -1);

        // timeout?
        tb_check_return_val(cfdn, 0);

        // sync
        tb_size_t i = 0;
        for (i = 0; i < cfdm && wait < maxn; i++)
        {
            // the handle
            tb_handle_t handle = tb_fd2handle(cfds[i].fd);
            tb_assert_and_check_return_val(handle, -1);

            // the events
            tb_size_t events = cfds[i].revents;
            tb_check_continue(events);

            // spak?
            if (handle == aiop->spak[1] && (events & POLLIN))
            {
                // read spak
                tb_char_t spak = '\0';
                if (1 != tb_socket_recv(aiop->spak[1], (tb_byte_t*)&spak, 1)) return -1;

                // killed?
                if (spak == 'k') return -1;

                // stop to wait
                stop = tb_true;

                // continue it
                continue ;
            }

            // skip spak
            tb_check_continue(handle != aiop->spak[1]);

            // the aioo
            tb_size_t       code = TB_AIOE_CODE_NONE;
            tb_cpointer_t   priv = tb_null;
            tb_aioo_t*      aioo = tb_null;
            tb_spinlock_enter(&rtor->lock.hash);
            if (rtor->hash)
            {
                aioo = (tb_aioo_t*)tb_hash_get(rtor->hash, handle);
                if (aioo) 
                {
                    // save code & data
                    code = aioo->code;
                    priv = aioo->priv;

                    // oneshot? clear it
                    if (aioo->code & TB_AIOE_CODE_ONESHOT)
                    {
                        aioo->code = TB_AIOE_CODE_NONE;
                        aioo->priv = tb_null;
                    }
                }
            }
            tb_spinlock_leave(&rtor->lock.hash);
            tb_check_continue(aioo && code);
            
            // init aioe
            tb_aioe_t   aioe = {0};
            aioe.priv   = priv;
            aioe.aioo   = aioo;
            if (events & POLLIN)
            {
                aioe.code |= TB_AIOE_CODE_RECV;
                if (code & TB_AIOE_CODE_ACPT) aioe.code |= TB_AIOE_CODE_ACPT;
            }
            if (events & POLLOUT) 
            {
                aioe.code |= TB_AIOE_CODE_SEND;
                if (code & TB_AIOE_CODE_CONN) aioe.code |= TB_AIOE_CODE_CONN;
            }
            if ((events & POLLHUP) && !(code & (TB_AIOE_CODE_RECV | TB_AIOE_CODE_SEND))) 
                aioe.code |= TB_AIOE_CODE_RECV | TB_AIOE_CODE_SEND;

            // save aioe
            list[wait++] = aioe;

            // oneshot?
            if (code & TB_AIOE_CODE_ONESHOT)
            {
                tb_spinlock_enter(&rtor->lock.pfds);
                struct pollfd* pfds = (struct pollfd*)tb_vector_data(rtor->pfds);
                if (pfds) pfds[i].events = 0;
                tb_spinlock_leave(&rtor->lock.pfds);
            }
        }
    }

    // ok
    return wait;
}
static tb_void_t tb_aiop_reactor_poll_exit(tb_aiop_reactor_t* reactor)
{
    tb_aiop_reactor_poll_t* rtor = (tb_aiop_reactor_poll_t*)reactor;
    if (rtor)
    {
        // exit pfds
        tb_spinlock_enter(&rtor->lock.pfds);
        if (rtor->pfds) tb_vector_exit(rtor->pfds);
        rtor->pfds = tb_null;
        tb_spinlock_leave(&rtor->lock.pfds);

        // exit cfds
        if (rtor->cfds) tb_vector_exit(rtor->cfds);
        rtor->cfds = tb_null;

        // exit hash
        tb_spinlock_enter(&rtor->lock.hash);
        if (rtor->hash) tb_hash_exit(rtor->hash);
        rtor->hash = tb_null;
        tb_spinlock_leave(&rtor->lock.hash);

        // exit lock
        tb_spinlock_exit(&rtor->lock.pfds);
        tb_spinlock_exit(&rtor->lock.hash);

        // free it
        tb_free(rtor);
    }
}
static tb_void_t tb_aiop_reactor_poll_cler(tb_aiop_reactor_t* reactor)
{
    tb_aiop_reactor_poll_t* rtor = (tb_aiop_reactor_poll_t*)reactor;
    if (rtor)
    {
        // clear pfds
        tb_spinlock_enter(&rtor->lock.pfds);
        if (rtor->pfds) tb_vector_clear(rtor->pfds);
        tb_spinlock_leave(&rtor->lock.pfds);

        // clear hash
        tb_spinlock_enter(&rtor->lock.hash);
        if (rtor->hash) tb_hash_clear(rtor->hash);
        tb_spinlock_leave(&rtor->lock.hash);

        // spak it
        if (reactor->aiop && reactor->aiop->spak[0])
            tb_socket_send(reactor->aiop->spak[0], (tb_byte_t const*)"p", 1);
    }
}
static tb_aiop_reactor_t* tb_aiop_reactor_poll_init(tb_aiop_t* aiop)
{
    // check
    tb_assert_and_check_return_val(aiop && aiop->maxn, tb_null);

    // done
    tb_bool_t               ok = tb_false;
    tb_aiop_reactor_poll_t* rtor = tb_null;
    do
    {
        // make reactor
        rtor = (tb_aiop_reactor_poll_t*)tb_malloc0(sizeof(tb_aiop_reactor_poll_t));
        tb_assert_and_check_break(rtor);

        // init base
        rtor->base.aiop = aiop;
        rtor->base.exit = tb_aiop_reactor_poll_exit;
        rtor->base.cler = tb_aiop_reactor_poll_cler;
        rtor->base.addo = tb_aiop_reactor_poll_addo;
        rtor->base.delo = tb_aiop_reactor_poll_delo;
        rtor->base.post = tb_aiop_reactor_poll_post;
        rtor->base.wait = tb_aiop_reactor_poll_wait;

        // init lock
        if (!tb_spinlock_init(&rtor->lock.pfds)) break;
        if (!tb_spinlock_init(&rtor->lock.hash)) break;

        // init pfds
        rtor->pfds = tb_vector_init(tb_align8((aiop->maxn >> 3) + 1), tb_item_func_mem(sizeof(struct pollfd), tb_null, tb_null));
        tb_assert_and_check_break(rtor->pfds);

        // init cfds
        rtor->cfds = tb_vector_init(tb_align8((aiop->maxn >> 3) + 1), tb_item_func_mem(sizeof(struct pollfd), tb_null, tb_null));
        tb_assert_and_check_break(rtor->cfds);

        // init hash
        rtor->hash = tb_hash_init(tb_align8(tb_isqrti(aiop->maxn) + 1), tb_item_func_ptr(tb_null, tb_null), tb_item_func_ptr(tb_null, tb_null));
        tb_assert_and_check_break(rtor->hash);

        // ok
        ok = tb_true;

    } while (0);

    // failed?
    if (!ok)
    {
        // exit it
        if (rtor) tb_aiop_reactor_poll_exit((tb_aiop_reactor_t*)rtor);
        rtor = tb_null;
    }

    // ok
    return (tb_aiop_reactor_t*)rtor;
}
