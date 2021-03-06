/* EasyTAG - tag editor for audio files
 * Copyright (C) 2015  David King <amigadave@amigadave.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "file_name.h"

#include "charset.h"
#include "file.h"

#include <string.h>

/*
 * Create a new File_Name structure
 */
File_Name *
et_file_name_new (void)
{
    File_Name *file_name;

    file_name = g_slice_new (File_Name);
    file_name->key = ET_Undo_Key_New ();
    file_name->saved = FALSE;
    file_name->value = NULL;
    file_name->value_utf8 = NULL;
    file_name->value_ck = NULL;

    return file_name;
}

/*
 * Frees a File_Name item.
 */
void
et_file_name_free (File_Name *file_name)
{
    g_return_if_fail (file_name != NULL);

    g_free (file_name->value);
    g_free (file_name->value_utf8);
    g_free (file_name->value_ck);
    g_slice_free (File_Name, file_name);
}

/*
 * Fill content of a FileName item according to the filename passed in argument (UTF-8 filename or not)
 * Calculate also the collate key.
 * It treats numbers intelligently so that "file1" "file10" "file5" is sorted as "file1" "file5" "file10"
 */
void
ET_Set_Filename_File_Name_Item (File_Name *FileName,
                                const gchar *filename_utf8,
                                const gchar *filename)
{
    g_return_if_fail (FileName != NULL);

    if (filename_utf8 && filename)
    {
        FileName->value_utf8 = g_strdup (filename_utf8);
        FileName->value = g_strdup (filename);
        FileName->value_ck = g_utf8_collate_key_for_filename (FileName->value_utf8, -1);
    }
    else if (filename_utf8)
    {
        FileName->value_utf8 = g_strdup (filename_utf8);
        FileName->value = filename_from_display (filename_utf8);
        FileName->value_ck = g_utf8_collate_key_for_filename (FileName->value_utf8, -1);
    }
    else if (filename)
    {
        FileName->value_utf8 = filename_to_display (filename);;
        FileName->value = g_strdup (filename);
        FileName->value_ck = g_utf8_collate_key_for_filename (FileName->value_utf8, -1);
    }
}

/*
 * Compares two File_Name items :
 *  - returns TRUE if there aren't the same
 *  - else returns FALSE
 */
gboolean
et_file_name_detect_difference (const File_Name *a,
                                const File_Name *b)
{
    const gchar *filename1_ck;
    const gchar *filename2_ck;

    g_return_val_if_fail (a && b, FALSE);

    if (a && !b) return TRUE;
    if (!a && b) return TRUE;

    /* Both a and b are != NULL. */
    if (!a->value && !b->value) return FALSE;
    if (a->value && !b->value) return TRUE;
    if (!a->value && b->value) return TRUE;

    /* Compare collate keys (with FileName->value converted to UTF-8 as it
     * contains raw data). */
    filename1_ck = a->value_ck;
    filename2_ck = b->value_ck;

    /* Filename changed ? (we check path + file). */
    if (strcmp (filename1_ck, filename2_ck) != 0)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}
