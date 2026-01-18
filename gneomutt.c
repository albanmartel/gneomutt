#include <gtk/gtk.h>
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
} AppContext;

/* --- UTILITAIRES --- */
void send_term_data(GtkWidget *terminal, const char *data) {
    if (!terminal) return;
    vte_terminal_feed_child(VTE_TERMINAL(terminal), data, -1);
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

gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)widget;
    if (event->keyval == GDK_KEY_F1) {
        on_help_clicked(NULL, user_data);
        return TRUE;
    }
    return FALSE;
}

void on_refresh_clicked(GtkButton *btn, gpointer user_data) { 
    (void)btn; (void)user_data;
    if (system(CMD_SYNC) == -1) g_warning("Erreur sync");
}

// Précédent (Touche k ou Flèche haut)
void on_prev_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    send_term_data(((AppContext *)user_data)->terminal, "k");
}

// Suivant (Touche j ou Flèche bas)
void on_next_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    send_term_data(((AppContext *)user_data)->terminal, "j");
}

// Écrire un nouveau mail (Touche m)
void on_write_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    send_term_data(((AppContext *)user_data)->terminal, "m");
}

// Répondre (Touche r)
void on_reply_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    send_term_data(((AppContext *)user_data)->terminal, "r");
}

// Répondre à tous (Touche g)
void on_reply_all_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    send_term_data(((AppContext *)user_data)->terminal, "g");
}

// Supprimer (Touche d)
void on_del_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    send_term_data(((AppContext *)user_data)->terminal, "d");
}

// Selectionner mail (Touche Enter)
void on_enter_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    // On envoie simplement le caractère de saut de ligne
    send_term_data(((AppContext *)user_data)->terminal, "\n");
}

void on_folder_clicked(GtkButton *btn, gpointer macro_keys) {
    AppContext *ctx = g_object_get_data(G_OBJECT(btn), "ctx");
    send_term_data(ctx->terminal, (const char *)macro_keys);
    gtk_widget_grab_focus(ctx->terminal);
}

void on_action_clicked(GtkButton *btn, gpointer key_str) {
    AppContext *ctx = g_object_get_data(G_OBJECT(btn), "ctx");
    send_term_data(ctx->terminal, (const char *)key_str);
    gtk_widget_grab_focus(ctx->terminal);
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

/* --- INITIALISATION UI --- */
int init_gui(AppContext *ctx, GtkBuilder *builder) {
    GError *error = NULL;
    // Chargement depuis la ressource au lieu du fichier
    if (!gtk_builder_add_from_resource(builder, "/com/monprojet/icons/interface.ui", &error)) {
        g_printerr("Erreur chargement interface : %s\n", error->message);
        g_error_free(error);
        return 0;
    }

    ctx->window = GTK_WIDGET(gtk_builder_get_object(builder, "window"));
    GError *icon_error = NULL;
    // On charge l'image depuis le chemin virtuel défini dans le XML
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_resource("/com/monprojet/icons/neomutt-icone.png", &icon_error);
    if (pixbuf) {
        gtk_window_set_icon(GTK_WINDOW(ctx->window), pixbuf);
        g_object_unref(pixbuf);
    } else {
        g_warning("Impossible de charger l'icône : %s", icon_error->message);
        g_error_free(icon_error);
    }
    ctx->terminal = vte_terminal_new();
    /* --- Bloc de recherche unique --- */
    ctx->search_entry = GTK_WIDGET(gtk_builder_get_object(builder, "main_search_entry"));
    ctx->search_combo = GTK_WIDGET(gtk_builder_get_object(builder, "search_options_combo"));
    GtkWidget *btn_search = GTK_WIDGET(gtk_builder_get_object(builder, "btn_execute_search"));
    
    if (btn_search && ctx->search_entry && ctx->search_combo) {
        g_signal_connect(btn_search, "clicked", G_CALLBACK(on_search_clicked), ctx);
        g_signal_connect(ctx->search_entry, "activate", G_CALLBACK(on_search_clicked), ctx);
    }
    
    GtkWidget *hbox_body = GTK_WIDGET(gtk_builder_get_object(builder, "hbox_body"));
    gtk_box_pack_start(GTK_BOX(hbox_body), ctx->terminal, TRUE, TRUE, 0);

    // Signaux de base
    g_signal_connect(ctx->terminal, "child-exited", G_CALLBACK(on_terminal_child_exited), ctx);
    g_signal_connect(ctx->window, "key-press-event", G_CALLBACK(on_key_press), ctx);

    // Spawn NeoMutt
    vte_terminal_spawn_async(VTE_TERMINAL(ctx->terminal), VTE_PTY_DEFAULT, NULL, 
                             (char *[]){CMD_NEOMUTT, NULL}, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, -1, NULL, NULL, NULL);

/* --- LIAISON DOSSIERS (CORRIGÉ ET COMPLÉTÉ) --- */
    const char *folders[][2] = {
        {"btn_inbox",      MACRO_INBOX}, 
        {"btn_sent",       MACRO_SENT}, 
        {"btn_trash",      MACRO_TRASH},
        {"btn_draft",      MACRO_DRAFT}, 
        {"btn_quarantine", MACRO_QUAR}, 
        {"btn_archives",   MACRO_ARCHIVES}
    };


    for(size_t i = 0; i < G_N_ELEMENTS(folders); i++) {
        GtkWidget *b = GTK_WIDGET(gtk_builder_get_object(builder, folders[i][0]));
        if(b) {
            g_object_set_data(G_OBJECT(b), "ctx", ctx);
            g_signal_connect(b, "clicked", G_CALLBACK(on_folder_clicked), (gpointer)folders[i][1]);
        }
    }

    /* --- BOUTONS RACCOURCIS --- */
    struct {
        const char *id;
        void (*callback)(GtkButton *, gpointer);
    } buttons[] = {
        {"btn_prev",      on_prev_clicked},
        {"btn_next",      on_next_clicked},
        {"btn_enter",     on_enter_clicked},
        {"btn_write",     on_write_clicked},
        {"btn_reply",     on_reply_clicked},
        {"btn_reply_all", on_reply_all_clicked},
        {"btn_del",       on_del_clicked},
        {"btn_stop",      on_stop_clicked} // Votre bouton Quitter
    };

    for (int i = 0; i < 7; i++) {
        GObject *obj = gtk_builder_get_object(builder, buttons[i].id);
        if (obj) {
            g_signal_connect(obj, "clicked", G_CALLBACK(buttons[i].callback), ctx);
        } else {
            g_print("Attention : bouton %s non trouvé dans le XML\n", buttons[i].id);
        }
    }

    // Boutons spéciaux
    GtkWidget *btn_help = GTK_WIDGET(gtk_builder_get_object(builder, "btn_help"));
    if(btn_help) g_signal_connect(btn_help, "clicked", G_CALLBACK(on_help_clicked), ctx);

    /* AUTRES BOUTONS */
    GtkWidget *btn_stop = GTK_WIDGET(gtk_builder_get_object(builder, "btn_stop"));
    if(btn_stop) {
	g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop_clicked), ctx);
    }

    // On branche aussi le bouton de synchronisation
    GObject *btn_sync = gtk_builder_get_object(builder, "btn_sync");
    if(btn_sync) g_signal_connect(btn_sync, "clicked", G_CALLBACK(on_refresh_clicked), ctx);

    // --- FIN DE LA FONCTION ---

    // On affiche tout
    gtk_widget_show_all(ctx->window);

    // ON AJOUTE CECI ICI : La fenêtre s'ouvre en grand
    gtk_window_maximize(GTK_WINDOW(ctx->window));

    gtk_widget_show_all(ctx->window);
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

    /* * Note : Une fois que init_gui a extrait les widgets (window, terminal, etc.)
     * et les a stockés dans 'ctx', le builder n'est plus nécessaire.
     * Les widgets eux-mêmes restent en mémoire car ils appartiennent à la fenêtre parente.
     */
    g_object_unref(builder);

    // 6. Boucle principale
    gtk_main();

    return 0;
}
