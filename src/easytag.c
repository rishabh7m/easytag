/* EasyTAG - Tag editor for audio files
 * Copyright (C) 2014  David King <amigadave@amigadave.com>
 * Copyright (C) 2000-2003  Jerome Couderc <easytag@gmail.com>
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

#include "config.h" // For definition of ENABLE_OGG
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#ifdef ENABLE_MP3
#include <id3tag.h>
#endif
#if defined ENABLE_MP3 && defined ENABLE_ID3LIB
#include <id3.h>
#endif
#include <sys/types.h>

#include "easytag.h"
#include "application_window.h"
#include "browser.h"
#include "log.h"
#include "misc.h"
#include "cddb_dialog.h"
#include "preferences_dialog.h"
#include "setting.h"
#include "scan_dialog.h"
#include "mpeg_header.h"
#include "id3_tag.h"
#include "ogg_tag.h"
#include "et_core.h"
#include "picture.h"
#include "charset.h"

#include "win32/win32dep.h"


/****************
 * Declarations *
 ****************/
static GtkWidget *QuitRecursionWindow = NULL;

/* Used to force to hide the msgbox when saving tag */
static gboolean SF_HideMsgbox_Write_Tag;
/* To remember which button was pressed when saving tag */
static gint SF_ButtonPressed_Write_Tag;
/* Used to force to hide the msgbox when renaming file */
static gboolean SF_HideMsgbox_Rename_File;
/* To remember which button was pressed when renaming file */
static gint SF_ButtonPressed_Rename_File;

#ifdef ENABLE_FLAC
    #include <FLAC/metadata.h>
#endif


/**************
 * Prototypes *
 **************/
static gboolean Write_File_Tag (ET_File *ETFile, gboolean hide_msgbox);
static gint Save_File (ET_File *ETFile, gboolean multiple_files,
                       gboolean force_saving_files);
static gint Save_Selected_Files_With_Answer (gboolean force_saving_files);
static gint Save_List_Of_Files (GList *etfilelist,
                                gboolean force_saving_files);

static gboolean et_rename_file (const char *old_filepath,
                                const char *new_filepath, GError **error);
static GList *read_directory_recursively (GList *file_list,
                                          GFileEnumerator *dir_enumerator,
                                          gboolean recurse);
static void Open_Quit_Recursion_Function_Window (void);
static void Destroy_Quit_Recursion_Function_Window (void);
static void et_on_quit_recursion_response (GtkDialog *dialog, gint response_id,
                                           gpointer user_data);

/*
 * Select a file in the "main list" using the ETFile adress of each item.
 */
void Action_Select_Nth_File_By_Etfile (ET_File *ETFile)
{
    if (!ETCore->ETFileDisplayedList)
        return;

    /* Save the current displayed data */
    ET_Save_File_Data_From_UI(ETCore->ETFileDisplayed);

    /* Display the item */
    et_application_window_browser_select_file_by_et_file (ET_APPLICATION_WINDOW (MainWindow),
                                                          ETFile, TRUE);
    ET_Displayed_File_List_By_Etfile(ETFile); // Just to update 'ETFileDisplayedList'
    ET_Display_File_Data_To_UI(ETFile);

    et_application_window_update_actions (ET_APPLICATION_WINDOW (MainWindow));
    et_application_window_scan_dialog_update_previews (ET_APPLICATION_WINDOW (MainWindow));
}

/*
 * Action when Save button is pressed
 */
void Action_Save_Selected_Files (void)
{
    Save_Selected_Files_With_Answer(FALSE);
}

void Action_Force_Saving_Selected_Files (void)
{
    Save_Selected_Files_With_Answer(TRUE);
}


/*
 * Will save the full list of file (not only the selected files in list)
 * and check if we must save also only the changed files or all files
 * (force_saving_files==TRUE)
 */
gint Save_All_Files_With_Answer (gboolean force_saving_files)
{
    GList *etfilelist;

    g_return_val_if_fail (ETCore != NULL && ETCore->ETFileList != NULL, FALSE);

    etfilelist = g_list_first (ETCore->ETFileList);

    return Save_List_Of_Files (etfilelist, force_saving_files);
}

/*
 * Will save only the selected files in the file list
 */
static gint
Save_Selected_Files_With_Answer (gboolean force_saving_files)
{
    gint toreturn;
    GList *etfilelist = NULL;
    GList *selfilelist = NULL;
    GList *l;
    ET_File *etfile;
    GtkTreeSelection *selection;

    selection = et_application_window_browser_get_selection (ET_APPLICATION_WINDOW (MainWindow));
    selfilelist = gtk_tree_selection_get_selected_rows(selection, NULL);

    for (l = selfilelist; l != NULL; l = g_list_next (l))
    {
        etfile = et_application_window_browser_get_et_file_from_path (ET_APPLICATION_WINDOW (MainWindow),
                                                                      l->data);
        etfilelist = g_list_prepend (etfilelist, etfile);
    }

    g_list_free_full (selfilelist, (GDestroyNotify)gtk_tree_path_free);

    etfilelist = g_list_reverse (etfilelist);
    toreturn = Save_List_Of_Files(etfilelist, force_saving_files);
    g_list_free(etfilelist);
    return toreturn;
}

/*
 * Save_List_Of_Files: Function to save a list of files.
 *  - force_saving_files = TRUE => force saving the file even if it wasn't changed
 *  - force_saving_files = FALSE => force saving only the changed files
 */
static gint
Save_List_Of_Files (GList *etfilelist, gboolean force_saving_files)
{
    EtApplicationWindow *window;
    gint       progress_bar_index;
    gint       saving_answer;
    gint       nb_files_to_save;
    gint       nb_files_changed_by_ext_program;
    gchar     *msg;
    gchar      progress_bar_text[30];
    GList *l;
    ET_File   *etfile_save_position = NULL;
    File_Tag  *FileTag;
    File_Name *FileNameNew;
    double     fraction;
    GAction *action;
    GtkWidget *widget_focused;
    GtkTreePath *currentPath = NULL;

    g_return_val_if_fail (ETCore != NULL, FALSE);

    window = ET_APPLICATION_WINDOW (MainWindow);

    /* Save the current position in the list */
    etfile_save_position = ETCore->ETFileDisplayed;

    /* Save the current displayed data */
    ET_Save_File_Data_From_UI(ETCore->ETFileDisplayed);

    /* Save widget that has current focus, to give it again the focus after saving */
    widget_focused = gtk_window_get_focus(GTK_WINDOW(MainWindow));

    /* Count the number of files to save */
    /* Count the number of files changed by an external program */
    nb_files_to_save = 0;
    nb_files_changed_by_ext_program = 0;

    for (l = etfilelist; l != NULL; l = g_list_next (l))
    {
        GFile *file;
        GFileInfo *fileinfo;

        ET_File *ETFile = (ET_File *)l->data;
        File_Tag  *FileTag  = (File_Tag *)ETFile->FileTag->data;
        File_Name *FileName = (File_Name *)ETFile->FileNameNew->data;
        gchar *filename_cur = ((File_Name *)ETFile->FileNameCur->data)->value;
        gchar *filename_cur_utf8 = ((File_Name *)ETFile->FileNameCur->data)->value_utf8;
        gchar *basename_cur_utf8 = g_path_get_basename(filename_cur_utf8);

        // Count only the changed files or all files if force_saving_files==TRUE
        if ( force_saving_files
        || (FileName && FileName->saved==FALSE) || (FileTag && FileTag->saved==FALSE) )
            nb_files_to_save++;

        file = g_file_new_for_path (filename_cur);
        fileinfo = g_file_query_info (file, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                      G_FILE_QUERY_INFO_NONE, NULL, NULL);
        g_object_unref (file);

        if (fileinfo)
        {
            if (ETFile->FileModificationTime
                != g_file_info_get_attribute_uint64 (fileinfo,
                                                     G_FILE_ATTRIBUTE_TIME_MODIFIED))
            {
                nb_files_changed_by_ext_program++;
            }

            g_object_unref (fileinfo);
        }
        g_free(basename_cur_utf8);
    }

    /* Initialize status bar */
    et_application_window_progress_set_fraction (window, 0.0);
    progress_bar_index = 0;
    g_snprintf(progress_bar_text, 30, "%d/%d", progress_bar_index, nb_files_to_save);
    et_application_window_progress_set_text (window, progress_bar_text);

    /* Set to unsensitive all command buttons (except Quit button) */
    et_application_window_disable_command_actions (window);
    et_application_window_browser_set_sensitive (window, FALSE);
    et_application_window_tag_area_set_sensitive (window, FALSE);
    et_application_window_file_area_set_sensitive (window, FALSE);

    /* Show msgbox (if needed) to ask confirmation ('SF' for Save File) */
    SF_HideMsgbox_Write_Tag = FALSE;
    SF_HideMsgbox_Rename_File = FALSE;

    Main_Stop_Button_Pressed = FALSE;
    /* Activate the stop button. */
    action = g_action_map_lookup_action (G_ACTION_MAP (MainWindow), "stop");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), FALSE);

    /*
     * Check if file was changed by an external program
     */
    if (nb_files_changed_by_ext_program > 0)
    {
        // Some files were changed by other program than EasyTAG
        GtkWidget *msgdialog = NULL;
        gint response;

        msgdialog = gtk_message_dialog_new(GTK_WINDOW(MainWindow),
                                           GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                           GTK_MESSAGE_WARNING,
                                           GTK_BUTTONS_NONE,
                                           ngettext ("A file was changed by an external program",
                                                     "%d files were changed by an external program",
                                                     nb_files_changed_by_ext_program),
                                           nb_files_changed_by_ext_program);
        gtk_dialog_add_buttons (GTK_DIALOG (msgdialog), _("_Discard"),
                                GTK_RESPONSE_NO, _("_Save"), GTK_RESPONSE_YES,
                                NULL);
        gtk_dialog_set_default_response (GTK_DIALOG (msgdialog),
                                         GTK_RESPONSE_YES);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(msgdialog),"%s",_("Do you want to continue saving the file?"));
        gtk_window_set_title(GTK_WINDOW(msgdialog),_("Quit"));

        response = gtk_dialog_run(GTK_DIALOG(msgdialog));
        gtk_widget_destroy(msgdialog);

        switch (response)
        {
            case GTK_RESPONSE_YES:
                break;
            case GTK_RESPONSE_NO:
            case GTK_RESPONSE_DELETE_EVENT:
                /* Skip the following loop. */
                Main_Stop_Button_Pressed = TRUE;
                break;
            default:
                g_assert_not_reached ();
                break;
        }
    }

    for (l = etfilelist; l != NULL && !Main_Stop_Button_Pressed;
         l = g_list_next (l))
    {
        FileTag = ((ET_File *)l->data)->FileTag->data;
        FileNameNew = ((ET_File *)l->data)->FileNameNew->data;

        /* We process only the files changed and not saved, or we force to save all
         * files if force_saving_files==TRUE */
        if ( force_saving_files
        || FileTag->saved == FALSE || FileNameNew->saved == FALSE )
        {
            /* ET_Display_File_Data_To_UI ((ET_File *)l->data);
             * Use of 'currentPath' to try to increase speed. Indeed, in many
             * cases, the next file to select, is the next in the list. */
            currentPath = et_application_window_browser_select_file_by_et_file2 (window,
                                                                                (ET_File *)l->data,
                                                                                FALSE,
                                                                                currentPath);

            fraction = (++progress_bar_index) / (double) nb_files_to_save;
            et_application_window_progress_set_fraction (window, fraction);
            g_snprintf(progress_bar_text, 30, "%d/%d", progress_bar_index, nb_files_to_save);
            et_application_window_progress_set_text (window,
                                                     progress_bar_text);

            /* Needed to refresh status bar */
            while (gtk_events_pending())
                gtk_main_iteration();

            // Save tag and rename file
            saving_answer = Save_File ((ET_File *)l->data,
                                       nb_files_to_save > 1 ? TRUE : FALSE,
                                       force_saving_files);

            if (saving_answer == -1)
            {
                /* Stop saving files + reinit progress bar */
                et_application_window_progress_set_text (window, "");
                et_application_window_progress_set_fraction (window, 0.0);
                et_application_window_status_bar_message (window,
                                                          _("Saving files was stopped"),
                                                          TRUE);
                /* To update state of command buttons */
                et_application_window_update_actions (window);
                et_application_window_browser_set_sensitive (window, TRUE);
                et_application_window_tag_area_set_sensitive (window, TRUE);
                et_application_window_file_area_set_sensitive (window, TRUE);

                if (currentPath)
                {
                    gtk_tree_path_free (currentPath);
                }
                return -1; /* We stop all actions */
            }
        }
    }

    if (currentPath)
        gtk_tree_path_free(currentPath);

    if (Main_Stop_Button_Pressed)
        msg = g_strdup (_("Saving files was stopped"));
    else
        msg = g_strdup (_("All files have been saved"));

    Main_Stop_Button_Pressed = FALSE;
    action = g_action_map_lookup_action (G_ACTION_MAP (MainWindow), "stop");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), FALSE);

    /* Return to the saved position in the list */
    ET_Display_File_Data_To_UI(etfile_save_position);
    et_application_window_browser_select_file_by_et_file (ET_APPLICATION_WINDOW (MainWindow),
                                                          etfile_save_position,
                                                          TRUE);

    et_application_window_browser_toggle_display_mode (window);

    /* To update state of command buttons */
    et_application_window_update_actions (ET_APPLICATION_WINDOW (MainWindow));
    et_application_window_browser_set_sensitive (window, TRUE);
    et_application_window_tag_area_set_sensitive (window, TRUE);
    et_application_window_file_area_set_sensitive (window, TRUE);

    /* Give again focus to the first entry, else the focus is passed to another */
    gtk_widget_grab_focus(GTK_WIDGET(widget_focused));

    et_application_window_progress_set_text (window, "");
    et_application_window_progress_set_fraction (window, 0.0);
    et_application_window_status_bar_message (window, msg, TRUE);
    g_free(msg);
    et_application_window_browser_refresh_list (window);
    return TRUE;
}



/*
 * Save changes of the ETFile (write tag and rename file)
 *  - multiple_files = TRUE  : when saving files, a msgbox appears with ability
 *                             to do the same action for all files.
 *  - multiple_files = FALSE : appears only a msgbox to ask confirmation.
 */
static gint
Save_File (ET_File *ETFile, gboolean multiple_files,
           gboolean force_saving_files)
{
    File_Tag  *FileTag;
    File_Name *FileNameNew;
    gint stop_loop = 0;
    //struct stat   statbuf;
    //gchar *filename_cur = ((File_Name *)ETFile->FileNameCur->data)->value;
    gchar *filename_cur_utf8 = ((File_Name *)ETFile->FileNameCur->data)->value_utf8;
    gchar *filename_new_utf8 = ((File_Name *)ETFile->FileNameNew->data)->value_utf8;
    gchar *basename_cur_utf8, *basename_new_utf8;
    gchar *dirname_cur_utf8, *dirname_new_utf8;

    g_return_val_if_fail (ETFile != NULL, 0);

    basename_cur_utf8 = g_path_get_basename(filename_cur_utf8);
    basename_new_utf8 = g_path_get_basename(filename_new_utf8);

    /* Save the current displayed data */
    //ET_Save_File_Data_From_UI((ET_File *)ETFileList->data); // Not needed, because it was done before
    FileTag     = ETFile->FileTag->data;
    FileNameNew = ETFile->FileNameNew->data;

    /*
     * Check if file was changed by an external program
     */
    /*stat(filename_cur,&statbuf);
    if (ETFile->FileModificationTime != statbuf.st_mtime)
    {
        // File was changed
        GtkWidget *msgbox = NULL;
        gint response;

        msg = g_strdup_printf(_("The file '%s' was changed by an external program.\nDo you want to continue?"),basename_cur_utf8);
        msgbox = msg_box_new(_("Write File"),
                             GTK_WINDOW(MainWindow),
                             NULL,
                             GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                             msg,
                             GTK_STOCK_DIALOG_WARNING,
                             GTK_STOCK_NO,  GTK_RESPONSE_NO,
                             GTK_STOCK_YES, GTK_RESPONSE_YES,
                             NULL);
        g_free(msg);

        response = gtk_dialog_run(GTK_DIALOG(msgbox));
        gtk_widget_destroy(msgbox);

        switch (response)
        {
            case GTK_RESPONSE_YES:
                break;
            case GTK_RESPONSE_NO:
            case GTK_RESPONSE_NONE:
                stop_loop = -1;
                return stop_loop;
                break;
        }
    }*/


    /*
     * First part: write tag information (artist, title,...)
     */
    // Note : the option 'force_saving_files' is only used to save tags
    if ( force_saving_files
    || FileTag->saved == FALSE ) // This tag had been already saved ?
    {
        GtkWidget *msgdialog = NULL;
        GtkWidget *msgdialog_check_button = NULL;
        gint response;

        if (g_settings_get_boolean (MainSettings, "confirm-rename-file")
            && !SF_HideMsgbox_Write_Tag)
        {
            // ET_Display_File_Data_To_UI(ETFile);

            msgdialog = gtk_message_dialog_new(GTK_WINDOW(MainWindow),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_QUESTION,
                                               GTK_BUTTONS_NONE,
                                               _("Do you want to write the tag of file ‘%s’?"),
                                               basename_cur_utf8);
            gtk_window_set_title(GTK_WINDOW(msgdialog),_("Confirm Tag Writing"));
            if (multiple_files)
            {
                GtkWidget *message_area;
                message_area = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(msgdialog));
                msgdialog_check_button = gtk_check_button_new_with_label(_("Repeat action for the remaining files"));
                gtk_container_add(GTK_CONTAINER(message_area),msgdialog_check_button);
                gtk_widget_show (msgdialog_check_button);
                gtk_dialog_add_buttons (GTK_DIALOG (msgdialog),
                                        _("_Discard"), GTK_RESPONSE_NO,
                                        _("_Cancel"), GTK_RESPONSE_CANCEL,
                                        _("_Save"), GTK_RESPONSE_YES, NULL);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(msgdialog_check_button), TRUE); // Checked by default
            }
            else
            {
                gtk_dialog_add_buttons (GTK_DIALOG (msgdialog),
                                        _("_Cancel"), GTK_RESPONSE_NO,
                                        _("_Save"), GTK_RESPONSE_YES, NULL);
            }

            gtk_dialog_set_default_response (GTK_DIALOG (msgdialog),
                                             GTK_RESPONSE_YES);
            SF_ButtonPressed_Write_Tag = response = gtk_dialog_run(GTK_DIALOG(msgdialog));
            // When check button in msgbox was activated : do not display the message again
            if (msgdialog_check_button && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(msgdialog_check_button)))
                SF_HideMsgbox_Write_Tag = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(msgdialog_check_button));
            gtk_widget_destroy(msgdialog);
        }else
        {
            if (SF_HideMsgbox_Write_Tag)
                response = SF_ButtonPressed_Write_Tag;
            else
                response = GTK_RESPONSE_YES;
        }

        switch (response)
        {
            case GTK_RESPONSE_YES:
            {
                gboolean rc;

                // if 'SF_HideMsgbox_Write_Tag is TRUE', then errors are displayed only in log
                rc = Write_File_Tag(ETFile,SF_HideMsgbox_Write_Tag);
                // if an error occurs when 'SF_HideMsgbox_Write_Tag is TRUE', we don't stop saving...
                if (rc != TRUE && !SF_HideMsgbox_Write_Tag)
                {
                    stop_loop = -1;
                    return stop_loop;
                }
                break;
            }
            case GTK_RESPONSE_NO:
                break;
            case GTK_RESPONSE_CANCEL:
            case GTK_RESPONSE_DELETE_EVENT:
                stop_loop = -1;
                return stop_loop;
                break;
            default:
                g_assert_not_reached ();
                break;
        }
    }


    /*
     * Second part: rename the file
     */
    // Do only if changed! (don't take force_saving_files into account)
    if ( FileNameNew->saved == FALSE ) // This filename had been already saved ?
    {
        GtkWidget *msgdialog = NULL;
        GtkWidget *msgdialog_check_button = NULL;
        gint response;

        if (g_settings_get_boolean (MainSettings, "confirm-rename-file")
            && !SF_HideMsgbox_Rename_File)
        {
            gchar *msgdialog_title = NULL;
            gchar *msg = NULL;
            gchar *msg1 = NULL;
            // ET_Display_File_Data_To_UI(ETFile);

            dirname_cur_utf8 = g_path_get_dirname(filename_cur_utf8);
            dirname_new_utf8 = g_path_get_dirname(filename_new_utf8);

            // Directories were renamed? or only filename?
            if (g_utf8_collate(dirname_cur_utf8,dirname_new_utf8) != 0)
            {
                if (g_utf8_collate(basename_cur_utf8,basename_new_utf8) != 0)
                {
                    // Directories and filename changed
                    msgdialog_title = g_strdup (_("Rename File and Directory"));
                    msg = g_strdup(_("File and directory rename confirmation required"));
                    msg1 = g_strdup_printf (_("Do you want to rename the file and directory ‘%s’ to ‘%s’?"),
                                           filename_cur_utf8, filename_new_utf8);
                }else
                {
                    // Only directories changed
                    msgdialog_title = g_strdup (_("Rename Directory"));
                    msg = g_strdup(_("Directory rename confirmation required"));
                    msg1 = g_strdup_printf (_("Do you want to rename the directory ‘%s’ to ‘%s’?"),
                                            dirname_cur_utf8,
                                            dirname_new_utf8);
                }
            }else
            {
                // Only filename changed
                msgdialog_title = g_strdup (_("Rename File"));
                msg = g_strdup(_("File rename confirmation required"));
                msg1 = g_strdup_printf (_("Do you want to rename the file ‘%s’ to ‘%s’?"),
                                       basename_cur_utf8, basename_new_utf8);
            }

            g_free(dirname_cur_utf8);
            g_free(dirname_new_utf8);

            msgdialog = gtk_message_dialog_new(GTK_WINDOW(MainWindow),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_QUESTION,
                                               GTK_BUTTONS_NONE,
                                               "%s",
                                               msg);
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(msgdialog),"%s",msg1);
            gtk_window_set_title(GTK_WINDOW(msgdialog),msgdialog_title);
            if (multiple_files)
            {
                GtkWidget *message_area;
                message_area = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(msgdialog));
                msgdialog_check_button = gtk_check_button_new_with_label(_("Repeat action for the remaining files"));
                gtk_container_add(GTK_CONTAINER(message_area),msgdialog_check_button);
                gtk_widget_show (msgdialog_check_button);
                gtk_dialog_add_buttons (GTK_DIALOG (msgdialog), _("_Discard"),
                                        GTK_RESPONSE_NO, _("_Cancel"),
                                        GTK_RESPONSE_CANCEL, _("_Save"),
                                        GTK_RESPONSE_YES, NULL);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(msgdialog_check_button), TRUE); // Checked by default
            }
            else
            {
                gtk_dialog_add_buttons (GTK_DIALOG (msgdialog), _("_Discard"),
                                        GTK_RESPONSE_NO, _("_Save"),
                                        GTK_RESPONSE_YES, NULL);
            }
            g_free(msg);
            g_free(msg1);
            g_free(msgdialog_title);
            gtk_dialog_set_default_response (GTK_DIALOG (msgdialog),
                                             GTK_RESPONSE_YES);
            SF_ButtonPressed_Rename_File = response = gtk_dialog_run(GTK_DIALOG(msgdialog));
            if (msgdialog_check_button && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(msgdialog_check_button)))
                SF_HideMsgbox_Rename_File = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(msgdialog_check_button));
            gtk_widget_destroy(msgdialog);
        }else
        {
            if (SF_HideMsgbox_Rename_File)
                response = SF_ButtonPressed_Rename_File;
            else
                response = GTK_RESPONSE_YES;
        }

        switch(response)
        {
            case GTK_RESPONSE_YES:
            {
                gboolean rc;
                GError *error = NULL;
                gchar *cur_filename = ((File_Name *)ETFile->FileNameCur->data)->value;
                gchar *new_filename = ((File_Name *)ETFile->FileNameNew->data)->value;
                rc = et_rename_file (cur_filename, new_filename, &error);

                // if 'SF_HideMsgbox_Rename_File is TRUE', then errors are displayed only in log
                if (!rc)
                {
                    if (!SF_HideMsgbox_Rename_File)
                    {
                        GtkWidget *msgdialog;

                        msgdialog = gtk_message_dialog_new (GTK_WINDOW (MainWindow),
                                                            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                            GTK_MESSAGE_ERROR,
                                                            GTK_BUTTONS_CLOSE,
                                                            _("Cannot rename file ‘%s’ to ‘%s’"),
                                                            filename_cur_utf8,
                                                            filename_new_utf8);
                        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (msgdialog),
                                                                  "%s",
                                                                  error->message);
                        gtk_window_set_title (GTK_WINDOW (msgdialog),
                                              _("Rename File Error"));

                        gtk_dialog_run (GTK_DIALOG (msgdialog));
                        gtk_widget_destroy (msgdialog);
                    }

                    Log_Print (LOG_ERROR,
                               _("Cannot rename file ‘%s’ to ‘%s’: %s"),
                               filename_cur_utf8, filename_new_utf8,
                               error->message);

                    et_application_window_status_bar_message (ET_APPLICATION_WINDOW (MainWindow),
                                                              _("File(s) not renamed"),
                                                              TRUE);
                    g_error_free (error);
                }

                // if an error occurs when 'SF_HideMsgbox_Rename_File is TRUE', we don't stop saving...
                if (!rc && !SF_HideMsgbox_Rename_File)
                {
                    stop_loop = -1;
                    return stop_loop;
                }

                /* Mark after renaming files. */
                ETFile->FileNameCur = ETFile->FileNameNew;
                ET_Mark_File_Name_As_Saved (ETFile);
                break;
            }
            case GTK_RESPONSE_NO:
                break;
            case GTK_RESPONSE_CANCEL:
            case GTK_RESPONSE_DELETE_EVENT:
                stop_loop = -1;
                return stop_loop;
                break;
            default:
                g_assert_not_reached ();
                break;
        }
    }

    g_free(basename_cur_utf8);
    g_free(basename_new_utf8);

    /* Refresh file into browser list */
    // Browser_List_Refresh_File_In_List(ETFile);

    return 1;
}

/*
 * et_rename_file:
 * @old_filepath: path of file to be renamed
 * @new_filepath: path of renamed file
 * @error: a #GError to provide information on errors, or %NULL to ignore
 *
 * Rename @old_filepath to @new_filepath.
 *
 * Returns: %TRUE if the rename was successful, %FALSE otherwise
 */
static gboolean
et_rename_file (const char *old_filepath, const char *new_filepath,
                GError **error)
{
    GFile *file_old;
    GFile *file_new;
    GFile *file_new_parent;

    g_return_val_if_fail (old_filepath != NULL && new_filepath != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    file_old = g_file_new_for_path (old_filepath);
    file_new = g_file_new_for_path (new_filepath);
    file_new_parent = g_file_get_parent (file_new);

    if (!g_file_make_directory_with_parents (file_new_parent, NULL, error))
    {
        /* Ignore an error if the directory already exists. */
        if (!g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
            g_object_unref (file_old);
            g_object_unref (file_new);
            g_object_unref (file_new_parent);
            g_assert (error == NULL || *error != NULL);
            return FALSE;
        }
        g_clear_error (error);
    }

    g_assert (error == NULL || *error == NULL);
    g_object_unref (file_new_parent);

    /* Move the file. */
    if (!g_file_move (file_old, file_new, G_FILE_COPY_NONE, NULL, NULL, NULL,
                      error))
    {
        if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
            /* Possibly a case change on a case-insensitive filesystem. */
            /* TODO: casefold the paths of both files, and check to see whether
             * they only differ by case? */
            gchar *tmp_filename;
            mode_t old_mode;
            gint fd;
            GFile *tmp_file;
            GError *tmp_error = NULL;

            tmp_filename = g_strconcat (old_filepath, ".XXXXXX", NULL);

            old_mode = umask (077);
            fd = mkstemp (tmp_filename);
            umask (old_mode);

            if (fd >= 0)
            {
                close (fd);
            }

            tmp_file = g_file_new_for_path (tmp_filename);
            g_free (tmp_filename);

            if (!g_file_move (file_old, tmp_file, G_FILE_COPY_OVERWRITE, NULL,
                              NULL, NULL, &tmp_error))
            {
                g_file_delete (tmp_file, NULL, NULL);

                g_object_unref (tmp_file);
                g_clear_error (error);
                g_propagate_error (error, tmp_error);
                goto err;
            }
            else
            {
                /* Move to temporary file succeeded, now move to the real new
                 * location. */
                if (!g_file_move (tmp_file, file_new, G_FILE_COPY_NONE, NULL,
                                  NULL, NULL, &tmp_error))
                {
                    g_file_move (tmp_file, file_old, G_FILE_COPY_NONE, NULL,
                                 NULL, NULL, NULL);
                    g_object_unref (tmp_file);
                    g_clear_error (error);
                    g_propagate_error (error, tmp_error);
                    goto err;
                }
                else
                {
                    /* Move succeeded, so clear the original error about the
                     * new file already existing. */
                    g_object_unref (tmp_file);
                    g_clear_error (error);
                    goto out;
                }
            }
        }
        else
        {
            /* Error moving file. */
            goto err;
        }
    }

out:
    g_object_unref (file_old);
    g_object_unref (file_new);
    g_assert (error == NULL || *error == NULL);
    return TRUE;

err:
    g_object_unref (file_new_parent);
    g_object_unref (file_old);
    g_object_unref (file_new);
    g_assert (error == NULL || *error != NULL);
    return FALSE;
}

/*
 * Write tag of the ETFile
 * Return TRUE => OK
 *        FALSE => error
 */
static gboolean
Write_File_Tag (ET_File *ETFile, gboolean hide_msgbox)
{
    GError *error = NULL;
    gchar *cur_filename_utf8 = ((File_Name *)ETFile->FileNameCur->data)->value_utf8;
    gchar *msg = NULL;
    gchar *basename_utf8;
    GtkWidget *msgdialog;

    basename_utf8 = g_path_get_basename(cur_filename_utf8);
    msg = g_strdup_printf (_("Writing tag of ‘%s’"),basename_utf8);
    et_application_window_status_bar_message (ET_APPLICATION_WINDOW (MainWindow),
                                              msg, TRUE);
    g_free(msg);
    msg = NULL;

    if (ET_Save_File_Tag_To_HD (ETFile, &error))
    {
        msg = g_strdup_printf (_("Wrote tag of ‘%s’"), basename_utf8);
        et_application_window_status_bar_message (ET_APPLICATION_WINDOW (MainWindow),
                                                  msg, TRUE);
        g_free (msg);
        g_free (basename_utf8);
        return TRUE;
    }

    Log_Print (LOG_ERROR, "%s", error->message);

    if (!hide_msgbox)
    {
        msgdialog = gtk_message_dialog_new(GTK_WINDOW(MainWindow),
                             GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                             GTK_MESSAGE_ERROR,
                             GTK_BUTTONS_CLOSE,
                             _("Cannot write tag in file ‘%s’"),
                             basename_utf8);
        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (msgdialog),
                                                  "%s", error->message);
        gtk_window_set_title(GTK_WINDOW(msgdialog),_("Tag Write Error"));

        gtk_dialog_run(GTK_DIALOG(msgdialog));
        gtk_widget_destroy(msgdialog);
    }

    g_clear_error (&error);
    g_free(basename_utf8);

    return FALSE;
}

/*
 * Scans the specified directory: and load files into a list.
 * If the path doesn't exist, we free the previous loaded list of files.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
gboolean Read_Directory (gchar *path_real)
{
    GFile *dir;
    GFileEnumerator *dir_enumerator;
    GError *error = NULL;
    gchar *msg;
    gchar  progress_bar_text[30];
    guint  nbrfile = 0;
    double fraction;
    GList *FileList = NULL;
    GList *l;
    gint   progress_bar_index = 0;
    GAction *action;
    EtApplicationWindow *window;

    g_return_val_if_fail (path_real != NULL, FALSE);

    ReadingDirectory = TRUE;    /* A flag to avoid to start another reading */

    /* Initialize file list */
    ET_Core_Free();
    ET_Core_Initialize();
    et_application_window_update_actions (ET_APPLICATION_WINDOW (MainWindow));

    window = ET_APPLICATION_WINDOW (MainWindow);

    /* Initialize browser list */
    et_application_window_browser_clear (window);

    /* Clear entry boxes  */
    et_application_window_file_area_clear (window);
    et_application_window_tag_area_clear (window);

    // Set to unsensitive the Browser Area, to avoid to select another file while loading the first one
    et_application_window_browser_set_sensitive (window, FALSE);

    /* Placed only here, to empty the previous list of files */
    dir = g_file_new_for_path (path_real);
    dir_enumerator = g_file_enumerate_children (dir,
                                                G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                                G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                                G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
                                                G_FILE_QUERY_INFO_NONE,
                                                NULL, &error);
    if (!dir_enumerator)
    {
        // Message if the directory doesn't exist...
        GtkWidget *msgdialog;
        gchar *path_utf8 = filename_to_display(path_real);

        msgdialog = gtk_message_dialog_new(GTK_WINDOW(MainWindow),
                                           GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                           GTK_MESSAGE_ERROR,
                                           GTK_BUTTONS_CLOSE,
                                           _("Cannot read directory ‘%s’"),
                                           path_utf8);
        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (msgdialog),
                                                  "%s", error->message);
        gtk_window_set_title(GTK_WINDOW(msgdialog),_("Directory Read Error"));

        gtk_dialog_run(GTK_DIALOG(msgdialog));
        gtk_widget_destroy(msgdialog);
        g_free(path_utf8);

        ReadingDirectory = FALSE; //Allow a new reading
        et_application_window_browser_set_sensitive (window, TRUE);
        g_object_unref (dir);
        g_error_free (error);
        return FALSE;
    }

    /* Open the window to quit recursion (since 27/04/2007 : not only into recursion mode) */
    Set_Busy_Cursor();
    action = g_action_map_lookup_action (G_ACTION_MAP (MainWindow), "stop");
    g_settings_bind (MainSettings, "browse-subdir", G_SIMPLE_ACTION (action),
                     "enabled", G_SETTINGS_BIND_GET);
    Open_Quit_Recursion_Function_Window();

    /* Read the directory recursively */
    msg = g_strdup_printf(_("Search in progress…"));
    et_application_window_status_bar_message (window, msg, FALSE);
    g_free (msg);
    /* Search the supported files. */
    FileList = read_directory_recursively (FileList, dir_enumerator,
                                           g_settings_get_boolean (MainSettings,
                                                                   "browse-subdir"));
    g_file_enumerator_close (dir_enumerator, NULL, &error);
    g_object_unref (dir_enumerator);
    g_object_unref (dir);

    nbrfile = g_list_length(FileList);

    et_application_window_progress_set_fraction (window, 0.0);
    g_snprintf(progress_bar_text, 30, "%d/%d", 0, nbrfile);
    et_application_window_progress_set_text (window, progress_bar_text);

    // Load the supported files (Extension recognized)
    for (l = FileList; l != NULL && !Main_Stop_Button_Pressed;
         l = g_list_next (l))
    {
        gchar *filename_real = l->data; /* Contains real filenames. */
        gchar *filename_utf8 = filename_to_display(filename_real);

        msg = g_strdup_printf (_("File: ‘%s’"), filename_utf8);
        et_application_window_status_bar_message (window, msg, FALSE);
        g_free(msg);
        g_free(filename_utf8);

        // Warning: Do not free filename_real because ET_Add_File.. uses it for internal structures
        ET_Add_File_To_File_List(filename_real);

        /* Update the progress bar. */
        fraction = (++progress_bar_index) / (double) nbrfile;
        et_application_window_progress_set_fraction (window, fraction);
        g_snprintf(progress_bar_text, 30, "%d/%d", progress_bar_index, nbrfile);
        et_application_window_progress_set_text (window, progress_bar_text);
        while (gtk_events_pending())
            gtk_main_iteration();
    }

    /* Just free the list, not the data. */
    g_list_free (FileList);
    et_application_window_progress_set_text (window, "");

    /* Close window to quit recursion */
    Destroy_Quit_Recursion_Function_Window();
    Main_Stop_Button_Pressed = FALSE;
    action = g_action_map_lookup_action (G_ACTION_MAP (MainWindow), "stop");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), FALSE);

    //ET_Debug_Print_File_List(ETCore->ETFileList,__FILE__,__LINE__,__FUNCTION__);

    if (ETCore->ETFileList)
    {
        //GList *etfilelist;
        /* Load the list of file into the browser list widget */
        et_application_window_browser_toggle_display_mode (window);

        /* Display the first file */
        //No need to select first item, because Browser_Display_Tree_Or_Artist_Album_List() does this
        //etfilelist = ET_Displayed_File_List_First();
        //if (etfilelist)
        //{
        //    ET_Display_File_Data_To_UI((ET_File *)etfilelist->data);
        //    Browser_List_Select_File_By_Etfile((ET_File *)etfilelist->data,FALSE);
        //}

        /* Prepare message for the status bar */
        if (g_settings_get_boolean (MainSettings, "browse-subdir"))
        {
            msg = g_strdup_printf (ngettext ("Found one file in this directory and subdirectories",
                                             "Found %d files in this directory and subdirectories",
                                             ETCore->ETFileDisplayedList_Length),
                                   ETCore->ETFileDisplayedList_Length);
        }
        else
        {
            msg = g_strdup_printf (ngettext ("Found one file in this directory",
                                             "Found %d files in this directory",
                                             ETCore->ETFileDisplayedList_Length),
                                   ETCore->ETFileDisplayedList_Length);
        }
    }else
    {
        /* Clear entry boxes */
        et_application_window_file_area_clear (ET_APPLICATION_WINDOW (MainWindow));
        et_application_window_tag_area_clear (ET_APPLICATION_WINDOW (MainWindow));

	/* Translators: No files, as in "0 files". */
        et_application_window_browser_label_set_text (ET_APPLICATION_WINDOW (MainWindow),
                                                      _("No files")); /* See in ET_Display_Filename_To_UI */

        /* Prepare message for the status bar */
        if (g_settings_get_boolean (MainSettings, "browse-subdir"))
            msg = g_strdup(_("No file found in this directory and subdirectories"));
        else
            msg = g_strdup(_("No file found in this directory"));
    }

    /* Update sensitivity of buttons and menus */
    et_application_window_update_actions (window);

    et_application_window_browser_set_sensitive (window, TRUE);

    et_application_window_progress_set_fraction (window, 0.0);
    et_application_window_status_bar_message (window, msg, FALSE);
    g_free (msg);
    Set_Unbusy_Cursor();
    ReadingDirectory = FALSE;

    return TRUE;
}



/*
 * Recurse the path to create a list of files. Return a GList of the files found.
 */
static GList *
read_directory_recursively (GList *file_list, GFileEnumerator *dir_enumerator,
                            gboolean recurse)
{
    GError *error = NULL;
    GFileInfo *info;
    const char *file_name;
    gboolean is_hidden;
    GFileType type;

    g_return_val_if_fail (dir_enumerator != NULL, file_list);

    while ((info = g_file_enumerator_next_file (dir_enumerator, NULL, &error))
           != NULL)
    {
        if (Main_Stop_Button_Pressed)
        {
            g_object_unref (info);
            return file_list;
        }

        file_name = g_file_info_get_name (info);
        is_hidden = g_file_info_get_is_hidden (info);
        type = g_file_info_get_file_type (info);

        /* Hidden directory like '.mydir' will also be browsed if allowed. */
        if (!is_hidden || (g_settings_get_boolean (MainSettings,
                                                   "browse-show-hidden")
                           && is_hidden))
        {
            if (type == G_FILE_TYPE_DIRECTORY)
            {
                if (recurse)
                {
                    /* Searching for files recursively. */
                    GFile *child_dir = g_file_get_child (g_file_enumerator_get_container (dir_enumerator),
                                                         file_name);
                    GFileEnumerator *childdir_enumerator;
                    GError *child_error = NULL;
                    childdir_enumerator = g_file_enumerate_children (child_dir,
                                                                     G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                                                     G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                                                     G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
                                                                     G_FILE_QUERY_INFO_NONE,
                                                                     NULL, &child_error);
                    if (!childdir_enumerator)
                    {
                        Log_Print (LOG_ERROR,
                                   _("Error opening directory ‘%s’: %s"),
                                   file_name, child_error->message);
                        g_error_free (child_error);
                        g_object_unref (child_dir);
                        g_object_unref (info);
                        continue;
                    }
                    file_list = read_directory_recursively (file_list,
                                                            childdir_enumerator,
                                                            recurse);
                    g_object_unref (child_dir);
                    g_file_enumerator_close (childdir_enumerator, NULL,
                                             &error);
                    g_object_unref (childdir_enumerator);
                }
            }
            else if (type == G_FILE_TYPE_REGULAR &&
                      ET_File_Is_Supported (file_name))
            {
                GFile *file = g_file_get_child (g_file_enumerator_get_container (dir_enumerator),
                                                file_name);
                gchar *file_path = g_file_get_path (file);
                /*Do not free this file_path, it will be used by g_list*/
                file_list = g_list_append (file_list, file_path);
                g_object_unref (file);
            }

            // Just to not block X events
            while (gtk_events_pending())
                gtk_main_iteration();
        }
        g_object_unref (info);
    }

    if (error)
    {
        Log_Print (LOG_ERROR, _("Cannot read directory ‘%s’"), error->message);
        g_error_free (error);
    }

    return file_list;
}

/*
 * Window with the 'STOP' button to stop recursion when reading directories
 */
static void
Open_Quit_Recursion_Function_Window (void)
{
    if (QuitRecursionWindow != NULL)
        return;
    QuitRecursionWindow = gtk_message_dialog_new (GTK_WINDOW (MainWindow),
                                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  GTK_MESSAGE_OTHER,
                                                  GTK_BUTTONS_NONE,
                                                  "%s",
                                                  _("Searching for audio files…"));
    gtk_window_set_title (GTK_WINDOW (QuitRecursionWindow), _("Searching"));
    gtk_dialog_add_button (GTK_DIALOG (QuitRecursionWindow), _("_Stop"),
                           GTK_RESPONSE_CANCEL);

    g_signal_connect (G_OBJECT (QuitRecursionWindow),"response",
                      G_CALLBACK (et_on_quit_recursion_response), NULL);

    gtk_widget_show_all(QuitRecursionWindow);
}

static void
Destroy_Quit_Recursion_Function_Window (void)
{
    if (QuitRecursionWindow)
    {
        gtk_widget_destroy(QuitRecursionWindow);
        QuitRecursionWindow = NULL;
        /*Statusbar_Message(_("Recursive file search interrupted."),FALSE);*/
    }
}

static void
et_on_quit_recursion_response (GtkDialog *dialog, gint response_id,
                               gpointer user_data)
{
    switch (response_id)
    {
        case GTK_RESPONSE_CANCEL:
            Action_Main_Stop_Button_Pressed ();
            Destroy_Quit_Recursion_Function_Window ();
            break;
        case GTK_RESPONSE_DELETE_EVENT:
            Destroy_Quit_Recursion_Function_Window ();
            break;
        default:
            g_assert_not_reached ();
            break;
    }
}

/*
 * To stop the recursive search within directories or saving files
 */
void Action_Main_Stop_Button_Pressed (void)
{
    GAction *action;

    action = g_action_map_lookup_action (G_ACTION_MAP (MainWindow), "stop");
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), FALSE);
    Main_Stop_Button_Pressed = TRUE;
}
