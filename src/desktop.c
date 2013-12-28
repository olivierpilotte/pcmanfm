/*
 *      desktop.c
 *
 *      Copyright 2010 - 2012 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
 *      Copyright 2012-2013 Andriy Grytsenko (LStranger) <andrej@rep.kiev.ua>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *      MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "desktop.h"
#include "pcmanfm.h"

#include <glib/gi18n.h>

#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <math.h>

#include <cairo-xlib.h>

#include "pref.h"
#include "main-win.h"

#include "gseal-gtk-compat.h"

/* compatibility with LibFM < 1.0.2 */
#if !FM_CHECK_VERSION(1, 0, 2)
# define FM_FOLDER_MODEL_COL_INFO COL_FILE_INFO
# define FM_FOLDER_MODEL_COL_ICON COL_FILE_ICON
#endif

#define SPACING 2
#define PADDING 6
#define MARGIN  2

/* the search dialog timeout (in ms) */
#define DESKTOP_SEARCH_DIALOG_TIMEOUT (5000)

struct _FmDesktopItem
{
    FmFileInfo* fi;
    GdkRectangle area; /* position of the item on the desktop */
    GdkRectangle icon_rect;
    GdkRectangle text_rect;
    gboolean is_special : 1; /* is this a special item like "My Computer", mounted volume, or "Trash" */
    gboolean is_mount : 1; /* is this a mounted volume*/
    gboolean is_selected : 1;
    gboolean is_rubber_banded : 1;
    gboolean is_prelight : 1;
    gboolean fixed_pos : 1;
};

struct _FmBackgroundCache
{
    FmBackgroundCache *next;
    char *filename;
#if GTK_CHECK_VERSION(3, 0, 0)
    cairo_surface_t *bg;
#else
    GdkPixmap *bg;
#endif
    FmWallpaperMode wallpaper_mode;
    time_t mtime;
};

static void queue_layout_items(FmDesktop* desktop);
static void redraw_item(FmDesktop* desktop, FmDesktopItem* item);

static FmFileInfoList* _dup_selected_files(FmFolderView* fv);
static FmPathList* _dup_selected_file_paths(FmFolderView* fv);
static void _select_all(FmFolderView* fv);
static void _unselect_all(FmFolderView* fv);

static FmDesktopItem* hit_test(FmDesktop* self, GtkTreeIter *it, int x, int y);

static void fm_desktop_view_init(FmFolderViewInterface* iface);

G_DEFINE_TYPE_WITH_CODE(FmDesktop, fm_desktop, GTK_TYPE_WINDOW,
                        G_IMPLEMENT_INTERFACE(FM_TYPE_FOLDER_VIEW, fm_desktop_view_init))

static GtkWindowGroup* win_group = NULL;
static FmDesktop **desktops = NULL;
static int n_screens = 0;
static guint icon_theme_changed = 0;
static GtkAccelGroup* acc_grp = NULL;

static FmFolder* desktop_folder = NULL;

static Atom XA_NET_WORKAREA = 0;
static Atom XA_NET_NUMBER_OF_DESKTOPS = 0;
static Atom XA_NET_CURRENT_DESKTOP = 0;
static Atom XA_XROOTMAP_ID = 0;
static Atom XA_XROOTPMAP_ID = 0;

static GdkCursor* hand_cursor = NULL;

static guint idle_config_save = 0;

enum {
#if N_FM_DND_DEST_DEFAULT_TARGETS > N_FM_DND_SRC_DEFAULT_TARGETS
    FM_DND_DEST_DESKTOP_ITEM = N_FM_DND_DEST_DEFAULT_TARGETS
#else
    FM_DND_DEST_DESKTOP_ITEM = N_FM_DND_SRC_DEFAULT_TARGETS
#endif
};

static GtkTargetEntry dnd_targets[] =
{
    {"application/x-desktop-item", GTK_TARGET_SAME_WIDGET, FM_DND_DEST_DESKTOP_ITEM}
};

static GdkAtom desktop_atom;

enum
{
    PROP_0,
    PROP_MONITOR,
    N_PROPERTIES
};

/* popup menu callbacks */
static void on_open_in_new_tab(GtkAction* act, gpointer user_data);
static void on_open_in_new_win(GtkAction* act, gpointer user_data);
static void on_open_folder_in_terminal(GtkAction* act, gpointer user_data);

static void on_fix_pos(GtkToggleAction* act, gpointer user_data);
static void on_snap_to_grid(GtkAction* act, gpointer user_data);

#if FM_CHECK_VERSION(1, 2, 0)
static void on_disable(GtkAction* act, gpointer user_data);
#endif

/* insert GtkUIManager XML definitions */
#include "desktop-ui.c"


/* ---------------------------------------------------------------------
    Items management and common functions */

static char* get_config_file(FmDesktop* desktop, gboolean create_dir)
{
    char *dir, *path;
    int i;

    for(i = 0; i < n_screens; i++)
        if(desktops[i] == desktop)
            break;
    if(i >= n_screens)
        return NULL;
    dir = pcmanfm_get_profile_dir(create_dir);
    path = g_strdup_printf("%s/desktop-items-%u.conf", dir, i);
    g_free(dir);
    return path;
}

static inline FmDesktopItem* desktop_item_new(FmFolderModel* model, GtkTreeIter* it)
{
    FmDesktopItem* item = g_slice_new0(FmDesktopItem);
    fm_folder_model_set_item_userdata(model, it, item);
    gtk_tree_model_get(GTK_TREE_MODEL(model), it, FM_FOLDER_MODEL_COL_INFO, &item->fi, -1);
    fm_file_info_ref(item->fi);
    return item;
}

static inline void desktop_item_free(FmDesktopItem* item)
{
    if(item->fi)
        fm_file_info_unref(item->fi);
    g_slice_free(FmDesktopItem, item);
}

static void calc_item_size(FmDesktop* desktop, FmDesktopItem* item, GdkPixbuf* icon)
{
    PangoRectangle rc2;

    /* icon rect */
    if(icon)
    {
        item->icon_rect.width = gdk_pixbuf_get_width(icon);
        item->icon_rect.height = gdk_pixbuf_get_height(icon);
        /* FIXME: RTL */
        item->icon_rect.x = item->area.x + (desktop->cell_w - item->icon_rect.width) / 2;
        item->icon_rect.y = item->area.y + desktop->ypad + (fm_config->big_icon_size - item->icon_rect.height) / 2;
        item->icon_rect.height += desktop->spacing;
    }
    else
    {
        item->icon_rect.width = fm_config->big_icon_size;
        item->icon_rect.height = fm_config->big_icon_size;
        item->icon_rect.x = item->area.x + desktop->xpad;
        item->icon_rect.y = item->area.y + desktop->ypad;
        item->icon_rect.height += desktop->spacing;
    }

    /* text label rect */
    pango_layout_set_text(desktop->pl, NULL, 0);
    pango_layout_set_height(desktop->pl, desktop->pango_text_h);
    pango_layout_set_width(desktop->pl, desktop->pango_text_w);
    pango_layout_set_text(desktop->pl, fm_file_info_get_disp_name(item->fi), -1);

    pango_layout_get_pixel_extents(desktop->pl, NULL, &rc2);
    pango_layout_set_text(desktop->pl, NULL, 0);

    /* FIXME: RTL */
    item->text_rect.x = item->area.x + (desktop->cell_w - rc2.width - 4) / 2;
    item->text_rect.y = item->icon_rect.y + item->icon_rect.height + rc2.y;
    item->text_rect.width = rc2.width + 4;
    item->text_rect.height = rc2.height + 4;
    item->area.width = MAX(item->icon_rect.width, item->text_rect.width);
    item->area.height = item->icon_rect.height + rc2.y + item->text_rect.height;
}

/* unfortunately we cannot load the "*" together with items because
   otherwise we will update pango layout on each load_items() which
   is resource wasting so we load config file once more instead */
static inline void load_config(FmDesktop* desktop)
{
    char* path;
    GKeyFile* kf;

    path = get_config_file(desktop, FALSE);
    if(!path)
        return;
    kf = g_key_file_new();
    if(g_key_file_load_from_file(kf, path, 0, NULL))
        /* item "*" is desktop config */
        fm_app_config_load_desktop_config(kf, "*", &desktop->conf);
    g_free(path);
    g_key_file_free(kf);
}

static inline void load_items(FmDesktop* desktop)
{
    GtkTreeIter it;
    char* path;
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GKeyFile* kf;

    if (!gtk_tree_model_get_iter_first(model, &it))
        return;
    path = get_config_file(desktop, FALSE);
    if(!path)
        return;
    kf = g_key_file_new();
    if(g_key_file_load_from_file(kf, path, 0, NULL))
    {
        do
        {
            FmDesktopItem* item;
            const char* name;
            GdkPixbuf* icon = NULL;

            item = fm_folder_model_get_item_userdata(desktop->model, &it);
            name = fm_file_info_get_name(item->fi);
            if(g_key_file_has_group(kf, name))
            {
                gtk_tree_model_get(model, &it, FM_FOLDER_MODEL_COL_ICON, &icon, -1);
                desktop->fixed_items = g_list_prepend(desktop->fixed_items, item);
                item->fixed_pos = TRUE;
                item->area.x = g_key_file_get_integer(kf, name, "x", NULL);
                item->area.y = g_key_file_get_integer(kf, name, "y", NULL);
                calc_item_size(desktop, item, icon);
                if(icon)
                    g_object_unref(icon);
            }
        }
        while(gtk_tree_model_iter_next(model, &it));
    }
    g_free(path);
    g_key_file_free(kf);
    queue_layout_items(desktop);
}

static inline void unload_items(FmDesktop* desktop)
{
    /* remove existing fixed items */
    g_list_free(desktop->fixed_items);
    desktop->fixed_items = NULL;
    desktop->focus = NULL;
    desktop->drop_hilight = NULL;
    desktop->hover_item = NULL;
    g_object_set(G_OBJECT(desktop), "tooltip-text", NULL, NULL);
}

static gint get_desktop_for_root_window(GdkWindow *root)
{
    gint desktop = -1;
    Atom ret_type;
    gulong len, after;
    int format;
    guchar* prop;

    if(XGetWindowProperty(GDK_WINDOW_XDISPLAY(root), GDK_WINDOW_XID(root),
                          XA_NET_CURRENT_DESKTOP, 0, 1, False, XA_CARDINAL, &ret_type,
                          &format, &len, &after, &prop) == Success &&
       prop != NULL)
    {
        desktop = (gint)*(guint32*)prop;
        XFree(prop);
    }
    return desktop;
}

/* save position of desktop icons */
static void save_item_pos(FmDesktop* desktop)
{
    GList* l;
    GString* buf;
    char* path = get_config_file(desktop, TRUE);

    if(!path)
        return;
    buf = g_string_sized_new(1024);

    /* save desktop config */
    if (desktop->conf.configured)
    {
        fm_app_config_save_desktop_config(buf, "*", &desktop->conf);
        g_string_append_c(buf, '\n');
    }

    /* save all items positions */
    for(l = desktop->fixed_items; l; l=l->next)
    {
        FmDesktopItem* item = (FmDesktopItem*)l->data;
        FmPath* fi_path = fm_file_info_get_path(item->fi);
        const char* p;
        /* write the file basename as group name */
        g_string_append_c(buf, '[');
        for(p = fm_path_get_basename(fi_path); *p; ++p)
        {
            switch(*p)
            {
            case '\r':
                g_string_append(buf, "\\r");
                break;
            case '\n':
                g_string_append(buf, "\\n");
                break;
            case '\\':
                g_string_append(buf, "\\\\");
                break;
            default:
                g_string_append_c(buf, *p);
            }
        }
        g_string_append(buf, "]\n");
        g_string_append_printf(buf, "x=%d\n"
                                    "y=%d\n\n",
                                    item->area.x, item->area.y);
    }
    g_file_set_contents(path, buf->str, buf->len, NULL);
    g_free(path);
    g_string_free(buf, TRUE);
    desktop->conf.changed = FALSE; /* reset it since we saved it */
}

static gboolean on_config_save_idle(gpointer _unused)
{
    int i;

    if (g_source_is_destroyed(g_main_current_source()))
        return FALSE;

    for (i = 0; i < n_screens; i++)
        if (desktops[i]->conf.changed)
            save_item_pos(desktops[i]);
    idle_config_save = 0;
    return FALSE;
}

static void queue_config_save(FmDesktop *desktop)
{
    desktop->conf.configured = TRUE;
    desktop->conf.changed = TRUE;
    if (idle_config_save == 0)
        idle_config_save = gdk_threads_add_idle(on_config_save_idle, NULL);
}

static GList* get_selected_items(FmDesktop* desktop, int* n_items)
{
    GList* items = NULL;
    int n = 0;
    FmDesktopItem* focus = NULL;
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GtkTreeIter it;
    if(gtk_tree_model_get_iter_first(model, &it)) do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        if(item->is_selected)
        {
            if(G_LIKELY(item != desktop->focus))
            {
                items = g_list_prepend(items, item);
                ++n;
            }
            else
                focus = item;
        }
    }
    while(gtk_tree_model_iter_next(model, &it));
    items = g_list_reverse(items);
    if(focus)
    {
        items = g_list_prepend(items, focus);
        ++n;
    }
    if(n_items)
        *n_items = n;
    return items;
}

static void copy_desktop_config(FmDesktopConfig *dst, FmDesktopConfig *src)
{
    int i;

    dst->wallpaper_mode = src->wallpaper_mode;
    dst->wallpaper = g_strdup(src->wallpaper);
    if (src->wallpapers_configured > 0)
    {
        dst->wallpapers = g_new(char *, src->wallpapers_configured);
        for (i = 0; i < src->wallpapers_configured; i++)
            dst->wallpapers[i] = g_strdup(src->wallpapers[i]);
    }
    else
        dst->wallpapers = NULL;
    dst->wallpapers_configured = src->wallpapers_configured;
    dst->wallpaper_common = src->wallpaper_common;
    dst->show_wm_menu = src->show_wm_menu;
    dst->configured = TRUE;
    dst->changed = FALSE;
    dst->desktop_bg = src->desktop_bg;
    dst->desktop_fg = src->desktop_fg;
    dst->desktop_shadow = src->desktop_shadow;
    dst->desktop_font = g_strdup(src->desktop_font);
    dst->desktop_sort_type = src->desktop_sort_type;
    dst->desktop_sort_by = src->desktop_sort_by;
}

#if FM_CHECK_VERSION(1, 2, 0)
/* ---------------------------------------------------------------------
    mounts handlers */

typedef struct
{
    GMount *mount; /* NULL for non-mounts */
    FmPath *path;
    FmFileInfo *fi;
    FmFileInfoJob *job;
} FmDesktopExtraItem;

static FmDesktopExtraItem *documents = NULL;
//static FmDesktopExtraItem *computer = NULL;
static FmDesktopExtraItem *trash_can = NULL;
//static FmDesktopExtraItem *applications = NULL;

static GVolumeMonitor *vol_mon = NULL;
/* under GDK lock */
static GSList *mounts = NULL;

static void _free_extra_item(FmDesktopExtraItem *item);

static gboolean on_idle_extra_item_add(gpointer user_data)
{
    FmDesktopExtraItem *item = user_data;
    int i;

    /* if mount is not NULL then it's new mount so add it to the list */
    if (item->mount)
    {
        mounts = g_slist_append(mounts, item);
        /* add it to all models that watch mounts */
        for (i = 0; i < n_screens; i++)
            if (desktops[i]->monitor >= 0 && desktops[i]->conf.show_mounts)
                fm_folder_model_extra_file_add(desktops[i]->model, item->fi,
                                               FM_FOLDER_MODEL_ITEMPOS_POST);
    }
    else if (item == documents)
    {
        /* add it to all models that watch documents */
        for (i = 0; i < n_screens; i++)
            if (desktops[i]->monitor >= 0 && desktops[i]->conf.show_documents)
            {
                fm_folder_model_extra_file_add(desktops[i]->model, item->fi,
                                               FM_FOLDER_MODEL_ITEMPOS_PRE);
                /* if this is extra item it might be loaded after the folder
                   therefore we have to reload fixed positions again to apply */
                unload_items(desktops[i]);
                load_items(desktops[i]);
            }
    }
    else if (item == trash_can)
    {
        /* add it to all models that watch trash can */
        for (i = 0; i < n_screens; i++)
            if (desktops[i]->monitor >= 0 && desktops[i]->conf.show_trash)
            {
                fm_folder_model_extra_file_add(desktops[i]->model, item->fi,
                                               FM_FOLDER_MODEL_ITEMPOS_PRE);
                unload_items(desktops[i]);
                load_items(desktops[i]);
            }
    }
    else
    {
        g_critical("got file info for unknown desktop item %s",
                   fm_path_get_basename(item->path));
        _free_extra_item(item);
    }
    return FALSE;
}

static gboolean trash_is_empty = FALSE; /* startup default */

/* returns TRUE if model should be updated */
static gboolean _update_trash_icon(FmDesktopExtraItem *item)
{
    GFile *gf = fm_file_new_for_uri("trash:///");
    GFileInfo *inf = g_file_query_info(gf, G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT, 0, NULL, NULL);
    const char *icon_name;
    GIcon *icon;
    guint32 n;

    g_object_unref(gf);
    if (!inf)
        return FALSE;

    n = g_file_info_get_attribute_uint32(inf, G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT);
    g_object_unref(inf);
    if (n > 0 && trash_is_empty)
        icon_name = "user-trash-full";
    else if (n == 0 && !trash_is_empty)
        icon_name = "user-trash";
    else /* not changed */
        return FALSE;
    trash_is_empty = (n == 0);
    icon = g_themed_icon_new_with_default_fallbacks(icon_name);
    fm_file_info_set_icon(item->fi, icon);
    g_object_unref(icon);
    return TRUE;
}

static void on_file_info_job_finished(FmFileInfoJob* job, gpointer user_data)
{
    FmDesktopExtraItem *item = user_data;
    FmFileInfo *fi;
    char *name;
    GIcon *icon;

    if(fm_file_info_list_is_empty(job->file_infos))
    {
        /* failed */
        g_critical("FmFileInfoJob failed on desktop mount update");
        _free_extra_item(item);
        return;
    }
    fi = fm_file_info_list_peek_head(job->file_infos);
    /* FIXME: check for duplicates? */
    item->fi = fm_file_info_ref(fi);
    item->job = NULL;
    g_object_unref(job);
    /* update some data with those from the mount */
    if (item->mount)
    {
        name = g_mount_get_name(item->mount);
        fm_file_info_set_disp_name(fi, name);
        g_free(name);
        icon = g_mount_get_icon(item->mount);
        fm_file_info_set_icon(fi, icon);
        g_object_unref(icon);
    }
    /* update trash can icon */
    else if (item == trash_can)
        _update_trash_icon(item);
    /* queue adding item to the list and folder models */
    gdk_threads_add_idle(on_idle_extra_item_add, item);
}

static void _free_extra_item(FmDesktopExtraItem *item)
{
    if (item->mount)
        g_object_unref(item->mount);
    fm_path_unref(item->path);
    if (item->fi)
        fm_file_info_unref(item->fi);
    if (item->job)
    {
        g_signal_handlers_disconnect_by_func(item->job, on_file_info_job_finished, item);
        fm_job_cancel(FM_JOB(item->job));
        g_object_unref(item->job);
    }
    g_slice_free(FmDesktopExtraItem, item);
}

static void on_mount_added(GVolumeMonitor *volume_monitor, GMount *mount,
                           gpointer _unused)
{
    GFile *file;
    FmDesktopExtraItem *item;

    /* get file info for the mount point */
    item = g_slice_new(FmDesktopExtraItem);
    item->mount = g_object_ref(mount);
    file = g_mount_get_root(mount);
    item->path = fm_path_new_for_gfile(file);
    g_object_unref(file);
    item->fi = NULL;
    item->job = fm_file_info_job_new(NULL, FM_FILE_INFO_JOB_NONE);
    fm_file_info_job_add(item->job, item->path);
    g_signal_connect(item->job, "finished", G_CALLBACK(on_file_info_job_finished), item);
    if (!fm_job_run_async(FM_JOB(item->job)))
    {
        g_critical("fm_job_run_async() failed on desktop mount update");
        _free_extra_item(item);
    }
}

static gboolean on_idle_extra_item_remove(gpointer user_data)
{
    GMount *mount = user_data;
    GSList *sl;
    FmDesktopExtraItem *item;
    int i;

    for (sl = mounts; sl; sl = sl->next)
    {
        item = sl->data;
        if (item->mount == mount)
            break;
    }
    if (sl)
    {
        for (i = 0; i < n_screens; i++)
            if (desktops[i]->monitor >= 0 && desktops[i]->conf.show_mounts)
                fm_folder_model_extra_file_remove(desktops[i]->model, item->fi);
        mounts = g_slist_delete_link(mounts, sl);
        _free_extra_item(item);
    }
    else
        g_warning("got unmount for unknown desktop item");
    g_object_unref(mount);
    return FALSE;
}

static void on_mount_removed(GVolumeMonitor *volume_monitor, GMount *mount,
                             gpointer _unused)
{
    gdk_threads_add_idle(on_idle_extra_item_remove, g_object_ref(mount));
}


/* ---------------------------------------------------------------------
    special items handlers */

static GFileMonitor *trash_monitor = NULL;

static void on_trash_changed(GFileMonitor *monitor, GFile *gf, GFile *other,
                             GFileMonitorEvent evt, FmDesktopExtraItem *item)
{
    int i;

    if (!_update_trash_icon(item))
        return;
    for (i = 0; i < n_screens; i++)
        if (desktops[i]->monitor >= 0 && desktops[i]->conf.show_trash)
            fm_folder_model_file_changed(desktops[i]->model, item->fi);
}

static FmDesktopExtraItem *_add_extra_item(const char *path_str)
{
    FmDesktopExtraItem *item;

    if (path_str == NULL) /* special directory does not exist */
        return NULL;

    item = g_slice_new(FmDesktopExtraItem);
    item->mount = NULL;
    item->path = fm_path_new_for_str(path_str);
    item->fi = NULL;
    item->job = fm_file_info_job_new(NULL, FM_FILE_INFO_JOB_NONE);
    fm_file_info_job_add(item->job, item->path);
    g_signal_connect(item->job, "finished", G_CALLBACK(on_file_info_job_finished), item);
    if (!fm_job_run_async(FM_JOB(item->job)))
    {
        g_critical("fm_job_run_async() failed on desktop special item update");
        _free_extra_item(item);
        item = NULL;
    }
    return item;
}
#endif

/* ---------------------------------------------------------------------
    accessibility handlers */

static void set_focused_item(FmDesktop* desktop, FmDesktopItem* item);
static inline gint fm_desktop_accessible_index(GtkWidget *desktop, gpointer item);

/* ---- accessible item mirror ---- */
typedef struct _FmDesktopItemAccessible FmDesktopItemAccessible;
typedef struct _FmDesktopItemAccessibleClass FmDesktopItemAccessibleClass;
#define FM_TYPE_DESKTOP_ITEM_ACCESSIBLE      (fm_desktop_item_accessible_get_type ())
#define FM_DESKTOP_ITEM_ACCESSIBLE(obj)      (G_TYPE_CHECK_INSTANCE_CAST((obj), \
                                              FM_TYPE_DESKTOP_ITEM_ACCESSIBLE, FmDesktopItemAccessible))
#define FM_IS_DESKTOP_ITEM_ACCESSIBLE(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj), \
                                              FM_TYPE_DESKTOP_ITEM_ACCESSIBLE))

static GType fm_desktop_item_accessible_get_type (void);

struct _FmDesktopItemAccessible
{
    AtkObject parent;
    FmDesktopItem *item;
    GtkWidget *widget;
    AtkStateSet *state_set;
    guint action_idle_handler;
    gint action_type;
};

struct _FmDesktopItemAccessibleClass
{
    AtkObjectClass parent_class;
};

static void atk_component_item_interface_init(AtkComponentIface *iface);
static void atk_action_item_interface_init(AtkActionIface *iface);
static void atk_image_item_interface_init(AtkImageIface *iface);

G_DEFINE_TYPE_WITH_CODE(FmDesktopItemAccessible, fm_desktop_item_accessible, ATK_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_COMPONENT, atk_component_item_interface_init)
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_ACTION, atk_action_item_interface_init)
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_IMAGE, atk_image_item_interface_init))

static void fm_desktop_item_accessible_init(FmDesktopItemAccessible *item)
{
    item->state_set = atk_state_set_new();
    atk_state_set_add_state(item->state_set, ATK_STATE_ENABLED);
    atk_state_set_add_state(item->state_set, ATK_STATE_FOCUSABLE);
    atk_state_set_add_state(item->state_set, ATK_STATE_SENSITIVE);
    atk_state_set_add_state(item->state_set, ATK_STATE_SELECTABLE);
    atk_state_set_add_state(item->state_set, ATK_STATE_VISIBLE);
    item->action_idle_handler = 0;
}

static void fm_desktop_item_accessible_finalize(GObject *object)
{
    FmDesktopItemAccessible *item = (FmDesktopItemAccessible *)object;

    g_return_if_fail(item != NULL);
    g_return_if_fail(FM_IS_DESKTOP_ITEM_ACCESSIBLE(item));

    if (item->widget)
        g_object_remove_weak_pointer(G_OBJECT(item->widget), (gpointer)&item->widget);
    if (item->state_set)
        g_object_unref (item->state_set);
    if (item->action_idle_handler)
    {
        g_source_remove(item->action_idle_handler);
        item->action_idle_handler = 0;
    }
}

static FmDesktopItemAccessible *fm_desktop_item_accessible_new(FmDesktop *desktop,
                                                               FmDesktopItem *item)
{
    const char *name;
    FmDesktopItemAccessible *atk_item;

    name = fm_file_info_get_disp_name(item->fi);
    atk_item = g_object_new(FM_TYPE_DESKTOP_ITEM_ACCESSIBLE,
                            "accessible-name", name, NULL);
    atk_item->item = item;
    atk_item->widget = GTK_WIDGET(desktop);
    g_object_add_weak_pointer(G_OBJECT(desktop), (gpointer)&atk_item->widget);
    return atk_item;
}

/* item interfaces */
static void fm_desktop_item_accessible_get_extents(AtkComponent *component,
                                                   gint *x, gint *y,
                                                   gint *width, gint *height,
                                                   AtkCoordType coord_type)
{
    FmDesktopItemAccessible *item;
    AtkObject *parent_obj;
    gint l_x, l_y;

    g_return_if_fail(FM_IS_DESKTOP_ITEM_ACCESSIBLE(component));
    item = FM_DESKTOP_ITEM_ACCESSIBLE(component);
    if (item->widget == NULL)
        return;
    if (atk_state_set_contains_state(item->state_set, ATK_STATE_DEFUNCT))
        return;

    *width = item->item->area.width;
    *height = item->item->area.height;
    parent_obj = gtk_widget_get_accessible(item->widget);
    atk_component_get_position(ATK_COMPONENT(parent_obj), &l_x, &l_y, coord_type);
    *x = l_x + item->item->area.x;
    *y = l_y + item->item->area.y;
}

static gboolean fm_desktop_item_accessible_grab_focus(AtkComponent *component)
{
    FmDesktopItemAccessible *item;

    g_return_val_if_fail(FM_IS_DESKTOP_ITEM_ACCESSIBLE(component), FALSE);
    item = FM_DESKTOP_ITEM_ACCESSIBLE(component);
    if (item->widget == NULL)
        return FALSE;
    if (atk_state_set_contains_state(item->state_set, ATK_STATE_DEFUNCT))
        return FALSE;

    gtk_widget_grab_focus(item->widget);
    set_focused_item(FM_DESKTOP(item->widget), item->item);
    return TRUE;
}

static void atk_component_item_interface_init(AtkComponentIface *iface)
{
    iface->get_extents = fm_desktop_item_accessible_get_extents;
    iface->grab_focus = fm_desktop_item_accessible_grab_focus;
}

/* NOTE: this is not very fast */
static GtkTreePath *fm_desktop_item_get_tree_path(FmDesktop *self, FmDesktopItem *item)
{
    GtkTreeModel *model = GTK_TREE_MODEL(self->model);
    GtkTreeIter it;

    if(gtk_tree_model_get_iter_first(model, &it)) do
    {
        if (fm_folder_model_get_item_userdata(self->model, &it) == item)
            return gtk_tree_model_get_path(model, &it);
    }
    while (gtk_tree_model_iter_next(model, &it));
    return NULL;
}

static gboolean fm_desktop_item_accessible_idle_do_action(gpointer data)
{
    FmDesktopItemAccessible *item;
    GtkTreePath *tp;

    if(g_source_is_destroyed(g_main_current_source()))
        return FALSE;

    item = FM_DESKTOP_ITEM_ACCESSIBLE(data);
    item->action_idle_handler = 0;
    if (item->widget != NULL)
    {
        tp = fm_desktop_item_get_tree_path(FM_DESKTOP(item->widget), item->item);
        if (tp)
        {
            fm_folder_view_item_clicked(FM_FOLDER_VIEW(item->widget), tp,
                                        item->action_type == 0 ? FM_FV_ACTIVATED : FM_FV_CONTEXT_MENU);
            gtk_tree_path_free(tp);
        }
    }
    return FALSE;
}

static gboolean fm_desktop_item_accessible_action_do_action(AtkAction *action,
                                                            gint i)
{
    FmDesktopItemAccessible *item;

    if (i != 0 && i != 1)
        return FALSE;

    item = FM_DESKTOP_ITEM_ACCESSIBLE(action);
    if (item->widget == NULL)
        return FALSE;
    if (atk_state_set_contains_state(item->state_set, ATK_STATE_DEFUNCT))
        return FALSE;
    if (!item->action_idle_handler)
    {
        item->action_type = i;
        item->action_idle_handler = gdk_threads_add_idle(fm_desktop_item_accessible_idle_do_action, item);
    }
    return TRUE;
}

static gint fm_desktop_item_accessible_action_get_n_actions(AtkAction *action)
{
    return 2;
}

static const gchar *fm_desktop_item_accessible_action_get_description(AtkAction *action, gint i)
{
    if (i == 0)
        return _("Activate file");
    else if (i == 1)
        return _("Show file menu");
    return NULL;
}

static const gchar *fm_desktop_item_accessible_action_get_name(AtkAction *action, gint i)
{
    if (i == 0)
        return "activate";
    if (i == 1)
        return "menu";
    return NULL;
}

static void atk_action_item_interface_init(AtkActionIface *iface)
{
    iface->do_action = fm_desktop_item_accessible_action_do_action;
    iface->get_n_actions = fm_desktop_item_accessible_action_get_n_actions;
    iface->get_description = fm_desktop_item_accessible_action_get_description;
    iface->get_name = fm_desktop_item_accessible_action_get_name;
    /* NOTE: we don't support descriptions change */
}

static const gchar *fm_desktop_item_accessible_image_get_image_description(AtkImage *image)
{
    FmDesktopItemAccessible *item = FM_DESKTOP_ITEM_ACCESSIBLE(image);

    /* FIXME: is there a better way to handle this? */
    return fm_file_info_get_desc(item->item->fi);
}

static void fm_desktop_item_accessible_image_get_image_size(AtkImage *image,
                                                            gint *width,
                                                            gint *height)
{
    FmDesktopItemAccessible *item = FM_DESKTOP_ITEM_ACCESSIBLE(image);

    if (item->widget == NULL)
        return;
    if (atk_state_set_contains_state(item->state_set, ATK_STATE_DEFUNCT))
        return;

    *width = item->item->icon_rect.width;
    *height = item->item->icon_rect.height;
}

static void fm_desktop_item_accessible_image_get_image_position(AtkImage *image,
                                                                gint *x,
                                                                gint *y,
                                                                AtkCoordType coord_type)
{
    FmDesktopItemAccessible *item = FM_DESKTOP_ITEM_ACCESSIBLE(image);

    if (item->widget == NULL)
        return;
    if (atk_state_set_contains_state(item->state_set, ATK_STATE_DEFUNCT))
        return;

    atk_component_get_position(ATK_COMPONENT(image), x, y, coord_type);
    *x += item->item->icon_rect.x - item->item->area.x;
    *y += item->item->icon_rect.y - item->item->area.y;
}

static void atk_image_item_interface_init(AtkImageIface *iface)
{
    iface->get_image_description = fm_desktop_item_accessible_image_get_image_description;
    /* NOTE: we don't support descriptions change */
    iface->get_image_size = fm_desktop_item_accessible_image_get_image_size;
    iface->get_image_position = fm_desktop_item_accessible_image_get_image_position;
}

static AtkObject *fm_desktop_item_accessible_get_parent(AtkObject *obj)
{
    FmDesktopItemAccessible *item = FM_DESKTOP_ITEM_ACCESSIBLE(obj);

    return item->widget ? gtk_widget_get_accessible(item->widget) : NULL;
}

static gint fm_desktop_item_accessible_get_index_in_parent(AtkObject *obj)
{
    FmDesktopItemAccessible *item = FM_DESKTOP_ITEM_ACCESSIBLE(obj);

    return item->widget ? fm_desktop_accessible_index(item->widget, item) : -1;
}

static AtkStateSet *fm_desktop_item_accessible_ref_state_set(AtkObject *obj)
{
    FmDesktopItemAccessible *item = FM_DESKTOP_ITEM_ACCESSIBLE(obj);
    FmDesktop *desktop;

    g_return_val_if_fail(item->state_set, NULL);

    /* update states first */
    if (item->widget != NULL)
    {
        desktop = (FmDesktop *)item->widget;
        if (desktop->focus == item->item)
            atk_state_set_add_state(item->state_set, ATK_STATE_FOCUSED);
        else
            atk_state_set_remove_state(item->state_set, ATK_STATE_FOCUSED);
    }
    if (item->item->is_selected)
        atk_state_set_add_state(item->state_set, ATK_STATE_SELECTED);
    else
        atk_state_set_remove_state(item->state_set, ATK_STATE_SELECTED);
    return g_object_ref(item->state_set);
}

static void fm_desktop_item_accessible_class_init(FmDesktopItemAccessibleClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    AtkObjectClass *atk_class = ATK_OBJECT_CLASS(klass);

    gobject_class->finalize = fm_desktop_item_accessible_finalize;

    atk_class->get_index_in_parent = fm_desktop_item_accessible_get_index_in_parent;
    atk_class->get_parent = fm_desktop_item_accessible_get_parent;
    atk_class->ref_state_set = fm_desktop_item_accessible_ref_state_set;
}

static gboolean fm_desktop_item_accessible_add_state(FmDesktopItemAccessible *item,
                                                     AtkStateType state_type)
{
    gboolean rc;

    rc = atk_state_set_add_state(item->state_set, state_type);
    atk_object_notify_state_change(ATK_OBJECT (item), state_type, TRUE);
    /* If state_type is ATK_STATE_VISIBLE, additional notification */
    if (state_type == ATK_STATE_VISIBLE)
        g_signal_emit_by_name(item, "visible-data-changed");
    return rc;
}

/* ---- accessible widget mirror ---- */
typedef struct _FmDesktopAccessible FmDesktopAccessible;
typedef struct _FmDesktopAccessibleClass FmDesktopAccessibleClass;
#define FM_TYPE_DESKTOP_ACCESSIBLE      (fm_desktop_accessible_get_type())
#define FM_DESKTOP_ACCESSIBLE(obj)      (G_TYPE_CHECK_INSTANCE_CAST((obj), \
                                         FM_TYPE_DESKTOP_ACCESSIBLE, FmDesktopAccessible))
#define FM_IS_DESKTOP_ACCESSIBLE(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj), \
                                         FM_TYPE_DESKTOP_ACCESSIBLE))

static GType fm_desktop_accessible_get_type (void);

typedef struct _FmDesktopAccessiblePriv FmDesktopAccessiblePriv;
struct _FmDesktopAccessiblePriv
{
    /* we don't catch model index but have own index in items list instead */
    GList *items;
    guint action_idle_handler;
};

#define FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(_d_) G_TYPE_INSTANCE_GET_PRIVATE(_d_, FM_TYPE_DESKTOP_ACCESSIBLE, FmDesktopAccessiblePriv)

static void atk_component_interface_init(AtkComponentIface *iface);
static void atk_selection_interface_init(AtkSelectionIface *iface);
static void atk_action_interface_init(AtkActionIface *iface);

/* we cannot use G_DEFINE_TYPE_WITH_CODE - no GtkWindowAccessible before 3.2.0 */
static void fm_desktop_accessible_init(FmDesktopAccessible *self);
static void fm_desktop_accessible_class_init(FmDesktopAccessibleClass *klass);
static gpointer fm_desktop_accessible_parent_class = NULL;

static void fm_desktop_accessible_class_intern_init(gpointer klass)
{
    fm_desktop_accessible_parent_class = g_type_class_peek_parent(klass);
    fm_desktop_accessible_class_init((FmDesktopAccessibleClass*)klass);
}

GType fm_desktop_accessible_get_type(void)
{
    static volatile gsize type_id_volatile = 0;

    if (g_once_init_enter(&type_id_volatile))
    {
        /*
         * Figure out the size of the class and instance
         * we are deriving from
         */
        AtkObjectFactory *factory;
        GTypeQuery query;
        GType derived_atk_type;
        GType g_define_type_id;

        factory = atk_registry_get_factory(atk_get_default_registry(),
                                           g_type_parent(FM_TYPE_DESKTOP));
        derived_atk_type = atk_object_factory_get_accessible_type(factory);
        g_type_query(derived_atk_type, &query);
        g_define_type_id = g_type_register_static_simple(derived_atk_type,
                                g_intern_static_string("FmDesktopAccessible"),
                                query.class_size,
                                (GClassInitFunc)fm_desktop_accessible_class_intern_init,
                                query.instance_size,
                                (GInstanceInitFunc)fm_desktop_accessible_init,
                                0);
        G_IMPLEMENT_INTERFACE(ATK_TYPE_COMPONENT, atk_component_interface_init)
        G_IMPLEMENT_INTERFACE(ATK_TYPE_SELECTION, atk_selection_interface_init)
        G_IMPLEMENT_INTERFACE(ATK_TYPE_ACTION, atk_action_interface_init)
        g_once_init_leave(&type_id_volatile, g_define_type_id);
    }
    return type_id_volatile;
}

static inline GList *fm_desktop_find_accessible_for_item(FmDesktopAccessiblePriv *priv, FmDesktopItem *item)
{
    GList *items = priv->items;

    while (items)
    {
        if (((FmDesktopItemAccessible *)items->data)->item == item)
            return items;
        items = items->next;
    }
    return NULL;
}

/* widget interfaces */
static AtkObject *fm_desktop_accessible_ref_accessible_at_point(AtkComponent *component,
                                                                gint x, gint y,
                                                                AtkCoordType coord_type)
{
    GtkWidget *widget = gtk_accessible_get_widget(GTK_ACCESSIBLE(component));
    FmDesktop *desktop;
    gint x_pos, y_pos;
    FmDesktopItem *item;
    GList *obj_l = NULL;
    GtkTreeIter it;

    if (widget == NULL)
        return NULL;
    desktop = FM_DESKTOP(widget);
    atk_component_get_extents(component, &x_pos, &y_pos, NULL, NULL, coord_type);
    item = hit_test(desktop, &it, x - x_pos, y - y_pos);
    if (item)
        obj_l = fm_desktop_find_accessible_for_item(FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(component), item);
    if (obj_l)
        return g_object_ref(obj_l->data);
    return NULL;
}

static void atk_component_interface_init(AtkComponentIface *iface)
{
    iface->ref_accessible_at_point = fm_desktop_accessible_ref_accessible_at_point;
}

static gboolean fm_desktop_accessible_add_selection(AtkSelection *selection, gint i)
{
    GtkWidget *widget = gtk_accessible_get_widget(GTK_ACCESSIBLE(selection));
    FmDesktop *desktop;
    FmDesktopAccessiblePriv *priv;
    FmDesktopItemAccessible *item;

    if (widget == NULL)
        return FALSE;

    desktop = FM_DESKTOP(widget);
    priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(selection);
    item = g_list_nth_data(priv->items, i);
    if (!item)
        return FALSE;
    item->item->is_selected = TRUE;
    redraw_item(desktop, item->item);
    atk_object_notify_state_change(ATK_OBJECT(item), ATK_STATE_SELECTED, TRUE);
    return TRUE;
}

static gboolean fm_desktop_accessible_clear_selection(AtkSelection *selection)
{
    GtkWidget *widget = gtk_accessible_get_widget(GTK_ACCESSIBLE(selection));

    if (widget == NULL)
        return FALSE;

    _unselect_all(FM_FOLDER_VIEW(widget));
    return TRUE;
}

static AtkObject *fm_desktop_accessible_ref_selection(AtkSelection *selection,
                                                      gint i)
{
    FmDesktopAccessiblePriv *priv;
    FmDesktopItemAccessible *item;
    GList *items;

    if (i < 0)
        return NULL;

    priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(selection);
    for (items = priv->items; items; items = items->next)
    {
        item = items->data;
        if (item->item->is_selected)
            if (i-- == 0)
                return g_object_ref(item);
    }
    return NULL;
}

static gint fm_desktop_accessible_get_selection_count(AtkSelection *selection)
{
    FmDesktopAccessiblePriv *priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(selection);
    FmDesktopItemAccessible *item;
    GList *items;
    gint i = 0;

    for (items = priv->items; items; items = items->next)
    {
        item = items->data;
        if (item->item->is_selected)
            i++;
    }
    return i;
}

static gboolean fm_desktop_accessible_is_child_selected(AtkSelection *selection,
                                                        gint i)
{
    FmDesktopAccessiblePriv *priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(selection);
    FmDesktopItemAccessible *item = g_list_nth_data(priv->items, i);

    if (item == NULL)
        return FALSE;
    return item->item->is_selected;
}

static gboolean fm_desktop_accessible_remove_selection(AtkSelection *selection,
                                                       gint i)
{
    GtkWidget *widget = gtk_accessible_get_widget(GTK_ACCESSIBLE(selection));
    FmDesktop *desktop;
    FmDesktopAccessiblePriv *priv;
    FmDesktopItemAccessible *item;
    GList *items;

    if (i < 0)
        return FALSE;
    if (widget == NULL)
        return FALSE;
    desktop = FM_DESKTOP(widget);

    priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(selection);
    for (items = priv->items; items; items = items->next)
    {
        item = items->data;
        if (item->item->is_selected)
            if (i-- == 0)
            {
                item->item->is_selected = FALSE;
                redraw_item(desktop, item->item);
                atk_object_notify_state_change(ATK_OBJECT(item), ATK_STATE_SELECTED, FALSE);
                return TRUE;
            }
    }
    return FALSE;
}

static gboolean fm_desktop_accessible_select_all_selection(AtkSelection *selection)
{
    GtkWidget *widget = gtk_accessible_get_widget(GTK_ACCESSIBLE(selection));

    if (widget == NULL)
        return FALSE;

    _select_all(FM_FOLDER_VIEW(widget));
    return TRUE;
}

static void atk_selection_interface_init(AtkSelectionIface *iface)
{
    iface->add_selection = fm_desktop_accessible_add_selection;
    iface->clear_selection = fm_desktop_accessible_clear_selection;
    iface->ref_selection = fm_desktop_accessible_ref_selection;
    iface->get_selection_count = fm_desktop_accessible_get_selection_count;
    iface->is_child_selected = fm_desktop_accessible_is_child_selected;
    iface->remove_selection = fm_desktop_accessible_remove_selection;
    iface->select_all_selection = fm_desktop_accessible_select_all_selection;
}

static gboolean fm_desktop_accessible_idle_do_action(gpointer data)
{
    GtkWidget *widget;
    FmDesktopAccessiblePriv *priv;

    if(g_source_is_destroyed(g_main_current_source()))
        return FALSE;

    priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(data);
    priv->action_idle_handler = 0;
    widget = gtk_accessible_get_widget(data);
    if (widget)
        fm_folder_view_item_clicked(FM_FOLDER_VIEW(widget), NULL, FM_FV_CONTEXT_MENU);
    return FALSE;
}

static gboolean fm_desktop_accessible_action_do_action(AtkAction *action, gint i)
{
    FmDesktopAccessiblePriv *priv;

    if (i != 0)
        return FALSE;
    if (gtk_accessible_get_widget(GTK_ACCESSIBLE(action)) == NULL)
        return FALSE;

    priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(action);
    if (!priv->action_idle_handler)
        priv->action_idle_handler = gdk_threads_add_idle(fm_desktop_accessible_idle_do_action, action);
    return TRUE;
}

static gint fm_desktop_accessible_action_get_n_actions(AtkAction *action)
{
    return 1;
}

static const gchar *fm_desktop_accessible_action_get_description(AtkAction *action, gint i)
{
    if (i == 0)
        return _("Show desktop menu");
    return NULL;
}

static const gchar *fm_desktop_accessible_action_get_name(AtkAction *action, gint i)
{
    if (i == 0)
        return "menu";
    return NULL;
}

static void atk_action_interface_init(AtkActionIface *iface)
{
    iface->do_action = fm_desktop_accessible_action_do_action;
    iface->get_n_actions = fm_desktop_accessible_action_get_n_actions;
    iface->get_description = fm_desktop_accessible_action_get_description;
    iface->get_name = fm_desktop_accessible_action_get_name;
    /* NOTE: we don't support descriptions change */
}

static void fm_desktop_accessible_finalize(GObject *object)
{
    FmDesktopAccessiblePriv *priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(object);
    FmDesktopItemAccessible *item;

    /* FIXME: should we g_assert(priv->items) here instead? */
    while (priv->items)
    {
        item = priv->items->data;
        item->item = NULL;
        fm_desktop_item_accessible_add_state(item, ATK_STATE_DEFUNCT);
        g_signal_emit_by_name(item, "children-changed::remove", 0, NULL, NULL);
        priv->items = g_list_remove_link(priv->items, priv->items);
        g_object_unref(item);
    }
    if (priv->action_idle_handler)
    {
        g_source_remove(priv->action_idle_handler);
        priv->action_idle_handler = 0;
    }
    G_OBJECT_CLASS(fm_desktop_accessible_parent_class)->finalize(object);
}

static gint fm_desktop_accessible_get_n_children(AtkObject *accessible)
{
    FmDesktopAccessiblePriv *priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(accessible);

    return g_list_length(priv->items);
}

static AtkObject *fm_desktop_accessible_ref_child(AtkObject *accessible,
                                                  gint index)
{
    FmDesktopAccessiblePriv *priv;
    FmDesktopItemAccessible *item;

    if (index < 0)
        return NULL;

    priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(accessible);
    item = g_list_nth_data(priv->items, index);
    if (!item)
        return NULL;
    return g_object_ref(item);
}

static void fm_desktop_accessible_initialize(AtkObject *accessible, gpointer data)
{
    if (ATK_OBJECT_CLASS(fm_desktop_accessible_parent_class)->initialize)
        ATK_OBJECT_CLASS(fm_desktop_accessible_parent_class)->initialize(accessible, data);
    atk_object_set_role(accessible, ATK_ROLE_WINDOW);
    /* FIXME: set name by monitor */
    atk_object_set_name(accessible, _("Desktop"));
}

static void fm_desktop_accessible_init(FmDesktopAccessible *object)
{
    FmDesktopAccessiblePriv *priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(object);

    priv->items = NULL;
}

static void fm_desktop_accessible_class_init(FmDesktopAccessibleClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    AtkObjectClass *atk_class = ATK_OBJECT_CLASS(klass);

    gobject_class->finalize = fm_desktop_accessible_finalize;

    atk_class->get_n_children = fm_desktop_accessible_get_n_children;
    atk_class->ref_child = fm_desktop_accessible_ref_child;
    atk_class->initialize = fm_desktop_accessible_initialize;

    g_type_class_add_private(klass, sizeof(FmDesktopAccessiblePriv));
}

/* ---- interface implementation ---- */
/* handy ATK support is added only in 3.2.0 so we should handle it manually */
static GType fm_desktop_accessible_factory_get_type (void);

typedef struct {
    AtkObjectFactory parent;
} FmDesktopAccessibleFactory;

typedef struct {
    AtkObjectFactoryClass parent_class;
} FmDesktopAccessibleFactoryClass;

G_DEFINE_TYPE_WITH_CODE(FmDesktopAccessibleFactory, fm_desktop_accessible_factory, ATK_TYPE_OBJECT_FACTORY,
                        atk_registry_set_factory_type(atk_get_default_registry(),
                                                      FM_TYPE_DESKTOP,
                                                      g_define_type_id); )

static GType fm_desktop_accessible_factory_get_accessible_type(void)
{
    return FM_TYPE_DESKTOP_ACCESSIBLE;
}

static AtkObject *fm_desktop_accessible_factory_create_accessible(GObject *object)
{
    AtkObject *accessible;

    g_return_val_if_fail(FM_IS_DESKTOP(object), NULL);

    accessible = g_object_new(FM_TYPE_DESKTOP_ACCESSIBLE, NULL);
    atk_object_initialize(accessible, object);
    return accessible;
}

static void fm_desktop_accessible_factory_class_init(FmDesktopAccessibleFactoryClass *klass)
{
    AtkObjectFactoryClass *factory_class = (AtkObjectFactoryClass *)klass;

    factory_class->create_accessible = fm_desktop_accessible_factory_create_accessible;
    factory_class->get_accessible_type = fm_desktop_accessible_factory_get_accessible_type;
}

static void fm_desktop_accessible_factory_init(FmDesktopAccessibleFactory *factory)
{
}

static AtkObject *fm_desktop_get_accessible(GtkWidget *widget)
{
    fm_desktop_accessible_factory_get_type(); /* just to activate it */
    return GTK_WIDGET_CLASS(fm_desktop_parent_class)->get_accessible(widget);
}

static inline gint fm_desktop_accessible_index(GtkWidget *desktop, gpointer item)
{
    AtkObject *desktop_atk = gtk_widget_get_accessible(desktop);
    FmDesktopAccessiblePriv *priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(desktop_atk);

    return g_list_index(priv->items, item);
}

static void fm_desktop_accessible_item_deleted(FmDesktop *desktop, FmDesktopItem *item)
{
    AtkObject *obj;
    FmDesktopAccessiblePriv *priv;
    GList *item_atk_l;
    FmDesktopItemAccessible *item_atk;
    gint index;

    obj = gtk_widget_get_accessible(GTK_WIDGET(desktop));
    if (obj != NULL && FM_IS_DESKTOP_ACCESSIBLE(obj))
    {
        priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(obj);
        item_atk_l = fm_desktop_find_accessible_for_item(priv, item);
        g_return_if_fail(item_atk_l != NULL);
        item_atk = item_atk_l->data;
        item_atk->item = NULL;
        index = g_list_position(priv->items, item_atk_l);
        fm_desktop_item_accessible_add_state(item_atk, ATK_STATE_DEFUNCT);
        g_signal_emit_by_name(obj, "children-changed::remove", index, NULL, NULL);
        priv->items = g_list_remove_link(priv->items, item_atk_l);
        g_object_unref(item_atk);
    }
}

static void fm_desktop_accessible_item_added(FmDesktop *desktop, FmDesktopItem *item,
                                             guint index)
{
    AtkObject *obj;
    FmDesktopAccessiblePriv *priv;
    FmDesktopItemAccessible *item_atk;

    obj = gtk_widget_get_accessible(GTK_WIDGET(desktop));
    if (obj != NULL && FM_IS_DESKTOP_ACCESSIBLE(obj))
    {
        priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(obj);
        item_atk = fm_desktop_item_accessible_new(desktop, item);
        g_warn_if_fail(index <= g_list_length(priv->items));
        priv->items = g_list_insert(priv->items, g_object_ref(item_atk), index);
        g_signal_emit_by_name(obj, "children-changed::add", index, NULL, NULL);
    }
}

static void fm_desktop_accessible_items_reordered(FmDesktop *desktop,
                                                  GtkTreeModel *model,
                                                  gint *new_order)
{
    AtkObject *obj;
    FmDesktopAccessiblePriv *priv;
    GList *new_list = NULL;
    int length, i;

    obj = gtk_widget_get_accessible(GTK_WIDGET(desktop));
    if (obj != NULL && FM_IS_DESKTOP_ACCESSIBLE(obj))
    {
        priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(obj);
        length = gtk_tree_model_iter_n_children(model, NULL);
        g_return_if_fail(length == (gint)g_list_length(priv->items));
        for (i = 0; i < length; i++)
        {
            g_assert(new_order[i] >= 0 && new_order[i] < length);
            new_list = g_list_prepend(new_list,
                                      g_list_nth_data(priv->items, new_order[i]));
        }
        g_list_free(priv->items);
        priv->items = g_list_reverse(new_list);
    }
}

static void fm_desktop_item_selected_changed(FmDesktop *desktop, FmDesktopItem *item)
{
    AtkObject *obj;
    FmDesktopAccessiblePriv *priv;
    GList *item_atk_l;

    obj = gtk_widget_get_accessible(GTK_WIDGET(desktop));
    if (obj != NULL && FM_IS_DESKTOP_ACCESSIBLE(obj))
    {
        priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(obj);
        item_atk_l = fm_desktop_find_accessible_for_item(priv, item);
        g_return_if_fail(item_atk_l != NULL);
        atk_object_notify_state_change(item_atk_l->data, ATK_STATE_SELECTED,
                                       item->is_selected);
    }
}

static void fm_desktop_accessible_focus_set(FmDesktop *desktop, FmDesktopItem *item)
{
    AtkObject *obj;
    FmDesktopAccessiblePriv *priv;
    GList *item_atk_l;

    obj = gtk_widget_get_accessible(GTK_WIDGET(desktop));
    if (obj != NULL && FM_IS_DESKTOP_ACCESSIBLE(obj))
    {
        priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(obj);
        item_atk_l = fm_desktop_find_accessible_for_item(priv, item);
        g_return_if_fail(item_atk_l != NULL);
        atk_object_notify_state_change(item_atk_l->data, ATK_STATE_FOCUSED, TRUE);
    }
}

static void fm_desktop_accessible_focus_unset(FmDesktop *desktop, FmDesktopItem *item)
{
    AtkObject *obj;
    FmDesktopAccessiblePriv *priv;
    GList *item_atk_l;

    obj = gtk_widget_get_accessible(GTK_WIDGET(desktop));
    if (obj != NULL && FM_IS_DESKTOP_ACCESSIBLE(obj))
    {
        priv = FM_DESKTOP_ACCESSIBLE_GET_PRIVATE(obj);
        item_atk_l = fm_desktop_find_accessible_for_item(priv, item);
        g_return_if_fail(item_atk_l != NULL);
        atk_object_notify_state_change(item_atk_l->data, ATK_STATE_FOCUSED, FALSE);
    }
}


/* ---------------------------------------------------------------------
    Desktop drawing */

static inline void get_item_rect(FmDesktopItem* item, GdkRectangle* rect)
{
    gdk_rectangle_union(&item->icon_rect, &item->text_rect, rect);
}

static gboolean is_pos_occupied(FmDesktop* desktop, FmDesktopItem* item)
{
    GList* l;
    for(l = desktop->fixed_items; l; l=l->next)
    {
        FmDesktopItem* fixed = (FmDesktopItem*)l->data;
        GdkRectangle rect;
        get_item_rect(fixed, &rect);
        if(gdk_rectangle_intersect(&rect, &item->icon_rect, NULL)
         ||gdk_rectangle_intersect(&rect, &item->text_rect, NULL))
            return TRUE;
    }
    return FALSE;
}

static void layout_items(FmDesktop* self)
{
    FmDesktopItem* item;
    GtkTreeModel* model = GTK_TREE_MODEL(self->model);
    GdkPixbuf* icon;
    GtkTreeIter it;
    int x, y, bottom;
    GtkTextDirection direction = gtk_widget_get_direction(GTK_WIDGET(self));

    y = self->ymargin;
    bottom = self->working_area.height - self->ymargin - self->cell_h;

    if(!gtk_tree_model_get_iter_first(model, &it))
    {
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return;
    }
    if(direction != GTK_TEXT_DIR_RTL) /* LTR or NONE */
    {
        x = self->xmargin;
        do
        {
            item = fm_folder_model_get_item_userdata(self->model, &it);
            icon = NULL;
            gtk_tree_model_get(model, &it, FM_FOLDER_MODEL_COL_ICON, &icon, -1);
            if(item->fixed_pos)
                calc_item_size(self, item, icon);
            else
            {
_next_position:
                item->area.x = self->working_area.x + x;
                item->area.y = self->working_area.y + y;
                calc_item_size(self, item, icon);
                while (y < item->area.y + item->area.height)
                    y += self->cell_h;
                if(y > bottom)
                {
                    x += self->cell_w;
                    y = self->ymargin;
                }
                /* check if this position is occupied by a fixed item */
                /* or its height does not fit into space that left */
                if(item->area.y + item->area.height > bottom ||
                   is_pos_occupied(self, item))
                    goto _next_position;
            }
            if(icon)
                g_object_unref(icon);
        }
        while(gtk_tree_model_iter_next(model, &it));
    }
    else /* RTL */
    {
        x = self->working_area.width - self->xmargin - self->cell_w;
        do
        {
            item = fm_folder_model_get_item_userdata(self->model, &it);
            icon = NULL;
            gtk_tree_model_get(model, &it, FM_FOLDER_MODEL_COL_ICON, &icon, -1);
            if(item->fixed_pos)
                calc_item_size(self, item, icon);
            else
            {
_next_position_rtl:
                item->area.x = self->working_area.x + x;
                item->area.y = self->working_area.y + y;
                calc_item_size(self, item, icon);
                while (y < item->area.y + item->area.height)
                    y += self->cell_h;
                if(y > bottom)
                {
                    x -= self->cell_w;
                    y = self->ymargin;
                }
                /* check if this position is occupied by a fixed item */
                /* or its height does not fit into space that left */
                if(item->area.y + item->area.height > bottom ||
                   is_pos_occupied(self, item))
                    goto _next_position_rtl;
            }
            if(icon)
                g_object_unref(icon);
        }
        while(gtk_tree_model_iter_next(model, &it));
    }
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static gboolean on_idle_layout(FmDesktop* desktop)
{
    desktop->idle_layout = 0;
    layout_items(desktop);
    return FALSE;
}

static void queue_layout_items(FmDesktop* desktop)
{
    if(0 == desktop->idle_layout)
        desktop->idle_layout = gdk_threads_add_idle((GSourceFunc)on_idle_layout, desktop);
}

static void paint_item(FmDesktop* self, FmDesktopItem* item, cairo_t* cr, GdkRectangle* expose_area, GdkPixbuf* icon)
{
#if GTK_CHECK_VERSION(3, 0, 0)
    GtkStyleContext* style;
#else
    GtkStyle* style;
#endif
    GtkWidget* widget = (GtkWidget*)self;
    GtkCellRendererState state = 0;
#if GTK_CHECK_VERSION(3, 0, 0)
    GdkRGBA rgba;
#else
    GdkWindow* window;
#endif
    int text_x, text_y;

#if GTK_CHECK_VERSION(3, 0, 0)
    style = gtk_widget_get_style_context(widget);
#else
    style = gtk_widget_get_style(widget);
    window = gtk_widget_get_window(widget);
#endif

    pango_layout_set_text(self->pl, NULL, 0);
    pango_layout_set_width(self->pl, self->pango_text_w);
    pango_layout_set_height(self->pl, self->pango_text_h);

    pango_layout_set_text(self->pl, fm_file_info_get_disp_name(item->fi), -1);

    /* FIXME: do we need to cache this? */
    text_x = item->area.x + (self->cell_w - self->text_w)/2 + 2;
    text_y = item->icon_rect.y + item->icon_rect.height + 2;

    if(item->is_selected || item == self->drop_hilight) /* draw background for text label */
    {
        state = GTK_CELL_RENDERER_SELECTED;

        cairo_save(cr);
        gdk_cairo_rectangle(cr, &item->text_rect);
#if GTK_CHECK_VERSION(3, 0, 0)
        gtk_style_context_get_background_color(style, GTK_STATE_FLAG_SELECTED, &rgba);
        gdk_cairo_set_source_rgba(cr, &rgba);
#else
        gdk_cairo_set_source_color(cr, &style->bg[GTK_STATE_SELECTED]);
#endif
        cairo_clip(cr);
        cairo_paint(cr);
        cairo_restore(cr);
#if GTK_CHECK_VERSION(3, 0, 0)
        gtk_style_context_get_color(style, GTK_STATE_FLAG_SELECTED, &rgba);
        gdk_cairo_set_source_rgba(cr, &rgba);
#else
        gdk_cairo_set_source_color(cr, &style->fg[GTK_STATE_SELECTED]);
#endif
    }
    else
    {
        /* the shadow */
        gdk_cairo_set_source_color(cr, &self->conf.desktop_shadow);
        cairo_move_to(cr, text_x + 1, text_y + 1);
        pango_cairo_show_layout(cr, self->pl);
        gdk_cairo_set_source_color(cr, &self->conf.desktop_fg);
    }
    /* real text */
    cairo_move_to(cr, text_x, text_y);
    /* FIXME: should we check if pango is 1.10 at least? */
    pango_cairo_show_layout(cr, self->pl);
    pango_layout_set_text(self->pl, NULL, 0);

    if(item == self->focus && gtk_widget_has_focus(widget))
#if GTK_CHECK_VERSION(3, 0, 0)
        gtk_render_focus(style, cr,
#else
        gtk_paint_focus(style, window, gtk_widget_get_state(widget),
                        expose_area, widget, "icon_view",
#endif
                        item->text_rect.x, item->text_rect.y, item->text_rect.width, item->text_rect.height);

    if(item == self->hover_item) /* hovered */
        g_object_set(G_OBJECT(self), "tooltip-text", fm_file_info_get_disp_name(item->fi), NULL);
    else
        g_object_set(G_OBJECT(self), "tooltip-text", NULL, NULL);

    /* draw the icon */
    g_object_set(self->icon_render, "pixbuf", icon, "info", item->fi, NULL);
#if GTK_CHECK_VERSION(3, 0, 0)
    gtk_cell_renderer_render(GTK_CELL_RENDERER(self->icon_render), cr, widget, &item->icon_rect, &item->icon_rect, state);
#else
    gtk_cell_renderer_render(GTK_CELL_RENDERER(self->icon_render), window, widget, &item->icon_rect, &item->icon_rect, expose_area, state);
#endif
}

static void redraw_item(FmDesktop* desktop, FmDesktopItem* item)
{
    GdkRectangle rect;
    gdk_rectangle_union(&item->icon_rect, &item->text_rect, &rect);
    --rect.x;
    --rect.y;
    rect.width += 2;
    rect.height += 2;
    gdk_window_invalidate_rect(gtk_widget_get_window(GTK_WIDGET(desktop)), &rect, FALSE);
}

static void move_item(FmDesktop* desktop, FmDesktopItem* item, int x, int y, gboolean redraw)
{
    int dx, dy;
    /* this call invalid the area occupied by the item and a redraw
     * is queued. */
    if(redraw)
        redraw_item(desktop, item);

    dx = x - item->area.x;
    dy = y - item->area.y;

    item->area.x = x;
    item->area.y = y;

    /* calc_item_size(desktop, item); */
    item->icon_rect.x += dx;
    item->icon_rect.y += dy;
    item->text_rect.x += dx;
    item->text_rect.y += dy;

    /* make the item use customized fixed position. */
    if(!item->fixed_pos)
    {
        item->fixed_pos = TRUE;
        desktop->fixed_items = g_list_prepend(desktop->fixed_items, item);
    }

    /* move the item to a new place, and queue a redraw for the new rect. */
    if(redraw)
        redraw_item(desktop, item);

#if 0
    /* check if the item is overlapped with another item */
    for(l = desktop->items; l; l=l->next)
    {
        FmDesktopItem* item2 = (FmDesktopItem*)l->data;
    }
#endif
}

static void calc_rubber_banding_rect(FmDesktop* self, int x, int y, GdkRectangle* rect)
{
    int x1, x2, y1, y2;
    if(self->drag_start_x < x)
    {
        x1 = self->drag_start_x;
        x2 = x;
    }
    else
    {
        x1 = x;
        x2 = self->drag_start_x;
    }

    if(self->drag_start_y < y)
    {
        y1 = self->drag_start_y;
        y2 = y;
    }
    else
    {
        y1 = y;
        y2 = self->drag_start_y;
    }

    rect->x = x1;
    rect->y = y1;
    rect->width = x2 - x1;
    rect->height = y2 - y1;
}

static void update_rubberbanding(FmDesktop* self, int newx, int newy)
{
    GtkTreeModel* model = GTK_TREE_MODEL(self->model);
    GtkTreeIter it;
    GdkRectangle old_rect, new_rect;
    //GdkRegion *region;
    GdkWindow *window;

    window = gtk_widget_get_window(GTK_WIDGET(self));

    calc_rubber_banding_rect(self, self->rubber_bending_x, self->rubber_bending_y, &old_rect);
    calc_rubber_banding_rect(self, newx, newy, &new_rect);

    gdk_window_invalidate_rect(window, &old_rect, FALSE);
    gdk_window_invalidate_rect(window, &new_rect, FALSE);
//    gdk_window_clear_area(((GtkWidget*)self)->window, new_rect.x, new_rect.y, new_rect.width, new_rect.height);
/*
    region = gdk_region_rectangle(&old_rect);
    gdk_region_union_with_rect(region, &new_rect);

//    gdk_window_invalidate_region(((GtkWidget*)self)->window, &region, TRUE);

    gdk_region_destroy(region);
*/
    self->rubber_bending_x = newx;
    self->rubber_bending_y = newy;

    /* update selection */
    if(gtk_tree_model_get_iter_first(model, &it)) do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(self->model, &it);
        gboolean selected;
        if(gdk_rectangle_intersect(&new_rect, &item->icon_rect, NULL) ||
            gdk_rectangle_intersect(&new_rect, &item->text_rect, NULL))
            selected = TRUE;
        else
            selected = FALSE;

        /* we cannot compare booleans, TRUE may be 1 or -1 */
        if ((item->is_rubber_banded && !selected) ||
            (!item->is_rubber_banded && selected))
        {
            item->is_selected = selected;
            redraw_item(self, item);
            fm_desktop_item_selected_changed(self, item);
        }
        item->is_rubber_banded = self->rubber_bending && selected;
    }
    while(gtk_tree_model_iter_next(model, &it));
}


static void paint_rubber_banding_rect(FmDesktop* self, cairo_t* cr, GdkRectangle* expose_area)
{
    GtkWidget* widget = (GtkWidget*)self;
    GdkRectangle rect;
    GdkColor clr;
    guchar alpha;

    calc_rubber_banding_rect(self, self->rubber_bending_x, self->rubber_bending_y, &rect);

    if(rect.width <= 0 || rect.height <= 0)
        return;

    if(!gdk_rectangle_intersect(expose_area, &rect, &rect))
        return;
/*
    gtk_widget_style_get(icon_view,
                        "selection-box-color", &clr,
                        "selection-box-alpha", &alpha,
                        NULL);
*/
    clr = gtk_widget_get_style (widget)->base[GTK_STATE_SELECTED];
    alpha = 64;  /* FIXME: should be themable in the future */

    cairo_save(cr);
    cairo_set_source_rgba(cr, (gdouble)clr.red/65535, (gdouble)clr.green/65536, (gdouble)clr.blue/65535, (gdouble)alpha/100);
    gdk_cairo_rectangle(cr, &rect);
    cairo_clip (cr);
    cairo_paint (cr);
    gdk_cairo_set_source_color(cr, &clr);
    cairo_rectangle (cr, rect.x + 0.5, rect.y + 0.5, rect.width - 1, rect.height - 1);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void update_background(FmDesktop* desktop, int is_it)
{
    GtkWidget* widget = (GtkWidget*)desktop;
    GdkPixbuf* pix, *scaled;
    cairo_t* cr;
    GdkScreen *screen = gtk_widget_get_screen(widget);
    GdkWindow* root = gdk_screen_get_root_window(screen);
    GdkWindow *window = gtk_widget_get_window(widget);
    FmBackgroundCache *cache;
#if GTK_CHECK_VERSION(3, 0, 0)
    cairo_pattern_t *pattern;
#endif

    Display* xdisplay;
    Pixmap xpixmap;
    Window xroot;
    int screen_num = gdk_screen_get_number(screen);

    char *wallpaper;

    if (!desktop->conf.wallpaper_common)
    {
        guint32 cur_desktop = desktop->cur_desktop;

        if(is_it >= 0) /* signal "changed::wallpaper" */
        {
            wallpaper = desktop->conf.wallpaper;
            if((gint)cur_desktop >= desktop->conf.wallpapers_configured)
            {
                int i;

                desktop->conf.wallpapers = g_renew(char *, desktop->conf.wallpapers, cur_desktop + 1);
                /* fill the gap with current wallpaper */
                for(i = MAX(desktop->conf.wallpapers_configured,0); i < (int)cur_desktop; i++)
                    desktop->conf.wallpapers[i] = g_strdup(wallpaper);
                desktop->conf.wallpapers[cur_desktop] = NULL;
                desktop->conf.wallpapers_configured = cur_desktop + 1;
            }
            g_free(desktop->conf.wallpapers[cur_desktop]);
            desktop->conf.wallpapers[cur_desktop] = g_strdup(wallpaper);
        }
        else /* desktop refresh */
        {
            if((gint)cur_desktop < desktop->conf.wallpapers_configured)
                wallpaper = desktop->conf.wallpapers[cur_desktop];
            else
                wallpaper = NULL;
            if (wallpaper == NULL && desktop->conf.wallpaper != NULL)
            {
                /* if we have wallpaper set for previous desktop but have not
                   for current one, it may mean one of two cases:
                   - we expanded number of desktops;
                   - we recently switched wallpaper_common off.
                   If we selected to use wallpaper image but current desktop
                   has no image set (i.e. one of cases above is happening),
                   it is reasonable and correct to use last selected image for
                   newly selected desktop instead of show plain color on it */
                wallpaper = desktop->conf.wallpaper;
                if ((gint)cur_desktop < desktop->conf.wallpapers_configured)
                    /* this means desktop->conf.wallpapers[cur_desktop] is NULL,
                       see above, we have to update it too in this case */
                    desktop->conf.wallpapers[cur_desktop] = g_strdup(wallpaper);
            }
            else
            {
                g_free(desktop->conf.wallpaper); /* update to current desktop */
                desktop->conf.wallpaper = g_strdup(wallpaper);
            }
        }
    }
    else
        wallpaper = desktop->conf.wallpaper;

    if(desktop->conf.wallpaper_mode != FM_WP_COLOR && wallpaper && *wallpaper)
    {
        struct stat st; /* for mtime */

        /* bug #3613571 - replacing the file will not affect the desktop
           we will call stat on each desktop change but it's inevitable */
        if (stat(wallpaper, &st) < 0)
            st.st_mtime = 0;
        for(cache = desktop->cache; cache; cache = cache->next)
            if(strcmp(wallpaper, cache->filename) == 0)
                break;
        if(cache && cache->wallpaper_mode == desktop->conf.wallpaper_mode
           && st.st_mtime == cache->mtime)
            pix = NULL; /* no new pix for it */
        else if((pix = gdk_pixbuf_new_from_file(wallpaper, NULL)))
        {
            if(cache)
            {
                /* the same file but mode was changed */
#if GTK_CHECK_VERSION(3, 0, 0)
                XFreePixmap(cairo_xlib_surface_get_display(cache->bg),
                            cairo_xlib_surface_get_drawable(cache->bg));
                cairo_surface_destroy(cache->bg);
#else
                g_object_unref(cache->bg);
#endif
                cache->bg = NULL;
            }
            else if(desktop->cache)
            {
                for(cache = desktop->cache; cache->next; )
                    cache = cache->next;
                cache->next = g_new0(FmBackgroundCache, 1);
                cache = cache->next;
            }
            else
                desktop->cache = cache = g_new0(FmBackgroundCache, 1);
            if(!cache->filename)
                cache->filename = g_strdup(wallpaper);
            cache->mtime = st.st_mtime;
            g_debug("adding new FmBackgroundCache for %s", wallpaper);
        }
        else
            /* if there is a cached image but with another mode and we cannot
               get it from file for new mode then just leave it in cache as is */
            cache = NULL;
    }
    else
        cache = NULL;

    if(!cache) /* solid color only */
    {
#if GTK_CHECK_VERSION(3, 0, 0)
        pattern = cairo_pattern_create_rgb(desktop->conf.desktop_bg.red / 65535.0,
                                           desktop->conf.desktop_bg.green / 65535.0,
                                           desktop->conf.desktop_bg.blue / 65535.0);
        gdk_window_set_background_pattern(window, pattern);
        cairo_pattern_destroy(pattern);
#else
        GdkColor bg = desktop->conf.desktop_bg;

        gdk_colormap_alloc_color(gdk_drawable_get_colormap(window), &bg, FALSE, TRUE);
        gdk_window_set_back_pixmap(window, NULL, FALSE);
        gdk_window_set_background(window, &bg);
#endif
        gdk_window_invalidate_rect(window, NULL, TRUE);
        return;
    }

    if(!cache->bg) /* no cached image found */
    {
        int src_w, src_h;
        int dest_w, dest_h;
        int x = 0, y = 0;
        src_w = gdk_pixbuf_get_width(pix);
        src_h = gdk_pixbuf_get_height(pix);
        if(desktop->conf.wallpaper_mode == FM_WP_TILE)
        {
            dest_w = src_w;
            dest_h = src_h;
        }
        else
        {
            GdkRectangle geom;
            gdk_screen_get_monitor_geometry(screen, desktop->monitor, &geom);
            dest_w = geom.width;
            dest_h = geom.height;
        }
#if GTK_CHECK_VERSION(3, 0, 0)
        xdisplay = GDK_WINDOW_XDISPLAY(root);
        /* this code is taken from libgnome-desktop */
        xpixmap = XCreatePixmap(xdisplay, RootWindow(xdisplay, screen_num),
                                dest_w, dest_h, DefaultDepth(xdisplay, screen_num));
        cache->bg = cairo_xlib_surface_create(xdisplay, xpixmap,
                                              GDK_VISUAL_XVISUAL(gdk_screen_get_system_visual(screen)),
                                              dest_w, dest_h);
        cr = cairo_create(cache->bg);
#else
        cache->bg = gdk_pixmap_new(window, dest_w, dest_h, -1);
        cr = gdk_cairo_create(cache->bg);
#endif
        if(gdk_pixbuf_get_has_alpha(pix)
            || desktop->conf.wallpaper_mode == FM_WP_CENTER
            || desktop->conf.wallpaper_mode == FM_WP_FIT)
        {
            gdk_cairo_set_source_color(cr, &desktop->conf.desktop_bg);
            cairo_rectangle(cr, 0, 0, dest_w, dest_h);
            cairo_fill(cr);
        }

        switch(desktop->conf.wallpaper_mode)
        {
        case FM_WP_TILE:
            break;
        case FM_WP_STRETCH:
            if(dest_w == src_w && dest_h == src_h)
                scaled = (GdkPixbuf*)g_object_ref(pix);
            else
                scaled = gdk_pixbuf_scale_simple(pix, dest_w, dest_h, GDK_INTERP_BILINEAR);
            g_object_unref(pix);
            pix = scaled;
            break;
        case FM_WP_FIT:
            if(dest_w != src_w || dest_h != src_h)
            {
                gdouble w_ratio = (float)dest_w / src_w;
                gdouble h_ratio = (float)dest_h / src_h;
                gdouble ratio = MIN(w_ratio, h_ratio);
                if(ratio != 1.0)
                {
                    src_w *= ratio;
                    src_h *= ratio;
                    scaled = gdk_pixbuf_scale_simple(pix, src_w, src_h, GDK_INTERP_BILINEAR);
                    g_object_unref(pix);
                    pix = scaled;
                }
            }
            /* continue to execute code in case FM_WP_CENTER */
        case FM_WP_CENTER:
            x = (dest_w - src_w)/2;
            y = (dest_h - src_h)/2;
            break;
        case FM_WP_COLOR: ; /* handled above */
        }
        gdk_cairo_set_source_pixbuf(cr, pix, x, y);
        cairo_paint(cr);
        cairo_destroy(cr);
        cache->wallpaper_mode = desktop->conf.wallpaper_mode;
    }
#if GTK_CHECK_VERSION(3, 0, 0)
    pattern = cairo_pattern_create_for_surface(cache->bg);
    gdk_window_set_background_pattern(window, pattern);
    cairo_pattern_destroy(pattern);
#else
    gdk_window_set_back_pixmap(window, cache->bg, FALSE);
#endif

    /* set root map here */
    xdisplay = GDK_WINDOW_XDISPLAY(root);
    xroot = RootWindow(xdisplay, screen_num);

#if GTK_CHECK_VERSION(3, 0, 0)
    xpixmap = cairo_xlib_surface_get_drawable(cache->bg);
#else
    xpixmap = GDK_WINDOW_XWINDOW(cache->bg);
#endif

    XChangeProperty(xdisplay, GDK_WINDOW_XID(root),
                    XA_XROOTMAP_ID, XA_PIXMAP, 32, PropModeReplace, (guchar*)&xpixmap, 1);

    XGrabServer (xdisplay);

#if 0
    result = XGetWindowProperty (display,
                                 RootWindow (display, screen_num),
                                 gdk_x11_get_xatom_by_name ("ESETROOT_PMAP_ID"),
                                 0L, 1L, False, XA_PIXMAP,
                                 &type, &format, &nitems,
                                 &bytes_after,
                                 &data_esetroot);

    if (data_esetroot != NULL) {
            if (result == Success && type == XA_PIXMAP &&
                format == 32 &&
                nitems == 1) {
                    gdk_error_trap_push ();
                    XKillClient (display, *(Pixmap *)data_esetroot);
                    gdk_error_trap_pop_ignored ();
            }
            XFree (data_esetroot);
    }

    XChangeProperty (display, RootWindow (display, screen_num),
                     gdk_x11_get_xatom_by_name ("ESETROOT_PMAP_ID"),
                     XA_PIXMAP, 32, PropModeReplace,
                     (guchar *) &xpixmap, 1);
#endif

    XChangeProperty(xdisplay, xroot, XA_XROOTPMAP_ID, XA_PIXMAP, 32,
                    PropModeReplace, (guchar*)&xpixmap, 1);

    XSetWindowBackgroundPixmap(xdisplay, xroot, xpixmap);
    XClearWindow(xdisplay, xroot);

    XFlush(xdisplay);
    XUngrabServer(xdisplay);

    if(pix)
        g_object_unref(pix);

    gdk_window_invalidate_rect(window, NULL, TRUE);
}


/* ---------------------------------------------------------------------
    FmFolder signal handlers */

static void on_folder_start_loading(FmFolder* folder, gpointer user_data)
{
    /* FIXME: should we delete the model here? */
}


static void on_folder_finish_loading(FmFolder* folder, gpointer user_data)
{
    int i;

    /* the desktop folder is just loaded, apply desktop items and positions */
    for(i = 0; i < n_screens; i++)
    {
        FmDesktop* desktop = desktops[i];
        if(desktop->monitor < 0)
            continue;
        unload_items(desktop);
        load_items(desktop);
    }
}

static FmJobErrorAction on_folder_error(FmFolder* folder, GError* err, FmJobErrorSeverity severity, gpointer user_data)
{
    if(err->domain == G_IO_ERROR)
    {
        if(err->code == G_IO_ERROR_NOT_MOUNTED && severity < FM_JOB_ERROR_CRITICAL)
        {
            FmPath* path = fm_folder_get_path(folder);
            if(fm_mount_path(NULL, path, TRUE))
                return FM_JOB_RETRY;
        }
    }
    fm_show_error(NULL, NULL, err->message);
    return FM_JOB_CONTINUE;
}


/* ---------------------------------------------------------------------
    FmFolderModel signal handlers */

static void on_row_deleting(FmFolderModel* model, GtkTreePath* tp,
                            GtkTreeIter* iter, gpointer data, FmDesktop* desktop)
{
    GList *l;

    for(l = desktop->fixed_items; l; l = l->next)
        if(l->data == data)
        {
            desktop->fixed_items = g_list_delete_link(desktop->fixed_items, l);
            break;
        }
    if((gpointer)desktop->focus == data)
    {
        GtkTreeIter it = *iter;
        fm_desktop_accessible_focus_unset(desktop, data);
        if(gtk_tree_model_iter_next(GTK_TREE_MODEL(model), &it))
            desktop->focus = fm_folder_model_get_item_userdata(model, &it);
        else
        {
            if(gtk_tree_path_prev(tp))
            {
                gtk_tree_model_get_iter(GTK_TREE_MODEL(model), &it, tp);
                gtk_tree_path_next(tp);
                desktop->focus = fm_folder_model_get_item_userdata(model, &it);
            }
            else
                desktop->focus = NULL;
        }
        if (desktop->focus)
            fm_desktop_accessible_focus_set(desktop, desktop->focus);
    }
    if((gpointer)desktop->drop_hilight == data)
        desktop->drop_hilight = NULL;
    if((gpointer)desktop->hover_item == data)
    {
        desktop->hover_item = NULL;
        /* bug #3615015: after deleting the item tooltip stuck on the desktop */
        g_object_set(G_OBJECT(desktop), "tooltip-text", NULL, NULL);
    }
    fm_desktop_accessible_item_deleted(desktop, data);
    desktop_item_free(data);
}

static void on_row_inserted(FmFolderModel* mod, GtkTreePath* tp, GtkTreeIter* it, FmDesktop* desktop)
{
    FmDesktopItem* item = desktop_item_new(mod, it);
    gint *indices = gtk_tree_path_get_indices(tp);
    fm_desktop_accessible_item_added(desktop, item, indices[0]);
    fm_folder_model_set_item_userdata(mod, it, item);
    queue_layout_items(desktop);
}

static void on_row_deleted(FmFolderModel* mod, GtkTreePath* tp, FmDesktop* desktop)
{
    queue_layout_items(desktop);
}

static void on_row_changed(FmFolderModel* model, GtkTreePath* tp, GtkTreeIter* it, FmDesktop* desktop)
{
    FmDesktopItem* item = fm_folder_model_get_item_userdata(model, it);

    fm_file_info_unref(item->fi);
    gtk_tree_model_get(GTK_TREE_MODEL(model), it, FM_FOLDER_MODEL_COL_INFO, &item->fi, -1);
    fm_file_info_ref(item->fi);

    redraw_item(desktop, item);
    /* queue_layout_items(desktop); */
}

static void on_rows_reordered(FmFolderModel* model, GtkTreePath* parent_tp, GtkTreeIter* parent_it, gpointer new_order, FmDesktop* desktop)
{
    fm_desktop_accessible_items_reordered(desktop, GTK_TREE_MODEL(model), new_order);
    queue_layout_items(desktop);
}


/* ---------------------------------------------------------------------
    Events handlers */

static void update_working_area(FmDesktop* desktop)
{
    GdkScreen* screen = gtk_widget_get_screen((GtkWidget*)desktop);
#if GTK_CHECK_VERSION(3, 4, 0)
    gdk_screen_get_monitor_workarea(screen, desktop->monitor, &desktop->working_area);
#else
    GdkWindow* root = gdk_screen_get_root_window(screen);
    Atom ret_type;
    gulong len, after;
    int format;
    guchar* prop;
    guint32 n_desktops, cur_desktop;
    gulong* working_area;

    /* default to screen size */
    gdk_screen_get_monitor_geometry(screen, desktop->monitor, &desktop->working_area);

    if(XGetWindowProperty(GDK_WINDOW_XDISPLAY(root), GDK_WINDOW_XID(root),
                       XA_NET_NUMBER_OF_DESKTOPS, 0, 1, False, XA_CARDINAL, &ret_type,
                       &format, &len, &after, &prop) != Success)
        goto _out;
    if(!prop)
        goto _out;
    n_desktops = *(guint32*)prop;
    XFree(prop);

    if(XGetWindowProperty(GDK_WINDOW_XDISPLAY(root), GDK_WINDOW_XID(root),
                       XA_NET_CURRENT_DESKTOP, 0, 1, False, XA_CARDINAL, &ret_type,
                       &format, &len, &after, &prop) != Success)
        goto _out;
    if(!prop)
        goto _out;
    cur_desktop = *(guint32*)prop;
    XFree(prop);

    if(XGetWindowProperty(GDK_WINDOW_XDISPLAY(root), GDK_WINDOW_XID(root),
                       XA_NET_WORKAREA, 0, 4 * 32, False, AnyPropertyType, &ret_type,
                       &format, &len, &after, &prop) != Success)
        goto _out;
    if(ret_type == None || format == 0 || len != n_desktops*4)
    {
        if(prop)
            XFree(prop);
        goto _out;
    }
    working_area = ((gulong*)prop) + cur_desktop * 4;

    if((gint)working_area[0] > desktop->working_area.x &&
       (gint)working_area[0] < desktop->working_area.x + desktop->working_area.width)
    {
        desktop->working_area.width -= (working_area[0] - desktop->working_area.x);
        desktop->working_area.x = (gint)working_area[0];
    }
    if((gint)working_area[1] > desktop->working_area.y &&
       (gint)working_area[1] < desktop->working_area.y + desktop->working_area.height)
    {
        desktop->working_area.height -= (working_area[1] - desktop->working_area.y);
        desktop->working_area.y = (gint)working_area[1];
    }
    if((gint)(working_area[0] + working_area[2]) < desktop->working_area.x + desktop->working_area.width)
        desktop->working_area.width = working_area[0] + working_area[2] - desktop->working_area.x;
    if((gint)(working_area[1] + working_area[3]) < desktop->working_area.y + desktop->working_area.height)
        desktop->working_area.height = working_area[1] + working_area[3] - desktop->working_area.y;
    g_debug("got working area: %d.%d.%d.%d", desktop->working_area.x, desktop->working_area.y,
            desktop->working_area.width, desktop->working_area.height);

    XFree(prop);
_out:
#endif
    queue_layout_items(desktop);
    return;
}

static GdkFilterReturn on_root_event(GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
    XPropertyEvent * evt = (XPropertyEvent*) xevent;
    FmDesktop* self = (FmDesktop*)data;
    if (evt->type == PropertyNotify)
    {
        if(evt->atom == XA_NET_WORKAREA)
            update_working_area(self);
        else if(evt->atom == XA_NET_CURRENT_DESKTOP)
        {
            gint desktop = get_desktop_for_root_window(gdk_screen_get_root_window(
                                    gtk_widget_get_screen(GTK_WIDGET(data))));
            if(desktop >= 0)
            {
                self->cur_desktop = (guint)desktop;
                if(!self->conf.wallpaper_common)
                    update_background(self, -1);
            }
        }
    }
    return GDK_FILTER_CONTINUE;
}

static void on_screen_size_changed(GdkScreen* screen, FmDesktop* desktop)
{
    GdkRectangle geom;
    gdk_screen_get_monitor_geometry(screen, desktop->monitor, &geom);
    gtk_window_resize((GtkWindow*)desktop, geom.width, geom.height);
}

static void reload_icons()
{
    int i;
    for(i=0; i < n_screens; ++i)
        if(desktops[i]->monitor >= 0)
            gtk_widget_queue_resize(GTK_WIDGET(desktops[i]));
}

static void on_big_icon_size_changed(FmConfig* cfg, FmFolderModel* model)
{
    fm_folder_model_set_icon_size(model, fm_config->big_icon_size);
    reload_icons();
}

static void on_icon_theme_changed(GtkIconTheme* theme, gpointer user_data)
{
    reload_icons();
}


/* ---------------------------------------------------------------------
    Popup handlers */

static void fm_desktop_update_popup(FmFolderView* fv, GtkWindow* window,
                                    GtkUIManager* ui, GtkActionGroup* act_grp,
                                    FmFileInfoList* files)
{
    GtkAction* act;

    /* remove 'Rename' item and accelerator */
    act = gtk_action_group_get_action(act_grp, "Rename");
    gtk_action_set_visible(act, FALSE);
    gtk_action_set_sensitive(act, FALSE);
    /* hide 'Show Hidden' item */
    act = gtk_action_group_get_action(act_grp, "ShowHidden");
    gtk_action_set_visible(act, FALSE);
    /* add 'Configure desktop' item replacing 'Properties' */
    act = gtk_action_group_get_action(act_grp, "Prop");
    gtk_action_set_visible(act, FALSE);
    //gtk_action_group_remove_action(act_grp, act);
    gtk_action_group_set_translation_domain(act_grp, NULL);
    gtk_action_group_add_actions(act_grp, desktop_actions,
                                 G_N_ELEMENTS(desktop_actions), window);
    gtk_ui_manager_add_ui_from_string(ui, desktop_menu_xml, -1, NULL);
}

static void fm_desktop_update_item_popup(FmFolderView* fv, GtkWindow* window,
                                         GtkUIManager* ui, GtkActionGroup* act_grp,
                                         FmFileInfoList* files)
{
    FmFileInfo* fi;
    GList* sel_items, *l;
    GtkAction* act;
    gboolean all_fixed = TRUE, has_fixed = FALSE;

    sel_items = get_selected_items(FM_DESKTOP(fv), NULL);
    for(l = sel_items; l; l=l->next)
    {
        FmDesktopItem* item = (FmDesktopItem*)l->data;
        if(item->fixed_pos)
            has_fixed = TRUE;
        else
            all_fixed = FALSE;
    }
    g_list_free(sel_items);

    fi = (FmFileInfo*)fm_file_info_list_peek_head(files);

    /* merge some specific menu items for folders */
    gtk_action_group_set_translation_domain(act_grp, NULL);
    if(fm_file_info_list_get_length(files) == 1 && fm_file_info_is_dir(fi))
    {
        gtk_action_group_add_actions(act_grp, folder_menu_actions,
                                     G_N_ELEMENTS(folder_menu_actions), fv);
        gtk_ui_manager_add_ui_from_string(ui, folder_menu_xml, -1, NULL);
    }
#if FM_CHECK_VERSION(1, 2, 0)
    if (fm_file_info_list_get_length(files) == 1 &&
        ((trash_can && trash_can->fi == fi) ||
         (documents && documents->fi == fi)))
    {
        gtk_action_group_add_actions(act_grp, extra_item_menu_actions,
                                     G_N_ELEMENTS(extra_item_menu_actions), fv);
        gtk_ui_manager_add_ui_from_string(ui, extra_item_menu_xml, -1, NULL);
        /* some menu items should be never available for extra items */
        act = gtk_action_group_get_action(act_grp, "Cut");
        gtk_action_set_visible(act, FALSE);
        act = gtk_action_group_get_action(act_grp, "Del");
        gtk_action_set_visible(act, FALSE);
        act = gtk_action_group_get_action(act_grp, "Rename");
        gtk_action_set_visible(act, FALSE);
    }
#endif

    /* merge desktop icon specific items */
    gtk_action_group_add_actions(act_grp, desktop_icon_actions,
                                 G_N_ELEMENTS(desktop_icon_actions), fv);
    act = gtk_action_group_get_action(act_grp, "Snap");
    gtk_action_set_sensitive(act, has_fixed);

    gtk_action_group_add_toggle_actions(act_grp, desktop_icon_toggle_actions,
                                        G_N_ELEMENTS(desktop_icon_toggle_actions),
                                        fv);
    act = gtk_action_group_get_action(act_grp, "Fix");
    gtk_toggle_action_set_active(GTK_TOGGLE_ACTION(act), all_fixed);

    gtk_ui_manager_add_ui_from_string(ui, desktop_icon_menu_xml, -1, NULL);
}

/* folder options work only with single folder - see above */
static void on_open_in_new_tab(GtkAction* act, gpointer user_data)
{
    FmDesktop* desktop = FM_DESKTOP(user_data);

    if(desktop->focus)
        fm_main_win_open_in_last_active(fm_file_info_get_path(desktop->focus->fi));
}

static void on_open_in_new_win(GtkAction* act, gpointer user_data)
{
    FmDesktop* desktop = FM_DESKTOP(user_data);

    if(desktop->focus)
        fm_main_win_add_win(NULL, fm_file_info_get_path(desktop->focus->fi));
}

static void on_open_folder_in_terminal(GtkAction* act, gpointer user_data)
{
    FmDesktop* desktop = FM_DESKTOP(user_data);

    if(desktop->focus /*&& !fm_file_info_is_virtual(fi)*/)
        pcmanfm_open_folder_in_terminal(NULL, fm_file_info_get_path(desktop->focus->fi));
}

static void on_fix_pos(GtkToggleAction* act, gpointer user_data)
{
    FmDesktop* desktop = FM_DESKTOP(user_data);
    GList* items = get_selected_items(desktop, NULL);
    GList* l;
    if(gtk_toggle_action_get_active(act))
    {
        for(l = items; l; l=l->next)
        {
            FmDesktopItem* item = (FmDesktopItem*)l->data;
            if(!item->fixed_pos)
            {
                item->fixed_pos = TRUE;
                desktop->fixed_items = g_list_prepend(desktop->fixed_items, item);
            }
        }
    }
    else
    {
        for(l = items; l; l=l->next)
        {
            FmDesktopItem* item = (FmDesktopItem*)l->data;
            item->fixed_pos = FALSE;
            desktop->fixed_items = g_list_remove(desktop->fixed_items, item);
        }
        layout_items(desktop);
    }
    g_list_free(items);
    queue_config_save(desktop);
}

#if FM_CHECK_VERSION(1, 2, 0)
static void on_disable(GtkAction* act, gpointer user_data)
{
    FmDesktop *desktop = FM_DESKTOP(user_data);
    GList *items = get_selected_items(desktop, NULL);
    FmDesktopItem *item = (FmDesktopItem*)items->data;

    g_list_free(items);
    if (trash_can && trash_can->fi == item->fi)
    {
        desktop->conf.show_trash = FALSE;
    }
    else if (documents && documents->fi == item->fi)
    {
        desktop->conf.show_documents = FALSE;
    }
    else /* else is error */
    {
        g_warning("invalid item to remove from desktop");
        return;
    }
    queue_config_save(desktop);
    fm_folder_model_extra_file_remove(desktop->model, item->fi);
}
#endif

/* round() is only available in C99. Don't use it now for portability. */
static inline double _round(double x)
{
    return (x > 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

static void on_snap_to_grid(GtkAction* act, gpointer user_data)
{
    FmDesktop* desktop = FM_DESKTOP(user_data);
    FmDesktopItem* item;
    GList* items = get_selected_items(desktop, NULL);
    GList* l;
    int x, y;
    GtkTextDirection direction = gtk_widget_get_direction(GTK_WIDGET(desktop));

    y = desktop->working_area.y + desktop->ymargin;
    //bottom = desktop->working_area.y + desktop->working_area.height - desktop->ymargin - desktop->cell_h;

    if(direction != GTK_TEXT_DIR_RTL) /* LTR or NONE */
        x = desktop->working_area.x + desktop->xmargin;
    else /* RTL */
        x = desktop->working_area.x + desktop->working_area.width - desktop->xmargin - desktop->cell_w;

    for(l = items; l; l = l->next)
    {
        int new_x, new_y;
        item = (FmDesktopItem*)l->data;
        if(!item->fixed_pos)
            continue;
        new_x = x + _round((double)(item->area.x - x) / desktop->cell_w) * desktop->cell_w;
        new_y = y + _round((double)(item->area.y - y) / desktop->cell_h) * desktop->cell_h;
        move_item(desktop, item, new_x, new_y, FALSE);
    }
    g_list_free(items);

    queue_layout_items(desktop);
}


/* ---------------------------------------------------------------------
    GtkWidget class default signal handlers */

static gboolean is_point_in_rect(GdkRectangle* rect, int x, int y)
{
    return rect->x < x && x < (rect->x + rect->width) && y > rect->y && y < (rect->y + rect->height);
}

static FmDesktopItem* hit_test(FmDesktop* self, GtkTreeIter *it, int x, int y)
{
    FmDesktopItem* item;
    GtkTreeModel* model = GTK_TREE_MODEL(self->model);
    if(gtk_tree_model_get_iter_first(model, it)) do
    {
        item = fm_folder_model_get_item_userdata(self->model, it);
        if(is_point_in_rect(&item->icon_rect, x, y)
         || is_point_in_rect(&item->text_rect, x, y))
            return item;
    }
    while(gtk_tree_model_iter_next(model, it));
    return NULL;
}

static FmDesktopItem* get_nearest_item(FmDesktop* desktop, FmDesktopItem* item,  GtkDirectionType dir)
{
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    FmDesktopItem* item2, *ret = NULL;
    guint min_x_dist, min_y_dist, dist;
    GtkTreeIter it;

    if(!gtk_tree_model_get_iter_first(model, &it))
        return NULL;
    if(!item) /* there is no focused item yet, select first one then */
        return fm_folder_model_get_item_userdata(desktop->model, &it);

    min_x_dist = min_y_dist = (guint)-1;
    item2 = NULL;

    switch(dir)
    {
    case GTK_DIR_LEFT:
        do
        {
            item2 = fm_folder_model_get_item_userdata(desktop->model, &it);
            if(item2->area.x >= item->area.x)
                continue;
            dist = item->area.x - item2->area.x;
            if(dist < min_x_dist)
            {
                ret = item2;
                min_x_dist = dist;
                min_y_dist = ABS(item->area.y - item2->area.y);
            }
            else if(dist == min_x_dist && item2 != ret) /* if there is another item of the same x distance */
            {
                /* get the one with smaller y distance */
                dist = ABS(item2->area.y - item->area.y);
                if(dist < min_y_dist)
                {
                    ret = item2;
                    min_y_dist = dist;
                }
            }
        }
        while(gtk_tree_model_iter_next(model, &it));
        break;
    case GTK_DIR_RIGHT:
        do
        {
            item2 = fm_folder_model_get_item_userdata(desktop->model, &it);
            if(item2->area.x <= item->area.x)
                continue;
            dist = item2->area.x - item->area.x;
            if(dist < min_x_dist)
            {
                ret = item2;
                min_x_dist = dist;
                min_y_dist = ABS(item->area.y - item2->area.y);
            }
            else if(dist == min_x_dist && item2 != ret) /* if there is another item of the same x distance */
            {
                /* get the one with smaller y distance */
                dist = ABS(item2->area.y - item->area.y);
                if(dist < min_y_dist)
                {
                    ret = item2;
                    min_y_dist = dist;
                }
            }
        }
        while(gtk_tree_model_iter_next(model, &it));
        break;
    case GTK_DIR_UP:
        do
        {
            item2 = fm_folder_model_get_item_userdata(desktop->model, &it);
            if(item2->area.y >= item->area.y)
                continue;
            dist = item->area.y - item2->area.y;
            if(dist < min_y_dist)
            {
                ret = item2;
                min_y_dist = dist;
                min_x_dist = ABS(item->area.x - item2->area.x);
            }
            else if(dist == min_y_dist && item2 != ret) /* if there is another item of the same y distance */
            {
                /* get the one with smaller x distance */
                dist = ABS(item2->area.x - item->area.x);
                if(dist < min_x_dist)
                {
                    ret = item2;
                    min_x_dist = dist;
                }
            }
        }
        while(gtk_tree_model_iter_next(model, &it));
        break;
    case GTK_DIR_DOWN:
        do
        {
            item2 = fm_folder_model_get_item_userdata(desktop->model, &it);
            if(item2->area.y <= item->area.y)
                continue;
            dist = item2->area.y - item->area.y;
            if(dist < min_y_dist)
            {
                ret = item2;
                min_y_dist = dist;
                min_x_dist = ABS(item->area.x - item2->area.x);
            }
            else if(dist == min_y_dist && item2 != ret) /* if there is another item of the same y distance */
            {
                /* get the one with smaller x distance */
                dist = ABS(item2->area.x - item->area.x);
                if(dist < min_x_dist)
                {
                    ret = item2;
                    min_x_dist = dist;
                }
            }
        }
        while(gtk_tree_model_iter_next(model, &it));
        break;
    case GTK_DIR_TAB_FORWARD: /* FIXME */
        break;
    case GTK_DIR_TAB_BACKWARD: /* FIXME */
        ;
    }
    return ret;
}

static gboolean has_selected_item(FmDesktop* desktop)
{
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GtkTreeIter it;
    if(gtk_tree_model_get_iter_first(model, &it)) do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        if(item->is_selected)
            return TRUE;
    }
    while(gtk_tree_model_iter_next(model, &it));
    return FALSE;
}

static void set_focused_item(FmDesktop* desktop, FmDesktopItem* item)
{
    if(item != desktop->focus)
    {
        FmDesktopItem* old_focus = desktop->focus;
        desktop->focus = item;
        if(old_focus)
        {
            redraw_item(desktop, old_focus);
            fm_desktop_accessible_focus_unset(desktop, old_focus);
        }
        if(item)
        {
            redraw_item(desktop, item);
            fm_desktop_accessible_focus_set(desktop, item);
        }
    }
}

/* This function is taken from xfdesktop */
static void forward_event_to_rootwin(GdkScreen *gscreen, GdkEvent *event)
{
    XButtonEvent xev, xev2;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_screen_get_display(gscreen));

    if (event->type == GDK_BUTTON_PRESS || event->type == GDK_BUTTON_RELEASE)
    {
        if (event->type == GDK_BUTTON_PRESS)
        {
            xev.type = ButtonPress;
            /*
             * rox has an option to disable the next
             * instruction. it is called "blackbox_hack". Does
             * anyone know why exactly it is needed?
             */
            XUngrabPointer(dpy, event->button.time);
        }
        else
            xev.type = ButtonRelease;

        xev.button = event->button.button;
        xev.x = event->button.x;    /* Needed for icewm */
        xev.y = event->button.y;
        xev.x_root = event->button.x_root;
        xev.y_root = event->button.y_root;
        xev.state = event->button.state;

        xev2.type = 0;
    }
    else if (event->type == GDK_SCROLL)
    {
        xev.type = ButtonPress;
        xev.button = event->scroll.direction + 4;
        xev.x = event->scroll.x;    /* Needed for icewm */
        xev.y = event->scroll.y;
        xev.x_root = event->scroll.x_root;
        xev.y_root = event->scroll.y_root;
        xev.state = event->scroll.state;

        xev2.type = ButtonRelease;
        xev2.button = xev.button;
    }
    else
        return ;
    xev.window = GDK_WINDOW_XID(gdk_screen_get_root_window(gscreen));
    xev.root = xev.window;
    xev.subwindow = None;
    xev.time = event->button.time;
    xev.same_screen = True;

    XSendEvent(dpy, xev.window, False, ButtonPressMask | ButtonReleaseMask,
                (XEvent *) & xev);
    if (xev2.type == 0)
        return ;

    /* send button release for scroll event */
    xev2.window = xev.window;
    xev2.root = xev.root;
    xev2.subwindow = xev.subwindow;
    xev2.time = xev.time;
    xev2.x = xev.x;
    xev2.y = xev.y;
    xev2.x_root = xev.x_root;
    xev2.y_root = xev.y_root;
    xev2.state = xev.state;
    xev2.same_screen = xev.same_screen;

    XSendEvent(dpy, xev2.window, False, ButtonPressMask | ButtonReleaseMask,
                (XEvent *) & xev2);
}


#if GTK_CHECK_VERSION(3, 0, 0)
static gboolean on_draw(GtkWidget* w, cairo_t* cr)
#else
static gboolean on_expose(GtkWidget* w, GdkEventExpose* evt)
#endif
{
    FmDesktop* self = (FmDesktop*)w;
#if !GTK_CHECK_VERSION(3, 0, 0)
    cairo_t* cr;
#endif
    GtkTreeModel* model = GTK_TREE_MODEL(self->model);
    GtkTreeIter it;
    GdkRectangle area;

#if GTK_CHECK_VERSION(3, 0, 0)
    if(G_UNLIKELY(!gtk_cairo_should_draw_window(cr, gtk_widget_get_window(w))))
        return FALSE;

    cairo_save(cr);
    gtk_cairo_transform_to_window(cr, w, gtk_widget_get_window(w));
    gdk_cairo_get_clip_rectangle(cr, &area);
#else
    if(G_UNLIKELY(! gtk_widget_get_visible (w) || ! gtk_widget_get_mapped (w)))
        return TRUE;

    cr = gdk_cairo_create(gtk_widget_get_window(w));
    area = evt->area;
#endif
    if(self->rubber_bending)
        paint_rubber_banding_rect(self, cr, &area);

    if(gtk_tree_model_get_iter_first(model, &it)) do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(self->model, &it);
        GdkRectangle* intersect, tmp, tmp2;
        GdkPixbuf* icon = NULL;
        if(gdk_rectangle_intersect(&area, &item->icon_rect, &tmp))
            intersect = &tmp;
        else
            intersect = NULL;

        if(gdk_rectangle_intersect(&area, &item->text_rect, &tmp2))
        {
            if(intersect)
                gdk_rectangle_union(intersect, &tmp2, intersect);
            else
                intersect = &tmp2;
        }

        if(intersect)
        {
            gtk_tree_model_get(model, &it, FM_FOLDER_MODEL_COL_ICON, &icon, -1);
            paint_item(self, item, cr, intersect, icon);
            if(icon)
                g_object_unref(icon);
        }
    }
    while(gtk_tree_model_iter_next(model, &it));
#if GTK_CHECK_VERSION(3, 0, 0)
    cairo_restore(cr);
#else
    cairo_destroy(cr);
#endif

    return TRUE;
}

static void on_size_allocate(GtkWidget* w, GtkAllocation* alloc)
{
    FmDesktop* self = (FmDesktop*)w;

    /* calculate item size */
    PangoContext* pc;
    PangoFontMetrics *metrics;
    int font_h;

    pc = gtk_widget_get_pango_context((GtkWidget*)self);

    metrics = pango_context_get_metrics(pc, NULL, NULL);

    font_h = pango_font_metrics_get_ascent(metrics) + pango_font_metrics_get_descent (metrics);
    pango_font_metrics_unref(metrics);

    font_h /= PANGO_SCALE;

    self->spacing = SPACING;
    self->xpad = self->ypad = PADDING;
    self->xmargin = self->ymargin = MARGIN;
    self->text_h = font_h * 2;
    self->text_w = 100;
#if FM_CHECK_VERSION(1, 2, 0)
    if (fm_config->show_full_names)
        self->pango_text_h = -1;
    else
#endif
        self->pango_text_h = self->text_h * PANGO_SCALE;
    self->pango_text_w = self->text_w * PANGO_SCALE;
    self->text_h += 4;
    self->text_w += 4; /* 4 is for drawing border */
    self->cell_h = fm_config->big_icon_size + self->spacing + self->text_h + self->ypad * 2;
    self->cell_w = MAX((gint)self->text_w, fm_config->big_icon_size) + self->xpad * 2;

    update_working_area(self);
    /* queue_layout_items(self); this is called in update_working_area */

    /* scale the wallpaper */
    if(gtk_widget_get_realized(w))
    {
        if(self->conf.wallpaper_mode != FM_WP_COLOR && self->conf.wallpaper_mode != FM_WP_TILE)
            update_background(self, -1);
    }

    GTK_WIDGET_CLASS(fm_desktop_parent_class)->size_allocate(w, alloc);
}

#if GTK_CHECK_VERSION(3, 0, 0)
static void on_get_preferred_width(GtkWidget *w, gint *minimal_width, gint *natural_width)
{
    GdkScreen* scr = gtk_widget_get_screen(w);
    gint monitor = FM_DESKTOP(w)->monitor;
    GdkRectangle geom;
    gdk_screen_get_monitor_geometry(scr, monitor, &geom);
    *minimal_width = *natural_width = geom.width;
}

static void on_get_preferred_height(GtkWidget *w, gint *minimal_height, gint *natural_height)
{
    GdkScreen* scr = gtk_widget_get_screen(w);
    gint monitor = FM_DESKTOP(w)->monitor;
    GdkRectangle geom;
    gdk_screen_get_monitor_geometry(scr, monitor, &geom);
    *minimal_height = *natural_height = geom.height;
}
#else
static void on_size_request(GtkWidget* w, GtkRequisition* req)
{
    GdkScreen* scr = gtk_widget_get_screen(w);
    gint monitor = FM_DESKTOP(w)->monitor;
    GdkRectangle geom;
    gdk_screen_get_monitor_geometry(scr, monitor, &geom);
    req->width = geom.width;
    req->height = geom.height;
}
#endif

static void _stop_rubberbanding(FmDesktop *self, gint x, gint y)
{
    /* re-enable Gtk+ DnD callbacks again */
    gpointer drag_data = g_object_get_data(G_OBJECT(self),
                            g_intern_static_string("gtk-site-data"));
    if(G_LIKELY(drag_data != NULL))
    {
        g_signal_handlers_unblock_matched(G_OBJECT(self), G_SIGNAL_MATCH_DATA,
                                          0, 0, NULL, NULL, drag_data);
    }
    self->rubber_bending = FALSE;
    update_rubberbanding(self, x, y);
    gtk_grab_remove(GTK_WIDGET(self));
}

static gboolean on_button_press(GtkWidget* w, GdkEventButton* evt)
{
    FmDesktop* self = (FmDesktop*)w;
    FmDesktopItem *item = NULL, *clicked_item = NULL;
    GtkTreeIter it;
    FmFolderViewClickType clicked = FM_FV_CLICK_NONE;

    clicked_item = hit_test(FM_DESKTOP(w), &it, (int)evt->x, (int)evt->y);

    if(evt->type == GDK_BUTTON_PRESS)
    {
        if(evt->button == 1)  /* left button */
        {
            self->button_pressed = TRUE;    /* store button state for drag & drop */
            self->drag_start_x = evt->x;
            self->drag_start_y = evt->y;
        }
        else if (self->rubber_bending)
        {
            /* LP #1071121: right click stops rubberbanding but
               leaves the selection area on the desktop.
               To avoid that weird thing we reset and stop rubberbanding now */
            _stop_rubberbanding(self, self->drag_start_x, self->drag_start_y);
        }

        /* if ctrl / shift is not pressed, deselect all. */
        /* FIXME: do [un]selection on button release */
        if(! (evt->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)))
        {
            /* don't cancel selection if clicking on selected items */
            if(!((evt->button == 1 || evt->button == 3) && clicked_item && clicked_item->is_selected))
                _unselect_all(FM_FOLDER_VIEW(self));
        }

        if(clicked_item)
        {
            if(evt->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
                clicked_item->is_selected = ! clicked_item->is_selected;
            else
                clicked_item->is_selected = TRUE;
            fm_desktop_item_selected_changed(self, clicked_item);

            if(self->focus && self->focus != item)
            {
                FmDesktopItem* old_focus = self->focus;
                self->focus = NULL;
                if(old_focus)
                {
                    redraw_item(self, old_focus);
                    fm_desktop_accessible_focus_unset(self, old_focus);
                }
            }
            self->focus = clicked_item;
            fm_desktop_accessible_focus_set(self, clicked_item);
            redraw_item(self, clicked_item);

            if(evt->button == 3)  /* right click, context menu */
                clicked = FM_FV_CONTEXT_MENU;
            else if(evt->button == 2)
                clicked = FM_FV_MIDDLE_CLICK;
        }
        else /* no item is clicked */
        {
            if(evt->button == 3)  /* right click on the blank area => desktop popup menu */
            {
                if(!self->conf.show_wm_menu)
                    clicked = FM_FV_CONTEXT_MENU;
            }
            else if(evt->button == 1)
            {
                /* disable Gtk+ DnD callbacks, because else rubberbanding will be interrupted */
                gpointer drag_data = g_object_get_data(G_OBJECT(self),
                                        g_intern_static_string("gtk-site-data"));
                if(G_LIKELY(drag_data != NULL))
                {
                    g_signal_handlers_block_matched(G_OBJECT(self),
                                                    G_SIGNAL_MATCH_DATA, 0, 0,
                                                    NULL, NULL, drag_data);
                }
                self->rubber_bending = TRUE;

                /* FIXME: if you foward the event here, this will break rubber bending... */
                /* forward the event to root window */
                /* forward_event_to_rootwin(gtk_widget_get_screen(w), evt); */

                gtk_grab_add(w);
                self->rubber_bending_x = evt->x;
                self->rubber_bending_y = evt->y;
            }
        }
    }
    else if(evt->type == GDK_2BUTTON_PRESS) /* activate items */
    {
        if(clicked_item && evt->button == 1)   /* left double click */
            clicked = FM_FV_ACTIVATED;
    }

    if(clicked != FM_FV_CLICK_NONE)
    {
        GtkTreeModel* model = GTK_TREE_MODEL(self->model);
        GtkTreePath* tp = NULL;

        if(clicked_item)
            tp = gtk_tree_model_get_path(model, &it);
        fm_folder_view_item_clicked(FM_FOLDER_VIEW(self), tp, clicked);
        if(tp)
            gtk_tree_path_free(tp);
    }
    /* forward the event to root window */
    else if(evt->button != 1)
        forward_event_to_rootwin(gtk_widget_get_screen(w), (GdkEvent*)evt);

    if(! gtk_widget_has_focus(w))
    {
        /* g_debug("we don't have the focus, grab it!"); */
        gtk_widget_grab_focus(w);
    }
    return TRUE;
}

static gboolean on_button_release(GtkWidget* w, GdkEventButton* evt)
{
    FmDesktop* self = (FmDesktop*)w;
    GtkTreeIter it;
    FmDesktopItem* clicked_item = hit_test(self, &it, evt->x, evt->y);

    self->button_pressed = FALSE;

    if(self->rubber_bending)
    {
        _stop_rubberbanding(self, evt->x, evt->y);
    }
    else if(self->dragging)
    {
        self->dragging = FALSE;
    }
    else if(fm_config->single_click && evt->button == 1)
    {
        if(clicked_item)
        {
            /* left single click */
            fm_launch_file_simple(GTK_WINDOW(w), NULL, clicked_item->fi, pcmanfm_open_folder, w);
            return TRUE;
        }
    }

    /* forward the event to root window */
    if(! clicked_item)
        forward_event_to_rootwin(gtk_widget_get_screen(w), (GdkEvent*)evt);

    return TRUE;
}

static gboolean on_single_click_timeout(gpointer user_data)
{
    FmDesktop* self = (FmDesktop*)user_data;
    GtkWidget* w = (GtkWidget*)self;
    GdkEventButton evt;
    GdkWindow* window;
    int x, y;

    if(g_source_is_destroyed(g_main_current_source()))
        return FALSE;
    window = gtk_widget_get_window(w);
    /* generate a fake button press */
    /* FIXME: will this cause any problem? needs to be redesigned later */
    evt.type = GDK_BUTTON_PRESS;
    evt.window = window;
    gdk_window_get_pointer(window, &x, &y, &evt.state);
    evt.x = x;
    evt.y = y;
    evt.state |= GDK_BUTTON_PRESS_MASK;
    evt.state &= ~GDK_BUTTON_MOTION_MASK;
    on_button_press(GTK_WIDGET(self), &evt);
    evt.type = GDK_BUTTON_RELEASE;
    evt.state &= ~GDK_BUTTON_PRESS_MASK;
    evt.state |= ~GDK_BUTTON_RELEASE_MASK;
    on_button_release(GTK_WIDGET(self), &evt);

    self->single_click_timeout_handler = 0;
    return FALSE;
}

static gboolean on_motion_notify(GtkWidget* w, GdkEventMotion* evt)
{
    FmDesktop* self = (FmDesktop*)w;
    if(! self->button_pressed)
    {
        if(fm_config->single_click)
        {
            GtkTreeIter it;
            FmDesktopItem* item = hit_test(self, &it, evt->x, evt->y);
            FmDesktopItem *hover_item = self->hover_item;
            GdkWindow* window;

            if(item != hover_item)
            {
                if(0 != self->single_click_timeout_handler)
                {
                    g_source_remove(self->single_click_timeout_handler);
                    self->single_click_timeout_handler = 0;
                }
                window = gtk_widget_get_window(w);
                self->hover_item = item;
                if (hover_item)
                    redraw_item(self, hover_item);
                if(item)
                {
                    redraw_item(self, item);
                    gdk_window_set_cursor(window, hand_cursor);
                    if(self->single_click_timeout_handler == 0)
#if FM_CHECK_VERSION(1, 2, 0)
                        if(fm_config->auto_selection_delay > 0)
                            self->single_click_timeout_handler = gdk_threads_add_timeout(fm_config->auto_selection_delay,
                                                                                         on_single_click_timeout, self);
#else
                        self->single_click_timeout_handler = gdk_threads_add_timeout(400, on_single_click_timeout, self); //400 ms
#endif
                        /* Making a loop to aviod the selection of the item */
                        /* on_single_click_timeout(self); */
                }
                else
                {
                    gdk_window_set_cursor(window, NULL);
                }
            }
        }
        else
        {
            GtkTreeIter it;
            FmDesktopItem* item = hit_test(self, &it, evt->x, evt->y);
            FmDesktopItem *hover_item = self->hover_item;

            if(item != hover_item)
            {
                self->hover_item = item;
                if (hover_item)
                    redraw_item(self, hover_item);
                if(item)
                    redraw_item(self, item);
            }
        }
        return TRUE;
    }

    if(self->dragging)
    {
    }
    else if(self->rubber_bending)
    {
        update_rubberbanding(self, evt->x, evt->y);
    }
    else
    {
        if (gtk_drag_check_threshold(w,
                                    self->drag_start_x,
                                    self->drag_start_y,
                                    evt->x, evt->y))
        {
            GtkTargetList* target_list;
            if(has_selected_item(self))
            {
                self->dragging = TRUE;
                target_list = gtk_drag_source_get_target_list(w);
                gtk_drag_begin(w, target_list,
                             GDK_ACTION_COPY|GDK_ACTION_MOVE|GDK_ACTION_LINK,
                             1, (GdkEvent*)evt);
            }
        }
    }

    return TRUE;
}

static gboolean on_leave_notify(GtkWidget* w, GdkEventCrossing *evt)
{
    FmDesktop* self = (FmDesktop*)w;
    if(self->single_click_timeout_handler)
    {
        g_source_remove(self->single_click_timeout_handler);
        self->single_click_timeout_handler = 0;
    }
    return TRUE;
}

static gboolean get_focused_item(FmDesktopItem* focus, GtkTreeModel* model, GtkTreeIter* it)
{
    FmDesktopItem* item;
    if(gtk_tree_model_get_iter_first(model, it)) do
    {
        item = fm_folder_model_get_item_userdata(FM_FOLDER_MODEL(model), it);
        if(item == focus)
            return item->is_selected;
    }
    while(gtk_tree_model_iter_next(model, it));
    return FALSE;
}

/* ---- Interactive search funcs: mostly picked from ExoIconView ---- */

/* Cut and paste from gtkwindow.c & gtkwidget.c */
static void send_focus_change(GtkWidget *widget, gboolean in)
{
  GdkEvent *fevent = gdk_event_new (GDK_FOCUS_CHANGE);

  fevent->focus_change.type = GDK_FOCUS_CHANGE;
  fevent->focus_change.window = g_object_ref (gtk_widget_get_window (widget));
  fevent->focus_change.in = in;

#if GTK_CHECK_VERSION(2, 22, 0)
  gtk_widget_send_focus_change (widget, fevent);
#else
  g_object_ref (widget);
  if (in)
    GTK_OBJECT_FLAGS (widget) |= GTK_HAS_FOCUS;
  else
    GTK_OBJECT_FLAGS (widget) &= ~(GTK_HAS_FOCUS);
  gtk_widget_event (widget, fevent);
  g_object_notify (G_OBJECT (widget), "has-focus");
  g_object_unref (widget);
#endif

  gdk_event_free (fevent);
}

static void desktop_search_dialog_hide(GtkWidget *search_dialog, FmDesktop *desktop)
{
    /* disconnect the "changed" signal handler */
    if (desktop->search_entry_changed_id != 0)
    {
        g_signal_handler_disconnect(desktop->search_entry, desktop->search_entry_changed_id);
        desktop->search_entry_changed_id = 0;
    }

    /* disable the flush timeout */
    if (desktop->search_timeout_id != 0)
    {
        g_source_remove(desktop->search_timeout_id);
        desktop->search_timeout_id = 0;
    }

    /* send focus-out event */
    send_focus_change(desktop->search_entry, FALSE);
    gtk_widget_hide(search_dialog);
    gtk_entry_set_text(GTK_ENTRY(desktop->search_entry), "");
}

static gboolean desktop_search_delete_event(GtkWidget *widget, GdkEventAny *evt,
                                            FmDesktop *desktop)
{
    /* hide the search dialog */
    desktop_search_dialog_hide(widget, desktop);
    return TRUE;
}

static gboolean desktop_search_timeout(gpointer user_data)
{
    FmDesktop *desktop = FM_DESKTOP(user_data);

    if (!g_source_is_destroyed(g_main_current_source()))
        desktop_search_dialog_hide(desktop->search_window, desktop);
    return FALSE;
}

static void desktop_search_timeout_destroy(gpointer user_data)
{
    FM_DESKTOP(user_data)->search_timeout_id = 0;
}

static void desktop_search_move(GtkWidget *widget, FmDesktop *desktop,
                                gboolean move_up)
{
    GtkTreeModel *model;
    const gchar *text;
    char *casefold, *key, *name;
    FmDesktopItem *item;
    GtkTreeIter it;
    gboolean found = FALSE;

    /* check if we have a model */
    if (desktop->model == NULL)
        return;
    model = GTK_TREE_MODEL(desktop->model);

    /* determine the current text for the search entry */
    text = gtk_entry_get_text(GTK_ENTRY(desktop->search_entry));
    if (G_UNLIKELY(text == NULL || text[0] == '\0'))
        return;

    /* determine the iterator of the focused item */
    if (!get_focused_item(desktop->focus, model, &it))
        return;

    /* normalize the pattern */
    casefold = g_utf8_casefold(text, -1);
    key = g_utf8_normalize(casefold, -1, G_NORMALIZE_ALL);
    g_free(casefold);
    /* let find matched item now */
    if (move_up)
    {
#if GTK_CHECK_VERSION(3, 0, 0)
        while (!found && gtk_tree_model_iter_previous(model, &it))
#else
        GtkTreePath *tp = gtk_tree_model_get_path(model, &it);
        while (!found && gtk_tree_path_prev(tp) && gtk_tree_model_get_iter(model, &it, tp))
#endif
        {
            item = fm_folder_model_get_item_userdata(desktop->model, &it);
            casefold = g_utf8_casefold(fm_file_info_get_disp_name(item->fi), -1);
            name = g_utf8_normalize(casefold, -1, G_NORMALIZE_ALL);
            g_free(casefold);
            found = (strncmp(name, key, strlen(key)) == 0);
            g_free(name);
        }
#if !GTK_CHECK_VERSION(3, 0, 0)
        gtk_tree_path_free(tp);
#endif
    }
    else
    {
        while (!found && gtk_tree_model_iter_next(model, &it))
        {
            item = fm_folder_model_get_item_userdata(desktop->model, &it);
            casefold = g_utf8_casefold(fm_file_info_get_disp_name(item->fi), -1);
            name = g_utf8_normalize(casefold, -1, G_NORMALIZE_ALL);
            g_free(casefold);
            found = (strncmp(name, key, strlen(key)) == 0);
            g_free(name);
        }
    }
    g_free(key);

    if (!found)
        return;

    /* unselect all items */
    _unselect_all(FM_FOLDER_VIEW(desktop));
    /* focus found item */
    item->is_selected = TRUE;
    fm_desktop_item_selected_changed(desktop, item);
    set_focused_item(desktop, item);
}

static gboolean desktop_search_scroll_event(GtkWidget *widget,
                                            GdkEventScroll *evt,
                                            FmDesktop *desktop)
{
    g_return_val_if_fail(GTK_IS_WIDGET(widget), FALSE);
    g_return_val_if_fail(FM_IS_DESKTOP(desktop), FALSE);

    if (evt->direction == GDK_SCROLL_UP)
        desktop_search_move(widget, desktop, TRUE);
    else if (evt->direction == GDK_SCROLL_DOWN)
        desktop_search_move(widget, desktop, FALSE);
    else
        return FALSE;

    return TRUE;
}

static void desktop_search_update_timeout(FmDesktop *desktop)
{
    if (desktop->search_timeout_id == 0)
        return;
    /* drop the previous timeout */
    g_source_remove(desktop->search_timeout_id);
    /* schedule a new timeout */
    desktop->search_timeout_id = gdk_threads_add_timeout_full(G_PRIORITY_LOW,
                                                              DESKTOP_SEARCH_DIALOG_TIMEOUT,
                                                              desktop_search_timeout,
                                                              desktop,
                                                              desktop_search_timeout_destroy);
}

static gboolean desktop_search_key_press_event(GtkWidget *widget,
                                               GdkEventKey *evt,
                                               FmDesktop *desktop)
{
    g_return_val_if_fail(GTK_IS_WIDGET(widget), FALSE);
    g_return_val_if_fail(FM_IS_DESKTOP(desktop), FALSE);

    /* close window and cancel the search */
    if (evt->keyval == GDK_KEY_Escape || evt->keyval == GDK_KEY_Tab)
        desktop_search_dialog_hide(widget, desktop);

    /* select previous matching iter */
    else if (evt->keyval == GDK_KEY_Up || evt->keyval == GDK_KEY_KP_Up)
        desktop_search_move(widget, desktop, TRUE);

    else if (((evt->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
             && (evt->keyval == GDK_KEY_g || evt->keyval == GDK_KEY_G))
        desktop_search_move(widget, desktop, TRUE);

    /* select next matching iter */
    else if (evt->keyval == GDK_KEY_Down || evt->keyval == GDK_KEY_KP_Down)
        desktop_search_move(widget, desktop, FALSE);

    else if (((evt->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == GDK_CONTROL_MASK)
             && (evt->keyval == GDK_KEY_g || evt->keyval == GDK_KEY_G))
        desktop_search_move(widget, desktop, FALSE);

    else
        return FALSE;

    /* renew the flush timeout */
    desktop_search_update_timeout(desktop);

    return TRUE;
}

static void desktop_search_activate(GtkEntry *entry, FmDesktop *desktop)
{
    GtkTreeModel *model;
    GtkTreePath *tp;
    GtkTreeIter it;

    /* hide the interactive search dialog */
    desktop_search_dialog_hide(desktop->search_window, desktop);

    /* check if we have a cursor item, and if so, activate it */
    if (desktop->focus)
    {
        /* only activate the cursor item if it's selected */
        if (desktop->focus->is_selected)
        {
            model = GTK_TREE_MODEL(desktop->model);
            if(get_focused_item(desktop->focus, model, &it))
            {
                tp = gtk_tree_model_get_path(model, &it);
                fm_folder_view_item_clicked(FM_FOLDER_VIEW(desktop), tp, FM_FV_ACTIVATED);
                if(tp)
                    gtk_tree_path_free(tp);
            }
        }
    }
}

#if GTK_CHECK_VERSION(2, 20, 0)
static void desktop_search_preedit_changed(GtkEntry *entry, gchar *preedit,
                                           FmDesktop *desktop)
#else
static void desktop_search_preedit_changed(GtkIMContext *im_context,
                                           FmDesktop *desktop)
#endif
{
    desktop->search_imcontext_changed = TRUE;

    /* re-register the search timeout */
    desktop_search_update_timeout(desktop);
}

static void desktop_search_position(FmDesktop *desktop)
{
    GtkRequisition requisition;
    gint x, y;

    /* make sure the search dialog is realized */
    gtk_widget_realize(desktop->search_window);

#if GTK_CHECK_VERSION(3, 0, 0)
    gtk_widget_get_preferred_size(desktop->search_window, NULL, &requisition);
#else
    gtk_widget_size_request(desktop->search_window, &requisition);
#endif

    /* put it into right upper corner */
    x = desktop->working_area.x + desktop->working_area.width - requisition.width;
    y = desktop->working_area.y;

    gtk_window_move(GTK_WINDOW(desktop->search_window), x, y);
}

static void desktop_search_init(GtkWidget *search_entry, FmDesktop *desktop)
{
    GtkTreeModel *model;
    const gchar *text;
    char *casefold, *key, *name;
    FmDesktopItem *item;
    GtkTreeIter it;
    gboolean found = FALSE;

    /* check if we have a model */
    if (desktop->model == NULL)
        return;
    model = GTK_TREE_MODEL(desktop->model);

    /* renew the flush timeout */
    desktop_search_update_timeout(desktop);

    /* determine the current text for the search entry */
    text = gtk_entry_get_text(GTK_ENTRY(desktop->search_entry));
    if (G_UNLIKELY(text == NULL || text[0] == '\0'))
        return;

    /* unselect all items */
    _unselect_all(FM_FOLDER_VIEW(desktop));

    /* normalize the pattern */
    casefold = g_utf8_casefold(text, -1);
    key = g_utf8_normalize(casefold, -1, G_NORMALIZE_ALL);
    g_free(casefold);
    /* find first matched item now */
    if (gtk_tree_model_get_iter_first(model, &it)) do
    {
        item = fm_folder_model_get_item_userdata(desktop->model, &it);
        casefold = g_utf8_casefold(fm_file_info_get_disp_name(item->fi), -1);
        name = g_utf8_normalize(casefold, -1, G_NORMALIZE_ALL);
        g_free(casefold);
        found = (strncmp(name, key, strlen(key)) == 0);
        g_free(name);
    }
    while (!found && gtk_tree_model_iter_next(model, &it));
    g_free(key);

    /* focus found item */
    if (!found)
        return;
    item->is_selected = TRUE;
    fm_desktop_item_selected_changed(desktop, item);
    set_focused_item(desktop, item);
}

static void desktop_search_ensure_window(FmDesktop *desktop)
{
    GtkWindow *window;
    GtkWidget *frame;
    GtkWidget *vbox;

    /* check if we already have a search window */
    if (G_LIKELY(desktop->search_window != NULL))
        return;

    /* allocate a new search window */
    desktop->search_window = gtk_window_new(GTK_WINDOW_POPUP);
    window = GTK_WINDOW(desktop->search_window);
    gtk_window_group_add_window(win_group, window);
    gtk_window_set_modal(window, TRUE);
    gtk_window_set_screen(window, gtk_widget_get_screen(GTK_WIDGET(desktop)));
    /* connect signal handlers */
    g_signal_connect(window, "delete-event", G_CALLBACK(desktop_search_delete_event), desktop);
    g_signal_connect(window, "scroll-event", G_CALLBACK(desktop_search_scroll_event), desktop);
    g_signal_connect(window, "key-press-event", G_CALLBACK(desktop_search_key_press_event), desktop);

    /* allocate the frame widget */
    frame = g_object_new(GTK_TYPE_FRAME, "shadow-type", GTK_SHADOW_ETCHED_IN, NULL);
    gtk_container_add(GTK_CONTAINER(window), frame);
    gtk_widget_show(frame);

    /* allocate the vertical box */
    vbox = g_object_new(GTK_TYPE_VBOX, "border-width", 3, NULL);
    gtk_container_add(GTK_CONTAINER(frame), vbox);
    gtk_widget_show(vbox);

    /* allocate the search entry widget */
    desktop->search_entry = gtk_entry_new();
    g_signal_connect(desktop->search_entry, "activate", G_CALLBACK(desktop_search_activate), desktop);
#if GTK_CHECK_VERSION(2, 20, 0)
    g_signal_connect(desktop->search_entry, "preedit-changed",
                     G_CALLBACK(desktop_search_preedit_changed), desktop);
#else
    g_signal_connect(GTK_ENTRY(desktop->search_entry)->im_context, "preedit-changed",
                     G_CALLBACK(desktop_search_preedit_changed), desktop);
#endif
    gtk_box_pack_start(GTK_BOX(vbox), desktop->search_entry, TRUE, TRUE, 0);
    gtk_widget_realize(desktop->search_entry);
    gtk_widget_show(desktop->search_entry);
}

static gboolean desktop_search_start(FmDesktop *desktop, gboolean keybinding)
{
    GTypeClass *klass;

    /* check if we already display the search window */
    if (desktop->search_window != NULL && gtk_widget_get_visible(desktop->search_window))
        return TRUE;

    desktop_search_ensure_window(desktop);

    /* clear search entry if we were started by a keybinding */
    if (G_UNLIKELY(keybinding))
        gtk_entry_set_text(GTK_ENTRY(desktop->search_entry), "");

    /* determine the position for the search dialog */
    desktop_search_position(desktop);

    /* display the search dialog */
    gtk_widget_show(desktop->search_window);

    /* connect "changed" signal for the entry */
    if (G_UNLIKELY(desktop->search_entry_changed_id == 0))
        desktop->search_entry_changed_id = g_signal_connect(G_OBJECT(desktop->search_entry),
                                                            "changed",
                                                            G_CALLBACK(desktop_search_init),
                                                            desktop);

    /* start the search timeout */
    desktop->search_timeout_id = gdk_threads_add_timeout_full(G_PRIORITY_LOW,
                                                              DESKTOP_SEARCH_DIALOG_TIMEOUT,
                                                              desktop_search_timeout,
                                                              desktop,
                                                              desktop_search_timeout_destroy);

    /* grab focus will select all the text, we don't want that to happen, so we
     * call the parent instance and bypass the selection change. This is probably
     * really hackish, but GtkTreeView does it as well *hrhr*
     */
    klass = g_type_class_peek_parent(GTK_ENTRY_GET_CLASS(desktop->search_entry));
    (*GTK_WIDGET_CLASS(klass)->grab_focus)(desktop->search_entry);

    /* send focus-in event */
    send_focus_change(desktop->search_entry, TRUE);

    /* search first matching iter */
    desktop_search_init(desktop->search_entry, desktop);

    return TRUE;
}

static gboolean on_key_press(GtkWidget* w, GdkEventKey* evt)
{
    FmDesktop* desktop = (FmDesktop*)w;
    FmDesktopItem* item;
    int modifier = (evt->state & gtk_accelerator_get_default_mod_mask());
    FmPathList* sels;
    GtkTreeModel* model;
    GtkTreePath* tp = NULL;
    GtkTreeIter it;
    char *old_text, *new_text;
    GdkScreen *screen;
    GdkEvent *new_event;
    guint popup_menu_id;
    gboolean retval;

    switch (evt->keyval)
    {
    case GDK_KEY_Escape:
        if (desktop->rubber_bending)
        {
            /* cancel rubberbanding now */
            _stop_rubberbanding(desktop, desktop->drag_start_x, desktop->drag_start_y);
            return TRUE;
        }
        break;
    case GDK_KEY_Left:
        item = get_nearest_item(desktop, desktop->focus, GTK_DIR_LEFT);
        if(item)
        {
            if(0 == modifier)
            {
                _unselect_all(FM_FOLDER_VIEW(desktop));
                item->is_selected = TRUE;
                fm_desktop_item_selected_changed(desktop, item);
            }
            set_focused_item(desktop, item);
        }
        return TRUE;
    case GDK_KEY_Right:
        item = get_nearest_item(desktop, desktop->focus, GTK_DIR_RIGHT);
        if(item)
        {
            if(0 == modifier)
            {
                _unselect_all(FM_FOLDER_VIEW(desktop));
                item->is_selected = TRUE;
                fm_desktop_item_selected_changed(desktop, item);
            }
            set_focused_item(desktop, item);
        }
        return TRUE;
    case GDK_KEY_Up:
        item = get_nearest_item(desktop, desktop->focus, GTK_DIR_UP);
        if(item)
        {
            if(0 == modifier)
            {
                _unselect_all(FM_FOLDER_VIEW(desktop));
                item->is_selected = TRUE;
                fm_desktop_item_selected_changed(desktop, item);
            }
            set_focused_item(desktop, item);
        }
        return TRUE;
    case GDK_KEY_Down:
        item = get_nearest_item(desktop, desktop->focus, GTK_DIR_DOWN);
        if(item)
        {
            if(0 == modifier)
            {
                _unselect_all(FM_FOLDER_VIEW(desktop));
                item->is_selected = TRUE;
                fm_desktop_item_selected_changed(desktop, item);
            }
            set_focused_item(desktop, item);
        }
        return TRUE;
    case GDK_KEY_space:
        if(modifier & GDK_CONTROL_MASK)
        {
            if(desktop->focus)
            {
                desktop->focus->is_selected = !desktop->focus->is_selected;
                redraw_item(desktop, desktop->focus);
                fm_desktop_item_selected_changed(desktop, desktop->focus);
            }
            return TRUE;
        }
        break;
    case GDK_KEY_F2:
        sels = _dup_selected_file_paths(FM_FOLDER_VIEW(desktop));
        if(sels)
        {
            fm_rename_file(GTK_WINDOW(desktop), fm_path_list_peek_head(sels));
            fm_path_list_unref(sels);
            return TRUE;
        }
        break;
    case GDK_KEY_Return:
    case GDK_KEY_ISO_Enter:
    case GDK_KEY_KP_Enter:
        if(modifier == 0 && desktop->focus)
        {
            model = GTK_TREE_MODEL(desktop->model);
            if(get_focused_item(desktop->focus, model, &it))
            {
                tp = gtk_tree_model_get_path(model, &it);
                fm_folder_view_item_clicked(FM_FOLDER_VIEW(desktop), tp, FM_FV_ACTIVATED);
                if(tp)
                    gtk_tree_path_free(tp);
            }
            return TRUE;
        }
        break;
    case GDK_KEY_F:
    case GDK_KEY_f:
        if (modifier == GDK_CONTROL_MASK)
            return desktop_search_start(desktop, TRUE);
        break;
    }
    if (GTK_WIDGET_CLASS(fm_desktop_parent_class)->key_press_event(w, evt))
        return TRUE;

    /* well, let try interactive search now */
    desktop_search_ensure_window(desktop);

    /* make sure the search window is realized */
    gtk_widget_realize(desktop->search_window);

    /* make a copy of the current text */
    old_text = gtk_editable_get_chars(GTK_EDITABLE(desktop->search_entry), 0, -1);

    /* make sure we don't accidently popup the context menu */
    popup_menu_id = g_signal_connect(desktop->search_entry, "popup-menu", G_CALLBACK(gtk_true), NULL);

    /* move the search window offscreen */
    screen = gtk_widget_get_screen(w);
    gtk_window_move(GTK_WINDOW(desktop->search_window),
                    gdk_screen_get_width(screen) + 1,
                    gdk_screen_get_height(screen) + 1);
    gtk_widget_show(desktop->search_window);

    /* allocate a new event to forward */
    new_event = gdk_event_copy((GdkEvent *)evt);
    g_object_unref(new_event->key.window);
    new_event->key.window = g_object_ref(gtk_widget_get_window(desktop->search_entry));

    /* send the event to the search entry. If the "preedit-changed" signal is
     * emitted during this event, priv->search_imcontext_changed will be set. */
    desktop->search_imcontext_changed = FALSE;
    retval = gtk_widget_event(desktop->search_entry, new_event);
    gtk_widget_hide(desktop->search_window);

    /* release the temporary event */
    gdk_event_free(new_event);

    /* disconnect the popup menu prevention */
    g_signal_handler_disconnect(desktop->search_entry, popup_menu_id);

    /* we check to make sure that the entry tried to handle the,
     * and that the text has actually changed. */
    new_text = gtk_editable_get_chars(GTK_EDITABLE(desktop->search_entry), 0, -1);
    retval = retval && (strcmp(new_text, old_text) != 0);
    g_free(old_text);
    g_free(new_text);

    /* if we're in a preedit or the text was modified */
    if (desktop->search_imcontext_changed || retval)
    {
        if (desktop_search_start(desktop, FALSE))
        {
            gtk_widget_grab_focus(w);
            return TRUE;
        }
        else
        {
            gtk_entry_set_text(GTK_ENTRY(desktop->search_entry), "");
            return FALSE;
        }
    }
    return FALSE;
}

#if 0
static void on_style_set(GtkWidget* w, GtkStyle* prev)
{
    FmDesktop* self = (FmDesktop*)w;
    PangoContext* pc = gtk_widget_get_pango_context(w);
    if(font_desc)
        pango_context_set_font_description(pc, font_desc);
    pango_layout_context_changed(self->pl);
}
#endif

static void on_direction_changed(GtkWidget* w, GtkTextDirection prev)
{
    FmDesktop* self = (FmDesktop*)w;
    pango_layout_context_changed(self->pl);
    queue_layout_items(self);
}

static void on_realize(GtkWidget* w)
{
    FmDesktop* self = (FmDesktop*)w;

    GTK_WIDGET_CLASS(fm_desktop_parent_class)->realize(w);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(w), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(w), TRUE);
    gtk_window_set_resizable((GtkWindow*)w, FALSE);

    update_background(self, -1);
}

static gboolean on_focus_in(GtkWidget* w, GdkEventFocus* evt)
{
    FmDesktop* self = (FmDesktop*) w;
    GtkTreeIter it;
#if !GTK_CHECK_VERSION(2, 22, 0)
    GTK_WIDGET_SET_FLAGS(w, GTK_HAS_FOCUS);
#endif
    if(!self->focus && gtk_tree_model_get_iter_first(GTK_TREE_MODEL(self->model), &it))
    {
        self->focus = fm_folder_model_get_item_userdata(self->model, &it);
        fm_desktop_accessible_focus_set(self, self->focus);
    }
    if(self->focus)
        redraw_item(self, self->focus);
    return FALSE;
}

static gboolean on_focus_out(GtkWidget* w, GdkEventFocus* evt)
{
    FmDesktop* self = (FmDesktop*) w;
    if(self->focus)
    {
#if !GTK_CHECK_VERSION(2, 22, 0)
        GTK_WIDGET_UNSET_FLAGS(w, GTK_HAS_FOCUS);
#endif
        redraw_item(self, self->focus);
    }
    return FALSE;
}

/* ---- Drag & Drop support ---- */
static gboolean on_drag_motion (GtkWidget *dest_widget,
                                GdkDragContext *drag_context,
                                gint x, gint y, guint time)
{
    GdkAtom target;
    GdkDragAction action = 0;
    FmDesktop* desktop = FM_DESKTOP(dest_widget);
    FmDesktopItem* item;
    GtkTreeIter it;

    /* check if we're dragging over an item */
    item = hit_test(desktop, &it, x, y);

    /* handle moving desktop items */
    if(!item)
    {
        if(fm_drag_context_has_target(drag_context, desktop_atom)
           && (gdk_drag_context_get_actions(drag_context) & GDK_ACTION_MOVE))
        {
            /* desktop item is being dragged */
            action = GDK_ACTION_MOVE; /* move desktop items */
            fm_dnd_dest_set_dest_file(desktop->dnd_dest, NULL);
        }
    }

    /* FmDndDest will do the rest */
    if(!action)
    {
        fm_dnd_dest_set_dest_file(desktop->dnd_dest,
                                  item ? item->fi : fm_folder_get_info(desktop_folder));
        target = fm_dnd_dest_find_target(desktop->dnd_dest, drag_context);
        if(target != GDK_NONE &&
           fm_dnd_dest_is_target_supported(desktop->dnd_dest, target))
            action = fm_dnd_dest_get_default_action(desktop->dnd_dest, drag_context, target);
    }
    gdk_drag_status(drag_context, action, time);

    if(desktop->drop_hilight != item)
    {
        FmDesktopItem* old_drop = desktop->drop_hilight;
        if(action) /* don't hilight non-dropable item, see #3591767 */
            desktop->drop_hilight = item;
        if(old_drop)
            redraw_item(desktop, old_drop);
        if(item && action)
            redraw_item(desktop, item);
    }

    return (action != 0);
}

static void on_drag_leave (GtkWidget *dest_widget,
                           GdkDragContext *drag_context,
                           guint time)
{
    FmDesktop* desktop = FM_DESKTOP(dest_widget);

    if(desktop->drop_hilight)
    {
        FmDesktopItem* old_drop = desktop->drop_hilight;
        desktop->drop_hilight = NULL;
        redraw_item(desktop, old_drop);
    }
}

static gboolean on_drag_drop (GtkWidget *dest_widget,
                              GdkDragContext *drag_context,
                              gint x, gint y, guint time)
{
    FmDesktop* desktop = FM_DESKTOP(dest_widget);
    FmDesktopItem* item;
    GtkTreeIter it;

    /* check if we're dropping on an item */
    item = hit_test(desktop, &it, x, y);

    /* handle moving desktop items */
    if(!item)
    {
        if(fm_drag_context_has_target(drag_context, desktop_atom)
           && (gdk_drag_context_get_actions(drag_context) & GDK_ACTION_MOVE))
        {
            /* desktop item is being dragged */
            gtk_drag_get_data(dest_widget, drag_context, desktop_atom, time);
            return TRUE;
        }
    }
    return FALSE;
}

static void on_drag_data_received (GtkWidget *dest_widget,
                                   GdkDragContext *drag_context,
                                   gint x, gint y, GtkSelectionData *sel_data,
                                   guint info, guint time)
{
    FmDesktop* desktop = FM_DESKTOP(dest_widget);
    GList *items, *l;
    int offset_x, offset_y;

    if(info != FM_DND_DEST_DESKTOP_ITEM)
        return;

    /* desktop items are being dragged */
    items = get_selected_items(desktop, NULL);
    offset_x = x - desktop->drag_start_x;
    offset_y = y - desktop->drag_start_y;
    for(l = items; l; l=l->next)
    {
        FmDesktopItem* item = (FmDesktopItem*)l->data;
        move_item(desktop, item, item->area.x + offset_x, item->area.y + offset_y, FALSE);
    }
    g_list_free(items);

    /* save position of desktop icons on next idle */
    queue_config_save(desktop);

    queue_layout_items(desktop);

    gtk_drag_finish(drag_context, TRUE, FALSE, time);
}

static void on_dnd_src_data_get(FmDndSrc* ds, FmDesktop* desktop)
{
    FmFileInfoList* files = _dup_selected_files(FM_FOLDER_VIEW(desktop));
    if(files)
    {
        fm_dnd_src_set_files(ds, files);
        fm_file_info_list_unref(files);
    }
}

/* ---------------------------------------------------------------------
    FmDesktop class main handlers */

#if 0
static void on_desktop_model_destroy(gpointer data, GObject* model)
{
    g_signal_handlers_disconnect_by_func(app_config, on_big_icon_size_changed, model);
    *(gpointer*)data = NULL;
}
#endif

#if FM_CHECK_VERSION(1, 0, 2)
static void on_sort_changed(GtkTreeSortable *model, FmDesktop *desktop)
{
    FmFolderModelCol by;
    FmSortMode type;

    if (!fm_folder_model_get_sort(FM_FOLDER_MODEL(model), &by, &type))
        /* FIXME: print error if failed */
        return;
    if (type == desktop->conf.desktop_sort_type &&
        by == desktop->conf.desktop_sort_by) /* not changed */
        return;
    desktop->conf.desktop_sort_type = type;
    desktop->conf.desktop_sort_by = by;
    queue_config_save(desktop);
}
#endif

static inline void connect_model(FmDesktop* desktop)
{
    /* FIXME: different screens should be able to use different models */
    desktop->model = fm_folder_model_new(desktop_folder, FALSE);
    fm_folder_model_set_icon_size(desktop->model, fm_config->big_icon_size);
    g_signal_connect(app_config, "changed::big_icon_size",
                     G_CALLBACK(on_big_icon_size_changed), desktop->model);
    g_signal_connect(desktop->model, "row-deleting", G_CALLBACK(on_row_deleting), desktop);
    g_signal_connect(desktop->model, "row-inserted", G_CALLBACK(on_row_inserted), desktop);
    g_signal_connect(desktop->model, "row-deleted", G_CALLBACK(on_row_deleted), desktop);
    g_signal_connect(desktop->model, "row-changed", G_CALLBACK(on_row_changed), desktop);
    g_signal_connect(desktop->model, "rows-reordered", G_CALLBACK(on_rows_reordered), desktop);
#if FM_CHECK_VERSION(1, 0, 2)
    fm_folder_model_set_sort(desktop->model, desktop->conf.desktop_sort_by,
                             desktop->conf.desktop_sort_type);
    g_signal_connect(desktop->model, "sort-column-changed", G_CALLBACK(on_sort_changed), desktop);
#else
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(desktop->model),
                                         desktop->conf.desktop_sort_by,
                                         desktop->conf.desktop_sort_type);
#endif
}

static inline void disconnect_model(FmDesktop* desktop)
{
    g_signal_handlers_disconnect_by_func(app_config, on_big_icon_size_changed, desktop->model);
    g_signal_handlers_disconnect_by_func(desktop->model, on_row_deleting, desktop);
    g_signal_handlers_disconnect_by_func(desktop->model, on_row_inserted, desktop);
    g_signal_handlers_disconnect_by_func(desktop->model, on_row_deleted, desktop);
    g_signal_handlers_disconnect_by_func(desktop->model, on_row_changed, desktop);
    g_signal_handlers_disconnect_by_func(desktop->model, on_rows_reordered, desktop);
#if FM_CHECK_VERSION(1, 0, 2)
    g_signal_handlers_disconnect_by_func(desktop->model, on_sort_changed, desktop);
#endif
    g_object_unref(desktop->model);
    desktop->model = NULL;
}

static void _clear_bg_cache(FmDesktop *self)
{
    while(self->cache)
    {
        FmBackgroundCache *bg = self->cache;

        self->cache = bg->next;
#if GTK_CHECK_VERSION(3, 0, 0)
        XFreePixmap(cairo_xlib_surface_get_display(bg->bg),
                    cairo_xlib_surface_get_drawable(bg->bg));
        cairo_surface_destroy(bg->bg);
#else
        g_object_unref(bg->bg);
#endif
        g_free(bg->filename);
        g_free(bg);
    }
}

#if FM_CHECK_VERSION(1, 2, 0)
static void on_show_full_names_changed(FmConfig *cfg, FmDesktop *self)
{
    if (fm_config->show_full_names)
    {
        self->pango_text_h = -1;
        pango_layout_set_ellipsize(self->pl, PANGO_ELLIPSIZE_NONE);
    }
    else
    {
        self->pango_text_h = self->text_h * PANGO_SCALE;
        pango_layout_set_ellipsize(self->pl, PANGO_ELLIPSIZE_END);
    }
    queue_layout_items(self);
}
#endif

#if GTK_CHECK_VERSION(3, 0, 0)
static void fm_desktop_destroy(GtkWidget *object)
#else
static void fm_desktop_destroy(GtkObject *object)
#endif
{
    FmDesktop *self;
    GdkScreen* screen;

    self = FM_DESKTOP(object);
    if(self->model) /* see bug #3533958 by korzhpavel@SF */
    {
        screen = gtk_widget_get_screen((GtkWidget*)self);
        gdk_window_remove_filter(gdk_screen_get_root_window(screen), on_root_event, self);

        g_signal_handlers_disconnect_by_func(screen, on_screen_size_changed, self);
#if FM_CHECK_VERSION(1, 2, 0)
        g_signal_handlers_disconnect_by_func(app_config, on_show_full_names_changed, self);
#endif

        gtk_window_group_remove_window(win_group, (GtkWindow*)self);

        disconnect_model(self);

        unload_items(self);

        g_object_unref(self->icon_render);
        g_object_unref(self->pl);

        if(self->single_click_timeout_handler)
            g_source_remove(self->single_click_timeout_handler);

        if(self->idle_layout)
            g_source_remove(self->idle_layout);

        g_signal_handlers_disconnect_by_func(self->dnd_src, on_dnd_src_data_get, self);
        g_object_unref(self->dnd_src);
        g_object_unref(self->dnd_dest);
    }

    if (self->conf.configured)
    {
        self->conf.configured = FALSE;
        if (self->conf.changed) /* if config was changed then save it now */
            save_item_pos(self);
        g_free(self->conf.wallpaper);
        if (self->conf.wallpapers_configured > 0)
        {
            int i;
            for (i = 0; i < self->conf.wallpapers_configured; i++)
                g_free(self->conf.wallpapers[i]);
            g_free(self->conf.wallpapers);
        }
        g_free(self->conf.desktop_font);
    }

    _clear_bg_cache(self);

    /* cancel any pending search timeout */
    if (G_UNLIKELY(self->search_timeout_id))
    {
        g_source_remove(self->search_timeout_id);
        self->search_timeout_id = 0;
    }

    /* destroy the interactive search dialog */
    if (G_UNLIKELY(self->search_window))
    {
        g_signal_handlers_disconnect_by_func(self->search_window, desktop_search_delete_event, self);
        g_signal_handlers_disconnect_by_func(self->search_window, desktop_search_scroll_event, self);
        g_signal_handlers_disconnect_by_func(self->search_window, desktop_search_key_press_event, self);
        gtk_widget_destroy(self->search_window);
        self->search_entry = NULL;
        self->search_window = NULL;
    }

#if GTK_CHECK_VERSION(3, 0, 0)
    GTK_WIDGET_CLASS(fm_desktop_parent_class)->destroy(object);
#else
    GTK_OBJECT_CLASS(fm_desktop_parent_class)->destroy(object);
#endif
}

static void fm_desktop_init(FmDesktop *self)
{
}

/* we should have a constructor to handle parameters */
static GObject* fm_desktop_constructor(GType type, guint n_construct_properties,
                                       GObjectConstructParam *construct_properties)
{
    GObject* object = G_OBJECT_CLASS(fm_desktop_parent_class)->constructor(type, n_construct_properties, construct_properties);
    FmDesktop* self = (FmDesktop*)object;
    GdkScreen* screen = gtk_widget_get_screen((GtkWidget*)self);
    GdkWindow* root;
    guint i;
    gint n;
    GdkRectangle geom;

    for(i = 0; i < n_construct_properties; i++)
        if(!strcmp(construct_properties[i].pspec->name, "monitor")
           && G_VALUE_HOLDS_INT(construct_properties[i].value))
            self->monitor = g_value_get_int(construct_properties[i].value);
    if(self->monitor < 0)
        return object; /* this monitor is disabled */
    g_debug("fm_desktop_constructor for monitor %d", self->monitor);
    gdk_screen_get_monitor_geometry(screen, self->monitor, &geom);
    gtk_window_set_default_size((GtkWindow*)self, geom.width, geom.height);
    gtk_window_move(GTK_WINDOW(self), geom.x, geom.y);
    gtk_widget_set_app_paintable((GtkWidget*)self, TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(self), GDK_WINDOW_TYPE_HINT_DESKTOP);
    gtk_widget_add_events((GtkWidget*)self,
                        GDK_POINTER_MOTION_MASK |
                        GDK_BUTTON_PRESS_MASK |
                        GDK_BUTTON_RELEASE_MASK |
                        GDK_KEY_PRESS_MASK|
                        GDK_PROPERTY_CHANGE_MASK);

    self->icon_render = fm_cell_renderer_pixbuf_new();
    g_object_set(self->icon_render, "follow-state", TRUE, NULL);
    g_object_ref_sink(self->icon_render);
    fm_cell_renderer_pixbuf_set_fixed_size(FM_CELL_RENDERER_PIXBUF(self->icon_render), fm_config->big_icon_size, fm_config->big_icon_size);

    /* FIXME: call pango_layout_context_changed() on the layout in response to the
     * "style-set" and "direction-changed" signals for the widget. */
    //pc = gtk_widget_get_pango_context((GtkWidget*)self);
    self->pl = gtk_widget_create_pango_layout((GtkWidget*)self, NULL);
    pango_layout_set_alignment(self->pl, PANGO_ALIGN_CENTER);
#if FM_CHECK_VERSION(1, 2, 0)
    if (fm_config->show_full_names)
        pango_layout_set_ellipsize(self->pl, PANGO_ELLIPSIZE_NONE);
    else
#endif
        pango_layout_set_ellipsize(self->pl, PANGO_ELLIPSIZE_END);
    pango_layout_set_wrap(self->pl, PANGO_WRAP_WORD_CHAR);
#if FM_CHECK_VERSION(1, 2, 0)
    g_signal_connect(app_config, "changed::show_full_names",
                     G_CALLBACK(on_show_full_names_changed), self);
#endif

    root = gdk_screen_get_root_window(screen);
    gdk_window_set_events(root, gdk_window_get_events(root)|GDK_PROPERTY_CHANGE_MASK);
    gdk_window_add_filter(root, on_root_event, self);
    g_signal_connect(screen, "size-changed", G_CALLBACK(on_screen_size_changed), self);

    n = get_desktop_for_root_window(root);
    if(n < 0)
        n = 0;
    self->cur_desktop = (guint)n;

    /* init dnd support */
    self->dnd_src = fm_dnd_src_new((GtkWidget*)self);
    /* add our own targets */
    fm_dnd_src_add_targets((GtkWidget*)self, dnd_targets, G_N_ELEMENTS(dnd_targets));
    g_signal_connect(self->dnd_src, "data-get", G_CALLBACK(on_dnd_src_data_get), self);

    self->dnd_dest = fm_dnd_dest_new_with_handlers((GtkWidget*)self);
    fm_dnd_dest_add_targets((GtkWidget*)self, dnd_targets, G_N_ELEMENTS(dnd_targets));

    gtk_window_group_add_window(win_group, GTK_WINDOW(self));

    connect_model(self);

    fm_folder_view_add_popup(FM_FOLDER_VIEW(self), GTK_WINDOW(self),
                             fm_desktop_update_popup);

    return object;
}

FmDesktop *fm_desktop_new(GdkScreen* screen, gint monitor)
{
    return g_object_new(FM_TYPE_DESKTOP, "screen", screen, "monitor", monitor, NULL);
}

static void fm_desktop_set_property(GObject *object, guint property_id,
                                    const GValue *value, GParamSpec *pspec)
{
    switch(property_id)
    {
        case PROP_MONITOR:
            FM_DESKTOP(object)->monitor = g_value_get_int(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void fm_desktop_get_property(GObject *object, guint property_id,
                                    GValue *value, GParamSpec *pspec)
{
    switch(property_id)
    {
        case PROP_MONITOR:
            g_value_set_int(value, FM_DESKTOP(object)->monitor);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

/* init for FmDesktop class */
static void fm_desktop_class_init(FmDesktopClass *klass)
{
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    typedef gboolean (*DeleteEvtHandler) (GtkWidget*, GdkEventAny*);
    char* atom_names[] = {"_NET_WORKAREA", "_NET_NUMBER_OF_DESKTOPS",
                          "_NET_CURRENT_DESKTOP", "_XROOTMAP_ID", "_XROOTPMAP_ID"};
    Atom atoms[G_N_ELEMENTS(atom_names)] = {0};
    GObjectClass* object_class = G_OBJECT_CLASS(klass);

#if GTK_CHECK_VERSION(3, 0, 0)
    widget_class->destroy = fm_desktop_destroy;
    widget_class->draw = on_draw;
    widget_class->get_preferred_width = on_get_preferred_width;
    widget_class->get_preferred_height = on_get_preferred_height;
#else
    GtkObjectClass *gtk_object_class = GTK_OBJECT_CLASS(klass);
    gtk_object_class->destroy = fm_desktop_destroy;

    widget_class->expose_event = on_expose;
    widget_class->size_request = on_size_request;
#endif
    widget_class->size_allocate = on_size_allocate;
    widget_class->button_press_event = on_button_press;
    widget_class->button_release_event = on_button_release;
    widget_class->motion_notify_event = on_motion_notify;
    widget_class->leave_notify_event = on_leave_notify;
    widget_class->key_press_event = on_key_press;
    /* widget_class->style_set = on_style_set; */
    widget_class->direction_changed = on_direction_changed;
    widget_class->realize = on_realize;
    widget_class->focus_in_event = on_focus_in;
    widget_class->focus_out_event = on_focus_out;
    /* widget_class->scroll_event = on_scroll; */
    widget_class->delete_event = (DeleteEvtHandler)gtk_true;
    widget_class->get_accessible = fm_desktop_get_accessible;

    widget_class->drag_motion = on_drag_motion;
    widget_class->drag_drop = on_drag_drop;
    widget_class->drag_data_received = on_drag_data_received;
    widget_class->drag_leave = on_drag_leave;
    /* widget_class->drag_data_get = on_drag_data_get; */

    if(XInternAtoms(gdk_x11_get_default_xdisplay(), atom_names,
                    G_N_ELEMENTS(atom_names), False, atoms))
    {
        XA_NET_WORKAREA = atoms[0];
        XA_NET_NUMBER_OF_DESKTOPS = atoms[1];
        XA_NET_CURRENT_DESKTOP = atoms[2];
        XA_XROOTMAP_ID = atoms[3];
        XA_XROOTPMAP_ID = atoms[4];
    }

    object_class->constructor = fm_desktop_constructor;
    object_class->set_property = fm_desktop_set_property;
    object_class->get_property = fm_desktop_get_property;

    g_object_class_install_property(object_class, PROP_MONITOR,
        g_param_spec_int("monitor", "Monitor",
                         "Monitor number where desktop is",
                         0, 127, 0, G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

    desktop_atom = gdk_atom_intern_static_string(dnd_targets[0].target);
}


//static void on_clicked(FmFolderView* fv, FmFolderViewClickType type, FmFileInfo* file)

//static void on_sel_changed(FmFolderView* fv, FmFileInfoList* sels)

//static void on_sort_changed(FmFolderView* fv)

/* ---------------------------------------------------------------------
    FmFolderView interface implementation */

static void _set_sel_mode(FmFolderView* fv, GtkSelectionMode mode)
{
    /* not implemented */
}

static GtkSelectionMode _get_sel_mode(FmFolderView* fv)
{
    return GTK_SELECTION_MULTIPLE;
}

#if !FM_CHECK_VERSION(1, 0, 2)
static void _set_sort(FmFolderView* fv, GtkSortType type, FmFolderModelViewCol by)
{
    if(type == (GtkSortType)desktop->conf.desktop_sort_type &&
       by == (FmFolderModelViewCol)desktop->conf.desktop_sort_by)
        return;
    desktop->conf.desktop_sort_type = type;
    desktop->conf.desktop_sort_by = by;
    pcmanfm_save_config(FALSE);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(FM_DESKTOP(fv)->model),
                                         by, type);
}

static void _get_sort(FmFolderView* fv, GtkSortType* type, FmFolderModelViewCol* by)
{
    if(type)
        *type = desktop->conf.desktop_sort_type;
    if(by)
        *by = desktop->conf.desktop_sort_by;
}
#endif

static void _set_show_hidden(FmFolderView* fv, gboolean show)
{
    /* not implemented */
}

static gboolean _get_show_hidden(FmFolderView* fv)
{
    return FALSE;
}

static FmFolder* _get_folder(FmFolderView* fv)
{
    return desktop_folder;
}

static void _set_model(FmFolderView* fv, FmFolderModel* model)
{
    /* not implemented */
}

static FmFolderModel* _get_model(FmFolderView* fv)
{
    return FM_DESKTOP(fv)->model;
}

static gint _count_selected_files(FmFolderView* fv)
{
    FmDesktop* desktop = FM_DESKTOP(fv);
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GtkTreeIter it;
    gint n = 0;
    if(!gtk_tree_model_get_iter_first(model, &it))
        return 0;
    do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        if(item->is_selected)
            n++;
    }
    while(gtk_tree_model_iter_next(model, &it));
    return n;
}

static FmFileInfoList* _dup_selected_files(FmFolderView* fv)
{
    FmDesktop* desktop = FM_DESKTOP(fv);
    FmFileInfoList* files = NULL;
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GtkTreeIter it;
    if(!gtk_tree_model_get_iter_first(model, &it))
        return NULL;
    do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        if(item->is_selected)
        {
            if(!files)
                files = fm_file_info_list_new();
            fm_file_info_list_push_tail(files, item->fi);
        }
    }
    while(gtk_tree_model_iter_next(model, &it));
    return files;
}

static FmPathList* _dup_selected_file_paths(FmFolderView* fv)
{
    FmDesktop* desktop = FM_DESKTOP(fv);
    FmPathList* files = NULL;
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    GtkTreeIter it;
    if(!gtk_tree_model_get_iter_first(model, &it))
        return NULL;
    do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        if(item->is_selected)
        {
            if(!files)
                files = fm_path_list_new();
            fm_path_list_push_tail(files, fm_file_info_get_path(item->fi));
        }
    }
    while(gtk_tree_model_iter_next(model, &it));
    return files;
}

static void _select_all(FmFolderView* fv)
{
    FmDesktop* desktop = FM_DESKTOP(fv);
    GtkTreeIter it;
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    if(!gtk_tree_model_get_iter_first(model, &it))
        return;
    do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        if(!item->is_selected)
        {
            item->is_selected = TRUE;
            redraw_item(desktop, item);
            fm_desktop_item_selected_changed(desktop, item);
        }
    }
    while(gtk_tree_model_iter_next(model, &it));
}

static void _unselect_all(FmFolderView* fv)
{
    FmDesktop* desktop = FM_DESKTOP(fv);
    GtkTreeIter it;
    GtkTreeModel* model = GTK_TREE_MODEL(desktop->model);
    if(!gtk_tree_model_get_iter_first(model, &it))
        return;
    do
    {
        FmDesktopItem* item = fm_folder_model_get_item_userdata(desktop->model, &it);
        if(item->is_selected)
        {
            item->is_selected = FALSE;
            redraw_item(desktop, item);
            fm_desktop_item_selected_changed(desktop, item);
        }
    }
    while(gtk_tree_model_iter_next(model, &it));
}

static void _select_invert(FmFolderView* fv)
{
    /* not implemented */
}

static void _select_file_path(FmFolderView* fv, FmPath* path)
{
    /* not implemented */
}

static void _get_custom_menu_callbacks(FmFolderView* fv,
                                       FmFolderViewUpdatePopup* popup,
                                       FmLaunchFolderFunc* launch)
{
    if(popup)
        *popup = fm_desktop_update_item_popup;
    if(launch)
        *launch = pcmanfm_open_folder;
}

/* init for FmFolderView interface implementation */
static void fm_desktop_view_init(FmFolderViewInterface* iface)
{
    iface->set_sel_mode = _set_sel_mode;
    iface->get_sel_mode = _get_sel_mode;
#if !FM_CHECK_VERSION(1, 0, 2)
    iface->set_sort = _set_sort;
    iface->get_sort = _get_sort;
#endif
    iface->set_show_hidden = _set_show_hidden;
    iface->get_show_hidden = _get_show_hidden;
    iface->get_folder = _get_folder;
    iface->set_model = _set_model;
    iface->get_model = _get_model;
    iface->count_selected_files = _count_selected_files;
    iface->dup_selected_files = _dup_selected_files;
    iface->dup_selected_file_paths = _dup_selected_file_paths;
    iface->select_all = _select_all;
    //iface->unselect_all = _unselect_all;
    iface->select_invert = _select_invert;
    iface->select_file_path = _select_file_path;
    iface->get_custom_menu_callbacks = _get_custom_menu_callbacks;
}


/* ---------------------------------------------------------------------
    Desktop preferences */

static GtkWindow* desktop_pref_dlg = NULL;

static void on_response(GtkDialog* dlg, int res, GtkWindow** pdlg)
{
    *pdlg = NULL;
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

/* preferences setup is tightly linked with desktop so should be here */
static void on_wallpaper_set(GtkFileChooserButton* btn, FmDesktop *desktop)
{
    char* file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(btn));
    g_free(desktop->conf.wallpaper);
    desktop->conf.wallpaper = file;
    queue_config_save(desktop);
    update_background(desktop, 0);
}

static void on_update_img_preview( GtkFileChooser *chooser, GtkImage* img )
{
    char* file = gtk_file_chooser_get_preview_filename( chooser );
    GdkPixbuf* pix = NULL;
    if( file )
    {
        pix = gdk_pixbuf_new_from_file_at_scale( file, 128, 128, TRUE, NULL );
        g_free( file );
    }
    if( pix )
    {
        gtk_file_chooser_set_preview_widget_active(chooser, TRUE);
        gtk_image_set_from_pixbuf( img, pix );
        g_object_unref( pix );
    }
    else
    {
        gtk_image_clear( img );
        gtk_file_chooser_set_preview_widget_active(chooser, FALSE);
    }
}

static void on_wallpaper_mode_changed(GtkComboBox* combo, FmDesktop *desktop)
{
    int sel = gtk_combo_box_get_active(combo);

    if(sel >= 0 && sel != (int)desktop->conf.wallpaper_mode)
    {
        desktop->conf.wallpaper_mode = sel;
        queue_config_save(desktop);
        update_background(desktop, 0);
    }
}

static void on_wallpaper_mode_changed2(GtkComboBox *combo, GtkWidget *wallpaper_box)
{
    /* update the box */
    if (wallpaper_box)
        gtk_widget_set_sensitive(wallpaper_box,
                                 gtk_combo_box_get_active(combo) != FM_WP_COLOR);
}

static void on_bg_color_set(GtkColorButton *btn, FmDesktop *desktop)
{
    GdkColor new_val;

    gtk_color_button_get_color(btn, &new_val);
    if (!gdk_color_equal(&desktop->conf.desktop_bg, &new_val))
    {
        desktop->conf.desktop_bg = new_val;
        queue_config_save(desktop);
        update_background(desktop, 0);
    }
}

static void on_wallpaper_common_toggled(GtkToggleButton* btn, FmDesktop *desktop)
{
    gboolean new_val = gtk_toggle_button_get_active(btn);

    if(desktop->conf.wallpaper_common != new_val)
    {
        desktop->conf.wallpaper_common = new_val;
        queue_config_save(desktop);
        update_background(desktop, 0);
    }
}

static void on_fg_color_set(GtkColorButton *btn, FmDesktop *desktop)
{
    GdkColor new_val;

    gtk_color_button_get_color(btn, &new_val);
    if (!gdk_color_equal(&desktop->conf.desktop_fg, &new_val))
    {
        desktop->conf.desktop_fg = new_val;
        queue_config_save(desktop);
        gtk_widget_queue_draw(GTK_WIDGET(desktop));
    }
}

static void on_shadow_color_set(GtkColorButton *btn, FmDesktop *desktop)
{
    GdkColor new_val;

    gtk_color_button_get_color(btn, &new_val);
    if (!gdk_color_equal(&desktop->conf.desktop_shadow, &new_val))
    {
        desktop->conf.desktop_shadow = new_val;
        queue_config_save(desktop);
        gtk_widget_queue_draw(GTK_WIDGET(desktop));
    }
}

static void on_wm_menu_toggled(GtkToggleButton* btn, FmDesktop *desktop)
{
    gboolean new_val = gtk_toggle_button_get_active(btn);

    if(desktop->conf.show_wm_menu != new_val)
    {
        desktop->conf.show_wm_menu = new_val;
        queue_config_save(desktop);
    }
}

static void on_desktop_font_set(GtkFontButton* btn, FmDesktop *desktop)
{
    const char* font = gtk_font_button_get_font_name(btn);

    if(font)
    {
        PangoFontDescription *font_desc;

        g_free(desktop->conf.desktop_font);
        desktop->conf.desktop_font = g_strdup(font);
        queue_config_save(desktop);
        if(desktop->monitor < 0)
            return;
        font_desc = pango_font_description_from_string(desktop->conf.desktop_font);
        if(font_desc)
        {
            PangoContext* pc = gtk_widget_get_pango_context((GtkWidget*)desktop);

            pango_context_set_font_description(pc, font_desc);
            pango_layout_context_changed(desktop->pl);
            gtk_widget_queue_resize(GTK_WIDGET(desktop));
            /* layout_items(desktop); */
            pango_font_description_free(font_desc);
        }
        /* gtk_widget_queue_draw(GTK_WIDGET(desktop)); */
    }
}

static void on_desktop_folder_new_win_toggled(GtkToggleButton* btn, FmDesktop *desktop)
{
    app_config->desktop_folder_new_win = gtk_toggle_button_get_active(btn);
    pcmanfm_save_config(FALSE);
}

#if FM_CHECK_VERSION(1, 2, 0)
static void on_show_documents_toggled(GtkToggleButton* btn, FmDesktop *desktop)
{
    gboolean new_val = gtk_toggle_button_get_active(btn);

    if(desktop->conf.show_documents != new_val)
    {
        desktop->conf.show_documents = new_val;
        queue_config_save(desktop);
        if (documents && documents->fi)
        {
            if (new_val)
                fm_folder_model_extra_file_add(desktop->model, documents->fi,
                                               FM_FOLDER_MODEL_ITEMPOS_PRE);
            else
                fm_folder_model_extra_file_remove(desktop->model, documents->fi);
        }
    }
}

static void on_show_trash_toggled(GtkToggleButton* btn, FmDesktop *desktop)
{
    gboolean new_val = gtk_toggle_button_get_active(btn);

    if(desktop->conf.show_trash != new_val)
    {
        desktop->conf.show_trash = new_val;
        queue_config_save(desktop);
        if (trash_can && trash_can->fi)
        {
            if (new_val)
                fm_folder_model_extra_file_add(desktop->model, trash_can->fi,
                                               FM_FOLDER_MODEL_ITEMPOS_PRE);
            else
                fm_folder_model_extra_file_remove(desktop->model, trash_can->fi);
        }
    }
}

static void on_show_mounts_toggled(GtkToggleButton* btn, FmDesktop *desktop)
{
    GSList *msl;
    gboolean new_val = gtk_toggle_button_get_active(btn);

    if(desktop->conf.show_mounts != new_val)
    {
        desktop->conf.show_mounts = new_val;
        queue_config_save(desktop);
        for (msl = mounts; msl; msl = msl->next)
        {
            FmDesktopExtraItem *mount = msl->data;
            if (new_val)
                fm_folder_model_extra_file_add(desktop->model, mount->fi,
                                               FM_FOLDER_MODEL_ITEMPOS_POST);
            else
                fm_folder_model_extra_file_remove(desktop->model, mount->fi);
        }
    }
}
#endif

void fm_desktop_preference(GtkAction *act, FmDesktop *desktop)
{
    if (desktop == NULL)
        return;

    if(!desktop_pref_dlg)
    {
        GtkBuilder* builder;
        GObject *item;
        GtkWidget *img_preview;

        builder = gtk_builder_new();
        gtk_builder_add_from_file(builder, PACKAGE_UI_DIR "/desktop-pref.ui", NULL);
        desktop_pref_dlg = GTK_WINDOW(gtk_builder_get_object(builder, "dlg"));
        item = gtk_builder_get_object(builder, "wallpaper");
        g_signal_connect(item, "file-set", G_CALLBACK(on_wallpaper_set), desktop);
        img_preview = gtk_image_new();
        gtk_misc_set_alignment(GTK_MISC(img_preview), 0.5, 0.0);
        gtk_widget_set_size_request( img_preview, 128, 128 );
        gtk_file_chooser_set_preview_widget(GTK_FILE_CHOOSER(item), img_preview);
        g_signal_connect( item, "update-preview", G_CALLBACK(on_update_img_preview), img_preview );
        if(desktop->conf.wallpaper)
            gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(item), desktop->conf.wallpaper);
        item = gtk_builder_get_object(builder, "wallpaper_mode");
        gtk_combo_box_set_active(GTK_COMBO_BOX(item), desktop->conf.wallpaper_mode);
        g_signal_connect(item, "changed", G_CALLBACK(on_wallpaper_mode_changed), desktop);
        item = gtk_builder_get_object(builder, "wallpaper_box");
        if (item)
        {
            g_signal_connect(gtk_builder_get_object(builder, "wallpaper_mode"),
                             "changed", G_CALLBACK(on_wallpaper_mode_changed2),
                             item);
            gtk_widget_set_sensitive(GTK_WIDGET(item),
                                     desktop->conf.wallpaper_mode != FM_WP_COLOR);
        }
        item = gtk_builder_get_object(builder, "desktop_bg");
        gtk_color_button_set_color(GTK_COLOR_BUTTON(item), &desktop->conf.desktop_bg);
        g_signal_connect(item, "color-set", G_CALLBACK(on_bg_color_set), desktop);
        item = gtk_builder_get_object(builder, "wallpaper_common");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(item), desktop->conf.wallpaper_common);
        g_signal_connect(item, "toggled", G_CALLBACK(on_wallpaper_common_toggled), desktop);
        item = gtk_builder_get_object(builder, "desktop_fg");
        gtk_color_button_set_color(GTK_COLOR_BUTTON(item), &desktop->conf.desktop_fg);
        g_signal_connect(item, "color-set", G_CALLBACK(on_fg_color_set), desktop);
        item = gtk_builder_get_object(builder, "desktop_shadow");
        gtk_color_button_set_color(GTK_COLOR_BUTTON(item), &desktop->conf.desktop_shadow);
        g_signal_connect(item, "color-set", G_CALLBACK(on_shadow_color_set), desktop);
        item = gtk_builder_get_object(builder, "show_wm_menu");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(item), desktop->conf.show_wm_menu);
        g_signal_connect(item, "toggled", G_CALLBACK(on_wm_menu_toggled), desktop);
        item = gtk_builder_get_object(builder, "desktop_font");
        if(desktop->conf.desktop_font)
            gtk_font_button_set_font_name(GTK_FONT_BUTTON(item), desktop->conf.desktop_font);
        g_signal_connect(item, "font-set", G_CALLBACK(on_desktop_font_set), desktop);
        item = gtk_builder_get_object(builder, "desktop_folder_new_win");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(item), app_config->desktop_folder_new_win);
        gtk_widget_show(GTK_WIDGET(item));
        g_signal_connect(item, "toggled", G_CALLBACK(on_desktop_folder_new_win_toggled), desktop);
#if FM_CHECK_VERSION(1, 2, 0)
        item = gtk_builder_get_object(builder, "show_documents");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(item), desktop->conf.show_documents);
        gtk_widget_set_sensitive(GTK_WIDGET(item), documents != NULL);
        g_signal_connect(item, "toggled", G_CALLBACK(on_show_documents_toggled), desktop);
        item = gtk_builder_get_object(builder, "show_trash");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(item), desktop->conf.show_trash);
        gtk_widget_set_sensitive(GTK_WIDGET(item), trash_can != NULL);
        g_signal_connect(item, "toggled", G_CALLBACK(on_show_trash_toggled), desktop);
        item = gtk_builder_get_object(builder, "show_mounts");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(item), desktop->conf.show_mounts);
        g_signal_connect(item, "toggled", G_CALLBACK(on_show_mounts_toggled), desktop);
        gtk_widget_show(GTK_WIDGET(gtk_builder_get_object(builder, "icons_page")));
#endif

        g_signal_connect(desktop_pref_dlg, "response", G_CALLBACK(on_response), &desktop_pref_dlg);
        g_object_unref(builder);

        pcmanfm_ref();
        g_signal_connect(desktop_pref_dlg, "destroy", G_CALLBACK(pcmanfm_unref), NULL);
        /* make dialog be valid only before the desktop is destroyed */
        gtk_window_set_transient_for(desktop_pref_dlg, GTK_WINDOW(desktop));
        gtk_window_set_destroy_with_parent(desktop_pref_dlg, TRUE);
    }
    gtk_window_present(desktop_pref_dlg);
}


/* ---------------------------------------------------------------------
    Interface functions */

void fm_desktop_manager_init(gint on_screen)
{
    GdkDisplay * gdpy;
    int i, n_scr, n_mon, scr, mon;
    const char* desktop_path;
#if FM_CHECK_VERSION(1, 2, 0)
    GFile *gf;
#endif

    if(! win_group)
        win_group = gtk_window_group_new();

    /* create the ~/Desktop folder if it doesn't exist. */
    desktop_path = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    /* FIXME: should we use a localized folder name instead? */
    g_mkdir_with_parents(desktop_path, 0700); /* ensure the existance of Desktop folder. */
    /* FIXME: should we store the desktop folder path in the annoying ~/.config/user-dirs.dirs file? */

    /* FIXME: should add a possibility to use different folders on screens */
    if(!desktop_folder)
    {
        desktop_folder = fm_folder_from_path(fm_path_get_desktop());
        g_signal_connect(desktop_folder, "start-loading", G_CALLBACK(on_folder_start_loading), NULL);
        g_signal_connect(desktop_folder, "finish-loading", G_CALLBACK(on_folder_finish_loading), NULL);
        g_signal_connect(desktop_folder, "error", G_CALLBACK(on_folder_error), NULL);
    }

    gdpy = gdk_display_get_default();
    n_scr = gdk_display_get_n_screens(gdpy);
    n_screens = 0;
    for(i = 0; i < n_scr; i++)
        n_screens += gdk_screen_get_n_monitors(gdk_display_get_screen(gdpy, i));
    desktops = g_new(FmDesktop*, n_screens);
    for(scr = 0, i = 0; scr < n_scr; scr++)
    {
        GdkScreen* screen = gdk_display_get_screen(gdpy, scr);
        n_mon = gdk_screen_get_n_monitors(screen);
        for(mon = 0; mon < n_mon; mon++)
        {
            gint mon_init = (on_screen < 0 || on_screen == (int)scr) ? (int)mon : (mon ? -2 : -1);
            FmDesktop *desktop = fm_desktop_new(screen, mon_init);
            GtkWidget *widget = GTK_WIDGET(desktop);
            PangoFontDescription *font_desc;
            PangoContext* pc;

            desktops[i++] = desktop;
            if(mon_init < 0)
                continue;
            /* realize it: without this, setting wallpaper or font won't work */
            gtk_widget_realize(widget);
            load_config(desktop);
            /* setup desktop->conf now if it wasn't loaded above */
            if (!desktop->conf.configured)
            {
                copy_desktop_config(&desktop->conf, &app_config->desktop_section);
                queue_config_save(desktop);
            }
            /* copy found configuration to use by next monitor */
            else if (!app_config->desktop_section.configured)
                copy_desktop_config(&app_config->desktop_section, &desktop->conf);
            /* set a proper desktop font if needed */
            if (desktop->conf.desktop_font == NULL)
                desktop->conf.desktop_font = g_strdup("Sans 12");
            font_desc = pango_font_description_from_string(desktop->conf.desktop_font);
            pc = gtk_widget_get_pango_context(widget);
            pango_context_set_font_description(pc, font_desc);
            pango_font_description_free(font_desc);
#if FM_CHECK_VERSION(1, 0, 2)
            fm_folder_model_set_sort(desktop->model,
                                     desktop->conf.desktop_sort_by,
                                     desktop->conf.desktop_sort_type);
#else
            gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(desktop->model),
                                                 desktop->conf.desktop_sort_by,
                                                 desktop->conf.desktop_sort_type);
#endif
            load_items(desktop);
            gtk_widget_show_all(widget);
            gdk_window_lower(gtk_widget_get_window(widget));
        }
    }

    icon_theme_changed = g_signal_connect(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_icon_theme_changed), NULL);

    hand_cursor = gdk_cursor_new(GDK_HAND2);

#if FM_CHECK_VERSION(1, 2, 0)
    /* create extra items */
    gf = fm_file_new_for_uri("trash:///");
    if (g_file_query_exists(gf, NULL))
        trash_can = _add_extra_item("trash:///");
    else
        trash_can = NULL;
    if (G_LIKELY(trash_can))
    {
        trash_monitor = fm_monitor_directory(gf, NULL);
        g_signal_connect(trash_monitor, "changed", G_CALLBACK(on_trash_changed), trash_can);
    }
    g_object_unref(gf);
    documents = _add_extra_item(g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS));
    /* FIXME: support some other dirs */
    vol_mon = g_volume_monitor_get();
    if (G_LIKELY(vol_mon))
    {
        GList *ml = g_volume_monitor_get_mounts(vol_mon), *l;

        /* if some mounts are already there, add them to own list */
        for (l = ml; l; l = l->next)
        {
            GMount *mount = G_MOUNT(l->data);
            on_mount_added(vol_mon, mount, NULL);
            g_object_unref(mount);
        }
        g_list_free(ml);
        g_signal_connect(vol_mon, "mount-added", G_CALLBACK(on_mount_added), NULL);
        g_signal_connect(vol_mon, "mount-removed", G_CALLBACK(on_mount_removed), NULL);
    }
#endif

    pcmanfm_ref();
}

void fm_desktop_manager_finalize()
{
    int i;

    if (idle_config_save)
    {
        g_source_remove(idle_config_save);
        idle_config_save = 0;
    }
    for(i = 0; i < n_screens; i++)
    {
        gtk_widget_destroy(GTK_WIDGET(desktops[i]));
    }
    g_free(desktops);
    n_screens = 0;
    g_object_unref(win_group);
    win_group = NULL;

    if(desktop_folder)
    {
        g_signal_handlers_disconnect_by_func(desktop_folder, on_folder_start_loading, NULL);
        g_signal_handlers_disconnect_by_func(desktop_folder, on_folder_finish_loading, NULL);
        g_signal_handlers_disconnect_by_func(desktop_folder, on_folder_error, NULL);
        g_object_unref(desktop_folder);
        desktop_folder = NULL;
    }

    g_signal_handler_disconnect(gtk_icon_theme_get_default(), icon_theme_changed);

    if(acc_grp)
        g_object_unref(acc_grp);
    acc_grp = NULL;

    if(hand_cursor)
    {
        gdk_cursor_unref(hand_cursor);
        hand_cursor = NULL;
    }

#if FM_CHECK_VERSION(1, 2, 0)
    if (G_LIKELY(documents))
    {
        _free_extra_item(documents);
        documents = NULL;
    }
    if (G_LIKELY(trash_can))
    {
        g_signal_handlers_disconnect_by_func(trash_monitor, on_trash_changed, trash_can);
        g_object_unref(trash_monitor);
        _free_extra_item(trash_can);
        trash_can = NULL;
    }
    if (G_LIKELY(vol_mon))
    {
        g_signal_handlers_disconnect_by_func(vol_mon, on_mount_added, NULL);
        g_signal_handlers_disconnect_by_func(vol_mon, on_mount_removed, NULL);
        g_object_unref(vol_mon);
        vol_mon = NULL;
    }
    while (mounts)
    {
        _free_extra_item(mounts->data);
        mounts = g_slist_delete_link(mounts, mounts);
    }
#endif

    pcmanfm_unref();
}

FmDesktop* fm_desktop_get(gint screen, gint monitor)
{
    int i = 0, n = 0;
    while(i < n_screens && n <= screen)
    {
        if(n == screen && desktops[i]->monitor == monitor)
            return desktops[i];
        i++;
        if(i < n_screens &&
           (desktops[i]->monitor == 0 || desktops[i]->monitor == -1))
            n++;
    }
    return NULL;
}

void fm_desktop_wallpaper_changed(FmDesktop *desktop)
{
    queue_config_save(desktop);
    update_background(desktop, 0);
}
