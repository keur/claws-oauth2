/*
 * Sylpheed -- a GTK+ based, lightweight, and fast e-mail client
 * Copyright (C) 1999-2006 Hiroyuki Yamamoto and the Sylpheed-Claws team
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef __LOGWINDOW_H__
#define __LOGWINDOW_H__

#include <glib.h>
#include <gtk/gtkwidget.h>

typedef struct _LogWindow	LogWindow;

struct _LogWindow
{
	GtkWidget *window;
	GtkWidget *scrolledwin;
	GtkWidget *text;

	GdkColor msg_color;
	GdkColor warn_color;
	GdkColor error_color;
	GdkColor in_color;
	GdkColor out_color;

	gboolean clip;
	guint	 clip_length;
	guint 	 hook_id;
	GtkTextBuffer *buffer;
	gboolean hidden;
};

LogWindow *log_window_create(void);
void log_window_init(LogWindow *logwin);
void log_window_show(LogWindow *logwin);
void log_window_set_clipping(LogWindow *logwin, gboolean clip, guint clip_length);

#endif /* __LOGWINDOW_H__ */
