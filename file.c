/* file.c
 * File I/O routines
 *
 * $Id: file.c,v 1.25 1999/05/11 20:07:47 gram Exp $
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@zing.org>
 * Copyright 1998 Gerald Combs
 *
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <gtk/gtk.h>

#ifdef WITH_WIRETAP
#include <pcap.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#ifdef NEED_SNPRINTF_H
# ifdef HAVE_STDARG_H
#  include <stdarg.h>
# else
#  include <varargs.h>
# endif
# include "snprintf.h"
#endif

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif

#include "ethereal.h"
#include "menu.h"
#include "packet.h"
#include "file.h"
#include "util.h"

#include "packet-ncp.h"

#define TAIL_TIMEOUT	2000  /* msec */

extern GtkWidget *packet_list, *prog_bar, *info_bar, *byte_view, *tree_view;
extern guint      file_ctx;
extern int	  sync_mode;
extern int        sync_pipe[];

guint cap_input_id, tail_timeout_id;

static guint32 firstsec, firstusec;
static guint32 lastsec, lastusec;

#ifdef WITH_WIRETAP
static void wtap_dispatch_cb(u_char *, const struct wtap_pkthdr *, int,
    const u_char *);
#else
static void pcap_dispatch_cb(u_char *, const struct pcap_pkthdr *,
    const u_char *);
#endif

static gint tail_timeout_cb(gpointer);

int
open_cap_file(char *fname, capture_file *cf) {
#ifndef WITH_WIRETAP
  guint32     magic[2];
  char        err_str[PCAP_ERRBUF_SIZE];
#endif
  struct stat cf_stat;

  /* First, make sure the file is valid */
  if (stat(fname, &cf_stat)) {
    simple_dialog(ESD_TYPE_WARN, NULL, "File does not exist.");
    return 1;
  }
  if (! S_ISREG(cf_stat.st_mode) && ! S_ISFIFO(cf_stat.st_mode)) {
    simple_dialog(ESD_TYPE_WARN, NULL, "The file you have chosen is invalid.");
    return 1;
  }

  /* Next, try to open the file */
  cf->fh = fopen(fname, "r");
  if (cf->fh == NULL)
    return (errno);

  fseek(cf->fh, 0L, SEEK_END);
  cf->f_len = ftell(cf->fh);
#ifndef WITH_WIRETAP
  fseek(cf->fh, 0L, SEEK_SET);
  fread(magic, sizeof(guint32), 2, cf->fh);
  fseek(cf->fh, 0L, SEEK_SET);
#endif
  fclose(cf->fh);
  cf->fh = NULL;
  /* set the file name beacuse we need it to set the follow stream filter */
  cf->filename = g_strdup( fname );

  /* Next, find out what type of file we're dealing with */
#ifdef WITH_WIRETAP 
  cf->cd_t  = WTAP_FILE_UNKNOWN;
#else
  cf->cd_t  = CD_UNKNOWN;
  cf->lnk_t = DLT_NULL;
  cf->swap  = 0;
#endif
  cf->count = 0;
  cf->drops = 0;
  cf->esec  = 0;
  cf->eusec = 0;
  cf->snap  = 0;
  if (cf->plist == NULL) {
    cf->plist       = g_list_alloc();
    cf->plist_first = cf->plist;
    cf->plist->data = (frame_data *) g_malloc(sizeof(frame_data));
  } else {
    cf->plist = g_list_first(cf->plist);
  }
  firstsec = 0, firstusec = 0;
  lastsec = 0, lastusec = 0;
 
#ifndef WITH_WIRETAP
  if (magic[0] == PCAP_MAGIC || magic[0] == SWAP32(PCAP_MAGIC)) {

    /* Pcap/Tcpdump file */
    cf->pfh = pcap_open_offline(fname, err_str);
    if (cf->pfh == NULL) {
#else
	cf->wth = wtap_open_offline(fname);
	if (cf->wth == NULL) {
#endif

      simple_dialog(ESD_TYPE_WARN, NULL, "Could not open file.");
      return 1;
    }

#ifndef WITH_WIRETAP
    if (cf->dfilter) {
      if (pcap_compile(cf->pfh, &cf->fcode, cf->dfilter, 1, 0) < 0) {
        simple_dialog(ESD_TYPE_WARN, NULL, "Unable to parse filter string "
          "\"%s\".", cf->dfilter);
      } else if (pcap_setfilter(cf->pfh, &cf->fcode) < 0) {
        simple_dialog(ESD_TYPE_WARN, NULL, "Can't install filter.");
      }
    }

    cf->fh   = pcap_file(cf->pfh);
    cf->swap = pcap_is_swapped(cf->pfh);    
    if ((cf->swap && BYTE_ORDER == BIG_ENDIAN) ||
      (!cf->swap && BYTE_ORDER == LITTLE_ENDIAN)) {
      /* Data is big-endian */
      cf->cd_t = CD_PCAP_BE;
    } else {
      cf->cd_t = CD_PCAP_LE;
    }
    cf->vers  = ( ((pcap_major_version(cf->pfh) & 0x0000ffff) << 16) |
                  pcap_minor_version(cf->pfh) );
    cf->snap  = pcap_snapshot(cf->pfh);
    cf->lnk_t = pcap_datalink(cf->pfh);
  } else if (ntohl(magic[0]) == SNOOP_MAGIC_1 && ntohl(magic[1]) == SNOOP_MAGIC_2) {
    simple_dialog(ESD_TYPE_WARN, NULL, "The snoop format is not yet supported.");
    return 1;
  }
  
  if (cf->cd_t == CD_UNKNOWN) {
    simple_dialog(ESD_TYPE_WARN, NULL, "Can't determine file type.");
    return 1;
  }
#else
    if (cf->dfilter) {
      if (wtap_offline_filter(cf->wth, cf->dfilter) < 0) {
        simple_dialog(ESD_TYPE_WARN, NULL, "Unable to parse filter string "
          "\"%s\".", cf->dfilter);
      }
    }
  cf->fh = wtap_file(cf->wth);
  cf->cd_t = wtap_file_type(cf->wth);
  cf->snap = wtap_snapshot_length(cf->wth);
#endif

  return 0;
}

/* Reset everything to a pristine state */
void
close_cap_file(capture_file *cf, void *w, guint context) {
  if (cf->fh) {
    fclose(cf->fh);
    cf->fh = NULL;
  }
#ifdef WITH_WIRETAP
  if (cf->wth) {
	  wtap_close(cf->wth);
	  cf->wth = NULL;
	}
#else
  if (cf->pfh) {
    pcap_close(cf->pfh);
    cf->pfh = NULL;
  }
#endif
  gtk_text_freeze(GTK_TEXT(byte_view));
  gtk_text_set_point(GTK_TEXT(byte_view), 0);
  gtk_text_forward_delete(GTK_TEXT(byte_view),
    gtk_text_get_length(GTK_TEXT(byte_view)));
  gtk_text_thaw(GTK_TEXT(byte_view));
  gtk_tree_clear_items(GTK_TREE(tree_view), 0,
    g_list_length(GTK_TREE(tree_view)->children));

  gtk_clist_freeze(GTK_CLIST(packet_list));
  gtk_clist_clear(GTK_CLIST(packet_list));
  gtk_clist_thaw(GTK_CLIST(packet_list));
  gtk_statusbar_pop(GTK_STATUSBAR(w), context);
}

int
load_cap_file(char *fname, capture_file *cf) {
  gchar  *name_ptr, *load_msg, *load_fmt = " Loading: %s...";
  gchar  *done_fmt = " File: %s  Drops: %d";
  gchar  *err_fmt  = " Error: Could not load '%s'";
  gint    timeout;
  size_t  msg_len;
  int     err;

  close_cap_file(cf, info_bar, file_ctx);

  /* Initialize protocol-speficic variables */
  ncp_init_protocol();

  if ((name_ptr = (gchar *) strrchr(fname, '/')) == NULL)
    name_ptr = fname;
  else
    name_ptr++;
  load_msg = g_malloc(strlen(name_ptr) + strlen(load_fmt) + 2);
  sprintf(load_msg, load_fmt, name_ptr);
  gtk_statusbar_push(GTK_STATUSBAR(info_bar), file_ctx, load_msg);
  
  timeout = gtk_timeout_add(250, file_progress_cb, (gpointer) &cf);
  
  err = open_cap_file(fname, cf);
#ifdef WITH_WIRETAP
  if ((err == 0) && (cf->cd_t != WTAP_FILE_UNKNOWN)) {
#else
  if ((err == 0) && (cf->cd_t != CD_UNKNOWN)) {
#endif
    gtk_clist_freeze(GTK_CLIST(packet_list));
#ifdef WITH_WIRETAP
    wtap_loop(cf->wth, 0, wtap_dispatch_cb, (u_char *) cf);
    wtap_close(cf->wth);
    cf->wth = NULL;
#else
    pcap_loop(cf->pfh, 0, pcap_dispatch_cb, (u_char *) cf);
    pcap_close(cf->pfh);
    cf->pfh = NULL;
#endif
    cf->plist = g_list_first(cf->plist);
    cf->fh = fopen(fname, "r");
    gtk_clist_thaw(GTK_CLIST(packet_list));
  }
  
  gtk_timeout_remove(timeout);
  gtk_progress_bar_update(GTK_PROGRESS_BAR(prog_bar), 0);

  gtk_statusbar_pop(GTK_STATUSBAR(info_bar), file_ctx);

  if (err == 0) {
    msg_len = strlen(name_ptr) + strlen(done_fmt) + 64;
    load_msg = g_realloc(load_msg, msg_len);

    if (cf->user_saved || !cf->save_file)
	    snprintf(load_msg, msg_len, done_fmt, name_ptr, cf->drops);
    else
	    snprintf(load_msg, msg_len, done_fmt, "<none>", cf->drops);

    gtk_statusbar_push(GTK_STATUSBAR(info_bar), file_ctx, load_msg);
    g_free(load_msg);

/*    name_ptr[-1] = '\0';  Why is this here? It causes problems with capture files */
#ifdef USE_ITEM
    set_menu_sensitivity("/File/Close", TRUE);
    set_menu_sensitivity("/File/Reload", TRUE);
#else
    set_menu_sensitivity("<Main>/File/Close", TRUE);
    set_menu_sensitivity("<Main>/File/Reload", TRUE);
#endif
  } else {
    msg_len = strlen(name_ptr) + strlen(err_fmt) + 2;
    load_msg = g_realloc(load_msg, msg_len);
    snprintf(load_msg, msg_len, err_fmt, name_ptr);
    gtk_statusbar_push(GTK_STATUSBAR(info_bar), file_ctx, load_msg);
    g_free(load_msg);
#ifdef USE_ITEM
    set_menu_sensitivity("/File/Close", FALSE);
    set_menu_sensitivity("/File/Save", FALSE);
    set_menu_sensitivity("/File/Save as", FALSE);
    set_menu_sensitivity("/File/Reload", FALSE);
#else
    set_menu_sensitivity("<Main>/File/Close", FALSE);
    set_menu_sensitivity("<Main>/File/Save", FALSE);
    set_menu_sensitivity("<Main>/File/Save as", FALSE);
    set_menu_sensitivity("<Main>/File/Reload", FALSE);
#endif
  }
  return err;
}

void 
cap_file_input_cb (gpointer data, gint source, GdkInputCondition condition) {
  
  capture_file *cf = (capture_file *)data;
  char buffer[256];

  /* avoid reentrancy problems and stack overflow */
  gtk_input_remove(cap_input_id);
  if (tail_timeout_id != -1) gtk_timeout_remove(tail_timeout_id);

  if (read(sync_pipe[0], buffer, 256) <= 0) {

    /* process data until end of file and stop capture (restore menu items) */
    gtk_clist_freeze(GTK_CLIST(packet_list));
#ifdef WITH_WIRETAP
    wtap_loop(cf->wth, 0, wtap_dispatch_cb, (u_char *) cf);      
#else
    pcap_loop(cf->pfh, 0, pcap_dispatch_cb, (u_char *) cf);
#endif
    gtk_clist_thaw(GTK_CLIST(packet_list));

#ifdef WITH_WIRETAP
    wtap_close(cf->wth);
    cf->wth = NULL;
#else
    pcap_close(cf->pfh);
    cf->pfh = NULL;
#endif
    cf->plist = g_list_first(cf->plist);
#ifdef USE_ITEM
    set_menu_sensitivity("/File/Open", TRUE);
    set_menu_sensitivity("/File/Close", TRUE);
    set_menu_sensitivity("/File/Save as", TRUE);
    set_menu_sensitivity("/File/Reload", TRUE);
    set_menu_sensitivity("/Tools/Capture", TRUE);
#else
    set_menu_sensitivity("<Main>/File/Open", TRUE);
    set_menu_sensitivity("<Main>/File/Close", TRUE);
    set_menu_sensitivity("<Main>/File/Save as", TRUE);
    set_menu_sensitivity("<Main>/File/Reload", TRUE);
    set_menu_sensitivity("<Main>/Tools/Capture", TRUE);
#endif
    gtk_statusbar_push(GTK_STATUSBAR(info_bar), file_ctx, " File: <none>");
    return;
  }

  gtk_clist_freeze(GTK_CLIST(packet_list));
#ifdef WITH_WIRETAP
  wtap_loop(cf->wth, 0, wtap_dispatch_cb, (u_char *) cf);      
#else
  pcap_loop(cf->pfh, 0, pcap_dispatch_cb, (u_char *) cf);
#endif
  gtk_clist_thaw(GTK_CLIST(packet_list));

  /* restore pipe handler */
  cap_input_id = gtk_input_add_full (sync_pipe[0],
				     GDK_INPUT_READ,
				     cap_file_input_cb,
				     NULL,
				     (gpointer) cf,
				     NULL);

  /* only useful in case of low amount of captured data */
  tail_timeout_id = gtk_timeout_add(TAIL_TIMEOUT, tail_timeout_cb, (gpointer) cf);

}

gint
tail_timeout_cb(gpointer data) {

  capture_file *cf = (capture_file *)data;

  /* avoid reentrancy problems and stack overflow */
  gtk_input_remove(cap_input_id);

  gtk_clist_freeze(GTK_CLIST(packet_list));
#ifdef WITH_WIRETAP
  wtap_loop(cf->wth, 0, wtap_dispatch_cb, (u_char *) cf);      
#else
  pcap_loop(cf->pfh, 0, pcap_dispatch_cb, (u_char *) cf);
#endif
  gtk_clist_thaw(GTK_CLIST(packet_list));

  cap_input_id = gtk_input_add_full (sync_pipe[0],
				     GDK_INPUT_READ,
				     cap_file_input_cb,
				     NULL,
				     (gpointer) cf,
				     NULL);

  return TRUE;
}

int
tail_cap_file(char *fname, capture_file *cf) {
  int     err;

  close_cap_file(cf, info_bar, file_ctx);

  /* Initialize protocol-speficic variables */
  ncp_init_protocol();
  
  err = open_cap_file(fname, cf);
#ifdef WITH_WIRETAP
  if ((err == 0) && (cf->cd_t != WTAP_FILE_UNKNOWN)) {
#else
  if ((err == 0) && (cf->cd_t != CD_UNKNOWN)) {
#endif

#ifdef USE_ITEM
    set_menu_sensitivity("/File/Open", FALSE);
    set_menu_sensitivity("/File/Close", FALSE);
    set_menu_sensitivity("/File/Reload", FALSE);
    set_menu_sensitivity("/Tools/Capture", FALSE);
#else
    set_menu_sensitivity("<Main>/File/Open", FALSE);
    set_menu_sensitivity("<Main>/File/Close", FALSE);
    set_menu_sensitivity("<Main>/File/Reload", FALSE);
    set_menu_sensitivity("<Main>/Tools/Capture", FALSE);
#endif
    cf->fh = fopen(fname, "r");
    tail_timeout_id = -1;
    cap_input_id = gtk_input_add_full (sync_pipe[0],
				       GDK_INPUT_READ,
				       cap_file_input_cb,
				       NULL,
				       (gpointer) cf,
				       NULL);
    gtk_statusbar_push(GTK_STATUSBAR(info_bar), file_ctx, 
		       " <live capture in progress>");
  }
  else {
#ifdef USE_ITEM
    set_menu_sensitivity("/File/Close", FALSE);
    set_menu_sensitivity("/File/Save", FALSE);
    set_menu_sensitivity("/File/Save as", FALSE);
    set_menu_sensitivity("/File/Reload", FALSE);
#else
    set_menu_sensitivity("<Main>/File/Close", FALSE);
    set_menu_sensitivity("<Main>/File/Save", FALSE);
    set_menu_sensitivity("<Main>/File/Save as", FALSE);
    set_menu_sensitivity("<Main>/File/Reload", FALSE);
#endif
    close(sync_pipe[0]);
  }
  return err;
}


static void
#ifdef WITH_WIRETAP
wtap_dispatch_cb(u_char *user, const struct wtap_pkthdr *phdr, int offset,
#else
pcap_dispatch_cb(u_char *user, const struct pcap_pkthdr *phdr,
#endif
  const u_char *buf) {
  frame_data   *fdata;
  gint          i, row;
  capture_file *cf = (capture_file *) user;

  while (gtk_events_pending())
    gtk_main_iteration();

  fdata   = cf->plist->data;
  cf->cur = fdata;
  cf->count++;

  fdata->pkt_len  = phdr->len;
  fdata->cap_len  = phdr->caplen;
#ifdef WITH_WIRETAP
  fdata->file_off = offset;
  fdata->lnk_t = phdr->pkt_encap;
#else
  fdata->file_off = ftell(pcap_file(cf->pfh)) - phdr->caplen;
#endif
  fdata->abs_secs  = phdr->ts.tv_sec;
  fdata->abs_usecs = phdr->ts.tv_usec;

  /* If we don't have the time stamp of the first packet, it's because this
     is the first packet.  Save the time stamp of this packet as the time
     stamp of the first packet. */
  if (!firstsec && !firstusec) {
    firstsec  = fdata->abs_secs;
    firstusec = fdata->abs_usecs;
  }

  /* Do the same for the time stamp of the previous packet. */
  if (!lastsec && !lastusec) {
    lastsec  = fdata->abs_secs;
    lastusec = fdata->abs_usecs;
  }

  /* Get the time elapsed between the first packet and this packet. */
  cf->esec = fdata->abs_secs - firstsec;
  if (firstusec <= fdata->abs_usecs) {
    cf->eusec = fdata->abs_usecs - firstusec;
  } else {
    cf->eusec = (fdata->abs_usecs + 1000000) - firstusec;
    cf->esec--;
  }
  fdata->rel_secs = cf->esec;
  fdata->rel_usecs = cf->eusec;
  
  /* Do the same for the previous packet */
  fdata->del_secs = fdata->abs_secs - lastsec;
  if (lastusec <= fdata->abs_usecs) {
    fdata->del_usecs = fdata->abs_usecs - lastusec;
  } else {
    fdata->del_usecs = (fdata->abs_usecs + 1000000) - lastusec;
    fdata->del_secs--;
  }
  lastsec = fdata->abs_secs;
  lastusec = fdata->abs_usecs;

  fdata->cinfo = &cf->cinfo;
  for (i = 0; i < fdata->cinfo->num_cols; i++) {
    fdata->cinfo->col_data[i][0] = '\0';
  }
  if (check_col(fdata, COL_NUMBER))
    col_add_fstr(fdata, COL_NUMBER, "%d", cf->count);
  dissect_packet(buf, fdata, NULL);
  row = gtk_clist_append(GTK_CLIST(packet_list), fdata->cinfo->col_data);
  fdata->cinfo = NULL;

  /* Make sure we always have an available list entry */
  if (cf->plist->next == NULL) {
    fdata = (frame_data *) g_malloc(sizeof(frame_data));
    g_list_append(cf->plist, (gpointer) fdata);
  }
  cf->plist = cf->plist->next;

}
