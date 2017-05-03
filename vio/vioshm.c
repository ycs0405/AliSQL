/* Copyright (c) 2010, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "vio_priv.h"

#if defined(_WIN32) && defined(HAVE_SMEM)

size_t vio_read_shared_memory(Vio *vio, uchar *buf, size_t size)
{
  size_t length;
  size_t remain_local;
  char *current_position;
  HANDLE events[2];
  DWORD timeout;
  DBUG_ENTER("vio_read_shared_memory");

  remain_local= size;
  current_position= buf;
  timeout= vio->read_timeout >= 0 ? vio->read_timeout : INFINITE;

  events[0]= vio->event_server_wrote;
  events[1]= vio->event_conn_closed;

  do
  {
    if (vio->shared_memory_remain == 0)
    {
      DWORD wait_status;

      wait_status= WaitForMultipleObjects(array_elements(events), events,
                                          FALSE, timeout);

      /*
         WaitForMultipleObjects can return next values:
         WAIT_OBJECT_0+0 - event from vio->event_server_wrote
         WAIT_OBJECT_0+1 - event from vio->event_conn_closed.
                           We can't read anything
         WAIT_ABANDONED_0 and WAIT_TIMEOUT - fail.  We can't read anything
      */
      if (wait_status != WAIT_OBJECT_0)
      {
        /*
          If wait_status is WAIT_TIMEOUT, set error code to indicate a
          timeout error. If vio->event_conn_closed was set, use an EOF
          condition (return value of zero) to indicate that the operation
          has been aborted.
        */
        if (wait_status == WAIT_TIMEOUT)
          SetLastError(SOCKET_ETIMEDOUT);
        else if (wait_status == (WAIT_OBJECT_0 + 1))
          DBUG_RETURN(0);

        DBUG_RETURN(-1);
      }

      vio->shared_memory_pos= vio->handle_map;
      vio->shared_memory_remain= uint4korr((ulong*)vio->shared_memory_pos);
      vio->shared_memory_pos+= 4;
    }

    length= size;

    if (vio->shared_memory_remain < length)
       length= vio->shared_memory_remain;
    if (length > remain_local)
       length= remain_local;

    memcpy(current_position, vio->shared_memory_pos, length);

    vio->shared_memory_remain-= length;
    vio->shared_memory_pos+= length;
    current_position+= length;
    remain_local-= length;

    if (!vio->shared_memory_remain)
    {
      if (!SetEvent(vio->event_client_read))
        DBUG_RETURN(-1);
    }
  } while (remain_local);
  length= size;

  DBUG_RETURN(length);
}


size_t vio_write_shared_memory(Vio *vio, const uchar *buf, size_t size)
{
  size_t length, remain, sz;
  HANDLE pos;
  const uchar *current_position;
  HANDLE events[2];
  DWORD timeout;
  DBUG_ENTER("vio_write_shared_memory");

  remain= size;
  current_position= buf;
  timeout= vio->write_timeout >= 0 ? vio->write_timeout : INFINITE;

  events[0]= vio->event_server_read;
  events[1]= vio->event_conn_closed;

  while (remain != 0)
  {
    DWORD wait_status;

    wait_status= WaitForMultipleObjects(array_elements(events), events,
                                        FALSE, timeout);

    if (wait_status != WAIT_OBJECT_0)
    {
      /* Set error code to indicate a timeout error or disconnect. */
      if (wait_status == WAIT_TIMEOUT)
        SetLastError(SOCKET_ETIMEDOUT);
      else
        SetLastError(ERROR_GRACEFUL_DISCONNECT);

      DBUG_RETURN((size_t) -1);
    }

    sz= (remain > shared_memory_buffer_length ? shared_memory_buffer_length :
         remain);

    int4store(vio->handle_map, sz);
    pos= vio->handle_map + 4;
    memcpy(pos, current_position, sz);
    remain-= sz;
    current_position+= sz;
    if (!SetEvent(vio->event_client_wrote))
      DBUG_RETURN((size_t) -1);
  }
  length= size;

  DBUG_RETURN(length);
}


my_bool vio_is_connected_shared_memory(Vio *vio)
{
  return (WaitForSingleObject(vio->event_conn_closed, 0) != WAIT_OBJECT_0);
}


void vio_delete_shared_memory(Vio *vio)
{
  DBUG_ENTER("vio_delete_shared_memory");

  if (!vio)
    DBUG_VOID_RETURN;

  if (vio->inactive == FALSE)
    vio->vioshutdown(vio);

  /*
    Close all handlers. UnmapViewOfFile and CloseHandle return non-zero
    result if they are success.
  */
  if (UnmapViewOfFile(vio->handle_map) == 0)
    DBUG_PRINT("vio_error", ("UnmapViewOfFile() failed"));
  
  if (CloseHandle(vio->event_server_wrote) == 0)
    DBUG_PRINT("vio_error", ("CloseHandle(vio->esw) failed"));
  
  if (CloseHandle(vio->event_server_read) == 0)
    DBUG_PRINT("vio_error", ("CloseHandle(vio->esr) failed"));
  
  if (CloseHandle(vio->event_client_wrote) == 0)
    DBUG_PRINT("vio_error", ("CloseHandle(vio->ecw) failed"));
  
  if (CloseHandle(vio->event_client_read) == 0)
    DBUG_PRINT("vio_error", ("CloseHandle(vio->ecr) failed"));

  if (CloseHandle(vio->handle_file_map) == 0)
    DBUG_PRINT("vio_error", ("CloseHandle(vio->hfm) failed"));

  if (CloseHandle(vio->event_conn_closed) == 0)
    DBUG_PRINT("vio_error", ("CloseHandle(vio->ecc) failed"));

  vio_delete(vio);

  DBUG_VOID_RETURN;
}

/*
  When "kill connection xx" is executed on an arbitrary thread it calls
  THD::shutdown_active_vio() on the THD referred to by xx. Since the
  thread serving the connection xx might be in the middle of a vio_read
  or vio_write, we cannot unmap the shared memory here.

  Therefore we here just signal the connection_closed event and give
  the thread servicing connection xx a chance to gracefully exit.
  All handles are closed and the VIO is cleaned up when vio_delete() is
  called and this completes the vio cleanup operation in its entirety.
*/
int vio_shutdown_shared_memory(Vio * vio)
{
  DBUG_ENTER("vio_shutdown_shared_memory");
  if (vio->inactive == FALSE)
  {
    /*
      Set event_conn_closed for notification of both client and server that
      connection is closed
    */
    SetEvent(vio->event_conn_closed);
  }

  vio->inactive= TRUE;
  vio->mysql_socket= MYSQL_INVALID_SOCKET;

  DBUG_RETURN(0);
}

int vio_cancel_shared_memory(Vio * vio, int how)
{
  DBUG_ENTER("vio_cancel_shared_memory");
  if (vio->inactive == FALSE)
  {
    /*
      Set event_conn_closed for notification of both client and server that
      connection is closed
    */
    SetEvent(vio->event_conn_closed);
  }

  vio->inactive= TRUE;
  DBUG_RETURN(0);
}

#endif /* #if defined(_WIN32) && defined(HAVE_SMEM) */

