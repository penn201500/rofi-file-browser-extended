#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <gmodule.h>
#include <gio/gio.h>

#include <rofi/mode.h>
#include <rofi/helper.h>
#include <rofi/mode-private.h>

#include <stdbool.h>
#include <dirent.h>
#include <nkutils-xdg-theme.h>

// ================================================================================================================= //

/* The starting directory. */
#define START_DIR g_get_current_dir()

/* The icon theme(s). */
#define ICON_THEMES "Adwaita"

/* The fallback icon themes. */
#define FALLBACK_ICON_THEMES "Adwaita", "gnome"

/* The message format. */
#define NO_HIDDEN_SYMBOL "[-]"
#define HIDDEN_SYMBOL "[+]"
#define PATH_SEP " / "

/* The name to display for the parent directory. */
#define PARENT_NAME ".."

/* The format used to display files and directories. Supports pango markup. */
#define FILE_FORMAT "%s"
#define DIRECTORY_FORMAT "%s"

/* The default command to use to open files. */
#define CMD "xdg-open '%s'"

/* The message to display when prompting the user to enter the program to open a file with.
   If the message contains %s, it will be replaced with the file name. */
#define OPEN_CUSTOM_MESSAGE_FORMAT "Enter command to open '%s' with, or cancel to go back."

// ================================================================================================================= //

G_MODULE_EXPORT Mode mode;

typedef enum FBFileType {
    UP,
    DIRECTORY,
    RFILE,
} FBFileType;

typedef struct {
    char* name;
    /* The absolute path of the file. */
    char* path;
    enum FBFileType type;
} FBFile;

typedef struct {
    /* Current directory */
    char* current_dir;

    /* ---- File list ---- */
    /* List of displayed files */
    FBFile* files;
    /* Number of displayed files */
    unsigned int num_files;
    /* Show hidden files */
    bool show_hidden;

    /* ---- Icons ---- */
    /* Loaded icons by their names */
    GHashTable* icons;
    /* Icon theme context */
    NkXdgThemeContext *xdg_context;

    /* ---- Custom command prompt ---- */
    /* Is the user currently opening a file with a custom program?
       This prompts the user for a program to open the file with. */
    bool open_custom;
    /* The selected file index to be opened with a custom program. */
    int open_custom_index;

    /* ---- Command line options ---- */
    /* Command to open files with */
    char* cmd;
    /* Show icons in the file browser. */
    bool show_icons;
    /* Show the status bar.. */
    bool show_status;
    /* Print the absolute file path of selected file instead of opening it. */
    bool dmenu;
    /* Use kb-mode-previous and kb-mode-next to toggle hidden files. */
    bool use_mode_keys;
    /* Status bar format. */
    char* hidden_symbol;
    char* no_hidden_symbol;
    char* path_sep;
    /* Icon themes with fallbacks */
    char** icon_themes;
} FileBrowserModePrivateData;

// ================================================================================================================= //

// TODO update doc

/**
 * @param Mode The current mode.
 *
 * Sets command line options in the private data.
 */
static void set_command_line_options ( FileBrowserModePrivateData* pd );

/**
 * @param Mode The current mode.
 *
 * Gets the files in the current directory and sets them in the private data.
 */
static void get_file_browser ( FileBrowserModePrivateData* pd );

/**
 * @param pd The private data.
 *
 * Frees the current file list.
 */
static void free_list ( FileBrowserModePrivateData* pd );

/**
 * @param path The path the get the absolute path of.
 * @param current_dir The current directory.
 *
 * Gets the absolute path of either an already absolute path or a relative path to the current directory.
 * If no file or directory with the path, exists, NULL is returned.
 *
 * @return The absolute path, or NULL if no file or directory with the path exists.
 *         If a path is returned, it should be free'd with g_free() afterwards.
 */
static char* get_absolute_path ( char* path, char* current_dir );

/**
 * TODO
 */
static void change_dir ( char* path, FileBrowserModePrivateData* pd );

/**
 * @param path The absolute path of the file to open.
 * @param sw The current mode.
 *
 * Opens the file at the given path or prints it's absolute path to stdout if the dmenu option is set.
 */
static void open_or_print_file ( char* path, FileBrowserModePrivateData* pd );

/**
 * @param file The file to get the icon names of.
 *
 * Gets a list of icon names for the file. The first icon name is the most specific, the following are fallback
 * icon names.
 *
 * @return The list of icon names. The list should be free'd with g_strfreev() afterwards.
 */
static char** get_icon_names ( FBFile file );

/**
 * @param icon_names The icon names to get the icon of.
 * @param icon_size The size of the icon to get.
 * @param sw The current mode.
 *
 * Gets the most specific icon possible for a list of icon names with fallbacks.
 *
 * @return An icon for the given list of icon names, or NULL if the icon doesn't exist.
 */
static cairo_surface_t* get_icon_surf ( char** icon_names, int icon_size, FileBrowserModePrivateData* pd );

/**
 * @param a The first file.
 * @param b The second file.
 * @param data unused
 *
 * Comparison function for sorting the file list.
 * Compares two files by the order in which they should appear in the file browser.
 * Directories should appear before regular files, directories and files should be sorted alphabetically.
 */
static gint compare ( gconstpointer a, gconstpointer b, gpointer data );

// ================================================================================================================= //

static int file_browser_init ( Mode* sw )
{
    if ( mode_get_private_data ( sw ) == NULL ) {
        FileBrowserModePrivateData* pd = g_malloc0 ( sizeof ( *pd ) );
        mode_set_private_data ( sw, ( void * ) pd );

        /* Command line options */
        set_command_line_options ( pd );

        /* Other values */
        pd->open_custom = false;
        pd->open_custom_index = -1;
        pd->files = NULL;
        pd->num_files = 0;
        pd->icons = NULL;
        pd->xdg_context = NULL;

        /* Set up icons if enabled. */
        if ( pd->show_icons ) {
            static const gchar * const fallback_icon_themes[] = {
                FALLBACK_ICON_THEMES,
                NULL
            };

            pd->xdg_context = nk_xdg_theme_context_new ( fallback_icon_themes, NULL );
            nk_xdg_theme_preload_themes_icon ( pd->xdg_context, ( const gchar * const * ) pd->icon_themes );
            pd->icons = g_hash_table_new_full ( g_str_hash, g_str_equal, g_free, ( void ( * ) ( void * ) ) cairo_surface_destroy );
        }

        /* Load the files. */
        get_file_browser ( pd );
    }

    return true;
}

static void file_browser_destroy ( Mode* sw )
{
    FileBrowserModePrivateData* pd = ( FileBrowserModePrivateData * ) mode_get_private_data ( sw );
    mode_set_private_data ( sw, NULL );

    if ( pd != NULL ) {
        /* Free file list. */
        free_list ( pd );

        /* Free icon themes and icons. */
        if ( pd->show_icons ) {
            g_hash_table_destroy ( pd->icons );
            nk_xdg_theme_context_free ( pd->xdg_context );
        }
        g_strfreev ( pd->icon_themes );

        /* Free the rest. */
        g_free ( pd->current_dir );
        g_free ( pd->cmd );
        g_free ( pd->hidden_symbol );
        g_free ( pd->no_hidden_symbol );
        g_free ( pd->path_sep );

        /* Fill with zeros, just in case. */
        memset ( ( void* ) pd , 0, sizeof ( pd ) );

        g_free ( pd );
    }
}

static unsigned int file_browser_get_num_entries ( const Mode* sw )
{
    const FileBrowserModePrivateData* pd = ( const FileBrowserModePrivateData * ) mode_get_private_data ( sw );

    if ( pd != NULL ) {
        if ( pd->open_custom ) {
            return 1;
        } else {
            return pd->num_files;
        }
    } else {
        return 0;
    }
}

static ModeMode file_browser_result ( Mode* sw, int mretv, char **input, unsigned int selected_line )
{
    FileBrowserModePrivateData* pd = ( FileBrowserModePrivateData * ) mode_get_private_data ( sw );
    ModeMode retv = RELOAD_DIALOG;

    /* Handle prompt for program to open file with. */
    if ( pd->open_custom ) {
        if ( mretv & ( MENU_OK | MENU_CUSTOM_INPUT | MENU_CUSTOM_ACTION ) ) {
            if ( strlen ( *input ) == 0 ) {
                char* file_path = pd->files[pd->open_custom_index].path;
                open_or_print_file ( file_path, pd );
                retv = MODE_EXIT;
            } else {
                char* file_path = pd->files[pd->open_custom_index].path;
                g_free ( pd->cmd );
                pd->cmd = g_strdup ( *input );
                open_or_print_file ( file_path, pd );
                retv = MODE_EXIT;
            }
        } else if ( mretv & MENU_CANCEL ) {
            pd->open_custom = false;
            pd->open_custom_index = -1;
            retv = RESET_DIALOG;
        }

    /* Handle Shift+Return. */
    } else if ( ( mretv & MENU_CUSTOM_ACTION ) && ( selected_line != -1 ) ) {
        pd->open_custom = true;
        pd->open_custom_index = selected_line;
        retv = RESET_DIALOG;

    /* Handle Return. */
    } else if ( mretv & MENU_OK ) {
        FBFile* entry = &( pd->files[selected_line] );
        if ( entry->type == UP || entry->type == DIRECTORY ) {
            change_dir ( entry->path, pd );
            get_file_browser ( pd );
            retv = RESET_DIALOG;
        } else {
            open_or_print_file ( entry->path, pd );
            retv = MODE_EXIT;
        }

    /* Handle custom input or Control+Return. */
    } else if ( mretv & MENU_CUSTOM_INPUT ) {

        /* Toggle hidden files with Control+Return. */
        if ( strlen ( *input ) == 0 ) {
            pd->show_hidden = !pd->show_hidden;
            get_file_browser ( pd );
            retv = RELOAD_DIALOG;

        /* Handle custom input. */
        } else {
            char *expanded_input = rofi_expand_path ( *input );
            char *file = g_filename_from_utf8 ( expanded_input, -1, NULL, NULL, NULL );
            g_free ( expanded_input );

            char* abs_path = get_absolute_path ( file, pd->current_dir );
            g_free ( file );

            if ( abs_path == NULL ) {
                retv = RELOAD_DIALOG;
            } else if ( g_file_test ( abs_path, G_FILE_TEST_IS_DIR ) ){
                change_dir ( abs_path, pd );
                get_file_browser ( pd );
                retv = RESET_DIALOG;
            } else if ( g_file_test ( abs_path, G_FILE_TEST_IS_REGULAR ) ) {
                open_or_print_file ( abs_path, pd );
                retv = MODE_EXIT;
            }

            g_free ( abs_path );
        }

    /* Enable hidden files with Shift+Right. */
    } else if ( pd->use_mode_keys && ( mretv & MENU_NEXT ) && !pd->show_hidden ) {
        pd->show_hidden = true;
        get_file_browser ( pd );
        retv = RELOAD_DIALOG;

    /* Disable hidden files with Shift+Left. */
    } else if ( pd->use_mode_keys && ( mretv & MENU_PREVIOUS ) && pd->show_hidden ) {
        pd->show_hidden = false;
        get_file_browser ( pd );
        retv = RELOAD_DIALOG;

    /* Default actions */
    } else if ( mretv & MENU_CANCEL ) {
        retv = MODE_EXIT;
    } else if ( mretv & MENU_NEXT ) {
        retv = NEXT_DIALOG;
    } else if ( mretv & MENU_PREVIOUS ) {
        retv = PREVIOUS_DIALOG;
    } else if ( mretv & MENU_QUICK_SWITCH ) {
        retv = ( mretv & MENU_LOWER_MASK );
    }

    return retv;
}

static int file_browser_token_match ( const Mode* sw, rofi_int_matcher **tokens, unsigned int index )
{
    FileBrowserModePrivateData* pd = ( FileBrowserModePrivateData * ) mode_get_private_data ( sw );

    if ( pd->open_custom ) {
        return true;
    } else {
        return helper_token_match ( tokens, pd->files[index].name);
    }
}

static char* file_browser_get_display_value ( const Mode* sw, unsigned int selected_line, G_GNUC_UNUSED int *state, G_GNUC_UNUSED GList **attr_list, int get_entry )
{
    FileBrowserModePrivateData* pd = ( FileBrowserModePrivateData * ) mode_get_private_data ( sw );

    if ( !get_entry ) return NULL;

    int index;
    if ( pd->open_custom ) {
        index = pd->open_custom_index;
    } else {
        index = selected_line;
    }

    // MARKUP flag, not defined in accessible headers
    *state |= 8;

    switch ( pd->files[index].type ) {
    case UP:
        return g_strdup ( PARENT_NAME );
    case RFILE:
        return g_strdup_printf ( FILE_FORMAT, pd->files[index].name );
    case DIRECTORY:
        return g_strdup_printf ( DIRECTORY_FORMAT, pd->files[index].name );
    default:
        return g_strdup ( "error" );
    }
}

static cairo_surface_t* file_browser_get_icon ( const Mode* sw, unsigned int selected_line, int height )
{
    FileBrowserModePrivateData* pd = ( FileBrowserModePrivateData * ) mode_get_private_data ( sw );

    int index;
    if ( pd->open_custom ) {
        index = pd->open_custom_index;
    } else {
        index = selected_line;
    }

    if ( pd->show_icons ) {
        char** icon_names = get_icon_names ( pd->files[index] );
        cairo_surface_t* icon =  get_icon_surf ( icon_names, height, pd );
        g_strfreev ( icon_names );

        return icon;
    } else {
        return NULL;
    }
}

static char* file_browser_get_message (const Mode* sw)
{
    FileBrowserModePrivateData* pd = ( FileBrowserModePrivateData * ) mode_get_private_data ( sw );

    if ( pd->open_custom ) {
        char* file_name = pd->files[pd->open_custom_index].name;
        char* message = g_strdup_printf ( OPEN_CUSTOM_MESSAGE_FORMAT,  file_name );
        return message;

    } else if ( pd->show_status ) {
        char** split = g_strsplit ( pd->current_dir, "/", -1 );
        char* join = g_strjoinv ( pd->path_sep, split );
        char* message = g_strconcat ( pd->show_hidden ? pd->hidden_symbol : pd->no_hidden_symbol, join, NULL );

        g_strfreev ( split );
        g_free ( join );

        return message;
    } else {
        return NULL;
    }
}

// ================================================================================================================= //

static void set_command_line_options ( FileBrowserModePrivateData* pd )
{
    pd->show_hidden = ( find_arg ( "-file-browser-show-hidden" ) != -1 );
    pd->show_icons = ( find_arg ( "-file-browser-disable-icons" ) == -1 );
    pd->dmenu = ( find_arg ( "-file-browser-dmenu" ) != -1 );
    pd->use_mode_keys = ( find_arg ( "-file-browser-disable-mode-keys" ) == -1 );
    pd->show_status = ( find_arg ( "-file-browser-disable-status" ) == -1 );

    char* cmd = NULL;
    if ( find_arg_str ( "-file-browser-cmd", &cmd ) ) {
        pd->cmd = g_strdup ( cmd );
    } else {
        pd->cmd = g_strdup ( CMD );
    }

    char* hidden_symbol = NULL;
    if ( find_arg_str ( "-file-browser-hidden-symbol", &hidden_symbol ) ) {
        pd->hidden_symbol = g_strdup ( hidden_symbol );
    } else {
        pd->hidden_symbol = g_strdup ( HIDDEN_SYMBOL );
    }

    char* no_hidden_symbol = NULL;
    if ( find_arg_str ( "-file-browser-no-hidden-symbol", &no_hidden_symbol ) ) {
        pd->no_hidden_symbol = g_strdup ( no_hidden_symbol );
    } else {
        pd->no_hidden_symbol = g_strdup ( NO_HIDDEN_SYMBOL );
    }

    char* path_sep = NULL;
    if ( find_arg_str ( "-file-browser-path-sep", &path_sep ) ) {
        pd->path_sep = g_strdup ( path_sep );
    } else {
        pd->path_sep = g_strdup ( PATH_SEP );
    }

    /* Set the start directory. */
    char* start_dir = NULL;
    if ( find_arg_str ( "-file-browser-dir", &start_dir ) ) {
        if ( g_file_test ( start_dir, G_FILE_TEST_EXISTS ) ) {
            pd->current_dir = g_strdup( start_dir );
        } else {
            fprintf ( stderr, "[file-browser] Start directory does not exist: %s\n", start_dir );
            pd->current_dir = g_strdup ( START_DIR );
        }
    } else {
        pd->current_dir = g_strdup ( START_DIR );
    }

    /* Set the icon themes. */
    const gchar *default_icon_themes[] = {
        ICON_THEMES,
        NULL
    };
    pd->icon_themes = g_strdupv ( ( char ** ) find_arg_strv ( "-file-browser-theme" ) );
    if ( pd->icon_themes == NULL ) {
        pd->icon_themes = g_strdupv ( ( char ** ) default_icon_themes );
    }
}

static void get_file_browser ( FileBrowserModePrivateData* pd )
{
    free_list ( pd );

    DIR *dir = opendir ( pd->current_dir );

    if ( dir != NULL ) {
        struct dirent *rd = NULL;
        while ( ( rd = readdir ( dir ) ) != NULL ) {
            /* Ignore rd if rd is the current directory or rd is a hidden file and show_hidden is false. */
            if ( ( g_strcmp0 ( rd->d_name, "." ) == 0 )
              || ( !pd->show_hidden
                    && rd->d_name[0] == '.'
                    && g_strcmp0 ( rd->d_name, ".." ) != 0 ) ) {
                continue;
            }

            if ( rd->d_type == DT_REG || rd->d_type == DT_DIR || rd->d_type == DT_LNK ) {
                pd->files = g_realloc ( pd->files, ( pd->num_files + 1 ) * sizeof ( FBFile ) );

                FBFile* entry = &( pd->files[pd->num_files] );

                entry->name = g_filename_to_utf8 ( rd->d_name, -1, NULL, NULL, NULL);
                entry->path = g_build_filename ( pd->current_dir, rd->d_name, NULL );

                if ( g_strcmp0 ( rd->d_name, ".." ) == 0 ) {
                    entry->type = UP;
                } else {
                    switch ( rd->d_type ) {
                        case DT_REG:
                            entry->type = RFILE;
                            break;
                        case DT_DIR:
                            entry->type = DIRECTORY;
                            break;
                        case DT_LNK:
                            entry->type = g_file_test ( entry->path, G_FILE_TEST_IS_DIR ) ? DIRECTORY : RFILE;
                    }
                }

                pd->num_files++;
            }
        }

        closedir ( dir );
    }

    g_qsort_with_data ( pd->files, pd->num_files, sizeof (FBFile ), compare, NULL );
}

static void free_list ( FileBrowserModePrivateData* pd )
{
    for ( unsigned int i = 0; i < pd->num_files; i++ ) {
        FBFile *fb = & ( pd->files[i] );
        g_free ( fb->name );
        g_free ( fb->path );
    }
    g_free (pd->files);
    pd->files  = NULL;
    pd->num_files = 0;
}

static char* get_absolute_path ( char* path, char* current_dir )
{
    /* Check if the path is already absolute. */
    if ( g_file_test ( path, G_FILE_TEST_EXISTS ) ) {
        return g_strdup ( path );

    /* Construct the absolute path and check if it exists. */
    } else {
        char* new_path = g_build_filename ( current_dir, path, NULL );
        if ( g_file_test ( new_path, G_FILE_TEST_EXISTS ) ) {
            return new_path;
        } else {
            g_free ( new_path );
            return NULL;
        }
    }
}

static void change_dir ( char* path, FileBrowserModePrivateData* pd )
{
    g_free ( pd->current_dir );

    /* Simplify the path (e.g. remove ".." by creating a GFile. */
    GFile *file = g_file_new_for_path ( path );
    char* simplified_path = g_file_get_path ( file );
    g_object_unref ( file );

    pd->current_dir = simplified_path;
}

static void open_or_print_file ( char* path, FileBrowserModePrivateData* pd )
{
    if ( pd->dmenu ) {
        printf("%s\n", path);

    } else {
        char* complete_cmd = NULL;

        if ( g_strrstr ( pd->cmd, "%s" ) != NULL ) {
            complete_cmd = g_strdup_printf ( pd->cmd, path );
        } else {
            complete_cmd = g_strconcat ( pd->cmd, " ", "'", path, "'", NULL );
        }

        helper_execute_command ( pd->current_dir, complete_cmd, false, NULL );

        g_free ( complete_cmd );
    }
}

static char** get_icon_names ( FBFile fbfile )
{
    static char* error_icon[] = { "error", NULL };
    static char* up_icon[] = { "go-up", NULL };
    static char* folder_icon[] = { "folder", NULL };

    /* Special cases: directory up and NULL. */
    if ( fbfile.path == NULL ) {
        return g_strdupv ( error_icon );
    } else if ( fbfile.type == UP ) {
        return g_strdupv ( up_icon );
    }

    char** names = NULL;
    GError *error = NULL;

    GFile *file = g_file_new_for_path ( fbfile.path );
    GFileInfo *file_info = g_file_query_info ( file, "standard::icon", G_FILE_QUERY_INFO_NONE, NULL, &error );

    if ( error == NULL ) {
        GIcon *icon = g_file_info_get_icon ( file_info );

        if ( G_IS_THEMED_ICON ( icon ) ) {
            names = g_strdupv ( ( char ** ) g_themed_icon_get_names ( G_THEMED_ICON ( icon ) ) );
        }

        g_object_unref ( icon );
    }

    g_object_unref ( file );

    if ( names != NULL ) {
        return names;
    } else {
        return g_strdupv ( error_icon );
    }
}

    static cairo_surface_t* get_icon_surf ( char** icon_names, int icon_size, FileBrowserModePrivateData* pd ) {
    for (int i = 0; icon_names[i] != NULL; i++) {

        cairo_surface_t *icon_surf = g_hash_table_lookup ( pd->icons, icon_names[i] );

        if ( icon_surf != NULL ) {
            return icon_surf;
        }

        gchar* icon_path = nk_xdg_theme_get_icon ( pd->xdg_context, ( const char ** ) pd->icon_themes,
                                        NULL, icon_names[i], icon_size, 1, true );

        if ( icon_path == NULL ) {
            continue;
        }

        if ( g_str_has_suffix ( icon_path, ".png" ) ) {
            icon_surf = cairo_image_surface_create_from_png ( icon_path );
        } else if ( g_str_has_suffix ( icon_path, ".svg" ) ) {
            icon_surf = cairo_image_surface_create_from_svg ( icon_path, icon_size );
        }

        g_free ( icon_path );

        if ( icon_surf != NULL ) {
            if ( cairo_surface_status ( icon_surf ) != CAIRO_STATUS_SUCCESS ) {
                cairo_surface_destroy ( icon_surf );
                icon_surf = NULL;
            } else {
                g_hash_table_insert ( pd->icons, g_strdup ( icon_names[i] ), icon_surf );
                return icon_surf;
            }
        }
    }

    return NULL;
}

static gint compare ( gconstpointer a, gconstpointer b, gpointer data )
{
    FBFile *fa = (FBFile*)a;
    FBFile *fb = (FBFile*)b;
    if ( fa->type != fb->type ){
        return (fa->type - fb->type);
    }

    return g_strcmp0 ( fa->name, fb->name );
}

// ================================================================================================================= //

Mode mode =
{
    .abi_version        = ABI_VERSION,
    .name               = "file-browser",
    .cfg_name_key       = "display-file-browser",
    ._init              = file_browser_init,
    ._get_num_entries   = file_browser_get_num_entries,
    ._result            = file_browser_result,
    ._destroy           = file_browser_destroy,
    ._token_match       = file_browser_token_match,
    ._get_display_value = file_browser_get_display_value,
    ._get_icon          = file_browser_get_icon,
    ._get_message       = file_browser_get_message,

    ._get_completion    = NULL,
    ._preprocess_input  = NULL,
    .private_data       = NULL,
    .free               = NULL,
};
