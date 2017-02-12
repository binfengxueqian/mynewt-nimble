/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <string.h>
#include <errno.h>
#include "console/console.h"
#include "nimble/ble.h"
#include "ble_hs_priv.h"
#include "ble_l2cap_priv.h"
#include "ble_l2cap_coc_priv.h"
#include "ble_l2cap_sig_priv.h"

#if MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) != 0

#define BLE_L2CAP_SDU_SIZE              2

STAILQ_HEAD(ble_l2cap_coc_srv_list, ble_l2cap_coc_srv);

static struct ble_l2cap_coc_srv_list ble_l2cap_coc_srvs;

static os_membuf_t ble_l2cap_coc_srv_mem[
    OS_MEMPOOL_SIZE(MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM),
                    sizeof (struct ble_l2cap_coc_srv))
];

static struct os_mempool ble_l2cap_coc_srv_pool;

static void
ble_l2cap_coc_dbg_assert_srv_not_inserted(struct ble_l2cap_coc_srv *srv)
{
#if MYNEWT_VAL(BLE_HS_DEBUG)
    struct ble_l2cap_coc_srv *cur;

    STAILQ_FOREACH(cur, &ble_l2cap_coc_srvs, next) {
        BLE_HS_DBG_ASSERT(cur != srv);
    }
#endif
}

static struct ble_l2cap_coc_srv *
ble_l2cap_coc_srv_alloc(void)
{
    struct ble_l2cap_coc_srv *srv;

    srv = os_memblock_get(&ble_l2cap_coc_srv_pool);
    if (srv != NULL) {
        memset(srv, 0, sizeof(*srv));
    }

    return srv;
}

int
ble_l2cap_coc_create_server(uint16_t psm, uint16_t mtu,
                                        ble_l2cap_event_fn *cb, void *cb_arg)
{
    struct ble_l2cap_coc_srv * srv;

    srv = ble_l2cap_coc_srv_alloc();
    if (!srv) {
            return BLE_HS_ENOMEM;
    }

    srv->psm = psm;
    srv->mtu = mtu;
    srv->cb = cb;
    srv->cb_arg = cb_arg;

    ble_l2cap_coc_dbg_assert_srv_not_inserted(srv);

    STAILQ_INSERT_HEAD(&ble_l2cap_coc_srvs, srv, next);

    return 0;
}

static uint16_t
ble_l2cap_coc_get_cid(void)
{
    static uint16_t next_cid = BLE_L2CAP_COC_CID_START;

    if (next_cid > BLE_L2CAP_COC_CID_END) {
            next_cid = BLE_L2CAP_COC_CID_START;
    }

    /*TODO: Make it smarter*/
    return next_cid++;
}

static struct ble_l2cap_coc_srv *
ble_l2cap_coc_srv_find(uint16_t psm)
{
    struct ble_l2cap_coc_srv *cur, *srv;

    srv = NULL;
    STAILQ_FOREACH(cur, &ble_l2cap_coc_srvs, next) {
        if (cur->psm == psm) {
                srv = cur;
                break;
        }
    }

    return srv;
}

static void
ble_l2cap_event_coc_received_data(struct ble_l2cap_chan *chan,
                                  struct os_mbuf *om)
{
    struct ble_l2cap_event event;

    event.type = BLE_L2CAP_EVENT_COC_DATA_RECEIVED;
    event.receive.chan = chan;
    event.receive.sdu_rx = om;

    chan->cb(&event, chan->cb_arg);
}

static int
ble_l2cap_coc_rx_fn(struct ble_l2cap_chan *chan)
{
    int rc;
    struct os_mbuf **om;
    struct ble_l2cap_coc_endpoint *rx;
    uint16_t om_total;

    /* Create a shortcut to rx_buf */
    om = &chan->rx_buf;
    BLE_HS_DBG_ASSERT(*om != NULL);

    /* Create a shortcut to rx endpoint */
    rx = &chan->coc_rx;

    om_total = OS_MBUF_PKTLEN(*om);
    rc = ble_hs_mbuf_pullup_base(om, om_total);
    if (rc != 0) {
        return rc;
    }

    /* Fist LE frame */
    if (OS_MBUF_PKTLEN(rx->sdu) == 0) {
        uint16_t sdu_len;

        sdu_len = get_le16((*om)->om_data);
        if (sdu_len > rx->mtu) {
            /* TODO Disconnect?*/
            BLE_HS_LOG(INFO, "error: sdu_len > rx->mtu (%d>%d)\n",
                       sdu_len, rx->mtu);
            return BLE_HS_EBADDATA;
        }

        BLE_HS_LOG(DEBUG, "sdu_len=%d, received LE frame=%d, credits=%d\n",
                   sdu_len, om_total, rx->credits);

        os_mbuf_adj(*om , BLE_L2CAP_SDU_SIZE);

        rc = os_mbuf_appendfrom(rx->sdu, *om, 0, om_total - BLE_L2CAP_SDU_SIZE);
        if (rc != 0) {
            /* FIXME: User shall give us big enough buffer.
             * need to handle it better
             */
            BLE_HS_LOG(INFO, "Could not append data rc=%d\n", rc);
            assert(0);
        }

        /* In RX case data_offset keeps incoming SDU len */
        rx->data_offset = sdu_len;

    } else {
        BLE_HS_LOG(DEBUG, "Continuation...received %d\n", (*om)->om_len);

        rc  = os_mbuf_appendfrom(rx->sdu, *om, 0, om_total);
        if (rc != 0) {
            /* FIXME: need to handle it better */
            BLE_HS_LOG(DEBUG, "Could not append data rc=%d\n", rc);
            assert(0);
        }
    }

    rx->credits--;

    if (OS_MBUF_PKTLEN(rx->sdu) == rx->data_offset) {
        struct os_mbuf *sdu_rx = rx->sdu;

        /* Lets get back control to os_mbuf to application.
         * Since it this callback application might want to set new sdu
         * we need to prepare space for this. Therefore we need sdu_rx
         */

        rx->sdu = NULL;
        rx->data_offset = 0;

        ble_l2cap_event_coc_received_data(chan, sdu_rx);

        goto done;
    }

    /* If we did not received full SDU and credits are 0 it means
     * that remote was sending us not fully filled up LE frames.
     * However, we still have buffer to for next LE Frame so lets give one more
     * credit to peer so it can send us full SDU
     */
    if (rx->credits == 0 && rx->sdu) {
        /* Remote did not send full SDU. Lets give him one more credits to do
         * so since we have still buffer to handle it
         */
        rx->credits = 1;
        ble_l2cap_sig_le_credits(chan, rx->credits);
    }

done:
    BLE_HS_LOG(DEBUG, "Received sdu_len=%d, credits left=%d\n",
               OS_MBUF_PKTLEN(rx->sdu), rx->credits);

    return 0;
}

struct ble_l2cap_chan *
ble_l2cap_coc_chan_alloc(uint16_t conn_handle, uint16_t psm, uint16_t mtu,
                         struct os_mbuf *sdu_rx, ble_l2cap_event_fn *cb,
                         void *cb_arg)
{
    struct ble_l2cap_chan *chan;

    chan = ble_l2cap_chan_alloc(conn_handle);
    if (!chan) {
        return NULL;
    }

    chan->psm = psm;
    chan->cb = cb;
    chan->cb_arg = cb_arg;
    chan->scid = ble_l2cap_coc_get_cid();
    chan->my_mtu = BLE_L2CAP_COC_MTU;
    chan->rx_fn = ble_l2cap_coc_rx_fn;
    chan->coc_rx.mtu = mtu;
    chan->coc_rx.credits = 10; /* FIXME Calculate it */
    chan->coc_rx.sdu = sdu_rx;

    return chan;
}

int
ble_l2cap_coc_create_srv_chan(uint16_t conn_handle, uint16_t psm,
                              struct ble_l2cap_chan **chan)
{
    struct ble_l2cap_coc_srv *srv;

    /* Check if there is server registered on this PSM */
    srv = ble_l2cap_coc_srv_find(psm);
    if (!srv) {
        return BLE_HS_ENOTSUP;
    }

    *chan = ble_l2cap_coc_chan_alloc(conn_handle, psm, srv->mtu, NULL, srv->cb,
                                     srv->cb_arg);
    if (!*chan) {
        return BLE_HS_ENOMEM;
    }

    return 0;
}

void
ble_l2cap_coc_cleanup_chan(struct ble_l2cap_chan *chan)
{
    /* PSM 0 is used for fixed channels. */
    if (chan->psm == 0) {
            return;
    }

    os_mbuf_free_chain(chan->coc_rx.sdu);
    os_mbuf_free_chain(chan->coc_tx.sdu);
}

void
ble_l2cap_coc_le_credits_update(uint16_t conn_handle, uint16_t dcid,
                                uint16_t credits)
{
    struct ble_hs_conn *conn;
    struct ble_l2cap_chan *chan;

    /* remote updated its credits */
    ble_hs_lock();
    conn = ble_hs_conn_find(conn_handle);
    if (!conn) {
        ble_hs_unlock();
        return;
    }

    chan = ble_hs_conn_chan_find_by_dcid(conn, dcid);
    if (!chan) {
        ble_hs_unlock();
        return;
    }

    if (chan->coc_tx.credits + credits > 0xFFFF) {
        BLE_HS_LOG(INFO, "LE CoC credits overflow...disconnecting\n");
        ble_l2cap_sig_disconnect(chan);
        ble_hs_unlock();
        return;
    }

    chan->coc_tx.credits += credits;
    ble_hs_unlock();
}

void
ble_l2cap_coc_recv_ready(struct ble_l2cap_chan *chan, struct os_mbuf *sdu_rx)
{
    struct ble_hs_conn *conn;
    struct ble_l2cap_chan *c;

    chan->coc_rx.sdu = sdu_rx;

    ble_hs_lock();
    conn = ble_hs_conn_find_assert(chan->conn_handle);
    c = ble_hs_conn_chan_find_by_scid(conn, chan->scid);
    if (!c) {
        ble_hs_unlock();
        return;
    }

    /* FIXME 10 is hardcoded - make it better.
     * We want to back only that much credits which remote side is missing
     * to be able to send complete SDU.
     */
    if (chan->coc_rx.credits < 10) {
        ble_l2cap_sig_le_credits(chan, 10 - chan->coc_rx.credits);
        chan->coc_rx.credits = 10;
    }

    ble_hs_unlock();
}

int
ble_l2cap_coc_init(void)
{
    STAILQ_INIT(&ble_l2cap_coc_srvs);

    return os_mempool_init(&ble_l2cap_coc_srv_pool,
                         MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM),
                         sizeof (struct ble_l2cap_coc_srv),
                         ble_l2cap_coc_srv_mem,
                         "ble_l2cap_coc_srv_pool");
}

#endif
