#include <gtk/gtk.h>
#include <zip.h>
#include <json-glib/json-glib.h>
#include <errno.h>

typedef struct {
    GtkWidget *main_window;
    GtkWidget *image_display;
    GtkWidget *spinner;
    GtkWidget *scrolled_image;
    gchar *zip_path;
    GHashTable *image_map;
    GHashTable *dependencies;
    GdkPixbuf *original_pixbuf;
} AppData;

static void debug_print_stored_data(AppData *data);
static GdkPixbuf* render_composite_image(AppData *data, const gchar *image_id, GError **error);
static gchar* read_file_from_zip(const char *zip_path, const char *inner_filename, gsize *size, GError **error);
static GdkPixbuf* load_pixbuf_from_memory(const gchar *buffer, gsize size, GError **error);
static void activate(GtkApplication *app, gpointer user_data);
static int on_command_line(GtkApplication *app, GApplicationCommandLine *cmdline, gpointer user_data);
static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void copy_to_hashtable_cb(JsonObject *object, const gchar *member_name, JsonNode *member_node, gpointer user_data);
static void on_scrolled_window_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data);
static void scale_image_to_fit(AppData *data);

int main(int argc, char **argv) {
    AppData *data = g_new0(AppData, 1);
    GtkApplication *app = gtk_application_new("com.example.compositebrowser", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), data);
    g_signal_connect(app, "activate", G_CALLBACK(activate), data);
    
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    
    g_object_unref(app);
    g_free(data->zip_path);
    if (data->image_map) g_hash_table_destroy(data->image_map);
    if (data->dependencies) g_hash_table_destroy(data->dependencies);
    if (data->original_pixbuf) g_object_unref(data->original_pixbuf);
    g_free(data);
    return status;
}

static void copy_to_hashtable_cb(JsonObject *object, const gchar *member_name, JsonNode *member_node, gpointer user_data) {
    GHashTable *hash_table = (GHashTable*)user_data;
    
    if (!JSON_NODE_HOLDS_VALUE(member_node) || json_node_get_value_type(member_node) != G_TYPE_STRING) {
        g_warning("Skipping non-string value for key '%s'", member_name);
        return;
    }
    
    const gchar *value_str = json_node_get_string(member_node);
    if (value_str) {
        g_hash_table_insert(hash_table, g_strdup(member_name), g_strdup(value_str));
    }
}

static void debug_print_stored_data(AppData *data) {
    g_print("\n\n--- VERIFYING STORED DATA FROM HASH TABLES ---\n");
    g_print("--- Image Map ---\n");
    
    GHashTableIter iter;
    gpointer key, value;
    
    g_hash_table_iter_init(&iter, data->image_map);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_print("  ID '%s' -> Filename '%s'\n", (gchar*)key, (gchar*)value);
    }
    
    g_print("--- Dependencies ---\n");
    g_hash_table_iter_init(&iter, data->dependencies);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_print("  ID '%s' -> Depends on '%s'\n", (gchar*)key, (gchar*)value);
    }
    g_print("--- END OF VERIFICATION ---\n\n");
}

// --- GTK Signal Handlers and UI Setup ---

static int on_command_line(GtkApplication *app, GApplicationCommandLine *cmdline, gpointer user_data) {
    AppData *data = (AppData*)user_data;
    gchar **argv;
    gint argc;
    
    argv = g_application_command_line_get_arguments(cmdline, &argc);
    if (argc < 2) {
        g_printerr("Usage: composite_browser <path/to/archive.dia>\n");
        g_strfreev(argv);
        return 1;
    }
    
    data->zip_path = g_strdup(argv[1]);
    g_application_activate(G_APPLICATION(app));
    g_strfreev(argv);
    return 0;
}

static void activate(GtkApplication *app, gpointer user_data) {
    AppData *data = (AppData*)user_data;
    g_autoptr(GError) error = NULL;
    gsize map_size = 0;

    // Parse the JSON
    g_autofree gchar *map_contents = read_file_from_zip(data->zip_path, "optimization_map.json", &map_size, &error);
    if (!map_contents) {
        g_printerr("ERROR: Could not read optimization_map.json: %s\n", error ? error->message : "Unknown error");
        return;
    }

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, map_contents, map_size, &error)) {
        g_printerr("ERROR: Could not parse JSON: %s\n", error ? error->message : "Unknown error");
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_printerr("ERROR: JSON root is not an object\n");
        return;
    }
    
    JsonObject *root_obj = json_node_get_object(root);

    data->image_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    data->dependencies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    g_print("EXTRACTING data from temporary JSON object into permanent hash tables...\n");
    
    if (json_object_has_member(root_obj, "image_map")) {
        JsonObject *temp_image_map = json_object_get_object_member(root_obj, "image_map");
        if (temp_image_map) {
            json_object_foreach_member(temp_image_map, copy_to_hashtable_cb, data->image_map);
        }
    }

    if (json_object_has_member(root_obj, "dependencies")) {
        JsonObject *temp_deps = json_object_get_object_member(root_obj, "dependencies");
        if (temp_deps) {
            json_object_foreach_member(temp_deps, copy_to_hashtable_cb, data->dependencies);
        }
    }
    
    g_print("EXTRACTION complete.\n");

    debug_print_stored_data(data);

    // Build the UI
    data->main_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(data->main_window), g_path_get_basename(data->zip_path));
    gtk_window_set_default_size(GTK_WINDOW(data->main_window), 800, 600);
    
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(data->main_window), paned);
    
    GtkWidget *scrolled_list = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *list_box = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scrolled_list), list_box);
    gtk_paned_add1(GTK_PANED(paned), scrolled_list);
    
    GtkWidget *scrolled_image = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_image), 
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    GtkWidget *overlay = gtk_overlay_new();
    data->image_display = gtk_image_new();
    
    g_signal_connect(scrolled_image, "size-allocate", G_CALLBACK(on_scrolled_window_size_allocate), data);
    
    gtk_container_add(GTK_CONTAINER(overlay), data->image_display);
    
    data->spinner = gtk_spinner_new();
    gtk_widget_set_halign(data->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(data->spinner, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), data->spinner);
    gtk_container_add(GTK_CONTAINER(scrolled_image), overlay);
    data->scrolled_image = scrolled_image;
    gtk_paned_add2(GTK_PANED(paned), scrolled_image);
    gtk_paned_set_position(GTK_PANED(paned), 200);

    GList *id_list = g_hash_table_get_keys(data->image_map);
    id_list = g_list_sort(id_list, (GCompareFunc)g_strcmp0);
    
    for (GList *l = id_list; l != NULL; l = l->next) {
        const gchar *image_id = (const gchar*)l->data;
        const gchar *filename = g_hash_table_lookup(data->image_map, image_id);
        
        GtkWidget *row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), gtk_label_new(filename ? filename : image_id));
        g_object_set_data_full(G_OBJECT(row), "image-id", g_strdup(image_id), g_free);
        gtk_list_box_insert(GTK_LIST_BOX(list_box), row, -1);
    }
    g_list_free(id_list);

    g_signal_connect(list_box, "row-selected", G_CALLBACK(on_row_selected), data);
    gtk_widget_show_all(data->main_window);
    gtk_widget_hide(data->spinner);
}

static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    AppData *data = (AppData*)user_data;
    if (row == NULL) return;
    
    gtk_widget_show(data->spinner);
    gtk_spinner_start(GTK_SPINNER(data->spinner));
    
    const char *image_id = g_object_get_data(G_OBJECT(row), "image-id");
    g_print("\n--- Row Selected: ID '%s' ---\n", image_id);
    
    GError *error = NULL;
    GdkPixbuf *pixbuf = render_composite_image(data, image_id, &error);
    
    if (pixbuf) {
        g_print("SUCCESS: Final image rendered. Displaying.\n");
        
        if (data->original_pixbuf) {
            g_object_unref(data->original_pixbuf);
        }
        data->original_pixbuf = g_object_ref(pixbuf);
        
        scale_image_to_fit(data);
        g_object_unref(pixbuf);
    } else {
        g_printerr("ERROR: Could not render '%s': %s\n", image_id, error ? error->message : "Unknown error");
        gtk_image_set_from_icon_name(GTK_IMAGE(data->image_display), "image-missing", GTK_ICON_SIZE_DIALOG);
        if (error) g_error_free(error);
    }
    
    gtk_spinner_stop(GTK_SPINNER(data->spinner));
    gtk_widget_hide(data->spinner);
}

static GdkPixbuf* render_composite_image(AppData *data, const gchar *image_id, GError **error) {
    GQueue *chain = g_queue_new();
    GHashTable *visited = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gchar *current_id = g_strdup(image_id);
    
    // Build dependency chain
    while (current_id) {
        // Check for cycles
        if (g_hash_table_contains(visited, current_id)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Circular dependency detected involving ID '%s'", current_id);
            g_free(current_id);
            g_queue_free_full(chain, g_free);
            g_hash_table_destroy(visited);
            return NULL;
        }
        
        g_hash_table_insert(visited, g_strdup(current_id), GINT_TO_POINTER(1));
        g_queue_push_head(chain, current_id);
        
        if (g_hash_table_contains(data->dependencies, current_id)) {
            const gchar *dependency = g_hash_table_lookup(data->dependencies, current_id);
            current_id = g_strdup(dependency);
        } else {
            current_id = NULL;
        }
    }
    
    g_hash_table_destroy(visited);
    
    // Start with the base image
    gchar *base_id = g_queue_pop_head(chain);
    const gchar *base_filename = g_hash_table_lookup(data->image_map, base_id);
    if (!base_filename) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Could not find filename for ID '%s'", base_id);
        g_free(base_id);
        g_queue_free_full(chain, g_free);
        return NULL;
    }
    
    gsize base_size = 0;
    g_autofree gchar *base_buffer = read_file_from_zip(data->zip_path, base_filename, &base_size, error);
    if (!base_buffer) {
        g_free(base_id);
        g_queue_free_full(chain, g_free);
        return NULL;
    }
    
    GdkPixbuf *canvas_pixbuf = load_pixbuf_from_memory(base_buffer, base_size, error);
    if (!canvas_pixbuf) {
        g_free(base_id);
        g_queue_free_full(chain, g_free);
        return NULL;
    }
    
    if (!gdk_pixbuf_get_has_alpha(canvas_pixbuf)) {
        GdkPixbuf *temp = gdk_pixbuf_add_alpha(canvas_pixbuf, FALSE, 0, 0, 0);
        g_object_unref(canvas_pixbuf);
        canvas_pixbuf = temp;
    }
    
    g_free(base_id);
    
    // Apply overlays in order
    while (!g_queue_is_empty(chain)) {
        gchar *overlay_id = g_queue_pop_head(chain);
        const gchar *overlay_filename = g_hash_table_lookup(data->image_map, overlay_id);
        
        if (!overlay_filename) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Could not find filename for overlay ID '%s'", overlay_id);
            g_free(overlay_id);
            g_object_unref(canvas_pixbuf);
            g_queue_free_full(chain, g_free);
            return NULL;
        }
        
        gsize overlay_size = 0;
        g_autofree gchar *overlay_buffer = read_file_from_zip(data->zip_path, overlay_filename, &overlay_size, error);
        if (!overlay_buffer) {
            g_free(overlay_id);
            g_object_unref(canvas_pixbuf);
            g_queue_free_full(chain, g_free);
            return NULL;
        }
        
        g_autoptr(GdkPixbuf) overlay_pixbuf_orig = load_pixbuf_from_memory(overlay_buffer, overlay_size, error);
        if (!overlay_pixbuf_orig) {
            g_free(overlay_id);
            g_object_unref(canvas_pixbuf);
            g_queue_free_full(chain, g_free);
            return NULL;
        }
        
        GdkPixbuf *overlay_to_composite = overlay_pixbuf_orig;
        g_autoptr(GdkPixbuf) temp_alpha_pixbuf = NULL;
        
        if (!gdk_pixbuf_get_has_alpha(overlay_to_composite)) {
            temp_alpha_pixbuf = gdk_pixbuf_add_alpha(overlay_to_composite, FALSE, 0, 0, 0);
            overlay_to_composite = temp_alpha_pixbuf;
        }
        
        int canvas_width = gdk_pixbuf_get_width(canvas_pixbuf);
        int canvas_height = gdk_pixbuf_get_height(canvas_pixbuf);
        int overlay_width = gdk_pixbuf_get_width(overlay_to_composite);
        int overlay_height = gdk_pixbuf_get_height(overlay_to_composite);
        
        // Composite overlay
        int composite_width = MIN(overlay_width, canvas_width);
        int composite_height = MIN(overlay_height, canvas_height);
        
        if (composite_width > 0 && composite_height > 0) {
            gdk_pixbuf_composite(overlay_to_composite, canvas_pixbuf,
                               0, 0, composite_width, composite_height,
                               0, 0, 1.0, 1.0, GDK_INTERP_NEAREST, 255);
        }
        
        g_free(overlay_id);
    }
    
    g_queue_free(chain);
    return canvas_pixbuf;
}

static gchar* read_file_from_zip(const char *zip_path, const char *inner_filename, gsize *size, GError **error) {
    if (!inner_filename) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Inner filename cannot be NULL");
        return NULL;
    }
    
    int err = 0;
    zip_t *archive = zip_open(zip_path, 0, &err);
    if (!archive) {
        zip_error_t zip_err;
        zip_error_init_with_code(&zip_err, err);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open zip archive '%s': %s", 
                   zip_path, zip_error_strerror(&zip_err));
        zip_error_fini(&zip_err);
        return NULL;
    }
    
    struct zip_stat stat;
    zip_stat_init(&stat);
    if (zip_stat(archive, inner_filename, 0, &stat) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "File not found in zip archive: %s", inner_filename);
        zip_close(archive);
        return NULL;
    }
    
    zip_file_t* file_in_zip = zip_fopen(archive, inner_filename, 0);
    if (!file_in_zip) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to open file '%s' inside zip archive: %s", 
                   inner_filename, zip_strerror(archive));
        zip_close(archive);
        return NULL;
    }
    
    guint8 *buffer = g_malloc(stat.size);
    zip_int64_t bytes_read = zip_fread(file_in_zip, buffer, stat.size);
    zip_fclose(file_in_zip);
    zip_close(archive);
    
    if (bytes_read < 0 || (guint64)bytes_read != stat.size) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, 
                   "Failed to read all bytes from '%s' in zip archive.", inner_filename);
        g_free(buffer);
        return NULL;
    }
    
    *size = (gsize)bytes_read;
    return (gchar*)buffer;
}

static GdkPixbuf* load_pixbuf_from_memory(const gchar *buffer, gsize size, GError **error) {
    g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new();
    
    if (!gdk_pixbuf_loader_write(loader, (const guint8*)buffer, size, error)) {
        return NULL;
    }
    
    if (!gdk_pixbuf_loader_close(loader, error)) {
        return NULL;
    }
    
    GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pixbuf) {
        g_object_ref(pixbuf);
    }
    return pixbuf;
}

// Image Scaling
static void on_scrolled_window_size_allocate(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data) {
    AppData *data = (AppData*)user_data;
    
    if (data->original_pixbuf) {
        scale_image_to_fit(data);
    }
}

static void scale_image_to_fit(AppData *data) {
    if (!data->original_pixbuf || !data->scrolled_image) return;
    
    GtkAllocation allocation;
    gtk_widget_get_allocation(data->scrolled_image, &allocation);
    
    int orig_width = gdk_pixbuf_get_width(data->original_pixbuf);
    int orig_height = gdk_pixbuf_get_height(data->original_pixbuf);
    
    int available_width = allocation.width - 5;
    int available_height = allocation.height - 5;
    
    if (available_width <= 0 || available_height <= 0) return;
    
    double scale_x = (double)available_width / orig_width;
    double scale_y = (double)available_height / orig_height;
    double scale = MIN(scale_x, scale_y);
    
    // Only scale down
    scale = MIN(scale, 1.0);
    
    int new_width = (int)(orig_width * scale);
    int new_height = (int)(orig_height * scale);
    
    if (new_width > 0 && new_height > 0) {
        GdkPixbuf *scaled_pixbuf = gdk_pixbuf_scale_simple(data->original_pixbuf, 
                                                          new_width, new_height, 
                                                          GDK_INTERP_BILINEAR);
        if (scaled_pixbuf) {
            gtk_image_set_from_pixbuf(GTK_IMAGE(data->image_display), scaled_pixbuf);
            g_object_unref(scaled_pixbuf);
        }
    }
}
