/* Carapace, the SnapOS store. It shows the programs from Nix: a few popular
 * ones with icons on the first page, and every package through the search
 * (snapos find). Installing runs `snapos add` (through pkexec), which adds the
 * program to /etc/snapos and builds the next system, so anything installed can
 * be removed or undone like every other change. While it works, Snappy plays
 * one of his little scenes. */
#include <gtk/gtk.h>
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef DATADIR
#define DATADIR "share/carapace"
#endif

typedef struct {
    char *attr, *name, *icon, *cat, *summary;
} App;

static App *apps;
static int napps;
static GHashTable *installed;

static struct {
    GtkWidget *win, *stack, *search, *side, *flow, *results, *results_title, *spinner;
    GtkWidget *page_icon, *page_name, *page_summary, *page_attr, *page_button, *page_cmd;
    App *shown;
    App search_app;                 /* a package found by the search, shown on the app page */
    GSubprocess *find;
    guint debounce;
} ui;

static const char *datadir(void) {
    const char *d = g_getenv("CARAPACE_DATA");
    return d && *d ? d : DATADIR;
}

static const char *nixdir(void) {
    const char *d = g_getenv("SNAPOS_NIX_DIR");
    if (d && *d) return d;
    if (access("/etc/snapos/configuration.nix", F_OK) == 0) return "/etc/snapos";
    return "/etc/nixos";
}

/* ---- the catalogue and what is installed --------------------------------- */

static void load_catalogue(void) {
    char *path = g_build_filename(datadir(), "apps.tsv", NULL);
    char *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        int n = g_strv_length(lines);
        apps = g_new0(App, n);
        for (int i = 0; i < n; i++) {
            if (!lines[i][0] || lines[i][0] == '#') continue;
            char **f = g_strsplit(lines[i], "\t", 5);
            if (g_strv_length(f) == 5) {
                App *a = &apps[napps++];
                a->attr = g_strdup(f[0]); a->name = g_strdup(f[1]); a->icon = g_strdup(f[2]);
                a->cat = g_strdup(f[3]); a->summary = g_strdup(f[4]);
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(text);
    }
    g_free(path);
}

/* The programs between the snapos:packages markers of configuration.nix. */
static void load_installed(void) {
    if (installed) g_hash_table_remove_all(installed);
    else installed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *path = g_build_filename(nixdir(), "configuration.nix", NULL);
    char *cfg = NULL;
    if (g_file_get_contents(path, &cfg, NULL, NULL)) {
        char *b = strstr(cfg, "# snapos:packages:begin");
        char *e = b ? strstr(b, "# snapos:packages:end") : NULL;
        char *p = b ? strchr(b, '[') : NULL;
        if (p && e && p < e) {
            for (p++; p < e && *p != ']'; ) {
                if (isalnum((unsigned char)*p) || *p == '_') {
                    char *s = p;
                    while (p < e && (isalnum((unsigned char)*p) || strchr("._+-", *p))) p++;
                    g_hash_table_add(installed, g_strndup(s, p - s));
                } else if (*p == '#') {
                    while (p < e && *p != '\n') p++;
                } else p++;
            }
        }
        g_free(cfg);
    }
    g_free(path);
}

static gboolean is_installed(const char *attr) { return installed && g_hash_table_contains(installed, attr); }

/* parts of SnapOS itself in the program list: not shown as removable programs */
static gboolean is_part_of_snapos(const char *attr) {
    static const char *parts[] = { "snapguard", "snapos-tools", "snapweb", "dpkg", "pciutils", NULL };
    for (int i = 0; parts[i]; i++) if (!strcmp(parts[i], attr)) return TRUE;
    return FALSE;
}

static App *find_app(const char *attr) {
    for (int i = 0; i < napps; i++) if (!strcmp(apps[i].attr, attr)) return &apps[i];
    return NULL;
}

/* ---- small pieces --------------------------------------------------------- */

/* The desktop's icon theme first; Papirus (SnapOS's icons) when it does not
 * have the program, as on Plasma, whose GTK theme is Breeze. */
static void set_icon(GtkImage *im, const char *name, int px) {
    static GtkIconTheme *papirus;
    GtkIconTheme *t = gtk_icon_theme_get_default();
    if (name && gtk_icon_theme_has_icon(t, name)) {
        gtk_image_set_from_icon_name(im, name, GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(im, px);
        return;
    }
    if (!papirus) { papirus = gtk_icon_theme_new(); gtk_icon_theme_set_custom_theme(papirus, "Papirus-Dark"); }
    GdkPixbuf *pb = name ? gtk_icon_theme_load_icon(papirus, name, px, GTK_ICON_LOOKUP_FORCE_SIZE, NULL) : NULL;
    if (pb) { gtk_image_set_from_pixbuf(im, pb); g_object_unref(pb); return; }
    gtk_image_set_from_icon_name(im, "application-x-executable", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(im, px);
}

static GtkWidget *icon(const char *name, int px) {
    GtkWidget *im = gtk_image_new();
    set_icon(GTK_IMAGE(im), name, px);
    return im;
}

static GtkWidget *label(const char *text, const char *cls, gboolean wrap) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (wrap) { gtk_label_set_line_wrap(GTK_LABEL(l), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(l), 40); }
    else gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    if (cls) gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    return l;
}

static void clear(GtkWidget *box) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *k = kids; k; k = k->next) gtk_widget_destroy(k->data);
    g_list_free(kids);
}

static void show_app(App *a);

static void on_card(GtkButton *b, gpointer data) { (void)b; show_app(data); }

static GtkWidget *card(App *a) {
    GtkWidget *btn = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "card");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(box), icon(a->icon, 48), FALSE, FALSE, 0);
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(text), label(a->name, "card-name", FALSE), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), label(a->summary, "dim", FALSE), FALSE, FALSE, 0);
    if (is_installed(a->attr)) gtk_box_pack_start(GTK_BOX(text), label("Installed", "ok", FALSE), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(btn), box);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_card), a);
    return btn;
}

/* ---- the first page: the popular programs, or one category -------------- */

static void show_explore(const char *cat) {
    clear(ui.flow);
    for (int i = 0; i < napps; i++) {
        if (cat && !strcmp(cat, "@installed")) { if (!is_installed(apps[i].attr)) continue; }
        else if (cat && strcmp(apps[i].cat, cat)) continue;
        gtk_flow_box_insert(GTK_FLOW_BOX(ui.flow), card(&apps[i]), -1);
    }
    if (cat && !strcmp(cat, "@installed") && installed) {          /* installed, and not in the catalogue */
        GHashTableIter it; gpointer k;
        g_hash_table_iter_init(&it, installed);
        while (g_hash_table_iter_next(&it, &k, NULL)) {
            if (find_app(k) || is_part_of_snapos(k)) continue;
            App *a = g_new0(App, 1);
            a->attr = g_strdup(k); a->name = g_strdup(k); a->icon = g_strdup(k); a->cat = g_strdup(""); a->summary = g_strdup("Installed from the search");
            gtk_flow_box_insert(GTK_FLOW_BOX(ui.flow), card(a), -1);
        }
    }
    gtk_widget_show_all(ui.flow);
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), "explore");
}

static void on_side(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box; (void)data;
    if (!row) return;
    const char *cat = g_object_get_data(G_OBJECT(row), "cat");
    show_explore(cat && *cat ? cat : NULL);
}

/* ---- the app page --------------------------------------------------------- */

static void install(App *a, gboolean add);

static void on_page_button(GtkButton *b, gpointer data) {
    (void)b; (void)data;
    if (ui.shown) install(ui.shown, !is_installed(ui.shown->attr));
}

static void show_app(App *a) {
    ui.shown = a;
    set_icon(GTK_IMAGE(ui.page_icon), a->icon, 96);
    gtk_label_set_text(GTK_LABEL(ui.page_name), a->name);
    gtk_label_set_text(GTK_LABEL(ui.page_summary), a->summary);
    char *attr = g_strdup_printf("Nix package: %s", a->attr);
    gtk_label_set_text(GTK_LABEL(ui.page_attr), attr);
    g_free(attr);
    gboolean have = is_installed(a->attr);
    gtk_button_set_label(GTK_BUTTON(ui.page_button), have ? "Remove" : "Install");
    GtkStyleContext *sc = gtk_widget_get_style_context(ui.page_button);
    if (have) { gtk_style_context_remove_class(sc, "suggested-action"); gtk_style_context_add_class(sc, "destructive-action"); }
    else { gtk_style_context_remove_class(sc, "destructive-action"); gtk_style_context_add_class(sc, "suggested-action"); }
    char *cmd = g_strdup_printf("$ snapos %s %s", have ? "remove" : "add", a->attr);
    gtk_label_set_text(GTK_LABEL(ui.page_cmd), cmd);
    g_free(cmd);
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), "app");
}

/* ---- the search: the catalogue first, then every Nix package -------------- */

static void on_result(GtkButton *b, gpointer data) {
    (void)b;
    App *found = data;
    App *known = find_app(found->attr);
    if (known) { show_app(known); return; }
    g_free(ui.search_app.attr); g_free(ui.search_app.name); g_free(ui.search_app.icon); g_free(ui.search_app.summary);
    ui.search_app.attr = g_strdup(found->attr); ui.search_app.name = g_strdup(found->attr);
    ui.search_app.icon = g_strdup(found->attr); ui.search_app.summary = g_strdup(found->summary);
    ui.search_app.cat = "";
    show_app(&ui.search_app);
}

static void add_result(const char *attr, const char *version, const char *summary) {
    App *known = find_app(attr);
    App *a = g_new0(App, 1);                     /* kept for the click; small and short-lived */
    a->attr = g_strdup(attr); a->summary = g_strdup(summary);
    GtkWidget *btn = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "card");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(box), icon(known ? known->icon : attr, 40), FALSE, FALSE, 0);
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(text), label(known ? known->name : attr, "card-name", FALSE), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), label(known ? known->summary : summary, "dim", FALSE), FALSE, FALSE, 0);
    char *pkg = g_strdup_printf("%s%s%s", attr, version && *version ? " · " : "", version ? version : "");
    gtk_box_pack_start(GTK_BOX(text), label(pkg, "mono", FALSE), FALSE, FALSE, 0);
    g_free(pkg);
    gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
    if (is_installed(attr)) gtk_box_pack_end(GTK_BOX(box), label("Installed", "ok", FALSE), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(btn), box);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_result), a);
    gtk_box_pack_start(GTK_BOX(ui.results), btn, FALSE, FALSE, 0);
    gtk_widget_show_all(btn);
}

/* nix search prints "* attr (version)" and then the description, indented */
static char *strip_ansi(const char *s) {
    GString *o = g_string_new(NULL);
    for (const char *p = s; *p; p++) {
        if (*p == 0x1b) { while (*p && !g_ascii_isalpha(*p)) p++; if (!*p) break; continue; }
        g_string_append_c(o, *p);
    }
    return g_string_free(o, FALSE);
}

/* lower is better: a program from the first page, then the name itself,
 * then names that start with it or have it, then the rest */
static int relevance(const char *attr, const char *q) {
    const char *base = strrchr(attr, '.') ? strrchr(attr, '.') + 1 : attr;
    int nested = strchr(attr, '.') ? 4 : 0;          /* python313Packages.x, octavePackages.x: libraries, after programs */
    if (find_app(attr)) return 0;
    if (!g_ascii_strcasecmp(base, q)) return 1 + nested;
    if (!g_ascii_strncasecmp(base, q, strlen(q))) return 2 + nested;
    if (strstr(base, q)) return 3 + nested;
    return 4 + nested;
}

static gint by_relevance(gconstpointer a, gconstpointer b, gpointer q) {
    char **x = *(char ***)a, **y = *(char ***)b;
    int d = relevance(x[0], q) - relevance(y[0], q);
    return d ? d : g_ascii_strcasecmp(x[0], y[0]);
}

static void on_found(GObject *src, GAsyncResult *res, gpointer data) {
    (void)data;
    char *out = NULL;
    GError *err = NULL;
    GSubprocess *p = G_SUBPROCESS(src);
    g_subprocess_communicate_utf8_finish(p, res, &out, NULL, &err);
    if (p != ui.find) { g_free(out); g_clear_error(&err); return; }   /* an older search */
    gtk_spinner_stop(GTK_SPINNER(ui.spinner));
    gtk_widget_hide(ui.spinner);
    int found = 0;
    const char *q = gtk_entry_get_text(GTK_ENTRY(ui.search));
    GPtrArray *hits = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
    if (out) {
        char **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *l = strip_ansi(lines[i]);
            if (l[0] == '*' && l[1] == ' ') {
                char *attr = l + 2, *ver = strchr(attr, ' ');
                char *summary = lines[i + 1] ? strip_ansi(lines[i + 1]) : g_strdup("");
                if (ver) { *ver++ = 0; if (*ver == '(') ver++; char *e = strrchr(ver, ')'); if (e) *e = 0; }
                if (g_str_has_prefix(attr, "legacyPackages.") || g_str_has_prefix(attr, "packages.")) {
                    char *sys = strchr(attr, '.');                 /* "legacyPackages.x86_64-linux.a.b" -> "a.b" */
                    char *rest = sys ? strchr(sys + 1, '.') : NULL;
                    if (rest) attr = rest + 1;
                }
                char *f[] = { attr, ver ? ver : "", g_strstrip(summary), NULL };
                g_ptr_array_add(hits, g_strdupv(f));
                found++;
                g_free(summary);
            }
            g_free(l);
        }
        g_strfreev(lines);
    }
    g_ptr_array_sort_with_data(hits, by_relevance, (gpointer)q);
    for (guint i = 0; i < hits->len && i < 150; i++) {
        char **h = g_ptr_array_index(hits, i);
        add_result(h[0], h[1], h[2]);
    }
    g_ptr_array_free(hits, TRUE);
    char *t = g_strdup_printf("%d programs found", found);
    if (!found) { g_free(t); t = g_strdup(err ? "The search could not run. Is the network up?" : "Nothing found. Try another word."); }
    gtk_label_set_text(GTK_LABEL(ui.results_title), t);
    g_free(t);
    if (g_getenv("CARAPACE_TEST_SEARCH")) printf("CARAPACE-FOUND %d\n", found), fflush(stdout);
    g_free(out);
    g_clear_error(&err);
}

static gboolean run_search(gpointer data) {
    (void)data;
    ui.debounce = 0;
    const char *q = gtk_entry_get_text(GTK_ENTRY(ui.search));
    if (!q || strlen(q) < 2) { show_explore(NULL); return G_SOURCE_REMOVE; }
    clear(ui.results);
    gtk_label_set_text(GTK_LABEL(ui.results_title), "Searching every Nix package (the first search takes a minute)…");
    gtk_widget_show(ui.spinner);
    gtk_spinner_start(GTK_SPINNER(ui.spinner));
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), "results");
    if (ui.find) { g_subprocess_force_exit(ui.find); g_clear_object(&ui.find); }
    const char *finder = g_getenv("CARAPACE_FIND");                 /* tests: another way to search */
    GError *err = NULL;
    if (finder && *finder)
        ui.find = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err, "sh", "-c", finder, "carapace", q, NULL);
    else
        ui.find = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err, "snapos", "find", q, NULL);
    if (!ui.find) { gtk_label_set_text(GTK_LABEL(ui.results_title), "The search could not start."); g_clear_error(&err); return G_SOURCE_REMOVE; }
    g_subprocess_communicate_utf8_async(ui.find, NULL, NULL, on_found, NULL);
    return G_SOURCE_REMOVE;
}

static void on_search(GtkEntry *e, gpointer data) {
    (void)e; (void)data;
    if (ui.debounce) g_source_remove(ui.debounce);
    ui.debounce = g_timeout_add(600, run_search, NULL);
}

/* ---- installing: pkexec snapos add, a progress window and one of Snappy's scenes */

typedef struct {
    GtkWidget *dlg, *title, *sub, *bar, *step[4], *last, *done;
    GDataInputStream *in;
    GSubprocess *proc;
    App *app;
    gboolean add;
    int stage;
    guint pulse;
} Job;

static const char *STEPS_ADD[4] = { "Checking the program", "Adding it to /etc/snapos", "Building the next system", "Ready: it opens from the menu" };
static const char *STEPS_REMOVE[4] = { "Checking", "Taking it out of /etc/snapos", "Building the next system", "Removed" };

static void set_stage(Job *j, int s) {
    if (s <= j->stage) return;
    j->stage = s;
    for (int i = 0; i < 4; i++) {
        const char *mark = i < s ? "✓" : i == s ? "●" : "○";
        char *t = g_strdup_printf("%s  %s", mark, (j->add ? STEPS_ADD : STEPS_REMOVE)[i]);
        gtk_label_set_text(GTK_LABEL(j->step[i]), t);
        g_free(t);
    }
}

static gboolean pulse(gpointer data) {
    Job *j = data;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(j->bar));
    return G_SOURCE_CONTINUE;
}

static void read_line(Job *j);

static void on_line(GObject *src, GAsyncResult *res, gpointer data) {
    Job *j = data;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, NULL, NULL);
    if (!line) return;
    char *clean = strip_ansi(line);
    g_strstrip(clean);
    if (*clean) {
        gtk_label_set_text(GTK_LABEL(j->last), clean);
        if (strstr(clean, "snapctl") || strstr(clean, "declared") || strstr(clean, "added") || strstr(clean, "removed")) set_stage(j, 2);
        if (strstr(clean, "building") || strstr(clean, "copying path") || strstr(clean, "rebuild")) set_stage(j, 2);
    }
    g_free(clean);
    g_free(line);
    read_line(j);
}

static void read_line(Job *j) {
    g_data_input_stream_read_line_async(j->in, G_PRIORITY_DEFAULT, NULL, on_line, j);
}

static void on_done(GObject *src, GAsyncResult *res, gpointer data) {
    Job *j = data;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
    int ok = g_subprocess_get_if_exited(j->proc) && g_subprocess_get_exit_status(j->proc) == 0;
    int st = g_subprocess_get_if_exited(j->proc) ? g_subprocess_get_exit_status(j->proc) : -1;
    g_source_remove(j->pulse);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(j->bar), ok ? 1.0 : 0.0);
    if (ok) {
        set_stage(j, 4);
        char *t = g_strdup_printf("%s is %s", j->app->name, j->add ? "installed" : "removed");
        gtk_label_set_text(GTK_LABEL(j->title), t);
        g_free(t);
        gtk_label_set_text(GTK_LABEL(j->sub), j->add ? "Find it in the menu. Undo anytime with snapos rollback." : "It is gone from the next system.");
    } else {
        gtk_label_set_text(GTK_LABEL(j->title), st == 126 || st == 127 ? "Cancelled" : "That did not work");
        gtk_label_set_text(GTK_LABEL(j->sub), "Nothing changed: your system is as it was.");
    }
    gtk_widget_set_sensitive(j->done, TRUE);
    load_installed();
    if (ui.shown) show_app(ui.shown);
    if (g_getenv("CARAPACE_TEST_INSTALL")) printf("CARAPACE-%s %s\n", ok ? "OK" : "FAILED", j->app->attr), fflush(stdout);
}

static char *pick_scene(void) {
    const char *fixed = g_getenv("CARAPACE_SCENE");
    char *dir = g_build_filename(datadir(), "scenes", NULL);
    if (fixed && *fixed) { char *f = g_strdup_printf("%s/%s.gif", dir, fixed); g_free(dir); return f; }
    GPtrArray *all = g_ptr_array_new_with_free_func(g_free);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const char *n;
        while ((n = g_dir_read_name(d))) if (g_str_has_suffix(n, ".gif")) g_ptr_array_add(all, g_build_filename(dir, n, NULL));
        g_dir_close(d);
    }
    char *pick = all->len ? g_strdup(g_ptr_array_index(all, g_random_int_range(0, all->len))) : NULL;
    g_ptr_array_free(all, TRUE);
    g_free(dir);
    return pick;
}

static void on_close(GtkButton *b, gpointer data) { (void)b; gtk_widget_destroy(((Job *)data)->dlg); }

static void install(App *a, gboolean add) {
    Job *j = g_new0(Job, 1);
    j->app = a; j->add = add; j->stage = -1;
    j->dlg = gtk_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(j->dlg), GTK_WINDOW(ui.win));
    gtk_window_set_modal(GTK_WINDOW(j->dlg), TRUE);
    gtk_window_set_title(GTK_WINDOW(j->dlg), add ? "Installing" : "Removing");
    gtk_window_set_default_size(GTK_WINDOW(j->dlg), 640, -1);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(j->dlg));
    gtk_container_set_border_width(GTK_CONTAINER(area), 18);
    gtk_box_set_spacing(GTK_BOX(area), 10);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_box_pack_start(GTK_BOX(head), icon(a->icon, 56), FALSE, FALSE, 0);
    GtkWidget *tt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    char *t = g_strdup_printf("%s %s", add ? "Installing" : "Removing", a->name);
    j->title = label(t, "title", FALSE);
    g_free(t);
    j->sub = label("This takes a few minutes. Your current system stays as it is until it finishes.", "dim", TRUE);
    gtk_box_pack_start(GTK_BOX(tt), j->title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tt), j->sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), tt, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(area), head, FALSE, FALSE, 0);
    j->bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(area), j->bar, FALSE, FALSE, 4);
    for (int i = 0; i < 4; i++) { j->step[i] = label("", "mono", FALSE); gtk_box_pack_start(GTK_BOX(area), j->step[i], FALSE, FALSE, 0); }
    j->last = label("", "dim-mono", FALSE);
    gtk_box_pack_start(GTK_BOX(area), j->last, FALSE, FALSE, 0);
    char *scene = pick_scene();
    if (scene) {
        GdkPixbufAnimation *anim = gdk_pixbuf_animation_new_from_file(scene, NULL);
        if (anim) {
            GtkWidget *frame = gtk_event_box_new();
            gtk_style_context_add_class(gtk_widget_get_style_context(frame), "scene");
            gtk_container_add(GTK_CONTAINER(frame), gtk_image_new_from_animation(anim));
            gtk_box_pack_start(GTK_BOX(area), frame, FALSE, FALSE, 6);
            g_object_unref(anim);
        }
        g_free(scene);
    }
    j->done = gtk_button_new_with_label("Close");
    gtk_widget_set_sensitive(j->done, FALSE);
    gtk_widget_set_halign(j->done, GTK_ALIGN_END);
    g_signal_connect(j->done, "clicked", G_CALLBACK(on_close), j);
    gtk_box_pack_start(GTK_BOX(area), j->done, FALSE, FALSE, 0);
    set_stage(j, 0);
    gtk_widget_show_all(j->dlg);

    /* pkexec needs the full path; CARAPACE_RUN replaces pkexec in tests */
    char *snapos = g_find_program_in_path("snapos");
    const char *runner = g_getenv("CARAPACE_RUN");
    GError *err = NULL;
    GSubprocessFlags fl = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE;
    if (runner && *runner)
        j->proc = g_subprocess_new(fl, &err, "sh", "-c", runner, "carapace", add ? "add" : "remove", a->attr, NULL);
    else
        j->proc = g_subprocess_new(fl, &err, "pkexec", snapos ? snapos : "/run/current-system/sw/bin/snapos", add ? "add" : "remove", a->attr, NULL);
    g_free(snapos);
    if (!j->proc) {
        gtk_label_set_text(GTK_LABEL(j->title), "Could not start");
        gtk_label_set_text(GTK_LABEL(j->sub), err ? err->message : "");
        gtk_widget_set_sensitive(j->done, TRUE);
        g_clear_error(&err);
        return;
    }
    set_stage(j, 1);
    j->pulse = g_timeout_add(120, pulse, j);
    j->in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(j->proc));
    read_line(j);
    g_subprocess_wait_async(j->proc, NULL, on_done, j);
}

/* ---- the window ----------------------------------------------------------- */

static const char *CSS =
    ".side { background: alpha(@theme_fg_color, 0.03); }"
    ".side row { padding: 8px 14px; border-radius: 8px; }"
    ".side .heading { font-size: 11px; opacity: 0.6; padding: 14px 14px 4px; }"
    ".card { padding: 12px; border-radius: 12px; }"
    ".card-name { font-weight: bold; font-size: 15px; }"
    ".title { font-weight: bold; font-size: 20px; }"
    ".page-name { font-weight: bold; font-size: 30px; }"
    ".dim { opacity: 0.7; }"
    ".ok { color: #5cc86e; font-size: 12px; }"
    ".mono { font-family: monospace; font-size: 12px; opacity: 0.75; }"
    ".dim-mono { font-family: monospace; font-size: 11px; opacity: 0.55; }"
    ".cmd { font-family: monospace; padding: 10px 14px; border-radius: 8px; background: alpha(@theme_fg_color, 0.06); }"
    ".scene { background: #0c0808; border-radius: 10px; padding: 6px; }";

static GtkWidget *side_row(const char *text, const char *cat) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(row), label(text, NULL, FALSE));
    g_object_set_data_full(G_OBJECT(row), "cat", g_strdup(cat), g_free);
    return row;
}

static GtkWidget *build_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(page), 28);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    ui.page_icon = gtk_image_new();
    gtk_box_pack_start(GTK_BOX(head), ui.page_icon, FALSE, FALSE, 0);
    GtkWidget *tt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    ui.page_name = label("", "page-name", FALSE);
    ui.page_summary = label("", "dim", TRUE);
    ui.page_attr = label("", "mono", FALSE);
    ui.page_button = gtk_button_new_with_label("Install");
    gtk_widget_set_halign(ui.page_button, GTK_ALIGN_START);
    g_signal_connect(ui.page_button, "clicked", G_CALLBACK(on_page_button), NULL);
    gtk_box_pack_start(GTK_BOX(tt), ui.page_name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tt), ui.page_summary, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tt), ui.page_attr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tt), ui.page_button, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(head), tt, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), head, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), label("How it installs", "title", FALSE), FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(page), label("It becomes part of your system, like everything in SnapOS: Carapace adds it to "
                                            "/etc/snapos and builds the next system. Removing it, or going back with snapos "
                                            "rollback, always works.", "dim", TRUE), FALSE, FALSE, 0);
    ui.page_cmd = label("", "cmd", FALSE);
    gtk_box_pack_start(GTK_BOX(page), ui.page_cmd, FALSE, FALSE, 0);
    GtkWidget *back = gtk_button_new_with_label("← Back");
    gtk_widget_set_halign(back, GTK_ALIGN_START);
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_explore), NULL);
    gtk_box_pack_start(GTK_BOX(page), back, FALSE, FALSE, 8);
    return page;
}

static gboolean test_start(gpointer data) {
    (void)data;
    const char *q = g_getenv("CARAPACE_TEST_SEARCH");
    if (q && *q) { gtk_entry_set_text(GTK_ENTRY(ui.search), q); }
    const char *attr = g_getenv("CARAPACE_TEST_INSTALL");
    if (attr && *attr) {
        App *a = find_app(attr);
        if (!a) { a = g_new0(App, 1); a->attr = g_strdup(attr); a->name = g_strdup(attr); a->icon = g_strdup(attr); a->cat = ""; a->summary = g_strdup(""); }
        show_app(a);
        install(a, !is_installed(a->attr));
    }
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    load_catalogue();
    load_installed();

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    ui.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(ui.win), 1100, 720);
    gtk_window_set_icon_name(GTK_WINDOW(ui.win), "carapace");
    g_signal_connect(ui.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *hb = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hb), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(hb), "Carapace");
    ui.search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.search), "Search every Nix package");
    gtk_widget_set_size_request(ui.search, 420, -1);
    g_signal_connect(ui.search, "changed", G_CALLBACK(on_search), NULL);
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(hb), ui.search);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), icon("carapace", 24));
    gtk_window_set_titlebar(GTK_WINDOW(ui.win), hb);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    ui.side = gtk_list_box_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(ui.side), "side");
    gtk_widget_set_size_request(ui.side, 210, -1);
    gtk_list_box_insert(GTK_LIST_BOX(ui.side), side_row("Explore", ""), -1);
    gtk_list_box_insert(GTK_LIST_BOX(ui.side), side_row("Installed", "@installed"), -1);
    GtkWidget *h = gtk_list_box_row_new();
    gtk_container_add(GTK_CONTAINER(h), label("CATEGORIES", "heading", FALSE));
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(h), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(h), FALSE);
    gtk_list_box_insert(GTK_LIST_BOX(ui.side), h, -1);
    const char *cats[] = { "Internet", "Games", "Graphics", "Audio & Video", "Office", "Development", "Utilities", NULL };
    for (int i = 0; cats[i]; i++) gtk_list_box_insert(GTK_LIST_BOX(ui.side), side_row(cats[i], cats[i]), -1);
    g_signal_connect(ui.side, "row-activated", G_CALLBACK(on_side), NULL);
    gtk_box_pack_start(GTK_BOX(body), ui.side, FALSE, FALSE, 0);

    ui.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ui.stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    ui.flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(ui.flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(ui.flow), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(ui.flow), 4);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(ui.flow), 10);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(ui.flow), 10);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(ui.flow), TRUE);
    gtk_widget_set_valign(ui.flow, GTK_ALIGN_START);
    gtk_container_set_border_width(GTK_CONTAINER(ui.flow), 22);
    gtk_container_add(GTK_CONTAINER(sc), ui.flow);
    gtk_stack_add_named(GTK_STACK(ui.stack), sc, "explore");

    GtkWidget *rs = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(rbox), 22);
    GtkWidget *rh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    ui.results_title = label("", "title", FALSE);
    ui.spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(rh), ui.results_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rh), ui.spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rbox), rh, FALSE, FALSE, 0);
    ui.results = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(rbox), ui.results, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(rs), rbox);
    gtk_stack_add_named(GTK_STACK(ui.stack), rs, "results");

    GtkWidget *ps = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(ps), build_page());
    gtk_stack_add_named(GTK_STACK(ui.stack), ps, "app");

    gtk_box_pack_start(GTK_BOX(body), ui.stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(ui.win), body);
    gtk_widget_show_all(ui.win);
    gtk_widget_hide(ui.spinner);
    show_explore(NULL);
    gtk_list_box_select_row(GTK_LIST_BOX(ui.side), gtk_list_box_get_row_at_index(GTK_LIST_BOX(ui.side), 0));
    if (g_getenv("CARAPACE_TEST_INSTALL") || g_getenv("CARAPACE_TEST_SEARCH")) g_timeout_add(800, test_start, NULL);
    gtk_main();
    return 0;
}
