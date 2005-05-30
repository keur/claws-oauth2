/*
 * Sylpheed -- a GTK+ based, lightweight, and fast e-mail client
 * Copyright (C) 1999-2004 Hiroyuki Yamamoto
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "defs.h"

#include <glib.h>
#include <gtk/gtkmain.h>
#include <gtk/gtkwindow.h>
#include <gtk/gtksignal.h>
#include <gtk/gtkprogressbar.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#include "intl.h"
#include "main.h"
#include "inc.h"
#include "mainwindow.h"
#include "folderview.h"
#include "summaryview.h"
#include "prefs_common.h"
#include "prefs_account.h"
#include "account.h"
#include "procmsg.h"
#include "socket.h"
#include "ssl.h"
#include "pop.h"
#include "recv.h"
#include "mbox.h"
#include "utils.h"
#include "gtkutils.h"
#include "statusbar.h"
#include "manage_window.h"
#include "stock_pixmap.h"
#include "progressdialog.h"
#include "inputdialog.h"
#include "alertpanel.h"
#include "folder.h"
#include "filtering.h"
#include "log.h"
#include "hooks.h"

static GList *inc_dialog_list = NULL;

static guint inc_lock_count = 0;

static GdkPixmap *currentxpm;
static GdkBitmap *currentxpmmask;
static GdkPixmap *errorxpm;
static GdkBitmap *errorxpmmask;
static GdkPixmap *okxpm;
static GdkBitmap *okxpmmask;

#define MSGBUFSIZE	8192

static void inc_finished		(MainWindow		*mainwin,
					 gboolean		 new_messages);
static gint inc_account_mail_real	(MainWindow		*mainwin,
					 PrefsAccount		*account);

static IncProgressDialog *inc_progress_dialog_create
					(gboolean		 autocheck);
static void inc_progress_dialog_set_list(IncProgressDialog	*inc_dialog);
static void inc_progress_dialog_destroy	(IncProgressDialog	*inc_dialog);

static IncSession *inc_session_new	(PrefsAccount		*account);
static void inc_session_destroy		(IncSession		*session);
static gint inc_start			(IncProgressDialog	*inc_dialog);
static IncState inc_pop3_session_do	(IncSession		*session);

static void inc_progress_dialog_update	(IncProgressDialog	*inc_dialog,
					 IncSession		*inc_session);

static void inc_progress_dialog_set_label
					(IncProgressDialog	*inc_dialog,
					 IncSession		*inc_session);
static void inc_progress_dialog_set_progress
					(IncProgressDialog	*inc_dialog,
					 IncSession		*inc_session);

static void inc_progress_dialog_update_periodic
					(IncProgressDialog	*inc_dialog,
					 IncSession		*inc_session);

static gint inc_recv_data_progressive	(Session	*session,
					 guint		 cur_len,
					 guint		 total_len,
					 gpointer	 data);
static gint inc_recv_data_finished	(Session	*session,
					 guint		 len,
					 gpointer	 data);
static gint inc_recv_message		(Session	*session,
					 const gchar	*msg,
					 gpointer	 data);
static gint inc_drop_message		(Pop3Session	*session,
					 const gchar	*file);

static void inc_put_error		(IncState	 istate,
					 Pop3Session 	*session);

static void inc_cancel_cb		(GtkWidget	*widget,
					 gpointer	 data);
static gint inc_dialog_delete_cb	(GtkWidget	*widget,
					 GdkEventAny	*event,
					 gpointer	 data);

static gint get_spool			(FolderItem	*dest,
					 const gchar	*mbox);

static gint inc_spool_account(PrefsAccount *account);
static gint inc_all_spool(void);
static void inc_autocheck_timer_set_interval	(guint		 interval);
static gint inc_autocheck_func			(gpointer	 data);

static void inc_notify_cmd		(gint new_msgs, 
 					 gboolean notify);
	
/**
 * inc_finished:
 * @mainwin: Main window.
 * @new_messages: TRUE if some messages have been received.
 * 
 * Update the folder view and the summary view after receiving
 * messages.  If @new_messages is FALSE, this function avoids unneeded
 * updating.
 **/
static void inc_finished(MainWindow *mainwin, gboolean new_messages)
{
	FolderItem *item;

	if (prefs_common.scan_all_after_inc)
		folderview_check_new(NULL);

	if (!new_messages && !prefs_common.scan_all_after_inc) return;

	if (prefs_common.open_inbox_on_inc) {
		item = cur_account && cur_account->inbox
			? folder_find_item_from_identifier(cur_account->inbox)
			: folder_get_default_inbox();
		folderview_unselect(mainwin->folderview);
		folderview_select(mainwin->folderview, item);
	}
}

void inc_mail(MainWindow *mainwin, gboolean notify)
{
	gint new_msgs = 0;
	gint account_new_msgs = 0;

	if (inc_lock_count) return;

	if (prefs_common.work_offline)
		if (alertpanel(_("Offline warning"), 
			       _("You're working offline. Override?"),
			       _("Yes"), _("No"), NULL) != G_ALERTDEFAULT)
		return;

	inc_lock();
	inc_autocheck_timer_remove();
	main_window_lock(mainwin);

	if (prefs_common.use_extinc && prefs_common.extinc_cmd) {
		/* external incorporating program */
		if (execute_command_line(prefs_common.extinc_cmd, FALSE) < 0) {
			main_window_unlock(mainwin);
			inc_autocheck_timer_set();
			return;
		}
	} else {
		account_new_msgs = inc_account_mail_real(mainwin, cur_account);
		if (account_new_msgs > 0)
			new_msgs += account_new_msgs;
	}

	inc_finished(mainwin, new_msgs > 0);
	main_window_unlock(mainwin);
 	inc_notify_cmd(new_msgs, notify);
	inc_autocheck_timer_set();
	inc_unlock();
}

void inc_pop_before_smtp(PrefsAccount *acc)
{
	IncProgressDialog *inc_dialog;
	IncSession *session;
	MainWindow *mainwin;

	mainwin = mainwindow_get_mainwindow();

    	session = inc_session_new(acc);
    	if (!session) return;
	POP3_SESSION(session->session)->pop_before_smtp = TRUE;
		
    	inc_dialog = inc_progress_dialog_create(FALSE);
    	inc_dialog->queue_list = g_list_append(inc_dialog->queue_list,
					       session);
	/* FIXME: assumes to attach to first main window */
	inc_dialog->mainwin = mainwin;
	inc_progress_dialog_set_list(inc_dialog);

	if (mainwin) {
		toolbar_main_set_sensitive(mainwin);
		main_window_set_menu_sensitive(mainwin);
	}
			
	inc_start(inc_dialog);
}

static gint inc_account_mail_real(MainWindow *mainwin, PrefsAccount *account)
{
	IncProgressDialog *inc_dialog;
	IncSession *session;
	
	switch (account->protocol) {
	case A_IMAP4:
	case A_NNTP:
		/* Melvin: bug [14]
		 * FIXME: it should return foldeview_check_new() value.
		 * TODO: do it when bug [19] is fixed (IMAP folder sets 
		 * an incorrect new message count)
		 */
		folderview_check_new(FOLDER(account->folder));
		return 0;
	case A_POP3:
	case A_APOP:
		session = inc_session_new(account);
		if (!session) return 0;
		
		inc_dialog = inc_progress_dialog_create(FALSE);
		inc_dialog->queue_list = g_list_append(inc_dialog->queue_list,
						       session);
		inc_dialog->mainwin = mainwin;
		inc_progress_dialog_set_list(inc_dialog);

		if (mainwin) {
			toolbar_main_set_sensitive(mainwin);
			main_window_set_menu_sensitive(mainwin);
		}
			
		return inc_start(inc_dialog);

	case A_LOCAL:
		return inc_spool_account(account);

	default:
		break;
	}
	return 0;
}

gint inc_account_mail(MainWindow *mainwin, PrefsAccount *account)
{
	gint new_msgs;

	if (inc_lock_count) return 0;

	if (prefs_common.work_offline)
		if (alertpanel(_("Offline warning"), 
			       _("You're working offline. Override?"),
			       _("Yes"), _("No"), NULL) != G_ALERTDEFAULT)
			return 0;

	inc_autocheck_timer_remove();
	main_window_lock(mainwin);

	new_msgs = inc_account_mail_real(mainwin, account);

	inc_finished(mainwin, new_msgs > 0);
	main_window_unlock(mainwin);
	inc_autocheck_timer_set();

	return new_msgs;
}

void inc_all_account_mail(MainWindow *mainwin, gboolean autocheck,
			  gboolean notify)
{
	GList *list, *queue_list = NULL;
	IncProgressDialog *inc_dialog;
	gint new_msgs = 0;
	gint account_new_msgs = 0;
	
	if (prefs_common.work_offline)
		if (alertpanel(_("Offline warning"), 
			       _("You're working offline. Override?"),
			       _("Yes"), _("No"), NULL) != G_ALERTDEFAULT)
		return;

	if (inc_lock_count) return;

	inc_autocheck_timer_remove();
	main_window_lock(mainwin);

	list = account_get_list();
	if (!list) {
		inc_finished(mainwin, new_msgs > 0);
		main_window_unlock(mainwin);
 		inc_notify_cmd(new_msgs, notify);
		inc_autocheck_timer_set();
		return;
	}

	/* check local folders */
	account_new_msgs = inc_all_spool();
	if (account_new_msgs > 0)
		new_msgs += account_new_msgs;

	/* check IMAP4 / News folders */
	for (list = account_get_list(); list != NULL; list = list->next) {
		PrefsAccount *account = list->data;
		if ((account->protocol == A_IMAP4 ||
		     account->protocol == A_NNTP) && account->recv_at_getall) {
			new_msgs += folderview_check_new(FOLDER(account->folder));
		}
	}

	/* check POP3 accounts */
	for (list = account_get_list(); list != NULL; list = list->next) {
		IncSession *session;
		PrefsAccount *account = list->data;

		if (account->recv_at_getall) {
			session = inc_session_new(account);
			if (session)
				queue_list = g_list_append(queue_list, session);
		}
	}

	if (queue_list) {
		inc_dialog = inc_progress_dialog_create(autocheck);
		inc_dialog->queue_list = queue_list;
		inc_dialog->mainwin = mainwin;
		inc_progress_dialog_set_list(inc_dialog);

		toolbar_main_set_sensitive(mainwin);
		main_window_set_menu_sensitive(mainwin);
		new_msgs += inc_start(inc_dialog);
	}

	inc_finished(mainwin, new_msgs > 0);
	main_window_unlock(mainwin);
 	inc_notify_cmd(new_msgs, notify);
	inc_autocheck_timer_set();
}

static IncProgressDialog *inc_progress_dialog_create(gboolean autocheck)
{
	IncProgressDialog *dialog;
	ProgressDialog *progress;

	dialog = g_new0(IncProgressDialog, 1);

	progress = progress_dialog_create();
	gtk_window_set_title(GTK_WINDOW(progress->window),
			     _("Retrieving new messages"));
	gtk_signal_connect(GTK_OBJECT(progress->cancel_btn), "clicked",
			   GTK_SIGNAL_FUNC(inc_cancel_cb), dialog);
	gtk_signal_connect(GTK_OBJECT(progress->window), "delete_event",
			   GTK_SIGNAL_FUNC(inc_dialog_delete_cb), dialog);
	/* manage_window_set_transient(GTK_WINDOW(progress->window)); */

	progress_dialog_set_value(progress, 0.0);

	stock_pixmap_gdk(progress->clist, STOCK_PIXMAP_COMPLETE,
			 &okxpm, &okxpmmask);
	stock_pixmap_gdk(progress->clist, STOCK_PIXMAP_CONTINUE,
			 &currentxpm, &currentxpmmask);
	stock_pixmap_gdk(progress->clist, STOCK_PIXMAP_ERROR,
			 &errorxpm, &errorxpmmask);

	if (prefs_common.recv_dialog_mode == RECV_DIALOG_ALWAYS ||
	    (prefs_common.recv_dialog_mode == RECV_DIALOG_MANUAL &&
	     !autocheck)) {
		dialog->show_dialog = TRUE;
		gtk_widget_show_now(progress->window);
	}

	dialog->dialog = progress;
	gettimeofday(&dialog->progress_tv, NULL);
	gettimeofday(&dialog->folder_tv, NULL);
	dialog->queue_list = NULL;
	dialog->cur_row = 0;

	inc_dialog_list = g_list_append(inc_dialog_list, dialog);

	return dialog;
}

static void inc_progress_dialog_set_list(IncProgressDialog *inc_dialog)
{
	GList *list;

	for (list = inc_dialog->queue_list; list != NULL; list = list->next) {
		IncSession *session = list->data;
		Pop3Session *pop3_session = POP3_SESSION(session->session);
		gchar *text[3];

		session->data = inc_dialog;

		text[0] = NULL;
		text[1] = pop3_session->ac_prefs->account_name;
		text[2] = _("Standby");
		gtk_clist_append(GTK_CLIST(inc_dialog->dialog->clist), text);
	}
}

static void inc_progress_dialog_clear(IncProgressDialog *inc_dialog)
{
	progress_dialog_set_value(inc_dialog->dialog, 0.0);
	progress_dialog_set_label(inc_dialog->dialog, "");
	if (inc_dialog->mainwin)
		main_window_progress_off(inc_dialog->mainwin);
}

static void inc_progress_dialog_destroy(IncProgressDialog *inc_dialog)
{
	g_return_if_fail(inc_dialog != NULL);

	inc_dialog_list = g_list_remove(inc_dialog_list, inc_dialog);

	if (inc_dialog->mainwin)
		main_window_progress_off(inc_dialog->mainwin);
	progress_dialog_destroy(inc_dialog->dialog);

	g_free(inc_dialog);
}

static IncSession *inc_session_new(PrefsAccount *account)
{
	IncSession *session;

	g_return_val_if_fail(account != NULL, NULL);

	if (account->protocol != A_POP3)
		return NULL;
	if (!account->recv_server || !account->userid)
		return NULL;

	session = g_new0(IncSession, 1);

	session->session = pop3_session_new(account);
	session->session->data = session;
	POP3_SESSION(session->session)->drop_message = inc_drop_message;
	session_set_recv_message_notify(session->session,
					inc_recv_message, session);
	session_set_recv_data_progressive_notify(session->session,
						 inc_recv_data_progressive,
						 session);
	session_set_recv_data_notify(session->session,
				     inc_recv_data_finished, session);

	return session;
}

static void inc_session_destroy(IncSession *session)
{
	g_return_if_fail(session != NULL);

	session_destroy(session->session);
	g_free(session);
}

static gint inc_start(IncProgressDialog *inc_dialog)
{
	IncSession *session;
	GtkCList *clist = GTK_CLIST(inc_dialog->dialog->clist);
	GList *qlist;
	Pop3Session *pop3_session;
	IncState inc_state;
	gint error_num = 0;
	gint new_msgs = 0;
	gchar *msg;
	gchar *fin_msg;
	FolderItem *processing, *inbox;
	MsgInfo *msginfo;
	GSList *msglist, *msglist_element;
	gboolean cancelled = FALSE;

	qlist = inc_dialog->queue_list;
	while (qlist != NULL) {
		GList *next = qlist->next;

		session = qlist->data;
		pop3_session = POP3_SESSION(session->session); 
		pop3_session->user = g_strdup(pop3_session->ac_prefs->userid);
		if (pop3_session->ac_prefs->passwd)
			pop3_session->pass =
				g_strdup(pop3_session->ac_prefs->passwd);
		else if (pop3_session->ac_prefs->tmp_pass)
			pop3_session->pass =
				g_strdup(pop3_session->ac_prefs->tmp_pass);
		else {
			gchar *pass;

			if (inc_dialog->show_dialog)
				manage_window_focus_in
					(inc_dialog->dialog->window,
					 NULL, NULL);

			pass = input_dialog_query_password
				(pop3_session->ac_prefs->recv_server,
				 pop3_session->user);

			if (inc_dialog->show_dialog)
				manage_window_focus_out
					(inc_dialog->dialog->window,
					 NULL, NULL);

			if (pass) {
				pop3_session->ac_prefs->tmp_pass =
					g_strdup(pass);
				pop3_session->pass = pass;
			}
		}

		qlist = next;
	}

#define SET_PIXMAP_AND_TEXT(xpm, xpmmask, str)				   \
{									   \
	gtk_clist_set_pixmap(clist, inc_dialog->cur_row, 0, xpm, xpmmask); \
	gtk_clist_set_text(clist, inc_dialog->cur_row, 2, str);		   \
}

	for (; inc_dialog->queue_list != NULL && !cancelled; inc_dialog->cur_row++) {
		session = inc_dialog->queue_list->data;
		pop3_session = POP3_SESSION(session->session);

		if (pop3_session->pass == NULL) {
			SET_PIXMAP_AND_TEXT(okxpm, okxpmmask, _("Cancelled"));
			inc_session_destroy(session);
			inc_dialog->queue_list =
				g_list_remove(inc_dialog->queue_list, session);
			continue;
		}

		inc_progress_dialog_clear(inc_dialog);
		gtk_clist_moveto(clist, inc_dialog->cur_row, -1, 1.0, 0.0);

		SET_PIXMAP_AND_TEXT(currentxpm, currentxpmmask,
				    _("Retrieving"));

		/* begin POP3 session */
		inc_state = inc_pop3_session_do(session);

		switch (inc_state) {
		case INC_SUCCESS:
			if (pop3_session->cur_total_num > 0)
				msg = g_strdup_printf(
					ngettext("Done (%d message (%s) received)",
						 "Done (%d messages (%s) received)",
					 pop3_session->cur_total_num),
					 pop3_session->cur_total_num,
					 to_human_readable(pop3_session->cur_total_recv_bytes));
			else
				msg = g_strdup_printf(_("Done (no new messages)"));
			SET_PIXMAP_AND_TEXT(okxpm, okxpmmask, msg);
			g_free(msg);
			break;
		case INC_CONNECT_ERROR:
			SET_PIXMAP_AND_TEXT(errorxpm, errorxpmmask,
					    _("Connection failed"));
			break;
		case INC_AUTH_FAILED:
			SET_PIXMAP_AND_TEXT(errorxpm, errorxpmmask,
					    _("Auth failed"));
			break;
		case INC_LOCKED:
			SET_PIXMAP_AND_TEXT(errorxpm, errorxpmmask,
					    _("Locked"));
			break;
		case INC_ERROR:
		case INC_NO_SPACE:
		case INC_IO_ERROR:
		case INC_SOCKET_ERROR:
		case INC_EOF:
			SET_PIXMAP_AND_TEXT(errorxpm, errorxpmmask, _("Error"));
			break;
		case INC_TIMEOUT:
			SET_PIXMAP_AND_TEXT(errorxpm, errorxpmmask, _("Timeout"));
			break;
		case INC_CANCEL:
			SET_PIXMAP_AND_TEXT(okxpm, okxpmmask, _("Cancelled"));
			if (!inc_dialog->show_dialog)
				cancelled = TRUE;
			break;
		default:
			break;
		}
		
		if (pop3_session->error_val == PS_AUTHFAIL) {
			if(!prefs_common.no_recv_err_panel) {
				if((prefs_common.recv_dialog_mode == RECV_DIALOG_ALWAYS) ||
				    ((prefs_common.recv_dialog_mode == RECV_DIALOG_MANUAL) && focus_window))
					manage_window_focus_in(inc_dialog->dialog->window, NULL, NULL);
			}
		}

		/* CLAWS: perform filtering actions on dropped message */
		/* CLAWS: get default inbox (perhaps per account) */
		if (pop3_session->ac_prefs->inbox) {
			/* CLAWS: get destination folder / mailbox */
			inbox = folder_find_item_from_identifier(pop3_session->ac_prefs->inbox);
			if (!inbox)
				inbox = folder_get_default_inbox();
		} else
			inbox = folder_get_default_inbox();

		/* get list of messages in processing */
		processing = folder_get_default_processing();
		folder_item_scan(processing);
		msglist = folder_item_get_msg_list(processing);

		/* process messages */
		folder_item_update_freeze();
		for(msglist_element = msglist; msglist_element != NULL; msglist_element = msglist_element->next) {
			gchar *filename;
			msginfo = (MsgInfo *) msglist_element->data;
			filename = folder_item_fetch_msg(processing, msginfo->msgnum);
			g_free(filename);
			if (!pop3_session->ac_prefs->filter_on_recv || 
			    !procmsg_msginfo_filter(msginfo))
				folder_item_move_msg(inbox, msginfo);
			procmsg_msginfo_free(msginfo);
		}
		folder_item_update_thaw();
		g_slist_free(msglist);

		statusbar_pop_all();

		new_msgs += pop3_session->cur_total_num;

		if (pop3_session->error_val == PS_AUTHFAIL &&
		    pop3_session->ac_prefs->tmp_pass) {
			g_free(pop3_session->ac_prefs->tmp_pass);
			pop3_session->ac_prefs->tmp_pass = NULL;
		}

		pop3_write_uidl_list(pop3_session);

		if (inc_state != INC_SUCCESS && inc_state != INC_CANCEL) {
			error_num++;
			if (inc_dialog->show_dialog)
				manage_window_focus_in
					(inc_dialog->dialog->window,
					 NULL, NULL);
			inc_put_error(inc_state, pop3_session);
			if (inc_dialog->show_dialog)
				manage_window_focus_out
					(inc_dialog->dialog->window,
					 NULL, NULL);
			if (inc_state == INC_NO_SPACE ||
			    inc_state == INC_IO_ERROR)
				break;
		}

		inc_session_destroy(session);
		inc_dialog->queue_list =
			g_list_remove(inc_dialog->queue_list, session);
	}

#undef SET_PIXMAP_AND_TEXT

	if (new_msgs > 0)
		fin_msg = g_strdup_printf(ngettext("Finished (%d new message)",
					  	   "Finished (%d new messages)",
					  	   new_msgs), new_msgs);
	else
		fin_msg = g_strdup_printf(_("Finished (no new messages)"));

	progress_dialog_set_label(inc_dialog->dialog, fin_msg);

#if 0
	if (error_num && !prefs_common.no_recv_err_panel) {
		if (inc_dialog->show_dialog)
			manage_window_focus_in(inc_dialog->dialog->window,
					       NULL, NULL);
		alertpanel_error_log(_("Some errors occurred while getting mail."));
		if (inc_dialog->show_dialog)
			manage_window_focus_out(inc_dialog->dialog->window,
						NULL, NULL);
	}
#endif

	while (inc_dialog->queue_list != NULL) {
		session = inc_dialog->queue_list->data;
		inc_session_destroy(session);
		inc_dialog->queue_list =
			g_list_remove(inc_dialog->queue_list, session);
	}

	if (prefs_common.close_recv_dialog || !inc_dialog->show_dialog)
		inc_progress_dialog_destroy(inc_dialog);
	else {
		gtk_window_set_title(GTK_WINDOW(inc_dialog->dialog->window),
				     fin_msg);
		gtk_label_set_text(GTK_LABEL(GTK_BIN(inc_dialog->dialog->cancel_btn)->child),
				   _("Close"));
	}

	g_free(fin_msg);

	return new_msgs;
}

static IncState inc_pop3_session_do(IncSession *session)
{
	Pop3Session *pop3_session = POP3_SESSION(session->session);
	IncProgressDialog *inc_dialog = (IncProgressDialog *)session->data;
	gchar *server;
	gushort port;
	gchar *buf;

	debug_print("getting new messages of account %s...\n",
		    pop3_session->ac_prefs->account_name);
		    
	pop3_session->ac_prefs->last_pop_login_time = time(NULL);

	buf = g_strdup_printf(_("%s: Retrieving new messages"),
			      pop3_session->ac_prefs->recv_server);
	gtk_window_set_title(GTK_WINDOW(inc_dialog->dialog->window), buf);
	g_free(buf);

	server = pop3_session->ac_prefs->recv_server;
#if USE_OPENSSL
	port = pop3_session->ac_prefs->set_popport ?
		pop3_session->ac_prefs->popport :
		pop3_session->ac_prefs->ssl_pop == SSL_TUNNEL ? 995 : 110;
	SESSION(pop3_session)->ssl_type = pop3_session->ac_prefs->ssl_pop;
	if (pop3_session->ac_prefs->ssl_pop != SSL_NONE)
		SESSION(pop3_session)->nonblocking =
			pop3_session->ac_prefs->use_nonblocking_ssl;
#else
	port = pop3_session->ac_prefs->set_popport ?
		pop3_session->ac_prefs->popport : 110;
#endif

	buf = g_strdup_printf(_("Connecting to POP3 server: %s..."), server);
	log_message("%s\n", buf);

	progress_dialog_set_label(inc_dialog->dialog, buf);
	g_free(buf);

	session_set_timeout(SESSION(pop3_session),
			    prefs_common.io_timeout_secs * 1000);

	if (session_connect(SESSION(pop3_session), server, port) < 0) {
		log_warning(_("Can't connect to POP3 server: %s:%d\n"),
			    server, port);
		if(!prefs_common.no_recv_err_panel) {
			if((prefs_common.recv_dialog_mode == RECV_DIALOG_ALWAYS) ||
			    ((prefs_common.recv_dialog_mode == RECV_DIALOG_MANUAL) && focus_window)) {
				manage_window_focus_in(inc_dialog->dialog->window, NULL, NULL);
			}
			alertpanel_error(_("Can't connect to POP3 server: %s:%d"),
					 server, port);
			manage_window_focus_out(inc_dialog->dialog->window, NULL, NULL);
		}
		session->inc_state = INC_CONNECT_ERROR;
		statusbar_pop_all();
		return INC_CONNECT_ERROR;
	}

	while (session_is_connected(SESSION(pop3_session)) &&
	       session->inc_state != INC_CANCEL)
		gtk_main_iteration();

	if (session->inc_state == INC_SUCCESS) {
		switch (pop3_session->error_val) {
		case PS_SUCCESS:
			switch (SESSION(pop3_session)->state) {
			case SESSION_ERROR:
				if (pop3_session->state == POP3_READY)
					session->inc_state = INC_CONNECT_ERROR;
				else
					session->inc_state = INC_ERROR;
				break;
			case SESSION_EOF:
				session->inc_state = INC_EOF;
				break;
			case SESSION_TIMEOUT:
				session->inc_state = INC_TIMEOUT;
				break;
			default:
				session->inc_state = INC_SUCCESS;
				break;
			}
			break;
		case PS_AUTHFAIL:
			session->inc_state = INC_AUTH_FAILED;
			break;
		case PS_IOERR:
			session->inc_state = INC_IO_ERROR;
			break;
		case PS_SOCKET:
			session->inc_state = INC_SOCKET_ERROR;
			break;
		case PS_LOCKBUSY:
			session->inc_state = INC_LOCKED;
			break;
		default:
			session->inc_state = INC_ERROR;
			break;
		}
	}

	session_disconnect(SESSION(pop3_session));
	statusbar_pop_all();

	return session->inc_state;
}

static void inc_progress_dialog_update(IncProgressDialog *inc_dialog,
				       IncSession *inc_session)
{
	inc_progress_dialog_set_label(inc_dialog, inc_session);
	inc_progress_dialog_set_progress(inc_dialog, inc_session);
}

static void inc_progress_dialog_set_label(IncProgressDialog *inc_dialog,
					  IncSession *inc_session)
{
	ProgressDialog *dialog = inc_dialog->dialog;
	Pop3Session *session;

	g_return_if_fail(inc_session != NULL);

	session = POP3_SESSION(inc_session->session);

	switch (session->state) {
	case POP3_GREETING:
		break;
	case POP3_GETAUTH_USER:
	case POP3_GETAUTH_PASS:
	case POP3_GETAUTH_APOP:
		progress_dialog_set_label(dialog, _("Authenticating..."));
		statusbar_print_all(_("Retrieving messages from %s (%s) ..."),
				    SESSION(session)->server,
				    session->ac_prefs->account_name);
		break;
	case POP3_GETRANGE_STAT:
		progress_dialog_set_label
			(dialog, _("Getting the number of new messages (STAT)..."));
		break;
	case POP3_GETRANGE_LAST:
		progress_dialog_set_label
			(dialog, _("Getting the number of new messages (LAST)..."));
		break;
	case POP3_GETRANGE_UIDL:
		progress_dialog_set_label
			(dialog, _("Getting the number of new messages (UIDL)..."));
		break;
	case POP3_GETSIZE_LIST:
		progress_dialog_set_label
			(dialog, _("Getting the size of messages (LIST)..."));
		break;
	case POP3_RETR:
	case POP3_RETR_RECV:
		break;
	case POP3_DELETE:
#if 0
		if (session->msg[session->cur_msg].recv_time <
			session->current_time) {
			gchar buf[MSGBUFSIZE];
			g_snprintf(buf, sizeof(buf), _("Deleting message %d"),
				   session->cur_msg);
			progress_dialog_set_label(dialog, buf);
		}
#endif
		break;
	case POP3_LOGOUT:
		progress_dialog_set_label(dialog, _("Quitting"));
		break;
	default:
		break;
	}
}

static void inc_progress_dialog_set_progress(IncProgressDialog *inc_dialog,
					     IncSession *inc_session)
{
	gchar buf[MSGBUFSIZE];
	Pop3Session *pop3_session = POP3_SESSION(inc_session->session);
	gchar *total_size_str;
	gint cur_total;
	gint total;

	if (!pop3_session->new_msg_exist) return;

	cur_total = inc_session->cur_total_bytes;
	total = pop3_session->total_bytes;
	if (pop3_session->state == POP3_RETR ||
	    pop3_session->state == POP3_RETR_RECV ||
	    pop3_session->state == POP3_DELETE) {
		Xstrdup_a(total_size_str, to_human_readable(total), return);
		g_snprintf(buf, sizeof(buf),
			   _("Retrieving message (%d / %d) (%s / %s)"),
			   pop3_session->cur_msg, pop3_session->count,
			   to_human_readable(cur_total), total_size_str);
		progress_dialog_set_label(inc_dialog->dialog, buf);
	}

	progress_dialog_set_percentage
		(inc_dialog->dialog,(gfloat)cur_total / (gfloat)total);

	gtk_progress_set_show_text
		(GTK_PROGRESS(inc_dialog->mainwin->progressbar), TRUE);
	g_snprintf(buf, sizeof(buf), "%d / %d",
		   pop3_session->cur_msg, pop3_session->count);
	gtk_progress_set_format_string
		(GTK_PROGRESS(inc_dialog->mainwin->progressbar), buf);
	gtk_progress_bar_update
		(GTK_PROGRESS_BAR(inc_dialog->mainwin->progressbar),
		 (gfloat)cur_total / (gfloat)total);

	if (pop3_session->cur_total_num > 0) {
		g_snprintf(buf, sizeof(buf),
			   ngettext("Retrieving (%d message (%s) received)",
			   	    "Retrieving (%d messages (%s) received)",
				    pop3_session->cur_total_num),
			   pop3_session->cur_total_num,
			   to_human_readable
			   (pop3_session->cur_total_recv_bytes));
		gtk_clist_set_text(GTK_CLIST(inc_dialog->dialog->clist),
				   inc_dialog->cur_row, 2, buf);
	}
}

static void inc_progress_dialog_update_periodic(IncProgressDialog *inc_dialog,
						IncSession *inc_session)
{
	struct timeval tv_cur;
	struct timeval tv_result;
	gint msec;

	gettimeofday(&tv_cur, NULL);

	tv_result.tv_sec = tv_cur.tv_sec - inc_dialog->progress_tv.tv_sec;
	tv_result.tv_usec = tv_cur.tv_usec - inc_dialog->progress_tv.tv_usec;
	if (tv_result.tv_usec < 0) {
		tv_result.tv_sec--;
		tv_result.tv_usec += 1000000;
	}

	msec = tv_result.tv_sec * 1000 + tv_result.tv_usec / 1000;
	if (msec > PROGRESS_UPDATE_INTERVAL) {
		inc_progress_dialog_update(inc_dialog, inc_session);
		inc_dialog->progress_tv.tv_sec = tv_cur.tv_sec;
		inc_dialog->progress_tv.tv_usec = tv_cur.tv_usec;
	}
}

static gint inc_recv_data_progressive(Session *session, guint cur_len,
				      guint total_len, gpointer data)
{
	IncSession *inc_session = (IncSession *)data;
	Pop3Session *pop3_session = POP3_SESSION(session);
	IncProgressDialog *inc_dialog;
	gint cur_total;

	g_return_val_if_fail(inc_session != NULL, -1);

	if (pop3_session->state != POP3_RETR &&
	    pop3_session->state != POP3_RETR_RECV &&
	    pop3_session->state != POP3_DELETE &&
	    pop3_session->state != POP3_LOGOUT) return 0;

	if (!pop3_session->new_msg_exist) return 0;

	cur_total = pop3_session->cur_total_bytes + cur_len;
	if (cur_total > pop3_session->total_bytes)
		cur_total = pop3_session->total_bytes;
	inc_session->cur_total_bytes = cur_total;

	inc_dialog = (IncProgressDialog *)inc_session->data;
	inc_progress_dialog_update_periodic(inc_dialog, inc_session);

	return 0;
}

static gint inc_recv_data_finished(Session *session, guint len, gpointer data)
{
	IncSession *inc_session = (IncSession *)data;
	IncProgressDialog *inc_dialog;

	g_return_val_if_fail(inc_session != NULL, -1);

	inc_dialog = (IncProgressDialog *)inc_session->data;

	inc_recv_data_progressive(session, 0, 0, inc_session);

	if (POP3_SESSION(session)->state == POP3_LOGOUT) {
		inc_progress_dialog_update(inc_dialog, inc_session);
	}

	return 0;
}

static gint inc_recv_message(Session *session, const gchar *msg, gpointer data)
{
	IncSession *inc_session = (IncSession *)data;
	IncProgressDialog *inc_dialog;

	g_return_val_if_fail(inc_session != NULL, -1);

	inc_dialog = (IncProgressDialog *)inc_session->data;

	switch (POP3_SESSION(session)->state) {
	case POP3_GETAUTH_USER:
	case POP3_GETAUTH_PASS:
	case POP3_GETAUTH_APOP:
	case POP3_GETRANGE_STAT:
	case POP3_GETRANGE_LAST:
	case POP3_GETRANGE_UIDL:
	case POP3_GETSIZE_LIST:
		inc_progress_dialog_update(inc_dialog, inc_session);
		break;
	case POP3_RETR:
		inc_recv_data_progressive(session, 0, 0, inc_session);
		break;
	case POP3_LOGOUT:
		inc_progress_dialog_update(inc_dialog, inc_session);
		break;
	default:
		break;
	}

	return 0;
}

static gint inc_drop_message(Pop3Session *session, const gchar *file)
{
	FolderItem *inbox;
	FolderItem *dropfolder;
	IncSession *inc_session = (IncSession *)(SESSION(session)->data);
	gint msgnum;

	g_return_val_if_fail(inc_session != NULL, -1);

	if (session->ac_prefs->inbox) {
		inbox = folder_find_item_from_identifier
			(session->ac_prefs->inbox);
		if (!inbox)
			inbox = folder_get_default_inbox();
	} else
		inbox = folder_get_default_inbox();
	if (!inbox) {
		unlink(file);
		return -1;
	}

	/* CLAWS: claws uses a global .processing folder for the filtering. */
	dropfolder = folder_get_default_processing();

	/* add msg file to drop folder */
	if ((msgnum = folder_item_add_msg(
			dropfolder, file, NULL, TRUE)) < 0) {
		unlink(file);
		return -1;
	}

	return 0;
}

static void inc_put_error(IncState istate, Pop3Session *session)
{
	gchar *log_msg = NULL;
	gchar *err_msg = NULL;
	gboolean fatal_error = FALSE;

	switch (istate) {
	case INC_CONNECT_ERROR:
		log_msg = _("Connection failed.");
		if (prefs_common.no_recv_err_panel)
			break;
		err_msg = g_strdup_printf(_("Connection to %s:%d failed."),
					  SESSION(session)->server, 
					  SESSION(session)->port);
		break;
	case INC_ERROR:
		log_msg = _("Error occurred while processing mail.");
		if (prefs_common.no_recv_err_panel)
			break;
		if (session->error_msg)
			err_msg = g_strdup_printf
				(_("Error occurred while processing mail:\n%s"),
				 session->error_msg);
		else
			err_msg = g_strdup(log_msg);
		break;
	case INC_NO_SPACE:
		log_msg = _("No disk space left.");
		err_msg = g_strdup(log_msg);
		fatal_error = TRUE;
		break;
	case INC_IO_ERROR:
		log_msg = _("Can't write file.");
		err_msg = g_strdup(log_msg);
		fatal_error = TRUE;
		break;
	case INC_SOCKET_ERROR:
		log_msg = _("Socket error.");
		if (prefs_common.no_recv_err_panel)
			break;
		err_msg = g_strdup_printf(_("Socket error on connection to %s:%d."),
					  SESSION(session)->server, 
					  SESSION(session)->port);
		break;
	case INC_EOF:
		log_msg = _("Connection closed by the remote host.");
		if (prefs_common.no_recv_err_panel)
			break;
		err_msg = g_strdup_printf(_("Connection to %s:%d closed by the remote host."), 
					  SESSION(session)->server, 
					  SESSION(session)->port);
		break;
	case INC_LOCKED:
		log_msg = _("Mailbox is locked.");
		if (prefs_common.no_recv_err_panel)
			break;
		if (session->error_msg)
			err_msg = g_strdup_printf(_("Mailbox is locked:\n%s"),
						  session->error_msg);
		else
			err_msg = g_strdup(log_msg);
		break;
	case INC_AUTH_FAILED:
		log_msg = _("Authentication failed.");
		if (prefs_common.no_recv_err_panel)
			break;
		if (session->error_msg)
			err_msg = g_strdup_printf
				(_("Authentication failed:\n%s"), session->error_msg);
		else
			err_msg = g_strdup(log_msg);
		break;
	case INC_TIMEOUT:
		log_msg = _("Session timed out.");
		if (prefs_common.no_recv_err_panel)
			break;
		err_msg = g_strdup_printf(_("Connection to %s:%d timed out."), 
					  SESSION(session)->server, 
					  SESSION(session)->port);
		break;
	default:
		break;
	}

	if (log_msg) {
		if (fatal_error)
			log_error("%s\n", log_msg);
		else
			log_warning("%s\n", log_msg);
	}
	if (err_msg) {
		alertpanel_error_log(err_msg);
		g_free(err_msg);
	}
}

static void inc_cancel(IncProgressDialog *dialog)
{
	IncSession *session;

	g_return_if_fail(dialog != NULL);

	if (dialog->queue_list == NULL) {
		inc_progress_dialog_destroy(dialog);
		return;
	}

	session = dialog->queue_list->data;

	session->inc_state = INC_CANCEL;

	log_message(_("Incorporation cancelled\n"));
}

gboolean inc_is_active(void)
{
	return (inc_dialog_list != NULL);
}

void inc_cancel_all(void)
{
	GList *cur;

	for (cur = inc_dialog_list; cur != NULL; cur = cur->next)
		inc_cancel((IncProgressDialog *)cur->data);
}

static void inc_cancel_cb(GtkWidget *widget, gpointer data)
{
	inc_cancel((IncProgressDialog *)data);
}

static gint inc_dialog_delete_cb(GtkWidget *widget, GdkEventAny *event,
				 gpointer data)
{
	IncProgressDialog *dialog = (IncProgressDialog *)data;

	if (dialog->queue_list == NULL)
		inc_progress_dialog_destroy(dialog);

	return TRUE;
}

static gint inc_spool_account(PrefsAccount *account)
{
	FolderItem *inbox;
	gchar *mbox;
	gint result;

	if (account->inbox) {
		inbox = folder_find_item_from_path(account->inbox);
		if (!inbox)
			inbox = folder_get_default_inbox();
	} else
		inbox = folder_get_default_inbox();

	if (is_file_exist(account->local_mbox))
		mbox = g_strdup(account->local_mbox);
	else if (is_dir_exist(account->local_mbox)) 
		mbox = g_strconcat(account->local_mbox, G_DIR_SEPARATOR_S,
				   g_get_user_name(), NULL);
	else {
		debug_print("%s: local mailbox not found.\n", 
			    account->local_mbox);
		return -1;
	}
	
	result = get_spool(inbox, mbox);
	g_free(mbox);
	
	statusbar_pop_all();
	
	return result;
}

static gint inc_all_spool(void)
{
	GList *list = NULL;
	gint new_msgs = 0;
	gint account_new_msgs = 0;

	list = account_get_list();
	if (!list) return 0;

	for (; list != NULL; list = list->next) {
		PrefsAccount *account = list->data;

		if ((account->protocol == A_LOCAL) &&
		    (account->recv_at_getall)) {
			account_new_msgs = inc_spool_account(account);
			if (account_new_msgs > 0)
				new_msgs += account_new_msgs;
		}
	}

	return new_msgs;
}

static gint get_spool(FolderItem *dest, const gchar *mbox)
{
	gint msgs, size;
	gint lockfd;
	gchar tmp_mbox[MAXPATHLEN + 1];

	g_return_val_if_fail(dest != NULL, -1);
	g_return_val_if_fail(mbox != NULL, -1);

	if (!is_file_exist(mbox) || (size = get_file_size(mbox)) == 0) {
		debug_print("%s: no messages in local mailbox.\n", mbox);
		return 0;
	} else if (size < 0)
		return -1;

	if ((lockfd = lock_mbox(mbox, LOCK_FLOCK)) < 0)
		return -1;

	g_snprintf(tmp_mbox, sizeof(tmp_mbox), "%s%ctmpmbox.%08x",
		   get_tmp_dir(), G_DIR_SEPARATOR, (gint)mbox);

	if (copy_mbox(mbox, tmp_mbox) < 0) {
		unlock_mbox(mbox, lockfd, LOCK_FLOCK);
		return -1;
	}

	debug_print("Getting new messages from %s into %s...\n",
		    mbox, dest->path);

	msgs = proc_mbox(dest, tmp_mbox, TRUE);

	unlink(tmp_mbox);
	if (msgs >= 0) empty_mbox(mbox);
	unlock_mbox(mbox, lockfd, LOCK_FLOCK);

	return msgs;
}

void inc_lock(void)
{
	inc_lock_count++;
}

void inc_unlock(void)
{
	if (inc_lock_count > 0)
		inc_lock_count--;
}

static guint autocheck_timer = 0;
static gpointer autocheck_data = NULL;

static void inc_notify_cmd(gint new_msgs, gboolean notify)
{

	gchar *buf, *numpos;

	if (!(new_msgs && notify && prefs_common.newmail_notify_cmd &&
	    *prefs_common.newmail_notify_cmd))
		     return;
	buf = g_strdup(prefs_common.newmail_notify_cmd);
	if ((numpos = strstr(buf, "%d")) != NULL) {
		gchar *buf2;

		*numpos = '\0';
		buf2 = g_strdup_printf("%s%d%s", buf, new_msgs, numpos + 2);
		g_free(buf);
		buf = buf2;
	}

	debug_print("executing new mail notification command: %s\n", buf);
	execute_command_line(buf, TRUE);

	g_free(buf);
}
 
void inc_autocheck_timer_init(MainWindow *mainwin)
{
	autocheck_data = mainwin;
	inc_autocheck_timer_set();
}

static void inc_autocheck_timer_set_interval(guint interval)
{
	inc_autocheck_timer_remove();
	/* last test is to avoid re-enabling auto_check after modifying 
	   the common preferences */
	if (prefs_common.autochk_newmail && autocheck_data
	    && prefs_common.work_offline == FALSE) {
		autocheck_timer = gtk_timeout_add
			(interval, inc_autocheck_func, autocheck_data);
		debug_print("added timer = %d\n", autocheck_timer);
	}
}

void inc_autocheck_timer_set(void)
{
	inc_autocheck_timer_set_interval(prefs_common.autochk_itv * 60000);
}

void inc_autocheck_timer_remove(void)
{
	if (autocheck_timer) {
		debug_print("removed timer = %d\n", autocheck_timer);
		gtk_timeout_remove(autocheck_timer);
		autocheck_timer = 0;
	}
}

static gint inc_autocheck_func(gpointer data)
{
	MainWindow *mainwin = (MainWindow *)data;

	if (inc_lock_count) {
		debug_print("autocheck is locked.\n");
		inc_autocheck_timer_set_interval(1000);
		return FALSE;
	}

 	inc_all_account_mail(mainwin, TRUE, prefs_common.newmail_notify_auto);

	return FALSE;
}
