/******************************************************************************
 *
 *  Copyright (C) 1999-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  this file contains SDP discovery functions
 *
 ******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "bt_target.h"
#include "bt_common.h"
#include "l2cdefs.h"
#include "log/log.h"
#include "hcidefs.h"
#include "hcimsgs.h"
#include "sdp_api.h"
#include "sdpint.h"
#include "btu.h"
#include "btm_api.h"


#ifndef SDP_DEBUG_RAW
#define SDP_DEBUG_RAW       FALSE
#endif

/********************************************************************************/
/*              L O C A L    F U N C T I O N     P R O T O T Y P E S            */
/********************************************************************************/
#if SDP_CLIENT_ENABLED == TRUE
static void          process_service_search_rsp (tCONN_CB* p_ccb, uint8_t* p_reply,
                                                 uint8_t* p_reply_end);
static void          process_service_attr_rsp (tCONN_CB* p_ccb, uint8_t* p_reply,
                                               uint8_t* p_reply_end);
static void          process_service_search_attr_rsp (tCONN_CB* p_ccb, uint8_t* p_reply,
                                                      uint8_t* p_reply_end);
static UINT8         *save_attr_seq (tCONN_CB *p_ccb, UINT8 *p, UINT8 *p_msg_end);
static tSDP_DISC_REC *add_record (tSDP_DISCOVERY_DB *p_db, BD_ADDR p_bda);
static UINT8         *add_attr (UINT8 *p, UINT8 *p_end, tSDP_DISCOVERY_DB *p_db, tSDP_DISC_REC *p_rec,
                                UINT16 attr_id, tSDP_DISC_ATTR *p_parent_attr, UINT8 nest_level);

/* Safety check in case we go crazy */
#define MAX_NEST_LEVELS     5

extern fixed_queue_t *btu_general_alarm_queue;

/*******************************************************************************
**
** Function         sdpu_build_uuid_seq
**
** Description      This function builds a UUID sequence from the list of
**                  passed UUIDs. It is also passed the address of the output
**                  buffer.
**
** Returns          Pointer to next byte in the output buffer.
**
*******************************************************************************/
static UINT8 *sdpu_build_uuid_seq (UINT8 *p_out, UINT16 num_uuids, tSDP_UUID *p_uuid_list,
                                   UINT16 bytes_left)
{
    UINT16  xx;
    UINT8   *p_len;

    if (bytes_left < 2) {
      SDP_TRACE_ERROR("SDP: No space for data element header");
      return (p_out);
    }


    /* First thing is the data element header */
    UINT8_TO_BE_STREAM  (p_out, (DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_BYTE);

    /* Remember where the length goes. Leave space for it. */
    p_len = p_out;
    p_out += 1;

    /* Account for data element header and length */
    bytes_left -= 2;

    /* Now, loop through and put in all the UUID(s) */
    for (xx = 0; xx < num_uuids; xx++, p_uuid_list++)
    {
        if (p_uuid_list->len + 1 > bytes_left) {
            SDP_TRACE_ERROR("SDP: Too many UUIDs for internal buffer");
            break;
        } else {
            bytes_left -= (p_uuid_list->len + 1);
        }

        if (p_uuid_list->len == 2)
        {
            UINT8_TO_BE_STREAM  (p_out, (UUID_DESC_TYPE << 3) | SIZE_TWO_BYTES);
            UINT16_TO_BE_STREAM (p_out, p_uuid_list->uu.uuid16);
        }
        else if (p_uuid_list->len == 4)
        {
            UINT8_TO_BE_STREAM  (p_out, (UUID_DESC_TYPE << 3) | SIZE_FOUR_BYTES);
            UINT32_TO_BE_STREAM (p_out, p_uuid_list->uu.uuid32);
        }
        else if (p_uuid_list->len == 16)
        {
            UINT8_TO_BE_STREAM (p_out, (UUID_DESC_TYPE << 3) | SIZE_SIXTEEN_BYTES);
            ARRAY_TO_BE_STREAM (p_out, p_uuid_list->uu.uuid128, p_uuid_list->len);
        }
        else
        {
            SDP_TRACE_ERROR("SDP: Passed Uuid is of Invalid length: %x",p_uuid_list->len);
        }
    }

    /* Now, put in the length */
    xx = (UINT16)(p_out - p_len - 1);
    UINT8_TO_BE_STREAM (p_len, xx);

    return (p_out);
}

/*******************************************************************************
**
** Function         sdp_snd_service_search_req
**
** Description      Send a service search request to the SDP server.
**
** Returns          void
**
*******************************************************************************/
static void sdp_snd_service_search_req(tCONN_CB *p_ccb, UINT8 cont_len, UINT8 * p_cont)
{
    UINT8           *p, *p_start, *p_param_len;
    BT_HDR          *p_cmd = (BT_HDR *) osi_malloc(SDP_DATA_BUF_SIZE);
    UINT16          param_len;
    UINT16          bytes_left = SDP_DATA_BUF_SIZE;

    /* Prepare the buffer for sending the packet to L2CAP */
    p_cmd->offset = L2CAP_MIN_OFFSET;
    p = p_start = (UINT8 *)(p_cmd + 1) + L2CAP_MIN_OFFSET;

    /* Build a service search request packet */
    UINT8_TO_BE_STREAM  (p, SDP_PDU_SERVICE_SEARCH_REQ);
    UINT16_TO_BE_STREAM (p, p_ccb->transaction_id);
    p_ccb->transaction_id++;

    /* Skip the length, we need to add it at the end */
    p_param_len = p;
    p += 2;

    /* Account for header size, max service record count and
     * continuation state */
    const UINT16 base_bytes = (sizeof(BT_HDR) + L2CAP_MIN_OFFSET +
                                 3u + /* service search request header */
                                 2u + /* param len */
                                 3u + ((p_cont) ? cont_len : 0));

    if (base_bytes > bytes_left) {
        SDP_TRACE_ERROR("SDP: Overran SDP data buffer");
        osi_free(p_cmd);
        return;
    }

    bytes_left -= base_bytes;

    /* Build the UID sequence. */
#if (defined(SDP_BROWSE_PLUS) && SDP_BROWSE_PLUS == TRUE)
    p = sdpu_build_uuid_seq (p, 1, &p_ccb->p_db->uuid_filters[p_ccb->cur_uuid_idx],
		             bytes_left);
#else
    /* Build the UID sequence. */
    p = sdpu_build_uuid_seq (p, p_ccb->p_db->num_uuid_filters, p_ccb->p_db->uuid_filters,
		             bytes_left);
#endif

    /* Set max service record count */
    UINT16_TO_BE_STREAM (p, sdp_cb.max_recs_per_search);

    /* Set continuation state */
    UINT8_TO_BE_STREAM (p, cont_len);

    /* if this is not the first request */
    if(cont_len && p_cont)
    {
        memcpy(p, p_cont, cont_len);
        p += cont_len;
    }

    /* Go back and put the parameter length into the buffer */
    param_len = (UINT16)(p - p_param_len - 2);
    UINT16_TO_BE_STREAM (p_param_len, param_len);

    p_ccb->disc_state = SDP_DISC_WAIT_HANDLES;

    /* Set the length of the SDP data in the buffer */
    p_cmd->len = (UINT16)(p - p_start);

#if (SDP_DEBUG_RAW == TRUE)
    SDP_TRACE_WARNING("sdp_snd_service_search_req cont_len :%d disc_state:%d",cont_len, p_ccb->disc_state);
#endif


    L2CA_DataWrite (p_ccb->connection_id, p_cmd);

    /* Start inactivity timer */
    alarm_set_on_queue(p_ccb->sdp_conn_timer, SDP_INACT_TIMEOUT_MS,
                       sdp_conn_timer_timeout, p_ccb, btu_general_alarm_queue);
}

/*******************************************************************************
**
** Function         sdp_disc_connected
**
** Description      This function is called when an SDP discovery attempt is
**                  connected.
**
** Returns          void
**
*******************************************************************************/
void sdp_disc_connected (tCONN_CB *p_ccb)
{
    if (p_ccb->is_attr_search)
    {
        p_ccb->disc_state = SDP_DISC_WAIT_SEARCH_ATTR;

        process_service_search_attr_rsp (p_ccb, NULL, NULL);
    }
    else
    {
        /* First step is to get a list of the handles from the server. */
        /* We are not searching for a specific attribute, so we will   */
        /* first search for the service, then get all attributes of it */

        p_ccb->num_handles = 0;
        sdp_snd_service_search_req(p_ccb, 0, NULL);
    }

}

/*******************************************************************************
**
** Function         sdp_disc_server_rsp
**
** Description      This function is called when there is a response from
**                  the server.
**
** Returns          void
**
*******************************************************************************/
void sdp_disc_server_rsp (tCONN_CB *p_ccb, BT_HDR *p_msg)
{
    UINT8           *p, rsp_pdu;
    BOOLEAN         invalid_pdu = TRUE;

#if (SDP_DEBUG_RAW == TRUE)
    SDP_TRACE_WARNING("sdp_disc_server_rsp disc_state:%d", p_ccb->disc_state);
#endif

    /* stop inactivity timer when we receive a response */
    alarm_cancel(p_ccb->sdp_conn_timer);

    /* Got a reply!! Check what we got back */
    p = (UINT8 *)(p_msg + 1) + p_msg->offset;
    uint8_t* p_end = p + p_msg->len;

    BE_STREAM_TO_UINT8 (rsp_pdu, p);

    p_msg->len--;

    switch (rsp_pdu)
    {
    case SDP_PDU_SERVICE_SEARCH_RSP:
        if (p_ccb->disc_state == SDP_DISC_WAIT_HANDLES)
        {
            process_service_search_rsp (p_ccb, p, p_end);
            invalid_pdu = FALSE;
        }
        break;

    case SDP_PDU_SERVICE_ATTR_RSP:
        if (p_ccb->disc_state == SDP_DISC_WAIT_ATTR)
        {
            process_service_attr_rsp (p_ccb, p, p_end);
            invalid_pdu = FALSE;
        }
        break;

    case SDP_PDU_SERVICE_SEARCH_ATTR_RSP:
        if (p_ccb->disc_state == SDP_DISC_WAIT_SEARCH_ATTR)
        {
            process_service_search_attr_rsp (p_ccb, p, p_end);
            invalid_pdu = FALSE;
        }
        break;
    }

    if (invalid_pdu)
    {
        SDP_TRACE_WARNING ("SDP - Unexp. PDU: %d in state: %d", rsp_pdu, p_ccb->disc_state);
        sdp_disconnect (p_ccb, SDP_GENERIC_ERROR);
    }
}

/******************************************************************************
**
** Function         process_service_search_rsp
**
** Description      This function is called when there is a search response from
**                  the server.
**
** Returns          void
**
*******************************************************************************/
static void process_service_search_rsp (tCONN_CB* p_ccb, uint8_t* p_reply,
                                        uint8_t* p_reply_end)
{
    UINT16      xx;
    UINT16      total, cur_handles, orig;
    UINT8       cont_len;

    if (p_reply + 8 > p_reply_end) {
        android_errorWriteLog(0x534e4554, "74249842");
         sdp_disconnect(p_ccb, SDP_GENERIC_ERROR);
         return;
    }

    /* Skip transaction, and param len */
    p_reply += 4;
    BE_STREAM_TO_UINT16 (total, p_reply);
    BE_STREAM_TO_UINT16 (cur_handles, p_reply);

    orig = p_ccb->num_handles;
    p_ccb->num_handles += cur_handles;
    if (p_ccb->num_handles == 0 || p_ccb->num_handles < orig)
    {
        SDP_TRACE_WARNING ("SDP - Rcvd ServiceSearchRsp, no matches");
        sdp_disconnect (p_ccb, SDP_NO_RECS_MATCH);
        return;
    }

    /* Save the handles that match. We will can only process a certain number. */
    if (total > sdp_cb.max_recs_per_search)
        total = sdp_cb.max_recs_per_search;
    if (p_ccb->num_handles > sdp_cb.max_recs_per_search)
        p_ccb->num_handles = sdp_cb.max_recs_per_search;

    if (p_reply + ((p_ccb->num_handles - orig) * 4) + 1 > p_reply_end) {
        android_errorWriteLog(0x534e4554, "74249842");
        sdp_disconnect(p_ccb, SDP_GENERIC_ERROR);
        return;
    }

    for (xx = orig; xx < p_ccb->num_handles; xx++)
        BE_STREAM_TO_UINT32 (p_ccb->handles[xx], p_reply);

    BE_STREAM_TO_UINT8 (cont_len, p_reply);
    if(cont_len != 0)
    {
        if(cont_len > SDP_MAX_CONTINUATION_LEN)
        {
            sdp_disconnect (p_ccb, SDP_INVALID_CONT_STATE);
            return;
        }
        if (p_reply + cont_len > p_reply_end) {
            android_errorWriteLog(0x534e4554, "68161546");
            sdp_disconnect(p_ccb, SDP_INVALID_CONT_STATE);
            return;
        }
        /* stay in the same state */
        sdp_snd_service_search_req(p_ccb, cont_len, p_reply);
    }
    else
    {
        /* change state */
        p_ccb->disc_state = SDP_DISC_WAIT_ATTR;

        /* Kick off the first attribute request */
        process_service_attr_rsp (p_ccb, NULL, NULL);
    }
}

/*******************************************************************************
**
** Function         sdp_copy_raw_data
**
** Description      copy the raw data
**
**
** Returns          BOOLEAN
**                          true if successful
**                          false if not copied
**
*******************************************************************************/
#if (SDP_RAW_DATA_INCLUDED == TRUE)
static BOOLEAN sdp_copy_raw_data (tCONN_CB *p_ccb, BOOLEAN offset)
{
    unsigned int    cpy_len, rem_len;
    UINT32          list_len;
    UINT8           *p;
    UINT8           *p_end;
    UINT8           type;

#if (SDP_DEBUG_RAW == TRUE)
    UINT8 num_array[SDP_MAX_LIST_BYTE_COUNT];
    UINT32 i;

    for (i = 0; i < p_ccb->list_len; i++)
    {
        sprintf((char *)&num_array[i*2],"%02X",(UINT8)(p_ccb->rsp_list[i]));
    }
    SDP_TRACE_WARNING("result :%s",num_array);
#endif

    if(p_ccb->p_db->raw_data)
    {
        cpy_len = p_ccb->p_db->raw_size - p_ccb->p_db->raw_used;
        list_len = p_ccb->list_len;
        p = &p_ccb->rsp_list[0];
        p_end = &p_ccb->rsp_list[0] + list_len;

        if(offset)
        {
            cpy_len -= 1;
            type = *p++;
            uint8_t* old_p = p;
            p = sdpu_get_len_from_type(p, p_end, type, &list_len);
            if (p == NULL || (p + list_len) > p_end)
            {
                SDP_TRACE_WARNING("%s: bad length", __func__);
                return FALSE;
            }
            if ((int)cpy_len < (p - old_p))
            {
                SDP_TRACE_WARNING("%s: no bytes left for data", __func__);
                return FALSE;
            }
            cpy_len -= (p - old_p);
        }
        if (list_len < cpy_len)
        {
            cpy_len = list_len;
        }
        rem_len = SDP_MAX_LIST_BYTE_COUNT - (unsigned int) (p - &p_ccb->rsp_list[0]);
        if (cpy_len > rem_len)
        {
            SDP_TRACE_WARNING("rem_len :%d less than cpy_len:%d", rem_len, cpy_len);
            cpy_len = rem_len;
        }
        SDP_TRACE_WARNING(
          "%s: list_len:%d cpy_len:%d p:%p p_ccb:%p p_db:%p raw_size:%d "
          "raw_used:%d raw_data:%p",
          __func__, list_len, cpy_len, p, p_ccb, p_ccb->p_db,
          p_ccb->p_db->raw_size, p_ccb->p_db->raw_used, p_ccb->p_db->raw_data);
        memcpy (&p_ccb->p_db->raw_data[p_ccb->p_db->raw_used], p, cpy_len);
        p_ccb->p_db->raw_used += cpy_len;
    }
    return TRUE;
}
#endif

/*******************************************************************************
**
** Function         process_service_attr_rsp
**
** Description      This function is called when there is a attribute response from
**                  the server.
**
** Returns          void
**
*******************************************************************************/
static void process_service_attr_rsp (tCONN_CB* p_ccb, uint8_t* p_reply,
                                      uint8_t* p_reply_end)
{
    UINT8           *p_start, *p_param_len;
    UINT16          param_len, list_byte_count;
    BOOLEAN         cont_request_needed = FALSE;

#if (SDP_DEBUG_RAW == TRUE)
    SDP_TRACE_WARNING("process_service_attr_rsp raw inc:%d",
        SDP_RAW_DATA_INCLUDED);
#endif
    /* If p_reply is NULL, we were called after the records handles were read */
    if (p_reply)
    {
#if (SDP_DEBUG_RAW == TRUE)
        SDP_TRACE_WARNING("ID & len: 0x%02x-%02x-%02x-%02x",
            p_reply[0], p_reply[1], p_reply[2], p_reply[3]);
#endif
        /* Skip transaction ID and length */
        p_reply += 4;

        BE_STREAM_TO_UINT16 (list_byte_count, p_reply);
#if (SDP_DEBUG_RAW == TRUE)
        SDP_TRACE_WARNING("list_byte_count:%d", list_byte_count);
#endif

        /* Copy the response to the scratchpad. First, a safety check on the length */
        if ((p_ccb->list_len + list_byte_count) > SDP_MAX_LIST_BYTE_COUNT)
        {
            sdp_disconnect (p_ccb, SDP_INVALID_PDU_SIZE);
            return;
        }

#if (SDP_DEBUG_RAW == TRUE)
        SDP_TRACE_WARNING("list_len: %d, list_byte_count: %d",
            p_ccb->list_len, list_byte_count);
#endif
        if (p_ccb->rsp_list == NULL)
            p_ccb->rsp_list = (UINT8 *)osi_malloc(SDP_MAX_LIST_BYTE_COUNT);
        memcpy(&p_ccb->rsp_list[p_ccb->list_len], p_reply, list_byte_count);
        p_ccb->list_len += list_byte_count;
        p_reply         += list_byte_count;
#if (SDP_DEBUG_RAW == TRUE)
        SDP_TRACE_WARNING("list_len: %d(attr_rsp)", p_ccb->list_len);

        /* Check if we need to request a continuation */
        SDP_TRACE_WARNING("*p_reply:%d(%d)", *p_reply, SDP_MAX_CONTINUATION_LEN);
#endif
        if (*p_reply)
        {
            if (*p_reply > SDP_MAX_CONTINUATION_LEN)
            {
                sdp_disconnect (p_ccb, SDP_INVALID_CONT_STATE);
                return;
            }
            cont_request_needed = TRUE;
        }
        else
        {

#if (SDP_RAW_DATA_INCLUDED == TRUE)
            SDP_TRACE_WARNING("process_service_attr_rsp");
            if (!sdp_copy_raw_data(p_ccb, FALSE)) {
                SDP_TRACE_ERROR("sdp_copy_raw_data failed");
                sdp_disconnect(p_ccb, SDP_ILLEGAL_PARAMETER);
                return;
            }
#endif

            /* Save the response in the database. Stop on any error */
            if (!save_attr_seq (p_ccb, &p_ccb->rsp_list[0], &p_ccb->rsp_list[p_ccb->list_len]))
            {
                sdp_disconnect (p_ccb, SDP_DB_FULL);
                return;
            }
            p_ccb->list_len = 0;
            p_ccb->cur_handle++;
        }
    }

    /* Now, ask for the next handle. Re-use the buffer we just got. */
    if (p_ccb->cur_handle < p_ccb->num_handles)
    {
        BT_HDR  *p_msg = (BT_HDR *)osi_malloc(SDP_DATA_BUF_SIZE);
        UINT8   *p;

        p_msg->offset = L2CAP_MIN_OFFSET;
        p = p_start = (UINT8 *)(p_msg + 1) + L2CAP_MIN_OFFSET;

        /* Get all the attributes from the server */
        UINT8_TO_BE_STREAM  (p, SDP_PDU_SERVICE_ATTR_REQ);
        UINT16_TO_BE_STREAM (p, p_ccb->transaction_id);
        p_ccb->transaction_id++;

        /* Skip the length, we need to add it at the end */
        p_param_len = p;
        p += 2;

        UINT32_TO_BE_STREAM (p, p_ccb->handles[p_ccb->cur_handle]);

        /* Max attribute byte count */
        UINT16_TO_BE_STREAM (p, sdp_cb.max_attr_list_size);

        /* If no attribute filters, build a wildcard attribute sequence */
        if (p_ccb->p_db->num_attr_filters)
            p = sdpu_build_attrib_seq (p, p_ccb->p_db->attr_filters, p_ccb->p_db->num_attr_filters);
        else
            p = sdpu_build_attrib_seq (p, NULL, 0);

        /* Was this a continuation request ? */
        if (cont_request_needed)
        {
            if ((p_reply + *p_reply + 1) <= p_reply_end) {
                memcpy(p, p_reply, *p_reply + 1);
                p += *p_reply + 1;
            } else {
                android_errorWriteLog(0x534e4554, "68161546");
            }
        }
        else
            UINT8_TO_BE_STREAM (p, 0);

        /* Go back and put the parameter length into the buffer */
        param_len = (UINT16)(p - p_param_len - 2);
        UINT16_TO_BE_STREAM (p_param_len, param_len);

        /* Set the length of the SDP data in the buffer */
        p_msg->len = (UINT16)(p - p_start);


        L2CA_DataWrite (p_ccb->connection_id, p_msg);

        /* Start inactivity timer */
        alarm_set_on_queue(p_ccb->sdp_conn_timer, SDP_INACT_TIMEOUT_MS,
                           sdp_conn_timer_timeout, p_ccb,
                           btu_general_alarm_queue);
    }
    else
    {
        sdp_disconnect (p_ccb, SDP_SUCCESS);
        return;
    }
}


/*******************************************************************************
**
** Function         process_service_search_attr_rsp
**
** Description      This function is called when there is a search attribute
**                  response from the server.
**
** Returns          void
**
*******************************************************************************/
static void process_service_search_attr_rsp (tCONN_CB* p_ccb, uint8_t* p_reply,
                                             uint8_t* p_reply_end)
{
    UINT8           *p, *p_start, *p_end, *p_param_len;
    UINT8           type;
    UINT32          seq_len;
    UINT16          param_len, lists_byte_count = 0;
    BOOLEAN         cont_request_needed = FALSE;

#if (SDP_DEBUG_RAW == TRUE)
    SDP_TRACE_WARNING("process_service_search_attr_rsp");
#endif
    /* If p_reply is NULL, we were called for the initial read */
    if (p_reply)
    {
        if (p_reply + 4 /* transaction ID and length */ + sizeof(lists_byte_count) >
            p_reply_end)
        {
            android_errorWriteLog(0x534e4554, "79884292");
            sdp_disconnect(p_ccb, SDP_INVALID_PDU_SIZE);
            return;
        }

#if (SDP_DEBUG_RAW == TRUE)
        SDP_TRACE_WARNING("ID & len: 0x%02x-%02x-%02x-%02x",
            p_reply[0], p_reply[1], p_reply[2], p_reply[3]);
#endif
        /* Skip transaction ID and length */
        p_reply += 4;

        BE_STREAM_TO_UINT16 (lists_byte_count, p_reply);
#if (SDP_DEBUG_RAW == TRUE)
        SDP_TRACE_WARNING("lists_byte_count:%d", lists_byte_count);
#endif

        /* Copy the response to the scratchpad. First, a safety check on the length */
        if ((p_ccb->list_len + lists_byte_count) > SDP_MAX_LIST_BYTE_COUNT)
        {
            sdp_disconnect (p_ccb, SDP_INVALID_PDU_SIZE);
            return;
        }

#if (SDP_DEBUG_RAW == TRUE)
        SDP_TRACE_WARNING("list_len: %d, list_byte_count: %d",
            p_ccb->list_len, lists_byte_count);
#endif

        if (p_reply + lists_byte_count + 1 /* continuation */ > p_reply_end)
        {
            android_errorWriteLog(0x534e4554, "79884292");
            sdp_disconnect(p_ccb, SDP_INVALID_PDU_SIZE);
            return;
        }

        if (p_ccb->rsp_list == NULL)
            p_ccb->rsp_list = (UINT8 *)osi_malloc(SDP_MAX_LIST_BYTE_COUNT);
        memcpy (&p_ccb->rsp_list[p_ccb->list_len], p_reply, lists_byte_count);
        p_ccb->list_len += lists_byte_count;
        p_reply         += lists_byte_count;
#if (SDP_DEBUG_RAW == TRUE)
        SDP_TRACE_WARNING("list_len: %d(search_attr_rsp)", p_ccb->list_len);

        /* Check if we need to request a continuation */
        SDP_TRACE_WARNING("*p_reply:%d(%d)", *p_reply, SDP_MAX_CONTINUATION_LEN);
#endif
        if (*p_reply)
        {
            if (*p_reply > SDP_MAX_CONTINUATION_LEN)
            {
                sdp_disconnect (p_ccb, SDP_INVALID_CONT_STATE);
                return;
            }

            cont_request_needed = TRUE;
        }
    }

#if (SDP_DEBUG_RAW == TRUE)
    SDP_TRACE_WARNING("cont_request_needed:%d", cont_request_needed);
#endif
    /* If continuation request (or first time request) */
    if ((cont_request_needed) || (!p_reply))
    {
        BT_HDR  *p_msg = (BT_HDR *)osi_malloc(SDP_DATA_BUF_SIZE);
        UINT8   *p;
        UINT16  bytes_left = SDP_DATA_BUF_SIZE;

        p_msg->offset = L2CAP_MIN_OFFSET;
        p = p_start = (UINT8 *)(p_msg + 1) + L2CAP_MIN_OFFSET;

        /* Build a service search request packet */
        UINT8_TO_BE_STREAM  (p, SDP_PDU_SERVICE_SEARCH_ATTR_REQ);
        UINT16_TO_BE_STREAM (p, p_ccb->transaction_id);
        p_ccb->transaction_id++;

        /* Skip the length, we need to add it at the end */
        p_param_len = p;
        p += 2;

        /* Account for header size, max service record count and
         * continuation state */
        const UINT16 base_bytes = (sizeof(BT_HDR) + L2CAP_MIN_OFFSET +
                                     3u + /* service search request header */
                                     2u + /* param len */
                                     3u + /* max service record count */
                                     ((p_reply) ? (*p_reply) : 0));

        if (base_bytes > bytes_left) {
            sdp_disconnect(p_ccb, SDP_INVALID_CONT_STATE);
            return;
        }

        bytes_left -= base_bytes;

        /* Build the UID sequence. */
#if (defined(SDP_BROWSE_PLUS) && SDP_BROWSE_PLUS == TRUE)
        p = sdpu_build_uuid_seq (p, 1, &p_ccb->p_db->uuid_filters[p_ccb->cur_uuid_idx],
                                 bytes_left);
#else
        p = sdpu_build_uuid_seq (p, p_ccb->p_db->num_uuid_filters, p_ccb->p_db->uuid_filters,
                                 bytes_left);
#endif

        /* Max attribute byte count */
        UINT16_TO_BE_STREAM (p, sdp_cb.max_attr_list_size);

        /* If no attribute filters, build a wildcard attribute sequence */
        if (p_ccb->p_db->num_attr_filters)
            p = sdpu_build_attrib_seq (p, p_ccb->p_db->attr_filters, p_ccb->p_db->num_attr_filters);
        else
            p = sdpu_build_attrib_seq (p, NULL, 0);

        /* No continuation for first request */
        if (p_reply)
        {
            if ((p_reply + *p_reply + 1) <= p_reply_end) {
                memcpy(p, p_reply, *p_reply + 1);
                p += *p_reply + 1;
            } else {
                android_errorWriteLog(0x534e4554, "68161546");
            }
        }
        else
            UINT8_TO_BE_STREAM (p, 0);

        /* Go back and put the parameter length into the buffer */
        param_len = p - p_param_len - 2;
        UINT16_TO_BE_STREAM (p_param_len, param_len);

        /* Set the length of the SDP data in the buffer */
        p_msg->len = p - p_start;


        L2CA_DataWrite (p_ccb->connection_id, p_msg);

        /* Start inactivity timer */
        alarm_set_on_queue(p_ccb->sdp_conn_timer, SDP_INACT_TIMEOUT_MS,
                           sdp_conn_timer_timeout, p_ccb,
                           btu_general_alarm_queue);

        return;
    }


    /*******************************************************************/
    /* We now have the full response, which is a sequence of sequences */
    /*******************************************************************/

#if (SDP_RAW_DATA_INCLUDED == TRUE)
    SDP_TRACE_WARNING("process_service_search_attr_rsp");
    if (!sdp_copy_raw_data (p_ccb, TRUE)) {
        SDP_TRACE_ERROR("sdp_copy_raw_data failed");
        sdp_disconnect(p_ccb, SDP_ILLEGAL_PARAMETER);
        return;
    }
#endif

    p = &p_ccb->rsp_list[0];

    /* The contents is a sequence of attribute sequences */
    type = *p++;

    if ((type >> 3) != DATA_ELE_SEQ_DESC_TYPE)
    {
        SDP_TRACE_WARNING ("SDP - Wrong type: 0x%02x in attr_rsp", type);
        return;
    }
    p = sdpu_get_len_from_type(p, p + p_ccb->list_len, type, &seq_len);
    if (p == NULL || (p + seq_len) > (p + p_ccb->list_len))
    {
        SDP_TRACE_WARNING("%s: bad length", __func__);
        sdp_disconnect(p_ccb, SDP_ILLEGAL_PARAMETER);
        return;
    }
    p_end = &p_ccb->rsp_list[p_ccb->list_len];

    if ((p + seq_len) != p_end)
    {
        sdp_disconnect (p_ccb, SDP_INVALID_CONT_STATE);
        return;
    }

    while (p < p_end)
    {
        p = save_attr_seq (p_ccb, p, &p_ccb->rsp_list[p_ccb->list_len]);
        if (!p)
        {
            sdp_disconnect (p_ccb, SDP_DB_FULL);
            return;
        }
    }

    /* Since we got everything we need, disconnect the call */
    sdp_disconnect (p_ccb, SDP_SUCCESS);
}

/*******************************************************************************
**
** Function         save_attr_seq
**
** Description      This function is called when there is a response from
**                  the server.
**
** Returns          pointer to next byte or NULL if error
**
*******************************************************************************/
static UINT8 *save_attr_seq (tCONN_CB *p_ccb, UINT8 *p, UINT8 *p_msg_end)
{
    UINT32      seq_len, attr_len;
    UINT16      attr_id;
    UINT8       type, *p_seq_end;
    tSDP_DISC_REC *p_rec;

    type = *p++;

    if ((type >> 3) != DATA_ELE_SEQ_DESC_TYPE)
    {
        SDP_TRACE_WARNING ("SDP - Wrong type: 0x%02x in attr_rsp", type);
        return (NULL);
    }
    p = sdpu_get_len_from_type(p, p_msg_end, type, &seq_len);
    if (p == NULL || (p + seq_len) > p_msg_end)
    {
        SDP_TRACE_WARNING ("SDP - Bad len in attr_rsp %d", seq_len);
        return (NULL);
    }

    /* Create a record */
    p_rec = add_record (p_ccb->p_db, p_ccb->device_address);
    if (!p_rec)
    {
        SDP_TRACE_WARNING ("SDP - DB full add_record");
        return (NULL);
    }

    p_seq_end = p + seq_len;

    while (p < p_seq_end)
    {
        /* First get the attribute ID */
        type = *p++;
        p = sdpu_get_len_from_type(p, p_msg_end, type, &attr_len);
        if (p == NULL || (p + attr_len) > p_seq_end)
        {
            SDP_TRACE_WARNING("%s: Bad len in attr_rsp %d", __func__, attr_len);
            return (NULL);
        }
        if (((type >> 3) != UINT_DESC_TYPE) || (attr_len != 2))
        {
            SDP_TRACE_WARNING ("SDP - Bad type: 0x%02x or len: %d in attr_rsp", type, attr_len);
            return (NULL);
        }
        BE_STREAM_TO_UINT16 (attr_id, p);

        /* Now, add the attribute value */
        p = add_attr (p, p_seq_end, p_ccb->p_db, p_rec, attr_id, NULL, 0);

        if (!p)
        {
            SDP_TRACE_WARNING ("SDP - DB full add_attr");
            return (NULL);
        }
    }

    return (p);
}


/*******************************************************************************
**
** Function         add_record
**
** Description      This function allocates space for a record from the DB.
**
** Returns          pointer to next byte in data stream
**
*******************************************************************************/
tSDP_DISC_REC *add_record (tSDP_DISCOVERY_DB *p_db, BD_ADDR p_bda)
{
    tSDP_DISC_REC   *p_rec;

    /* See if there is enough space in the database */
    if (p_db->mem_free < sizeof (tSDP_DISC_REC))
        return (NULL);

    p_rec = (tSDP_DISC_REC *) p_db->p_free_mem;
    p_db->p_free_mem += sizeof (tSDP_DISC_REC);
    p_db->mem_free   -= sizeof (tSDP_DISC_REC);

    p_rec->p_first_attr = NULL;
    p_rec->p_next_rec   = NULL;

    memcpy (p_rec->remote_bd_addr, p_bda, BD_ADDR_LEN);

    /* Add the record to the end of chain */
    if (!p_db->p_first_rec)
        p_db->p_first_rec = p_rec;
    else
    {
        tSDP_DISC_REC   *p_rec1 = p_db->p_first_rec;

        while (p_rec1->p_next_rec)
            p_rec1 = p_rec1->p_next_rec;

        p_rec1->p_next_rec = p_rec;
    }

    return (p_rec);
}

#define SDP_ADDITIONAL_LIST_MASK        0x80
/*******************************************************************************
**
** Function         add_attr
**
** Description      This function allocates space for an attribute from the DB
**                  and copies the data into it.
**
** Returns          pointer to next byte in data stream
**
*******************************************************************************/
static UINT8 *add_attr (UINT8 *p, UINT8 *p_end, tSDP_DISCOVERY_DB *p_db, tSDP_DISC_REC *p_rec,
                        UINT16 attr_id, tSDP_DISC_ATTR *p_parent_attr, UINT8 nest_level)
{
    tSDP_DISC_ATTR  *p_attr;
    UINT32          attr_len;
    UINT32          total_len;
    UINT16          attr_type;
    UINT16          id;
    UINT8           type;
    UINT8           *p_attr_end;
    UINT8           is_additional_list = nest_level & SDP_ADDITIONAL_LIST_MASK;

    nest_level &= ~(SDP_ADDITIONAL_LIST_MASK);

    type = *p++;
    p = sdpu_get_len_from_type(p, p_end, type, &attr_len);
    if (p == NULL || (p + attr_len) > p_end)
    {
        SDP_TRACE_WARNING("%s: bad length in attr_rsp", __func__);
        return NULL;
    }
    attr_len &= SDP_DISC_ATTR_LEN_MASK;
    attr_type = (type >> 3) & 0x0f;

    /* See if there is enough space in the database */
    if (attr_len > 4)
        total_len = attr_len - 4 + (UINT16)sizeof (tSDP_DISC_ATTR);
    else
        total_len = sizeof (tSDP_DISC_ATTR);

    p_attr_end = p + attr_len;
    if (p_attr_end > p_end)
    {
        android_errorWriteLog(0x534e4554, "115900043");
        SDP_TRACE_WARNING("%s: SDP - Attribute length beyond p_end", __func__);
        return NULL;
    }

    /* Ensure it is a multiple of 4 */
    total_len = (total_len + 3) & ~3;

    /* See if there is enough space in the database */
    if (p_db->mem_free < total_len)
        return (NULL);

    p_attr                = (tSDP_DISC_ATTR *) p_db->p_free_mem;
    p_attr->attr_id       = attr_id;
    p_attr->attr_len_type = (UINT16)attr_len | (attr_type << 12);
    p_attr->p_next_attr = NULL;

    /* Store the attribute value */
    switch (attr_type)
    {
    case UINT_DESC_TYPE:
        if( (is_additional_list != 0) && (attr_len == 2) )
        {
            BE_STREAM_TO_UINT16 (id, p);
            if(id != ATTR_ID_PROTOCOL_DESC_LIST)
                p -= 2;
            else
            {
                /* Reserve the memory for the attribute now, as we need to add sub-attributes */
                p_db->p_free_mem += sizeof (tSDP_DISC_ATTR);
                p_db->mem_free   -= sizeof (tSDP_DISC_ATTR);
                total_len         = 0;

                /* SDP_TRACE_DEBUG ("SDP - attr nest level:%d(list)", nest_level); */
                if (nest_level >= MAX_NEST_LEVELS)
                {
                    SDP_TRACE_ERROR ("SDP - attr nesting too deep");
                    return p_attr_end;
                }

                /* Now, add the list entry */
                p = add_attr (p, p_end, p_db, p_rec, ATTR_ID_PROTOCOL_DESC_LIST,
                              p_attr, (UINT8)(nest_level + 1));

                break;
            }
        }
        /* Case falls through */

    case TWO_COMP_INT_DESC_TYPE:
        switch (attr_len)
        {
        case 1:
            p_attr->attr_value.v.u8 = *p++;
            break;
        case 2:
            BE_STREAM_TO_UINT16 (p_attr->attr_value.v.u16, p);
            break;
        case 4:
            BE_STREAM_TO_UINT32 (p_attr->attr_value.v.u32, p);
            break;
        default:
            BE_STREAM_TO_ARRAY (p, p_attr->attr_value.v.array, (INT32)attr_len);
            break;
        }
        break;

    case UUID_DESC_TYPE:
        switch (attr_len)
        {
        case 2:
            BE_STREAM_TO_UINT16 (p_attr->attr_value.v.u16, p);
            break;
        case 4:
            BE_STREAM_TO_UINT32 (p_attr->attr_value.v.u32, p);
            if (p_attr->attr_value.v.u32 < 0x10000)
            {
                attr_len = 2;
                p_attr->attr_len_type = (UINT16)attr_len | (attr_type << 12);
                p_attr->attr_value.v.u16 = (UINT16) p_attr->attr_value.v.u32;

            }
            break;
        case 16:
            /* See if we can compress his UUID down to 16 or 32bit UUIDs */
            if (sdpu_is_base_uuid (p))
            {
                if ((p[0] == 0) && (p[1] == 0))
                {
                    p_attr->attr_len_type = (p_attr->attr_len_type & ~SDP_DISC_ATTR_LEN_MASK) | 2;
                    p += 2;
                    BE_STREAM_TO_UINT16 (p_attr->attr_value.v.u16, p);
                    p += MAX_UUID_SIZE - 4;
                }
                else
                {
                    p_attr->attr_len_type = (p_attr->attr_len_type & ~SDP_DISC_ATTR_LEN_MASK) | 4;
                    BE_STREAM_TO_UINT32 (p_attr->attr_value.v.u32, p);
                    p += MAX_UUID_SIZE - 4;
                }
            }
            else
            {
                 /* coverity[overrun-local] */
                 /*
                    Event overrun-local: Overrun of static array "p_attr->attr_value.v.array" of size 4 at position 15 with index variable "ijk"
                    False-positive: SDP uses scratch buffer to hold the attribute value.
                    The actual size of tSDP_DISC_ATVAL does not matter.
                    If the array size in tSDP_DISC_ATVAL is increase, we would increase the system RAM usage unnecessarily
                */
                BE_STREAM_TO_ARRAY (p, p_attr->attr_value.v.array, (INT32)attr_len);
            }
            break;
        default:
            SDP_TRACE_WARNING ("SDP - bad len in UUID attr: %d", attr_len);
            return p_attr_end;
        }
        break;

    case DATA_ELE_SEQ_DESC_TYPE:
    case DATA_ELE_ALT_DESC_TYPE:
        /* Reserve the memory for the attribute now, as we need to add sub-attributes */
        p_db->p_free_mem += sizeof (tSDP_DISC_ATTR);
        p_db->mem_free   -= sizeof (tSDP_DISC_ATTR);
        total_len         = 0;

        /* SDP_TRACE_DEBUG ("SDP - attr nest level:%d", nest_level); */
        if (nest_level >= MAX_NEST_LEVELS)
        {
            SDP_TRACE_ERROR ("SDP - attr nesting too deep");
            return p_attr_end;
        }
        if(is_additional_list != 0 || attr_id == ATTR_ID_ADDITION_PROTO_DESC_LISTS)
            nest_level |= SDP_ADDITIONAL_LIST_MASK;
        /* SDP_TRACE_DEBUG ("SDP - attr nest level:0x%x(finish)", nest_level); */

        while (p < p_attr_end)
        {
            /* Now, add the list entry */
            p = add_attr (p, p_end, p_db, p_rec, 0, p_attr, (UINT8)(nest_level + 1));

            if (!p)
                return (NULL);
        }
        break;

    case TEXT_STR_DESC_TYPE:
    case URL_DESC_TYPE:
        BE_STREAM_TO_ARRAY (p, p_attr->attr_value.v.array, (INT32)attr_len);
        break;

    case BOOLEAN_DESC_TYPE:
        switch (attr_len)
        {
        case 1:
            p_attr->attr_value.v.u8 = *p++;
            break;
        default:
            SDP_TRACE_WARNING ("SDP - bad len in boolean attr: %d", attr_len);
            return p_attr_end;
        }
        break;

    default:    /* switch (attr_type) */
        break;
    }

    p_db->p_free_mem += total_len;
    p_db->mem_free   -= total_len;

    /* Add the attribute to the end of the chain */
    if (!p_parent_attr)
    {
        if (!p_rec->p_first_attr)
            p_rec->p_first_attr = p_attr;
        else
        {
            tSDP_DISC_ATTR  *p_attr1 = p_rec->p_first_attr;

            while (p_attr1->p_next_attr)
                p_attr1 = p_attr1->p_next_attr;

            p_attr1->p_next_attr = p_attr;
        }
    }
    else
    {
        if (!p_parent_attr->attr_value.v.p_sub_attr)
        {
            p_parent_attr->attr_value.v.p_sub_attr = p_attr;
            /* SDP_TRACE_DEBUG ("parent:0x%x(id:%d), ch:0x%x(id:%d)",
                p_parent_attr, p_parent_attr->attr_id, p_attr, p_attr->attr_id); */
        }
        else
        {
            tSDP_DISC_ATTR  *p_attr1 = p_parent_attr->attr_value.v.p_sub_attr;
            /* SDP_TRACE_DEBUG ("parent:0x%x(id:%d), ch1:0x%x(id:%d)",
                p_parent_attr, p_parent_attr->attr_id, p_attr1, p_attr1->attr_id); */

            while (p_attr1->p_next_attr)
                p_attr1 = p_attr1->p_next_attr;

            p_attr1->p_next_attr = p_attr;
            /* SDP_TRACE_DEBUG ("new ch:0x%x(id:%d)", p_attr, p_attr->attr_id); */
        }
    }

    return (p);
}

#endif  /* CLIENT_ENABLED == TRUE */
