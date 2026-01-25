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

/*--- Taille tableau des pointeurs de dossiers ---*/
#define NB_FOLDERS 7

#define ERR_SYNC "Erreur sync"
#define ERR_BUILDER "Erreur fatale : impossible d'initialiser l'interface.\n"

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
    GtkWidget *date_combo;
    GtkWidget *folder_buttons[NB_FOLDERS];
} AppContext;

/* --- CONFIGURATION DES TOUCHES (Arrow Keys Mapping) --- */
typedef struct {
    guint keyval;        // La touche pressée (ex: GDK_KEY_Left)
    const char *command; // La commande envoyée à NeoMutt (ex: "k")
} KeyMapping;

static const KeyMapping arrow_map[] = {
    {GDK_KEY_Left,  "k"},   // Left Arrow
    {GDK_KEY_Right, "j"},   // Right Arrow
    {GDK_KEY_Up,    "-"},   // Up Arrow
    {GDK_KEY_Down,  " "}    // Down Arrow (Space)
};

/* --- UTILITAIRES --- */
void send_term_data(GtkWidget *terminal, const char *data) {
    if (!terminal) return;
    vte_terminal_feed_child(VTE_TERMINAL(terminal), data, -1);
}

void update_active_folder_ui(GtkWidget *active_button, AppContext *ctx) {
    if (!ctx) return;

    // G_N_ELEMENTS calcule automatiquement le nombre de cases du tableau
    for (size_t i = 0; i < G_N_ELEMENTS(ctx->folder_buttons); i++) {
        if (ctx->folder_buttons[i]) {
            GtkStyleContext *style = gtk_widget_get_style_context(ctx->folder_buttons[i]);
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

    /* Associer le tableau de configuration aux nouveaux raccourci clavier */
    for (size_t i = 0; i < G_N_ELEMENTS(arrow_map); i++) {
        if (event->keyval == arrow_map[i].keyval) {
            cmd = arrow_map[i].command;
            break; // On a trouvé la correspondance, on sort de la boucle
        }
    }

    /* Vérification du cmd et de la présence du terminal */
    if (cmd && ctx->terminal) {
        vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), cmd, -1);
        return TRUE; 
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
    if (system(CMD_SYNC) == -1) g_warning(ERR_SYNC);
}

void on_folder_clicked(GtkButton *btn, gpointer macro_keys) {
    AppContext *ctx = g_object_get_data(G_OBJECT(btn), "ctx");
    const char *macro = (const char *)macro_keys;

    if (ctx && ctx->terminal && macro) {
        send_term_data(ctx->terminal, macro);
        
        /* --- MISE À JOUR VISUELLE --- */
        update_active_folder_ui(GTK_WIDGET(btn), ctx);
        
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
    const char *option = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ctx->search_combo));
    const char *date_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ctx->date_combo));

    // Si tout est vide, on ne fait rien
    if ((!text || strlen(text) == 0) && (g_strcmp0(date_id, "any") == 0)) return;

    // 1. Lancement de Notmuch
    vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), "\007:exec vfolder-from-query\n", -1);

    // 2. Construction de la requête Notmuch complexe
    if (date_id && g_strcmp0(date_id, "any") != 0) {
        if (g_strcmp0(date_id, "today") == 0)      vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), "date:today ", -1);
        else if (g_strcmp0(date_id, "week") == 0)  vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), "date:7d.. ", -1);
        else if (g_strcmp0(date_id, "month") == 0) vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), "date:1m.. ", -1);
    }

    // 3. Préfixe de champ (from: ou subject:)
    if (g_strcmp0(option, "from") == 0) {
        vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), "from:", -1);
    } else if (g_strcmp0(option, "sub") == 0) {
        vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), "subject:", -1);
    }

    // 4. Texte et validation finale
    vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), text, -1);
    vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), "\n", -1);

    // Reset UI
    gtk_entry_set_text(GTK_ENTRY(ctx->search_entry), "");
    gtk_widget_grab_focus(ctx->terminal);
}

/* --- INITIALISATION UI --- */
int init_gui(AppContext *ctx, GtkBuilder *builder) {
    GError *error = NULL;

    // CHANGEMENT 1 : Supprimez ou commentez la ligne ctx->builder = builder;
    // ctx->builder = builder; 

    /*--- 0. STYLE CSS ---*/
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
        /* Vos styles existants pour les boutons */
        ".folder-active { background-color: #3584e4; color: white; border-radius: 5px; } "
        
        /* Le cadre autour du terminal */
        "#terminal_frame { "
        "   border: 2px solid #454545; " /* Couleur de la bordure */
        "   background-color: #1a1a1a; " /* Doit correspondre au fond de NeoMutt */
        "   margin: 2px; "               /* Espace extérieur au cadre */
        "} "
        
        /* Suppression des bordures internes du terminal */
        "vte-terminal { "
        "   padding: 30px; "
        "   background-color: #3584e4; "
        "}", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider), 
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );

    /*--- 1. Chargement ---*/ 
    if (!gtk_builder_add_from_resource(builder, "/com/monprojet/icons/interface.ui", &error)) {
        g_printerr("Erreur chargement interface : %s\n", error->message);
        g_error_free(error);
        return 0;
    }

    /*--- 2. Fenêtre et Icône ---*/
    ctx->window = GTK_WIDGET(gtk_builder_get_object(builder, "window"));
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_resource("/com/monprojet/icons/neomutt-icone.png", NULL);
    if (pixbuf) {
        gtk_window_set_icon(GTK_WINDOW(ctx->window), pixbuf);
        g_object_unref(pixbuf);
    }

    /*--- 3. Terminal ---*/
    ctx->terminal = GTK_WIDGET(gtk_builder_get_object(builder, "terminal"));
    gtk_widget_set_hexpand(ctx->terminal, TRUE);
    gtk_widget_set_vexpand(ctx->terminal, TRUE);
    gtk_widget_set_halign(ctx->terminal, GTK_ALIGN_FILL);
    gtk_widget_set_valign(ctx->terminal, GTK_ALIGN_FILL);
    /*--- Force le terminal à ne pas réserver d'espace pour des caractères incomplets */
    vte_terminal_set_scroll_on_output(VTE_TERMINAL(ctx->terminal), TRUE);

    /*--- 4. Recherche ---*/
    ctx->search_entry = GTK_WIDGET(gtk_builder_get_object(builder, "main_search_entry"));
    ctx->search_combo = GTK_WIDGET(gtk_builder_get_object(builder, "search_options_combo"));
    ctx->date_combo = GTK_WIDGET(gtk_builder_get_object(builder, "date_search_combo"));

    GtkWidget *btn_search = GTK_WIDGET(gtk_builder_get_object(builder, "btn_execute_search"));
    if (btn_search) {
        g_signal_connect(btn_search, "clicked", G_CALLBACK(on_search_clicked), ctx);
        g_signal_connect(ctx->search_entry, "activate", G_CALLBACK(on_search_clicked), ctx);
    }

    /*--- 5. Signaux et Spawn ---*/
    g_signal_connect(ctx->terminal, "child-exited", G_CALLBACK(on_terminal_child_exited), ctx);
    g_signal_connect(ctx->window, "key-press-event", G_CALLBACK(on_key_press), ctx);
    vte_terminal_spawn_async(VTE_TERMINAL(ctx->terminal), VTE_PTY_DEFAULT, NULL, (char *[]){CMD_NEOMUTT, NULL}, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, -1, NULL, NULL, NULL);

    /* --- MUTUALISATION DES DOSSIERS --- */
    struct { const char *id; const char *macro; } folders[] = {
        {"btn_inbox", MACRO_INBOX}, {"btn_sent", MACRO_SENT}, {"btn_locale", MACRO_LOCALE},
        {"btn_trash", MACRO_TRASH}, {"btn_draft", MACRO_DRAFT}, 
        {"btn_quarantine", MACRO_QUAR}, {"btn_archives", MACRO_ARCHIVES}
    };

    for(size_t i = 0; i < G_N_ELEMENTS(folders); i++) {
        GtkWidget *b = GTK_WIDGET(gtk_builder_get_object(builder, folders[i].id));
        
        // CHANGEMENT 2 : On stocke l'adresse du bouton dans notre tableau persistant
        ctx->folder_buttons[i] = b; 
        
        if(b) {
            g_object_set_data(G_OBJECT(b), "ctx", ctx);
            g_signal_connect(b, "clicked", G_CALLBACK(on_folder_clicked), (gpointer)folders[i].macro);
        }
    }

    /* --- MUTUALISATION DES RACCOURCIS --- */
    struct { const char *id; const char *key; } shortcuts[] = {
        {"btn_prev", KEY_PREV}, {"btn_next", KEY_NEXT}, {"btn_enter", "\n"},
        {"btn_write", KEY_WRITE}, {"btn_reply", KEY_REPLY}, {"btn_reply_all", KEY_REPLY_ALL}, {"btn_del", KEY_DEL}
    };
    for (size_t i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
        GtkWidget *obj = GTK_WIDGET(gtk_builder_get_object(builder, shortcuts[i].id));
        if (obj) {
            g_object_set_data(G_OBJECT(obj), "key-to-send", (gpointer)shortcuts[i].key);
            g_signal_connect(obj, "clicked", G_CALLBACK(on_action_clicked), ctx);
        }
    }

    // CHANGEMENT 3 : On active visuellement l'Inbox en utilisant le tableau, sans le builder
    if (ctx->folder_buttons[0]) {
        update_active_folder_ui(ctx->folder_buttons[0], ctx);
    }

    /* --- BOUTONS SPÉCIAUX --- */
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
        g_printerr(ERR_BUILDER);
        g_object_unref(builder);
        return 1;
    }

    /** Libération du builder MAINTENANT
    * L'interface GTK reste en vie
    * car ctx contient les pointeurs directs. */
    g_object_unref(builder);
    
    // 6. Boucle principale
    gtk_main();


    return 0;
}
