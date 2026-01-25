#include <gtk/gtk.h>
#include <stddef.h>
#include <vte/vte.h>
#include <sys/prctl.h>
#include <stdlib.h>
#include <string.h>

#ifdef DEBUG
    #define DEBUG_LOG(fmt, ...) g_print("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
    #define DEBUG_LOG(fmt, ...)
#endif

/* --- CONFIGURATION --- */
#define PROGRAMME_NAME "gneomutt"
#define UI_FILE        "interface.ui"
#define CMD_NEOMUTT    "/usr/bin/neomutt"
#define CMD_SYNC       "mbsync -a && notmuch new &"

#define MACRO_INBOX "gi"
#define MACRO_SENT "go"
#define MACRO_QUAR "gq"
#define MACRO_DRAFT "gd"
#define MACRO_ARCHIVES "ga"
#define MACRO_TRASH "gt"
#define MACRO_LOCALE "gl"

#define KEY_DEL "d"
#define KEY_NEXT "j"
#define KEY_PREV "k"
#define KEY_WRITE "m"
#define KEY_REPLY "r"
#define KEY_REPLY_ALL "g"

const char *HELP_TEXT = 
"GUIDE DE RÉFÉRENCE NEOMUTT\n"
"==========================\n\n"
"1. NAVIGATION\n"
"---------------------\n"
"j / k         : Déplacer la sélection (Bas / Haut)\n"
"Entrée        : Ouvrir le message ou la pièce jointe\n"
"Espace        : Faire défiler le texte (Page suivante)\n"
"q             : Retour en arrière / Quitter\n\n"
"2. GESTION DES MESSAGES\n"
"-----------------------\n"
"m             : Rédiger un nouveau message\n"
"r             : Répondre à l'expéditeur\n"
"g             : Répondre à TOUS (Group reply)\n"
"f             : Transférer le message (Forward)\n"
"d             : Marquer pour suppression\n"
"u             : Annuler une suppression (Undelete)\n"
"$             : Sauvegarder et synchroniser la boîte\n\n"
"3. RECHERCHE ET FILTRES\n"
"-----------------------\n"
"/             : Rechercher dans le dossier actuel\n"
"n             : Résultat de recherche suivant\n"
"l             : Limiter l'affichage (ex: type 'all')\n\n"
"4. ACTIONS AVANCÉES\n"
"-------------------\n"
"v             : Voir les pièces jointes\n"
"t             : 'Taguer' un message (sélection multiple)\n"
";             : Appliquer l'action suivante aux messages tagués\n"
"              (Exemple : ';d' pour tout supprimer)\n";

typedef struct {
    GtkWidget *window;
    GtkWidget *terminal;
    GtkWidget *search_entry;
    GtkWidget *search_combo;
    GtkBuilder *builder;
} AppContext;

/* --- UTILITAIRES --- */
void send_term_data(GtkWidget *terminal, const char *data) {
    if (!terminal) return;
    vte_terminal_feed_child(VTE_TERMINAL(terminal), data, -1);
}

void update_active_folder_ui(GtkWidget *active_button, GtkBuilder *builder) {
    if (!GTK_IS_BUILDER(builder)) return;

    const char *ids[] = {
        "btn_inbox", "btn_sent", "btn_trash", "btn_draft", 
        "btn_quarantine", "btn_archives", "btn_locale"
    };
    
    for (size_t i = 0; i < G_N_ELEMENTS(ids); i++) {
        GObject *obj = gtk_builder_get_object(builder, ids[i]);
        if (obj) {
            GtkStyleContext *style = gtk_widget_get_style_context(GTK_WIDGET(obj));
            gtk_style_context_remove_class(style, "folder-active");
        }
    }
    
    if (active_button) {
        GtkStyleContext *style = gtk_widget_get_style_context(active_button);
        gtk_style_context_add_class(style, "folder-active");
    }
}

/* --- CALLBACKS --- */
void on_terminal_child_exited(VteTerminal *terminal, int status, gpointer user_data) {
    (void)terminal; (void)status; (void)user_data;
    gtk_main_quit();
}

void on_help_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppContext *ctx = (AppContext *)user_data;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Aide", GTK_WINDOW(ctx->window),
                            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                            "_Fermer", GTK_RESPONSE_CLOSE, NULL);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view)), HELP_TEXT, -1);
    
    gtk_container_add(GTK_CONTAINER(content_area), text_view);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void on_stop_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppContext *ctx = (AppContext *)user_data;
    
    // 1. Envoyer la commande d'exécution immédiate (sans confirmation)
    // On envoie : <exit> ou simplement la touche 'x' si bindée par défaut
    // Pour être universel, on peut envoyer : :q!\n 
    send_term_data(ctx->terminal, "\033:q!\n"); 

    // 2. Si NeoMutt ne se ferme pas (ex: bloqué), on force la fermeture de la fenêtre
    // Cela déclenchera la destruction du terminal et l'arrêt du programme
    gtk_window_close(GTK_WINDOW(ctx->window));
}

gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)widget; 
    AppContext *ctx = (AppContext *)user_data;

    /*--- CAS 1 si entrée clavier dans la barre de recherche ---*/ 
    if (gtk_widget_has_focus(ctx->search_entry)) {
        return FALSE; 
    }


    /* --- SURCHARGE DES TOUCHES FLÉCHÉES --- */
    const char *cmd = NULL;

    switch (event->keyval) {
        case GDK_KEY_Left:
            cmd = "k";      // Flèche Gauche -> Précédent
            break;
        case GDK_KEY_Right:
            cmd = "j";      // Flèche Droite -> Suivant
            break;
        case GDK_KEY_Up:
            cmd = "-";      // Flèche Haut -> Page précédente (ou action '-')
            break;
        case GDK_KEY_Down:
            cmd = " ";      // Flèche Bas -> Espace (Page suivante / Lire)
            break;
    }

    if (cmd && ctx->terminal) {
        vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), cmd, -1);
        return TRUE; // On arrête le traitement ici pour cette touche
    }

    /*--- CAS 2. Gestion Ctrl + Q  et F1 ---*/
    if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_q) {
        on_stop_clicked(NULL, ctx);
        return TRUE;
    }

    // 3. REDIRECTION SYSTÉMATIQUE
    // Si on n'est pas dans la recherche, on envoie TOUT au terminal
    if (ctx->terminal && event->string) {
        // On s'assure que le terminal a le focus interne
        if (!gtk_widget_has_focus(ctx->terminal)) {
            gtk_widget_grab_focus(ctx->terminal);
        }

        // On envoie la touche
        vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), event->string, -1);
        return TRUE; // On "consomme" l'événement pour que GTK ne l'utilise pas pour les boutons
    }

    return FALSE;
}

void on_refresh_clicked(GtkButton *btn, gpointer user_data) { 
    (void)btn; (void)user_data;
    if (system(CMD_SYNC) == -1) g_warning("Erreur sync");
}

void on_folder_clicked(GtkButton *btn, gpointer macro_keys) {
    AppContext *ctx = g_object_get_data(G_OBJECT(btn), "ctx");
    const char *macro = (const char *)macro_keys;

    if (ctx && ctx->terminal && macro) {
        send_term_data(ctx->terminal, macro);
        
        /* --- MISE À JOUR VISUELLE --- */
        update_active_folder_ui(GTK_WIDGET(btn), ctx->builder);
        
        gtk_widget_grab_focus(ctx->terminal);
    }
}

void on_action_clicked(GtkButton *btn, gpointer user_data) {
    // 1. On récupère le contexte (soit via user_data, soit via l'objet)
    AppContext *ctx = (AppContext *)user_data; 
    
    // 2. On récupère la touche stockée dans le bouton
    const char *key = g_object_get_data(G_OBJECT(btn), "key-to-send");

    if (ctx && ctx->terminal && key) {
        send_term_data(ctx->terminal, key);
        // Important pour garder le contrôle au clavier immédiatement
        gtk_widget_grab_focus(ctx->terminal);
    }
}

void on_search_clicked(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    AppContext *ctx = (AppContext *)user_data;
    const char *text = gtk_entry_get_text(GTK_ENTRY(ctx->search_entry));
    if (!text || strlen(text) == 0) return;
    const char *option = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ctx->search_combo));

    // --- NOUVELLE SÉQUENCE SÉCURISÉE ---
    // 1. \007 : Annule tout (Ctrl-G)
    // 2. :    : Ouvre la ligne de commande NeoMutt
    // 3. exec vfolder-from-query : La commande que vous avez validée
    // 4. \n   : Valide l'ouverture de la recherche
    send_term_data(ctx->terminal, "\007:exec vfolder-from-query\n");

    // 5. On envoie les critères Notmuch
    if (g_strcmp0(option, "from") == 0) {
        send_term_data(ctx->terminal, "from:");
    } else if (g_strcmp0(option, "sub") == 0) {
        send_term_data(ctx->terminal, "subject:");
    }

    // 6. On envoie le texte et on valide la recherche finale
    send_term_data(ctx->terminal, text);
    send_term_data(ctx->terminal, "\n");

    // Nettoyage et focus
    gtk_entry_set_text(GTK_ENTRY(ctx->search_entry), "");
    gtk_widget_grab_focus(ctx->terminal);
}

/* --- INITIALISATION UI --- */
int init_gui(AppContext *ctx, GtkBuilder *builder) {
    GError *error = NULL;

    ctx->builder = builder;

    /*--- 0. STYLE CSS BOITE AUX LETTRES ACTIVES ---*/
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
        ".folder-active { "
        "   background-color: #3584e4; " // Bleu GNOME
        "   color: white; "
        "   font-weight: bold; "
        "   border-radius: 5px; "
        "}", -1, NULL);

    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /*--- 1. Chargement de l'interface ---*/ 
    if (!gtk_builder_add_from_resource(builder, "/com/monprojet/icons/interface.ui", &error)) {
        g_printerr("Erreur chargement interface : %s\n", error->message);
        g_error_free(error);
        return 0;
    }

    /*--- 2. Configuration de la fenêtre et de l'icône ---*/
    ctx->window = GTK_WIDGET(gtk_builder_get_object(builder, "window"));
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_resource("/com/monprojet/icons/neomutt-icone.png", NULL);
    if (pixbuf) {
        gtk_window_set_icon(GTK_WINDOW(ctx->window), pixbuf);
        g_object_unref(pixbuf);
    }

    /*--- 3. Récupération du terminal (Correction du plein écran) ---*/
    ctx->terminal = GTK_WIDGET(gtk_builder_get_object(builder, "terminal"));
    gtk_widget_set_hexpand(ctx->terminal, TRUE);
    gtk_widget_set_vexpand(ctx->terminal, TRUE);

    /*--- 4. Recherche ---*/
    ctx->search_entry = GTK_WIDGET(gtk_builder_get_object(builder, "main_search_entry"));
    ctx->search_combo = GTK_WIDGET(gtk_builder_get_object(builder, "search_options_combo"));
    GtkWidget *btn_search = GTK_WIDGET(gtk_builder_get_object(builder, "btn_execute_search"));
    if (btn_search) {
        g_signal_connect(btn_search, "clicked", G_CALLBACK(on_search_clicked), ctx);
        g_signal_connect(ctx->search_entry, "activate", G_CALLBACK(on_search_clicked), ctx);
    }

    /*--- 5. Signaux système et Lancement NeoMutt ---*/
    g_signal_connect(ctx->terminal, "child-exited", G_CALLBACK(on_terminal_child_exited), ctx);
    g_signal_connect(ctx->window, "key-press-event", G_CALLBACK(on_key_press), ctx);

    vte_terminal_spawn_async(VTE_TERMINAL(ctx->terminal), VTE_PTY_DEFAULT, NULL, 
                             (char *[]){CMD_NEOMUTT, NULL}, NULL, G_SPAWN_SEARCH_PATH, 
                             NULL, NULL, NULL, -1, NULL, NULL, NULL);

    /* --- MUTUALISATION DES DOSSIERS --- */
    struct { const char *id; const char *macro; } folders[] = {
        {"btn_inbox", MACRO_INBOX}, {"btn_sent", MACRO_SENT}, {"btn_locale", MACRO_LOCALE},
        {"btn_trash", MACRO_TRASH}, {"btn_draft", MACRO_DRAFT}, 
        {"btn_quarantine", MACRO_QUAR}, {"btn_archives", MACRO_ARCHIVES}
    };

    for(size_t i = 0; i < G_N_ELEMENTS(folders); i++) {
        GtkWidget *b = GTK_WIDGET(gtk_builder_get_object(builder, folders[i].id));
        if(b) {
            g_object_set_data(G_OBJECT(b), "ctx", ctx); // On attache le contexte au bouton
            g_signal_connect(b, "clicked", G_CALLBACK(on_folder_clicked), (gpointer)folders[i].macro);
        }
    }

    /* --- MUTUALISATION DES RACCOURCIS (Utilise on_action_clicked) --- */
    struct { const char *id; const char *key; } shortcuts[] = {
        {"btn_prev", KEY_PREV}, {"btn_next", KEY_NEXT}, {"btn_enter", "\n"},
        {"btn_write", KEY_WRITE}, {"btn_reply", KEY_REPLY}, 
        {"btn_reply_all", KEY_REPLY_ALL}, {"btn_del", KEY_DEL}
    };

    for (size_t i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
        GtkWidget *obj = GTK_WIDGET(gtk_builder_get_object(builder, shortcuts[i].id));
        if (obj) {
            g_object_set_data(G_OBJECT(obj), "key-to-send", (gpointer)shortcuts[i].key);
            g_signal_connect(obj, "clicked", G_CALLBACK(on_action_clicked), ctx);
        }
    }

    /*--- Forces L'état actif sur INBOX au démarrage ---*/
    GtkWidget *btn_inbox = GTK_WIDGET(gtk_builder_get_object(builder, "btn_inbox"));
    if (btn_inbox) {
        update_active_folder_ui(btn_inbox, builder); // Utilise l'argument builder direct
    }

    /* --- BOUTONS SPÉCIAUX (Logique unique) --- */
    struct { const char *id; GCallback cb; } special[] = {
        {"btn_help", G_CALLBACK(on_help_clicked)},
        {"btn_stop", G_CALLBACK(on_stop_clicked)},
        {"btn_sync", G_CALLBACK(on_refresh_clicked)}
    };

    for (size_t i = 0; i < G_N_ELEMENTS(special); i++) {
        GtkWidget *btn = GTK_WIDGET(gtk_builder_get_object(builder, special[i].id));
        if(btn) g_signal_connect(btn, "clicked", special[i].cb, ctx);
    }

    /*--- 6. Affichage final ---*/
    gtk_widget_show_all(ctx->window);
    gtk_window_maximize(GTK_WINDOW(ctx->window));

    /*--- Le terminal prend le focus ---*/
    gtk_widget_grab_focus(ctx->terminal);

    return 1;
}

/* --- MAIN --- */
int main(int argc, char *argv[]) {
    // 1. Nommer le processus (utile pour 'top' ou 'ps')
    prctl(PR_SET_NAME, PROGRAMME_NAME, 0, 0, 0);

    // 2. Initialisation de GTK
    gtk_init(&argc, &argv);

    // 3. Initialisation du contexte
    // Il est conseillé de mettre la structure à zéro pour éviter les pointeurs sauvages
    AppContext ctx;
    memset(&ctx, 0, sizeof(AppContext));

    // 4. Création du Builder
    GtkBuilder *builder = gtk_builder_new();

    // 5. Chargement de l'interface
    if (!init_gui(&ctx, builder)) {
        g_printerr("Erreur fatale : impossible d'initialiser l'interface.\n");
        g_object_unref(builder);
        return 1;
    }
    
    // 6. Boucle principale
    gtk_main();

    /* * Note : Une fois que init_gui a extrait les widgets (window, terminal, etc.)
     * et les a stockés dans 'ctx', le builder n'est plus nécessaire.
     * Les widgets eux-mêmes restent en mémoire car ils appartiennent à la fenêtre parente.
     */
    g_object_unref(builder);


    return 0;
}
