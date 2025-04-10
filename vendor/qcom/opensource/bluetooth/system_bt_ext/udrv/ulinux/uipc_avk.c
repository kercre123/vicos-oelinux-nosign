/******************************************************************************
 *
 *  Copyright (C) 2009-2012 Broadcom Corporation
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

/*****************************************************************************
 *
 *  Filename:      uipc.c
 *
 *  Description:   UIPC implementation for bluedroid
 *
 *****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "audio_a2dp_hw.h"
#include "bt_types.h"
#include "bt_utils.h"
#include "bt_common.h"
#include "osi/include/osi.h"
#include "osi/include/socket_utils/sockets.h"
#include "uipc_avk.h"
/*****************************************************************************
**  Constants & Macros
******************************************************************************/

#define PCM_FILENAME "/data/test.pcm"

#define MAX(a,b) ((a)>(b)?(a):(b))

#define CASE_RETURN_STR(const) case const: return #const;

#define UIPC_DISCONNECTED (-1)

#define UIPC_LOCK() /*BTIF_TRACE_EVENT(" %s lock", __FUNCTION__);*/ pthread_mutex_lock(&uipc_avk_main.mutex);
#define UIPC_UNLOCK() /*BTIF_TRACE_EVENT("%s unlock", __FUNCTION__);*/ pthread_mutex_unlock(&uipc_avk_main.mutex);

#define SAFE_FD_ISSET(fd, set) (((fd) == -1) ? FALSE : FD_ISSET((fd), (set)))

#define UIPC_FLUSH_BUFFER_SIZE 1024

/*****************************************************************************
**  Local type definitions
******************************************************************************/

typedef enum {
    UIPC_TASK_FLAG_DISCONNECT_CHAN = 0x1,
} tUIPC_TASK_FLAGS;

typedef struct {
    int srvfd;
    int fd;
    int read_poll_tmo_ms;
    int task_evt_flags;   /* event flags pending to be processed in read task */
    tUIPC_EVENT cond_flags;
    pthread_mutex_t cond_mutex;
    pthread_cond_t  cond;
    tUIPC_AVK_RCV_CBACK *cback;
} tUIPC_CHAN;

typedef struct {
    pthread_t tid; /* main thread id */
    int running;
    pthread_mutex_t mutex;

    fd_set active_set;
    fd_set read_set;
    int max_fd;
    int signal_fds[2];

    tUIPC_CHAN ch[UIPC_CH_NUM];
} tUIPC_MAIN;


/*****************************************************************************
**  Static variables
******************************************************************************/

static tUIPC_MAIN uipc_avk_main;


/*****************************************************************************
**  Static functions
******************************************************************************/

static int uipc_avk_close_ch_locked(tUIPC_CH_ID ch_id);

/*****************************************************************************
**  Externs
******************************************************************************/


/*****************************************************************************
**   Helper functions
******************************************************************************/


const char* dump_avk_uipc_event(tUIPC_EVENT event)
{
    switch(event)
    {
        CASE_RETURN_STR(UIPC_AVK_Open_EVT)
        CASE_RETURN_STR(UIPC_AVK_Close_EVT)
        CASE_RETURN_STR(UIPC_AVK_RX_DATA_EVT)
        CASE_RETURN_STR(UIPC_AVK_RX_DATA_READY_EVT)
        CASE_RETURN_STR(UIPC_AVK_TX_DATA_READY_EVT)
        default:
            return "UNKNOWN MSG ID";
    }
}

/*****************************************************************************
**   socket helper functions
*****************************************************************************/

static inline int create_server_socket(const char* name)
{
    int s = socket(AF_LOCAL, SOCK_STREAM, 0);
#ifndef ANDROID
    struct sockaddr_un addr;
#endif

    if (s < 0)
        return -1;

    BTIF_TRACE_EVENT("create_server_socket %s", name);

#ifdef ANDROID
    if(socket_local_server_bind(s, name, ANDROID_SOCKET_NAMESPACE_ABSTRACT) < 0)
#else
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_LOCAL;
    strncpy(addr.sun_path, name, sizeof(addr.sun_path)-1);
    unlink(name);

    if(bind(s, (struct sockaddr*)&addr, sizeof(addr)) == -1)
#endif
    {
        BTIF_TRACE_EVENT("socket failed to create (%s)", strerror(errno));
        close(s);
        return -1;
    }

    if(listen(s, 5) < 0)
    {
        BTIF_TRACE_EVENT("listen failed", strerror(errno));
        close(s);
        return -1;
    }

    BTIF_TRACE_EVENT("created socket fd %d", s);
    return s;
}

static int accept_server_socket(int sfd)
{
    struct sockaddr_un remote;
    struct pollfd pfd;
    int fd;
    socklen_t len = sizeof(struct sockaddr_un);

    BTIF_TRACE_EVENT("accept fd %d", sfd);

    /* make sure there is data to process */
    pfd.fd = sfd;
    pfd.events = POLLIN;

    if (TEMP_FAILURE_RETRY(poll(&pfd, 1, 0)) == 0)
    {
        BTIF_TRACE_EVENT("accept poll timeout");
        return -1;
    }

    //BTIF_TRACE_EVENT("poll revents 0x%x", pfd.revents);

    if ((fd = TEMP_FAILURE_RETRY(accept(sfd, (struct sockaddr *)&remote, &len))) == -1)
    {
         BTIF_TRACE_ERROR("sock accept failed (%s)", strerror(errno));
         return -1;
    }

    //BTIF_TRACE_EVENT("new fd %d", fd);

    return fd;
}

/*****************************************************************************
**
**   uipc helper functions
**
*****************************************************************************/

static int uipc_main_init(void)
{
    int i;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&uipc_avk_main.mutex, &attr);

    BTIF_TRACE_EVENT("### uipc_main_init ###");

    /* setup interrupt socket pair */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, uipc_avk_main.signal_fds) < 0)
    {
        return -1;
    }

    FD_SET(uipc_avk_main.signal_fds[0], &uipc_avk_main.active_set);
    uipc_avk_main.max_fd = MAX(uipc_avk_main.max_fd, uipc_avk_main.signal_fds[0]);

    for (i=0; i< UIPC_CH_NUM; i++)
    {
        tUIPC_CHAN *p = &uipc_avk_main.ch[i];
        p->srvfd = UIPC_DISCONNECTED;
        p->fd = UIPC_DISCONNECTED;
        p->task_evt_flags = 0;
        pthread_cond_init(&p->cond, NULL);
        pthread_mutex_init(&p->cond_mutex, NULL);
        p->cback = NULL;
    }

    return 0;
}

void uipc_avk_main_cleanup(void)
{
    int i;

    BTIF_TRACE_EVENT("uipc_avk_main_cleanup");

    close(uipc_avk_main.signal_fds[0]);
    close(uipc_avk_main.signal_fds[1]);

    /* close any open channels */
    for (i=0; i<UIPC_CH_NUM; i++)
        uipc_avk_close_ch_locked(i);
}



/* check pending events in read task */
static void uipc_check_task_flags_locked(void)
{
    int i;

    for (i=0; i<UIPC_CH_NUM; i++)
    {
        //BTIF_TRACE_EVENT("CHECK TASK FLAGS %x %x",  uipc_avk_main.ch[i].task_evt_flags, UIPC_TASK_FLAG_DISCONNECT_CHAN);
        if (uipc_avk_main.ch[i].task_evt_flags & UIPC_TASK_FLAG_DISCONNECT_CHAN)
        {
            uipc_avk_main.ch[i].task_evt_flags &= ~UIPC_TASK_FLAG_DISCONNECT_CHAN;
            uipc_avk_close_ch_locked(i);
        }

        /* add here */

    }
}


static int uipc_check_fd_locked(tUIPC_CH_ID ch_id)
{
    if (ch_id >= UIPC_CH_NUM)
        return -1;

    //BTIF_TRACE_EVENT("CHECK SRVFD %d (ch %d)", uipc_avk_main.ch[ch_id].srvfd, ch_id);

    if (SAFE_FD_ISSET(uipc_avk_main.ch[ch_id].srvfd, &uipc_avk_main.read_set))
    {
        BTIF_TRACE_EVENT("INCOMING CONNECTION ON CH %d", ch_id);

        uipc_avk_main.ch[ch_id].fd = accept_server_socket(uipc_avk_main.ch[ch_id].srvfd);

        BTIF_TRACE_EVENT("NEW FD %d", uipc_avk_main.ch[ch_id].fd);

        if ((uipc_avk_main.ch[ch_id].fd > 0) && uipc_avk_main.ch[ch_id].cback)
        {
            /*  if we have a callback we should add this fd to the active set
                and notify user with callback event */
            BTIF_TRACE_EVENT("ADD FD %d TO ACTIVE SET", uipc_avk_main.ch[ch_id].fd);
            FD_SET(uipc_avk_main.ch[ch_id].fd, &uipc_avk_main.active_set);
            uipc_avk_main.max_fd = MAX(uipc_avk_main.max_fd, uipc_avk_main.ch[ch_id].fd);
        }

        if (uipc_avk_main.ch[ch_id].fd < 0)
        {
            BTIF_TRACE_ERROR("FAILED TO ACCEPT CH %d (%s)", ch_id, strerror(errno));
            return -1;
        }

        if (uipc_avk_main.ch[ch_id].cback)
            uipc_avk_main.ch[ch_id].cback(ch_id, UIPC_AVK_Open_EVT);
    }

    //BTIF_TRACE_EVENT("CHECK FD %d (ch %d)", uipc_avk_main.ch[ch_id].fd, ch_id);

    if (SAFE_FD_ISSET(uipc_avk_main.ch[ch_id].fd, &uipc_avk_main.read_set))
    {
        //BTIF_TRACE_EVENT("INCOMING DATA ON CH %d", ch_id);

        if (uipc_avk_main.ch[ch_id].cback)
            uipc_avk_main.ch[ch_id].cback(ch_id, UIPC_AVK_RX_DATA_READY_EVT);
    }
    return 0;
}

static void uipc_check_interrupt_locked(void)
{
    if (SAFE_FD_ISSET(uipc_avk_main.signal_fds[0], &uipc_avk_main.read_set))
    {
        char sig_recv = 0;
        //BTIF_TRACE_EVENT("UIPC INTERRUPT");
        TEMP_FAILURE_RETRY(recv(uipc_avk_main.signal_fds[0], &sig_recv, sizeof(sig_recv), MSG_WAITALL));
    }
}

static inline void uipc_wakeup_locked(void)
{
    char sig_on = 1;
    BTIF_TRACE_EVENT("UIPC SEND WAKE UP");
    TEMP_FAILURE_RETRY(send(uipc_avk_main.signal_fds[1], &sig_on, sizeof(sig_on), 0));
}

static int uipc_setup_server_locked(tUIPC_CH_ID ch_id, char *name, tUIPC_AVK_RCV_CBACK *cback)
{
    int fd;

    BTIF_TRACE_EVENT("SETUP CHANNEL SERVER %d", ch_id);

    if (ch_id >= UIPC_CH_NUM)
        return -1;

    UIPC_LOCK();

    fd = create_server_socket(name);

    if (fd < 0)
    {
        BTIF_TRACE_ERROR("failed to setup %s", name, strerror(errno));
        UIPC_UNLOCK();
         return -1;
    }

    BTIF_TRACE_EVENT("ADD SERVER FD TO ACTIVE SET %d", fd);
    FD_SET(fd, &uipc_avk_main.active_set);
    uipc_avk_main.max_fd = MAX(uipc_avk_main.max_fd, fd);

    uipc_avk_main.ch[ch_id].srvfd = fd;
    uipc_avk_main.ch[ch_id].cback = cback;
    uipc_avk_main.ch[ch_id].read_poll_tmo_ms = DEFAULT_READ_POLL_TMO_MS;

    /* trigger main thread to update read set */
    uipc_wakeup_locked();

    UIPC_UNLOCK();

    return 0;
}

static void uipc_flush_ch_locked(tUIPC_CH_ID ch_id)
{
    char buf[UIPC_FLUSH_BUFFER_SIZE];
    struct pollfd pfd;
    int ret;

    pfd.events = POLLIN;
    pfd.fd = uipc_avk_main.ch[ch_id].fd;

    if (uipc_avk_main.ch[ch_id].fd == UIPC_DISCONNECTED)
    {
        BTIF_TRACE_EVENT("%s() - fd disconnected. Exiting", __FUNCTION__);
        return;
    }

    while (1)
    {
        ret = TEMP_FAILURE_RETRY(poll(&pfd, 1, 1));
        BTIF_TRACE_VERBOSE("%s() - polling fd %d, revents: 0x%x, ret %d",
                __FUNCTION__, pfd.fd, pfd.revents, ret);

        if (pfd.revents & (POLLERR|POLLHUP))
        {
            BTIF_TRACE_EVENT("%s() - POLLERR or POLLHUP. Exiting", __FUNCTION__);
            return;
        }

        if (ret <= 0)
        {
            BTIF_TRACE_EVENT("%s() - error (%d). Exiting", __FUNCTION__, ret);
            return;
        }

        /* read sufficiently large buffer to ensure flush empties socket faster than
           it is getting refilled */
        TEMP_FAILURE_RETRY(read(pfd.fd, &buf, UIPC_FLUSH_BUFFER_SIZE));
    }
}


static void uipc_flush_locked(tUIPC_CH_ID ch_id)
{
    if (ch_id >= UIPC_CH_NUM)
        return;

    switch(ch_id)
    {
        case UIPC_CH_ID_AVK_CTRL:
            uipc_flush_ch_locked(UIPC_CH_ID_AVK_CTRL);
            break;

        case UIPC_CH_ID_AVK_AUDIO:
            uipc_flush_ch_locked(UIPC_CH_ID_AVK_AUDIO);
            break;
    }
}


static int uipc_avk_close_ch_locked(tUIPC_CH_ID ch_id)
{
    int wakeup = 0;

    BTIF_TRACE_EVENT("CLOSE CHANNEL %d", ch_id);

    if (ch_id >= UIPC_CH_NUM)
        return -1;

    if (uipc_avk_main.ch[ch_id].srvfd != UIPC_DISCONNECTED)
    {
        BTIF_TRACE_EVENT("CLOSE SERVER (FD %d)", uipc_avk_main.ch[ch_id].srvfd);
        close(uipc_avk_main.ch[ch_id].srvfd);
        FD_CLR(uipc_avk_main.ch[ch_id].srvfd, &uipc_avk_main.active_set);
        uipc_avk_main.ch[ch_id].srvfd = UIPC_DISCONNECTED;
        wakeup = 1;
    }

    if (uipc_avk_main.ch[ch_id].fd != UIPC_DISCONNECTED)
    {
        BTIF_TRACE_EVENT("CLOSE CONNECTION (FD %d)", uipc_avk_main.ch[ch_id].fd);
        close(uipc_avk_main.ch[ch_id].fd);
        FD_CLR(uipc_avk_main.ch[ch_id].fd, &uipc_avk_main.active_set);
        uipc_avk_main.ch[ch_id].fd = UIPC_DISCONNECTED;
        wakeup = 1;
    }

    /* notify this connection is closed */
    if (uipc_avk_main.ch[ch_id].cback)
        uipc_avk_main.ch[ch_id].cback(ch_id, UIPC_AVK_Close_EVT);

    /* trigger main thread update if something was updated */
    if (wakeup)
        uipc_wakeup_locked();

    return 0;
}


void uipc_avk_close_locked(tUIPC_CH_ID ch_id)
{
    if (uipc_avk_main.ch[ch_id].srvfd == UIPC_DISCONNECTED)
    {
        BTIF_TRACE_EVENT("CHANNEL %d ALREADY CLOSED", ch_id);
        return;
    }

    /* schedule close on this channel */
    uipc_avk_main.ch[ch_id].task_evt_flags |= UIPC_TASK_FLAG_DISCONNECT_CHAN;
    uipc_wakeup_locked();
}


static void uipc_read_task(void *arg)
{
    int ch_id;
    int result;
    UNUSED(arg);

    prctl(PR_SET_NAME, (unsigned long)"uipc-main", 0, 0, 0);

    raise_priority_a2dp(TASK_UIPC_READ);

    while (uipc_avk_main.running)
    {
        uipc_avk_main.read_set = uipc_avk_main.active_set;

        result = TEMP_FAILURE_RETRY(select(uipc_avk_main.max_fd+1, &uipc_avk_main.read_set, NULL, NULL, NULL));

        if (result == 0)
        {
            BTIF_TRACE_EVENT("select timeout");
            continue;
        }
        else if (result < 0)
        {
            BTIF_TRACE_EVENT("select failed %s", strerror(errno));
            continue;
        }

        UIPC_LOCK();

        /* clear any wakeup interrupt */
        uipc_check_interrupt_locked();

        /* check pending task events */
        uipc_check_task_flags_locked();

        /* make sure we service audio channel first */
        uipc_check_fd_locked(UIPC_CH_ID_AVK_AUDIO);

        /* check for other connections */
        for (ch_id = 0; ch_id < UIPC_CH_NUM; ch_id++)
        {
            if (ch_id != UIPC_CH_ID_AVK_AUDIO)
                uipc_check_fd_locked(ch_id);
        }

        UIPC_UNLOCK();
    }

    BTIF_TRACE_EVENT("UIPC READ THREAD EXITING");

    uipc_avk_main_cleanup();

    uipc_avk_main.tid = 0;

    BTIF_TRACE_EVENT("UIPC READ THREAD DONE");
}


int uipc_avk_start_main_server_thread(void)
{
    uipc_avk_main.running = 1;

    if (pthread_create(&uipc_avk_main.tid, (const pthread_attr_t *) NULL, (void*)uipc_read_task, NULL) < 0)
    {
        BTIF_TRACE_ERROR("uipc_thread_create pthread_create failed:%d", errno);
        return -1;
    }

    return 0;
}

/* blocking call */
void uipc_avk_stop_main_server_thread(void)
{
    /* request shutdown of read thread */
    UIPC_LOCK();
    uipc_avk_main.running = 0;
    uipc_wakeup_locked();
    UIPC_UNLOCK();

    /* wait until read thread is fully terminated */
    /* tid might hold pointer value where it's value
       is negative vaule with singed bit is set, so
       corrected the logic to check zero or non zero */
    if (uipc_avk_main.tid)
        pthread_join(uipc_avk_main.tid, NULL);
}

/*******************************************************************************
 **
 ** Function         UIPC_AVK_Init
 **
 ** Description      Initialize UIPC module
 **
 ** Returns          void
 **
 *******************************************************************************/

void UIPC_AVK_Init(void *p_data)
{
    UNUSED(p_data);

    BTIF_TRACE_DEBUG("UIPC_AVK_Init");

    memset(&uipc_avk_main, 0, sizeof(tUIPC_MAIN));

    uipc_main_init();

    uipc_avk_start_main_server_thread();
}

/*******************************************************************************
 **
 ** Function         UIPC_AVK_Open
 **
 ** Description      Open UIPC interface
 **
 ** Returns          TRUE in case of success, FALSE in case of failure.
 **
 *******************************************************************************/
BOOLEAN UIPC_AVK_Open(tUIPC_CH_ID ch_id, tUIPC_AVK_RCV_CBACK *p_cback)
{
    BTIF_TRACE_DEBUG("UIPC_AVK_Open : ch_id %d, p_cback %x", ch_id, p_cback);

    UIPC_LOCK();

    if (ch_id >= UIPC_CH_NUM)
    {
        UIPC_UNLOCK();
        return FALSE;
    }

    if (uipc_avk_main.ch[ch_id].srvfd != UIPC_DISCONNECTED)
    {
        BTIF_TRACE_EVENT("CHANNEL %d ALREADY OPEN", ch_id);
        UIPC_UNLOCK();
        return 0;
    }

    switch(ch_id)
    {
        case UIPC_CH_ID_AVK_CTRL:
            uipc_setup_server_locked(ch_id, A2DP_AVK_CTRL_PATH, p_cback);
            break;

        case UIPC_CH_ID_AVK_AUDIO:
            uipc_setup_server_locked(ch_id, A2DP_AVK_DATA_PATH, p_cback);
            break;
    }

    UIPC_UNLOCK();

    return TRUE;
}

/*******************************************************************************
 **
 ** Function         UIPC_AVK_Close
 **
 ** Description      Close UIPC interface
 **
 ** Returns          void
 **
 *******************************************************************************/

void UIPC_AVK_Close(tUIPC_CH_ID ch_id)
{
    BTIF_TRACE_DEBUG("UIPC_AVK_Close : ch_id %d", ch_id);

    /* special case handling uipc shutdown */
    if (ch_id != UIPC_CH_ID_ALL)
    {
        UIPC_LOCK();
        uipc_avk_close_locked(ch_id);
        UIPC_UNLOCK();
    }
    else
    {
        BTIF_TRACE_DEBUG("UIPC_AVK_Close : waiting for shutdown to complete");
        uipc_avk_stop_main_server_thread();
        BTIF_TRACE_DEBUG("UIPC_AVK_Close : shutdown complete");
    }
}

/*******************************************************************************
 **
 ** Function         UIPC_AVK_SendBuf
 **
 ** Description      Called to transmit a message over UIPC.
 **                  Message buffer will be freed by UIPC_AVK_SendBuf.
 **
 ** Returns          TRUE in case of success, FALSE in case of failure.
 **
 *******************************************************************************/
BOOLEAN UIPC_AVK_SendBuf(tUIPC_CH_ID ch_id, BT_HDR *p_msg)
{
    UNUSED(p_msg);

    BTIF_TRACE_DEBUG("UIPC_AVK_SendBuf : ch_id %d NOT IMPLEMENTED", ch_id);

    UIPC_LOCK();

    /* currently not used */

    UIPC_UNLOCK();

    return FALSE;
}

/*******************************************************************************
 **
 ** Function         UIPC_AVK_Send
 **
 ** Description      Called to transmit a message over UIPC.
 **
 ** Returns          TRUE in case of success, FALSE in case of failure.
 **
 *******************************************************************************/
BOOLEAN UIPC_AVK_Send(tUIPC_CH_ID ch_id, UINT16 msg_evt, UINT8 *p_buf,
        UINT16 msglen)
{
    UNUSED(msg_evt);

    BTIF_TRACE_DEBUG("UIPC_AVK_Send : ch_id:%d %d bytes", ch_id, msglen);

    UIPC_LOCK();

    if (TEMP_FAILURE_RETRY(write(uipc_avk_main.ch[ch_id].fd, p_buf, msglen)) < 0)
    {
        BTIF_TRACE_ERROR("failed to write (%s)", strerror(errno));
    }

    UIPC_UNLOCK();

    return FALSE;
}

/*******************************************************************************
 **
 ** Function         UIPC_AVK_ReadBuf
 **
 ** Description      Called to read a message from UIPC.
 **
 ** Returns          void
 **
 *******************************************************************************/
void UIPC_AVK_ReadBuf(tUIPC_CH_ID ch_id, BT_HDR *p_msg)
{
    UNUSED(p_msg);

    BTIF_TRACE_DEBUG("UIPC_ReadBuf : ch_id:%d NOT IMPLEMENTED", ch_id);

    UIPC_LOCK();
    UIPC_UNLOCK();
}

/*******************************************************************************
 **
 ** Function         UIPC_AVK_Read
 **
 ** Description      Called to read a message from UIPC.
 **
 ** Returns          return the number of bytes read.
 **
 *******************************************************************************/

UINT32 UIPC_AVK_Read(tUIPC_CH_ID ch_id, UINT16 *p_msg_evt, UINT8 *p_buf, UINT32 len)
{
    int n;
    int n_read = 0;
    int fd = uipc_avk_main.ch[ch_id].fd;
    struct pollfd pfd;
    UNUSED(p_msg_evt);

    if (ch_id >= UIPC_CH_NUM)
    {
        BTIF_TRACE_ERROR("UIPC_AVK_Read : invalid ch id %d", ch_id);
        return 0;
    }

    if (fd == UIPC_DISCONNECTED)
    {
        BTIF_TRACE_ERROR("UIPC_AVK_Read : channel %d closed", ch_id);
        return 0;
    }

    //BTIF_TRACE_DEBUG("UIPC_AVK_Read : ch_id %d, len %d, fd %d, polltmo %d", ch_id, len,
    //        fd, uipc_avk_main.ch[ch_id].read_poll_tmo_ms);

    while (n_read < (int)len)
    {
        pfd.fd = fd;
        pfd.events = POLLIN|POLLHUP;

        /* make sure there is data prior to attempting read to avoid blocking
           a read for more than poll timeout */
        if (TEMP_FAILURE_RETRY(poll(&pfd, 1, uipc_avk_main.ch[ch_id].read_poll_tmo_ms)) == 0)
        {
            BTIF_TRACE_EVENT("poll timeout (%d ms)", uipc_avk_main.ch[ch_id].read_poll_tmo_ms);
            break;
        }

        //BTIF_TRACE_EVENT("poll revents %x", pfd.revents);

        if (pfd.revents & (POLLHUP|POLLNVAL) )
        {
            BTIF_TRACE_EVENT("poll : channel detached remotely");
            UIPC_LOCK();
            uipc_avk_close_locked(ch_id);
            UIPC_UNLOCK();
            return 0;
        }

        n = TEMP_FAILURE_RETRY(recv(fd, p_buf+n_read, len-n_read, 0));

        //BTIF_TRACE_EVENT("read %d bytes", n);

        if (n == 0)
        {
            BTIF_TRACE_EVENT("UIPC_AVK_Read : channel detached remotely");
            UIPC_LOCK();
            uipc_avk_close_locked(ch_id);
            UIPC_UNLOCK();
            return 0;
        }

        if (n < 0)
        {
            BTIF_TRACE_EVENT("UIPC_AVK_Read : read failed (%s)", strerror(errno));
            return 0;
        }

        n_read+=n;

    }

    return n_read;
}

/*******************************************************************************
**
** Function         UIPC_AVK_Ioctl
**
** Description      Called to control UIPC.
**
** Returns          void
**
*******************************************************************************/

extern BOOLEAN UIPC_AVK_Ioctl(tUIPC_CH_ID ch_id, UINT32 request, void *param)
{
    BTIF_TRACE_DEBUG("#### UIPC_AVK_Ioctl : ch_id %d, request %d ####", ch_id, request);

    UIPC_LOCK();

    switch(request)
    {
        case UIPC_REQ_RX_FLUSH:
            uipc_flush_locked(ch_id);
            break;

        case UIPC_REG_CBACK:
            //BTIF_TRACE_EVENT("register callback ch %d srvfd %d, fd %d", ch_id, uipc_avk_main.ch[ch_id].srvfd, uipc_avk_main.ch[ch_id].fd);
            uipc_avk_main.ch[ch_id].cback = (tUIPC_AVK_RCV_CBACK*)param;
            break;

        case UIPC_REG_REMOVE_ACTIVE_READSET:

            /* user will read data directly and not use select loop */
            if (uipc_avk_main.ch[ch_id].fd != UIPC_DISCONNECTED)
            {
                /* remove this channel from active set */
                FD_CLR(uipc_avk_main.ch[ch_id].fd, &uipc_avk_main.active_set);

                /* refresh active set */
                uipc_wakeup_locked();
            }
            break;

        case UIPC_SET_READ_POLL_TMO:
            uipc_avk_main.ch[ch_id].read_poll_tmo_ms = (intptr_t)param;
            BTIF_TRACE_EVENT("UIPC_SET_READ_POLL_TMO : CH %d, TMO %d ms", ch_id, uipc_avk_main.ch[ch_id].read_poll_tmo_ms );
            break;

        default:
            BTIF_TRACE_EVENT("UIPC_AVK_Ioctl : request not handled (%d)", request);
            break;
    }

    UIPC_UNLOCK();

    return FALSE;
}

