/* Copyright 2001,2002 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

#include "or.h"

extern or_options_t options; /* command-line and config-file options */

int connection_edge_process_inbuf(connection_t *conn) {

  assert(conn);
  assert(conn->type == CONN_TYPE_AP || conn->type == CONN_TYPE_EXIT);

  if(conn->inbuf_reached_eof) {
#ifdef HALF_OPEN
    /* eof reached; we're done reading, but we might want to write more. */ 
    conn->done_receiving = 1;
    shutdown(conn->s, 0); /* XXX check return, refactor NM */
    if (conn->done_sending)
      conn->marked_for_close = 1;

    /* XXX Factor out common logic here and in circuit_about_to_close NM */
    circ = circuit_get_by_conn(conn);
    if (!circ)
      return -1;

    memset(&cell, 0, sizeof(cell_t));
    cell.command = CELL_RELAY;
    cell.length = RELAY_HEADER_SIZE;
    SET_CELL_RELAY_COMMAND(cell, RELAY_COMMAND_END);
    SET_CELL_STREAM_ID(cell, conn->stream_id);
    cell.aci = circ->n_aci;

    if (circuit_deliver_relay_cell(&cell, circ, CELL_DIRECTION(conn->type), conn->cpath_layer) < 0) {
      log(LOG_DEBUG,"circuit_deliver_relay_cell failed. Closing.");
      circuit_close(circ);
    }
    return 0;
#else 
    /* eof reached, kill it. */
    log_fn(LOG_DEBUG,"conn reached eof. Closing.");
    return -1;
#endif
  }

  switch(conn->state) {
    case AP_CONN_STATE_SOCKS_WAIT:
      return ap_handshake_process_socks(conn);
    case AP_CONN_STATE_OPEN:
    case EXIT_CONN_STATE_OPEN:
      if(connection_package_raw_inbuf(conn) < 0)
        return -1;
      return 0;
    case EXIT_CONN_STATE_CONNECTING:
      log_fn(LOG_DEBUG,"text from server while in 'connecting' state at exit. Leaving it on buffer.");
      return 0;
  }

  return 0;
}

int connection_edge_send_command(connection_t *fromconn, circuit_t *circ, int relay_command) {
  cell_t cell;
  int cell_direction;

  if(!circ) {
    log_fn(LOG_DEBUG,"no circ. Closing.");
    return -1;
  }

  memset(&cell, 0, sizeof(cell_t));
  if(fromconn && fromconn->type == CONN_TYPE_AP) {
    cell.aci = circ->n_aci;
    cell_direction = CELL_DIRECTION_OUT;
  } else {
    /* NOTE: if !fromconn, we assume that it's heading towards the OP */
    cell.aci = circ->p_aci;
    cell_direction = CELL_DIRECTION_IN;
  }

  cell.command = CELL_RELAY;
  SET_CELL_RELAY_COMMAND(cell, relay_command);
  if(fromconn)
    SET_CELL_STREAM_ID(cell, fromconn->stream_id);
  else
    SET_CELL_STREAM_ID(cell, ZERO_STREAM);

  cell.length = RELAY_HEADER_SIZE;
  log_fn(LOG_INFO,"delivering %d cell %s.", relay_command, cell_direction == CELL_DIRECTION_OUT ? "forward" : "backward");

  if(circuit_deliver_relay_cell(&cell, circ, cell_direction, fromconn ? fromconn->cpath_layer : NULL) < 0) {
    log_fn(LOG_DEBUG,"circuit_deliver_relay_cell failed. Closing.");
    circuit_close(circ);
    return 0;
  }
  return 0;
}

int connection_edge_process_relay_cell(cell_t *cell, circuit_t *circ, connection_t *conn,
                                       int edge_type, crypt_path_t *layer_hint) {
  int relay_command;
  static int num_seen=0;

  /* an incoming relay cell has arrived */

  assert(cell && circ);

  relay_command = CELL_RELAY_COMMAND(*cell);
//  log_fn(LOG_DEBUG,"command %d stream %d", relay_command, stream_id);
  num_seen++;
  log_fn(LOG_DEBUG,"Now seen %d relay cells here.", num_seen);

  /* either conn is NULL, in which case we've got a control cell, or else
   * conn points to the recognized stream. */

  if(conn && conn->state != AP_CONN_STATE_OPEN && conn->state != EXIT_CONN_STATE_OPEN) {
    if(conn->type == CONN_TYPE_EXIT && relay_command == RELAY_COMMAND_END) {
      log_fn(LOG_INFO,"Exit got end before we're connected. Marking for close.");
      conn->marked_for_close = 1;
      if(conn->state == EXIT_CONN_STATE_RESOLVING) {
        log_fn(LOG_INFO,"...and informing resolver we don't want the answer anymore.");
        dns_cancel_pending_resolve(conn->address, conn);
      }
    } else {
      log_fn(LOG_DEBUG,"Got an unexpected relay cell, not in 'open' state. Dropping.");
    }
    return 0;
  }

  switch(relay_command) {
    case RELAY_COMMAND_BEGIN:
      if(edge_type == EDGE_AP) {
        log_fn(LOG_INFO,"relay begin request unsupported. Dropping.");
        return 0;
      }
      if(conn) {
        log_fn(LOG_INFO,"begin cell for known stream. Dropping.");
        return 0;
      }
      return connection_exit_begin_conn(cell, circ);
    case RELAY_COMMAND_DATA:
      if((edge_type == EDGE_AP && --layer_hint->deliver_window < 0) ||
         (edge_type == EDGE_EXIT && --circ->deliver_window < 0)) {
        log_fn(LOG_DEBUG,"circ deliver_window below 0. Killing.");
        return -1; /* XXX kill the whole circ? */
      }
      log_fn(LOG_DEBUG,"circ deliver_window now %d.", edge_type == EDGE_AP ? layer_hint->deliver_window : circ->deliver_window);

      if(circuit_consider_sending_sendme(circ, edge_type, layer_hint) < 0)
        return -1;

      if(!conn) {
        log_fn(LOG_DEBUG,"relay cell dropped, unknown stream %d.",*(int*)conn->stream_id);
        return 0;
      }

      if(--conn->deliver_window < 0) { /* is it below 0 after decrement? */
        log_fn(LOG_DEBUG,"conn deliver_window below 0. Killing.");
        return -1; /* somebody's breaking protocol. kill the whole circuit. */
      }

      if(connection_write_to_buf(cell->payload + RELAY_HEADER_SIZE,
                                 cell->length - RELAY_HEADER_SIZE, conn) < 0) {
        conn->marked_for_close = 1;
        return 0;
      }
      if(connection_consider_sending_sendme(conn, edge_type) < 0)
        conn->marked_for_close = 1;
      return 0;
    case RELAY_COMMAND_END:
      if(!conn) {
        log_fn(LOG_DEBUG,"end cell dropped, unknown stream %d.",*(int*)conn->stream_id);
        return 0;
      }
      log_fn(LOG_DEBUG,"end cell for stream %d. Removing stream.",*(int*)conn->stream_id);

#ifdef HALF_OPEN
      conn->done_sending = 1;
      shutdown(conn->s, 1); /* XXX check return; refactor NM */
      if (conn->done_receiving)
        conn->marked_for_close = 1;
#endif
      conn->marked_for_close = 1;
      break;
    case RELAY_COMMAND_EXTEND:
      if(conn) {
        log_fn(LOG_INFO,"'extend' for non-zero stream. Dropping.");
        return 0;
      }
      return circuit_extend(cell, circ);
    case RELAY_COMMAND_EXTENDED:
      if(edge_type == EDGE_EXIT) {
        log_fn(LOG_INFO,"'extended' unsupported at exit. Dropping.");
        return 0;
      }
      log_fn(LOG_DEBUG,"Got an extended cell! Yay.");
      if(circuit_finish_handshake(circ, cell->payload+RELAY_HEADER_SIZE) < 0) {
        log_fn(LOG_INFO,"circuit_finish_handshake failed.");
        return -1;
      }
      return circuit_send_next_onion_skin(circ);
    case RELAY_COMMAND_TRUNCATE:
      if(edge_type == EDGE_AP) {
        log_fn(LOG_INFO,"'truncate' unsupported at AP. Dropping.");
        return 0;
      }
      if(circ->n_conn) {
        connection_send_destroy(circ->n_aci, circ->n_conn);
        circ->n_conn = NULL;
      }
      log_fn(LOG_DEBUG, "Processed 'truncate', replying.");
      return connection_edge_send_command(NULL, circ, RELAY_COMMAND_TRUNCATED);
    case RELAY_COMMAND_TRUNCATED:
      if(edge_type == EDGE_EXIT) {
        log_fn(LOG_INFO,"'truncated' unsupported at exit. Dropping.");
        return 0;
      }
      return circuit_truncated(circ, layer_hint);
    case RELAY_COMMAND_CONNECTED:
      if(edge_type == EDGE_EXIT) {
        log_fn(LOG_INFO,"'connected' unsupported at exit. Dropping.");
        return 0;
      }
      if(!conn) {
        log_fn(LOG_DEBUG,"connected cell dropped, unknown stream %d.",*(int*)conn->stream_id);
        break;
      }
      log_fn(LOG_DEBUG,"Connected! Notifying application.");
      if(ap_handshake_socks_reply(conn, SOCKS4_REQUEST_GRANTED) < 0) {
        conn->marked_for_close = 1;
      }
      break;
    case RELAY_COMMAND_SENDME:
      if(!conn) {
        if(edge_type == EDGE_AP) {
          assert(layer_hint);
          layer_hint->package_window += CIRCWINDOW_INCREMENT;
          log_fn(LOG_DEBUG,"circ-level sendme at AP, packagewindow %d.", layer_hint->package_window);
          circuit_resume_edge_reading(circ, EDGE_AP, layer_hint);
        } else {
          assert(!layer_hint);
          circ->package_window += CIRCWINDOW_INCREMENT;
          log_fn(LOG_DEBUG,"circ-level sendme at exit, packagewindow %d.", circ->package_window);
          circuit_resume_edge_reading(circ, EDGE_EXIT, layer_hint);
        }
        return 0;
      }
      conn->package_window += STREAMWINDOW_INCREMENT;
      log_fn(LOG_DEBUG,"stream-level sendme, packagewindow now %d.", conn->package_window);
      connection_start_reading(conn);
      connection_package_raw_inbuf(conn); /* handle whatever might still be on the inbuf */
      break;
    default:
      log_fn(LOG_DEBUG,"unknown relay command %d.",relay_command);
  }
  return 0;
}

int connection_edge_finished_flushing(connection_t *conn) {
  int e, len=sizeof(e);

  assert(conn);
  assert(conn->type == CONN_TYPE_AP || conn->type == CONN_TYPE_EXIT);

  switch(conn->state) {
    case EXIT_CONN_STATE_CONNECTING:
      if (getsockopt(conn->s, SOL_SOCKET, SO_ERROR, (void*)&e, &len) < 0)  { /* not yet */
        if(errno != EINPROGRESS){
          /* yuck. kill it. */
          log_fn(LOG_DEBUG,"in-progress exit connect failed. Removing.");
          return -1;
        } else {
          log_fn(LOG_DEBUG,"in-progress exit connect still waiting.");
          return 0; /* no change, see if next time is better */
        }
      }
      /* the connect has finished. */

      log_fn(LOG_DEBUG,"Exit connection to %s:%u established.",
          conn->address,conn->port);

      conn->state = EXIT_CONN_STATE_OPEN;
      connection_watch_events(conn, POLLIN); /* stop writing, continue reading */
      if(connection_wants_to_flush(conn)) /* in case there are any queued relay cells */
        connection_start_writing(conn);
      return
        connection_edge_send_command(conn, circuit_get_by_conn(conn), RELAY_COMMAND_CONNECTED) || /* deliver a 'connected' relay cell back through the circuit. */
        connection_process_inbuf(conn); /* in case the server has written anything */
    case AP_CONN_STATE_OPEN:
    case EXIT_CONN_STATE_OPEN:
      connection_stop_writing(conn);
      return connection_consider_sending_sendme(conn, conn->type);
    default:
      log_fn(LOG_DEBUG,"BUG: called in unexpected state.");
      return 0;
  }

  return 0;
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
