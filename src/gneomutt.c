#include <dirent.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <vte/vte.h>
#include <webkit2/webkit2.h>

#ifdef DEBUG
#define DEBUG_LOG(fmt, ...) g_print("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...)
#endif

/* --- CONFIGURATION --- */
#define PROGRAMME_NAME "gneomutt"
#define UI_FILE "interface.ui"
#define CMD_NEOMUTT "/usr/bin/neomutt"
#define CMD_EMLTOMAIL "/usr/local/bin/eml_to_html"
#define CMD_SYNC "mbsync -a && notmuch new &"
#define LOCAL_EML "/tmp/mail.eml"
#define LOCAL_HTML "/tmp/mutt_render/index.html"
#define LOCAL_ASSETS "/tmp/mutt_render/eml_assets/"

#define MACRO_INBOX "gi"
#define MACRO_SENT "go"
#define MACRO_QUAR "gq"
#define MACRO_DRAFT "gd"
#define MACRO_ARCHIVES "ga"
#define MACRO_TRASH "gt"
#define MACRO_LOCALE "gl"
#define MACRO_EMPTY "S"

#define KEY_DEL "d"
#define KEY_RETURN "i"
#define KEY_NEXT "j"
#define KEY_PREV "k"
#define KEY_WRITE "m"
#define KEY_REPLY "r"
#define KEY_REPLY_ALL "g"
#define KEY_VIEW "X"

/* --- Macros d'impression PDF ---*/
#define GTK_PRINT_KEY_PRINT_BACKEND "print-backend"
#define GTK_PRINT_KEY_OUTPUT_URI "output-uri"
#define GTK_PRINT_KEY_OUTPUT_FILE_FORMAT "output-file-format"

/*--- Taille tableau des pointeurs de dossiers ---*/
#define NB_FOLDERS 8

#define MSG_VIEW "Clic détecté ! Tentative de passage à la page HTML...\n"

#define ERR_SYNC "Erreur sync"
#define ERR_BUILDER "Erreur fatale : impossible d'initialiser l'interface.\n"
#define ERR_INTERFACE "Erreur chargement interface : %s\n"
#define ERR_WEB8WIN "ERREUR : 'web_container' introuvable dans interface.ui\n"

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
  GtkStack *main_stack;
  GtkWidget *web_view;
  WebKitSettings *web_settings;
  GtkWidget *context_menu;
  gboolean html_generation_in_progress;
  GFileMonitor *file_monitor;
  gulong terminal_child_exited_signal_id;
  GPid terminal_pid;
  GFile *src_file;
  GFile *dest_file;
} AppContext;

/* --- CONFIGURATION DES TOUCHES (Arrow Keys Mapping) --- */
typedef struct {
  guint keyval;        // La touche pressée (ex: GDK_KEY_Left)
  const char *command; // La commande envoyée à NeoMutt (ex: "k")
} KeyMapping;

static const KeyMapping arrow_map[] = {
    {GDK_KEY_Left, "k"},  // Left Arrow
    {GDK_KEY_Right, "j"}, // Right Arrow
    {GDK_KEY_Up, "-"},    // Up Arrow
    {GDK_KEY_Down, " "}   // Down Arrow (Space)
};

/* Définition des boutons de navigations (barre du haut) */
struct {
  const char *id;
  const char *key;
} shortcuts[] = {
    {"btn_return", KEY_RETURN},       {"btn_prev", KEY_PREV},
    {"btn_next", KEY_NEXT},           {"btn_enter", "\n"},
    {"btn_write", KEY_WRITE},         {"btn_reply", KEY_REPLY},
    {"btn_reply_all", KEY_REPLY_ALL}, {"btn_del", KEY_DEL},
    {"btn_view", KEY_VIEW},
};
;

/* Définition des boutons de boîtes (colonne de gauche) */
struct {
  const char *id;
  const char *macro;
} folders[] = {
    {"btn_inbox", MACRO_INBOX},       {"btn_sent", MACRO_SENT},
    {"btn_locale", MACRO_LOCALE},     {"btn_trash", MACRO_TRASH},
    {"btn_draft", MACRO_DRAFT},       {"btn_quarantine", MACRO_QUAR},
    {"btn_archives", MACRO_ARCHIVES}, {"btn_empty_trash", MACRO_EMPTY}};

/* La définition des boutons navigation/outils "special" un peu partout */
/* Impossible de mettre la définition ici avant celle des callbacks*/
/* Mise en commentaire pour augmenter la lisibilité de attribution des boutons*/
/*
  struct {
    const char *id;
    GCallback cb;
  } special[] = {{"btn_help", G_CALLBACK(on_help_clicked)},
                 {"btn_stop", G_CALLBACK(on_stop_clicked)},
                 {"btn_sync", G_CALLBACK(on_refresh_clicked)}};
*/

/* --- UTILITAIRES --- */
void send_term_data(GtkWidget *terminal, const char *data) {
  if (!terminal)
    return;
  vte_terminal_feed_child(VTE_TERMINAL(terminal), data, -1);
}

void update_active_folder_ui(GtkWidget *active_button, AppContext *ctx) {
  if (!ctx)
    return;

  // G_N_ELEMENTS calcule automatiquement le nombre de cases du tableau
  for (size_t i = 0; i < G_N_ELEMENTS(ctx->folder_buttons); i++) {
    if (ctx->folder_buttons[i]) {
      GtkStyleContext *style =
          gtk_widget_get_style_context(ctx->folder_buttons[i]);
      gtk_style_context_remove_class(style, "folder-active");
    }
  }

  if (active_button) {
    GtkStyleContext *style = gtk_widget_get_style_context(active_button);
    gtk_style_context_add_class(style, "folder-active");
  }
}

/* --- CALLBACKS --- */
// Fonction utilitaire centralisée

static void on_conversion_finished(GPid pid, gint status, gpointer user_data) {
  AppContext *ctx = (AppContext *)user_data;

  g_spawn_close_pid(pid);
  ctx->terminal_pid = 0;
  ctx->html_generation_in_progress = FALSE;

  // Grâce au weak pointer, si le stack a été détruit, ctx->main_stack VAUT
  // NULL. Pas de macro GTK_IS_STACK ici sur un pointeur potentiellement détruit
  // !
  if (ctx->main_stack == NULL) {
    DEBUG_LOG("L'interface graphique a été détruite pendant la conversion. "
              "Sortie propre.");
    return;
  }

  // Si on arrive ici, le stack EXISTE, donc ses enfants (web_view) aussi.
  if (status == 0) {
    DEBUG_LOG("Conversion réussie en arrière-plan ! Chargement de l'UI...");

    gchar *uri = g_filename_to_uri(LOCAL_HTML, NULL, NULL);
    if (uri) {
      webkit_web_view_load_uri(WEBKIT_WEB_VIEW(ctx->web_view), uri);
      g_free(uri);
    }

    gtk_stack_set_visible_child_name(ctx->main_stack, "html_page");
  } else {
    g_warning("La conversion asynchrone a échoué (Code de sortie : %d).",
              status);
  }
}

void perform_html_conversion(AppContext *ctx) {
  if (ctx->html_generation_in_progress) {
    DEBUG_LOG("Conversion déjà en cours, on patiente.");
    return;
  }

  DEBUG_LOG("Lancement asynchrone de la conversion...");

  if (!g_file_test(LOCAL_EML, G_FILE_TEST_EXISTS)) {
    g_warning("Fichier source introuvable : %s", LOCAL_EML);
    return;
  }

  gchar *dir = g_path_get_dirname(LOCAL_HTML);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);

  // Préparation des arguments pour g_spawn_async
  gchar *argv_cmd[] = {CMD_EMLTOMAIL, LOCAL_EML, LOCAL_HTML, NULL};
  GError *error = NULL;

  // Lancement asynchrone du processus
  if (g_spawn_async(NULL, argv_cmd, NULL,
                    G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                    &(ctx->terminal_pid), &error)) {

    ctx->html_generation_in_progress = TRUE;

    // On surveille la fin du processus de manière non-bloquante
    g_child_watch_add(ctx->terminal_pid, on_conversion_finished, ctx);

    DEBUG_LOG("Processus lancé avec le PID %d. L'interface GTK reste fluide !",
              ctx->terminal_pid);

    // Optionnel : Vous pouvez afficher un spinner de chargement ici dans votre
    // stack gtk_stack_set_visible_child_name(ctx->main_stack, "loading_page");

  } else {
    g_warning("Échec du lancement asynchrone : %s", error->message);
    g_error_free(error);
  }
}

// Vos callbacks deviennent alors très simples
void on_terminal_child_exited(VteTerminal *t, int s, gpointer d) {
  // 1. Vérification de base : `d` (data) n'est pas NULL
  if (d == NULL) {
    g_warning("Callback on_terminal_child_exited appelé avec un pointeur de "
              "données NULL.");
    gtk_main_quit();
    return;
  }

  // 2. Cast sécurisé avec vérification du type (si AppContext est un GObject)
  AppContext *ctx = (AppContext *)d;

  // 3. Vérification que `ctx` est valide (optionnel : utiliser une magie ou un
  // canary)
  if (ctx == NULL) {
    g_warning("Contexte AppContext NULL dans on_terminal_child_exited.");
    gtk_main_quit();
    return;
  }

  // 4. Vérification que `ctx->html_generation_in_progress` est accessible
  //    (évite les accès à une mémoire non initialisée)
  if (!ctx->html_generation_in_progress) {
    // 5. Vérification supplémentaire : si le terminal est toujours valide
    if (t != NULL && GTK_IS_WIDGET(t)) {
      g_debug("Terminal %p a terminé avec le statut %d. Quittons proprement.",
              (gpointer)t, s);
    } else {
      g_warning("Terminal invalide dans on_terminal_child_exited.");
    }
    gtk_main_quit();
    return;
  }

  // 6. Vérification que `perform_html_conversion` peut être appelé en toute
  // sécurité
  g_debug("Début de la conversion HTML pour le contexte %p.", (gpointer)ctx);
  perform_html_conversion(ctx);
  ctx->html_generation_in_progress = FALSE;
  g_debug("Conversion HTML terminée pour le contexte %p.", (gpointer)ctx);
}

void on_help_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  AppContext *ctx = (AppContext *)user_data;
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Aide", GTK_WINDOW(ctx->window),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "_Fermer",
      GTK_RESPONSE_CLOSE, NULL);

  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *text_view = gtk_text_view_new();
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view)),
                           HELP_TEXT, -1);

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

  // 2. Si NeoMutt ne se ferme pas (ex: bloqué), on force la fermeture de la
  // fenêtre Cela déclenchera la destruction du terminal et l'arrêt du programme
  gtk_window_close(GTK_WINDOW(ctx->window));
}

gboolean on_key_press(GtkWidget *widget, GdkEventKey *event,
                      gpointer user_data) {
  (void)widget;
  AppContext *ctx = (AppContext *)user_data;

  /*--- Détection de Ctrl + Shift + C (Copier) --*/
  if ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_SHIFT_MASK) &&
      (event->keyval == GDK_KEY_C || event->keyval == GDK_KEY_C)) {

    DEBUG_LOG("Copie du texte sélectionné vers le presse-papier");
    vte_terminal_copy_clipboard_format(VTE_TERMINAL(ctx->terminal),
                                       VTE_FORMAT_TEXT);
    return TRUE; // On arrête la propagation de l'événement
  }

  /*--- Détection de Ctrl + Shift + V (Coller - Optionnel mais pratique) --*/
  if ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_SHIFT_MASK) &&
      (event->keyval == GDK_KEY_V || event->keyval == GDK_KEY_V)) {

    vte_terminal_paste_clipboard(VTE_TERMINAL(ctx->terminal));
    return TRUE;
  }

  /*--- CAS 1 si entrée clavier dans la barre de recherche ---*/
  if (gtk_widget_has_focus(ctx->search_entry)) {
    return FALSE;
  }

  /* --- SURCHARGE DES TOUCHES FLÉCHÉES --- */
  const char *cmd = NULL;

  /* Associer le tableau de configuration aux nouveaux raccourcis clavier */
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

  // Mapper F1 vers l'aide contextuelle '?'
  if (event->keyval == GDK_KEY_F1) {
    // On envoie le caractère '?' au terminal
    vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), "?", -1);

    // On force le focus pour pouvoir naviguer dans l'aide immédiatement
    gtk_widget_grab_focus(ctx->terminal);

    DEBUG_LOG("F1 pressé : Redirection vers l'aide contextuelle (?)");
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
    return TRUE; // On "consomme" l'événement pour que GTK ne l'utilise pas pour
                 // les boutons
  }

  return FALSE;
}

void on_refresh_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  (void)user_data;
  if (system(CMD_SYNC) == -1)
    g_warning(ERR_SYNC);
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
  const char *option =
      gtk_combo_box_get_active_id(GTK_COMBO_BOX(ctx->search_combo));
  const char *date_id =
      gtk_combo_box_get_active_id(GTK_COMBO_BOX(ctx->date_combo));

  // Si tout est vide, on ne fait rien
  if ((!text || strlen(text) == 0) && (g_strcmp0(date_id, "any") == 0))
    return;

  // 1. Lancement de Notmuch
  vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal),
                          "\007:exec vfolder-from-query\n", -1);

  // 2. Construction de la requête Notmuch complexe
  if (date_id && g_strcmp0(date_id, "any") != 0) {
    if (g_strcmp0(date_id, "today") == 0)
      vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), "date:today ", -1);
    else if (g_strcmp0(date_id, "week") == 0)
      vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), "date:7d.. ", -1);
    else if (g_strcmp0(date_id, "month") == 0)
      vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), "date:1m.. ", -1);
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

static void on_file_created(GFileMonitor *monitor, GFile *file,
                            GFile *other_file, GFileMonitorEvent event_type,
                            gpointer user_data) {
  // 1. Déclaration de toutes les variables au début
  g_autofree gchar *file_path = NULL;
  g_autofree gchar *other_file_path =
      NULL; // 👈 Réajouté pour éviter l'erreur de compilation
  AppContext *ctx = NULL;

  // 2. Vérifications de sécurité de base
  g_return_if_fail(user_data != NULL);
  g_return_if_fail(file != NULL);

  // 3. Récupération du contexte
  ctx = (AppContext *)user_data;

  // 4. Si le moniteur reçu n'est plus valide ou n'est plus le moniteur actif,
  // on sort
  if (!G_IS_FILE_MONITOR(monitor) || ctx->file_monitor != monitor) {
    return;
  }

  // 5. On vérifie que l'événement concerne bien le fichier de destination
  // attendu
  if (ctx->dest_file != NULL) {
    if (!g_file_equal(file, ctx->dest_file)) {
      g_debug(
          "Événement ignoré : modification sur un fichier tiers non attendu.");
      return;
    }
  }

  // 6. Extraction des chemins textuels pour les logs et les tests
  file_path = g_file_get_path(file);
  if (other_file != NULL) {
    other_file_path = g_file_get_path(other_file);
  }

  // ====================================================================
  // 🛑 LOGIQUE STANDARD DES ÉVÉNEMENTS ASYNCHRONES
  // ====================================================================

  // Cas A : Le fichier est créé vide (le système commence tout juste à
  // l'écrire) On logue l'info mais on s'arrête là pour attendre que l'écriture
  // se termine.
  if (event_type == G_FILE_MONITOR_EVENT_CREATED) {
    g_debug("Fichier cible détecté : %s. Attente de la fin de l'écriture...",
            file_path);
    return;
  }

  // Cas B : Détection d'un renommage ou remplacement atomique (Utilisation de
  // other_file)
  if (event_type == G_FILE_MONITOR_EVENT_RENAMED && other_file_path != NULL) {
    g_debug("Remplacement atomique détecté : %s a été déplacé vers %s",
            other_file_path, file_path);
  }

  // Déclencheur final : L'écriture est achevée (CHANGES_DONE_HINT), le fichier
  // a bougé (RENAMED) OU par sécurité, le fichier préexistait déjà sur le
  // disque lors du signal (g_file_test)
  if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
      event_type == G_FILE_MONITOR_EVENT_RENAMED ||
      (file_path != NULL &&
       g_file_test(file_path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))) {

    g_debug(
        "Le fichier %s est stable et disponible. Lancement de la conversion !",
        file_path);

    // 1. On lance enfin la conversion HTML !
    perform_html_conversion(ctx);

    // 2. Nettoyage complet et fermeture définitive du moniteur
    if (G_IS_FILE_MONITOR(monitor)) {
      g_signal_handlers_disconnect_by_data(monitor, ctx);
      g_file_monitor_cancel(monitor);
    }

    // 3. Désallocation sécurisée de nos pointeurs globaux pour la prochaine
    // fois
    g_clear_object(&ctx->dest_file);
    g_clear_object(&ctx->src_file);
    g_clear_object(&ctx->file_monitor);
  }
}

void on_view_html_clicked(GtkWidget *widget, gpointer user_data) {
  g_debug("Widget cliqué : %p", (gpointer)widget);

  // 1. Vérification de base : user_data n'est pas NULL
  g_return_if_fail(user_data != NULL);
  AppContext *ctx = (AppContext *)user_data;

  // 2. Vérification que ctx->terminal est un VteTerminal valide
  g_return_if_fail(ctx->terminal != NULL);
  g_return_if_fail(VTE_IS_TERMINAL(ctx->terminal));

  // 3. Envoyer la commande au terminal
  vte_terminal_feed_child(VTE_TERMINAL(ctx->terminal), KEY_VIEW, -1);

  // 4. Vérification que LOCAL_EML est un chemin valide
  if (LOCAL_EML == NULL || *LOCAL_EML == '\0') {
    g_warning("LOCAL_EML est NULL ou vide.");
    return;
  }

  // ====================================================================
  // NETTOYAGE ULTRA-SÉCURISÉ DE L'ANCIEN MONITEUR
  // Fait place nette AVANT de créer quoi que ce soit de nouveau.
  // ====================================================================
  if (ctx->file_monitor != NULL) {
    DEBUG_LOG("Annulation et nettoyage de l'ancien moniteur de fichier...");

    // On demande à GIO d'arrêter d'écouter les événements sur l'ancien
    // fichier
    g_file_monitor_cancel(ctx->file_monitor);

    // On libère proprement l'objet. Si GLib l'avait déjà détruit en interne
    // suite à la suppression du fichier, g_clear_object gère la situation
    // sans crasher.
    g_clear_object(&ctx->file_monitor);
  }

  // 5. Créer le GFile pour le nouveau mail
  GFile *file = g_file_new_for_path(LOCAL_EML);
  if (file == NULL) {
    g_warning("Impossible de créer le GFile pour le chemin : %s", LOCAL_EML);
    return;
  }

  // 6. Créer le GFileMonitor avec gestion d'erreur
  GError *error = NULL;
  GFileMonitor *monitor =
      g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, &error);
  if (monitor == NULL) {
    g_warning("Impossible de surveiller le fichier %s : %s", LOCAL_EML,
              error->message);
    g_error_free(error);
    g_object_unref(file);
    return;
  }

  ctx->src_file = g_file_new_for_path(LOCAL_EML);
  ctx->dest_file = g_file_new_for_path(LOCAL_HTML);

  // 7. Connecter le signal de manière "intelligente" et sécurisée
  // On donne un vrai GObject (le widget bouton ou ctx->main_stack) à GLib pour
  // valider la fonction, et on passe G_CONNECT_SWAPPED pour que 'ctx' soit
  // envoyé en premier au callback.
  g_signal_connect_object(
      monitor, "changed", G_CALLBACK(on_file_created),
      widget, // 👈 Un vrai GObject (le bouton cliqué) pour satisfaire GLib
      G_CONNECT_SWAPPED);

  // 8. Stockage du nouveau moniteur tout frais dans notre contexte nettoyé
  ctx->file_monitor = monitor;

  // 9. Libérer le GFile local (le moniteur a pris sa propre référence
  // interne)
  g_object_unref(file);
}

void on_refresh_viewhtml(GtkButton *G_GNUC_UNUSED button, gpointer user_data) {
  // 1. On récupère notre structure globale AppContext
  AppContext *ctx = (AppContext *)user_data;

  if (ctx && ctx->web_view) {
    DEBUG_LOG("Rafraîchir cliqué : Rechargement forcé du HTML existant.");

    // 2. On force le rechargement en ignorant le cache
    webkit_web_view_reload_bypass_cache(WEBKIT_WEB_VIEW(ctx->web_view));
  } else {
    g_warning("Impossible de rafraîchir : web_view non initialisée.");
  }
}

void vider_repertoire(const char *chemin) {
  DIR *d = opendir(chemin);
  struct dirent *p;
  char path_complet[1024];

  if (!d)
    return;

  while ((p = readdir(d)) != NULL) {
    // On ignore les dossiers spéciaux "." et ".."
    if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, ".."))
      continue;

    // On construit le chemin complet
    snprintf(path_complet, sizeof(path_complet), "%s/%s", chemin, p->d_name);

    // On supprime l'élément (cette version simple ne gère pas les
    // sous-dossiers) Pour gérer les sous-dossiers, il faudrait appeler la
    // fonction récursivement ici
    if (remove(path_complet) == 0) {
      printf("Supprime : %s\n", path_complet);
    } else {
      printf("Impossible de supprimer : %s\n", path_complet);
    }
  }
  closedir(d);
}

void on_back_clicked(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  AppContext *ctx = (AppContext *)user_data;

  // Supprimer les fichiers eml et html
  remove(LOCAL_HTML);
  remove(LOCAL_EML);
  vider_repertoire(LOCAL_ASSETS);

  // Revenir à l'interface NeoMutt
  gtk_stack_set_visible_child_name(GTK_STACK(ctx->main_stack), "neomutt_page");

  // Redonner le focus au terminal pour pouvoir continuer à utiliser le
  // clavier
  gtk_widget_grab_focus(ctx->terminal);
}

void on_print_pdf_clicked(GtkButton *G_GNUC_UNUSED button, gpointer user_data) {
  AppContext *ctx = (AppContext *)user_data;

  if (!ctx->web_view) {
    g_printerr("Erreur: web_view n'existe pas !\n");
    return;
  }

  // 1. Créer l'opération d'impression pour cette vue
  WebKitPrintOperation *print_operation =
      webkit_print_operation_new(WEBKIT_WEB_VIEW(ctx->web_view));

  // 2. Lancer la boîte de dialogue d'impression standard GTK
  // La fonction retourne un résultat (WEBKIT_PRINT_OPERATION_RESPONSE_PRINT
  // ou CANCEL)
  webkit_print_operation_run_dialog(print_operation, GTK_WINDOW(ctx->window));

  // 3. Nettoyage
  g_object_unref(print_operation);
}

void on_toggle_images_clicked(GtkWidget *button, gpointer user_data) {
  AppContext *ctx = (AppContext *)user_data;

  // 1. Récupérer l'état actuel (par défaut FALSE si non défini)
  gpointer data = g_object_get_data(G_OBJECT(button), "allow-images");
  gboolean current_state = GPOINTER_TO_INT(data);

  // 2. Inverser l'état
  gboolean new_state = !current_state;

  // 3. Sauvegarder le nouvel état pour le prochain clic
  g_object_set_data(G_OBJECT(button), "allow-images",
                    GINT_TO_POINTER(new_state));

  g_print("Action demandée : %s les images distantes\n",
          new_state ? "AUTORISER" : "BLOQUER");

  // 4. Configurer WebKit avec les permissions réseau requises
  WebKitSettings *settings =
      webkit_web_view_get_settings(WEBKIT_WEB_VIEW(ctx->web_view));

  // Règle de base : charger ou non les images
  webkit_settings_set_auto_load_images(settings, new_state);

  // CLÉ DE VOUTE : Permettre à notre fichier HTML local (/tmp/...)
  // d'accéder à des adresses HTTP/HTTPS distantes pour télécharger les
  // images.
  webkit_settings_set_allow_universal_access_from_file_urls(settings,
                                                            new_state);

  // 5. Appliquer les réglages réels
  webkit_web_view_set_settings(WEBKIT_WEB_VIEW(ctx->web_view), settings);

  // 6. Mettre à jour le texte du bouton pour l'utilisateur
  if (GTK_IS_BUTTON(button)) {
    gtk_button_set_label(GTK_BUTTON(button), new_state ? "Bloquer les images"
                                                       : "Afficher les images");
  }

  // 7. Forcer le rechargement complet en ignorant le cache de l'ancienne page
  // bloquée
  webkit_web_view_reload_bypass_cache(WEBKIT_WEB_VIEW(ctx->web_view));
}

// Fonction universelle pour COPIER le texte sélectionné du widget actif
void on_global_copy_activated(G_GNUC_UNUSED GtkWidget *widget,
                              gpointer user_data) {
  AppContext *ctx = (AppContext *)user_data;

  GtkWidget *focus_widget = gtk_window_get_focus(GTK_WINDOW(ctx->window));
  if (focus_widget != NULL) {
    g_signal_emit_by_name(focus_widget, "copy-clipboard");
  }
}

// Fonction universelle pour COLLER le texte dans le widget actif
void on_global_paste_activated(G_GNUC_UNUSED GtkWidget *widget,
                               gpointer user_data) {
  AppContext *ctx = (AppContext *)user_data;

  GtkWidget *focus_widget = gtk_window_get_focus(GTK_WINDOW(ctx->window));
  if (focus_widget != NULL) {
    g_signal_emit_by_name(focus_widget, "paste-clipboard");
  }
}

// Gestionnaire du clic droit sur la fenêtre
static gboolean on_window_button_press(G_GNUC_UNUSED GtkWidget *widget,
                                       GdkEventButton *event,
                                       gpointer user_data) {
  AppContext *ctx = (AppContext *)user_data;

  if (event->type == GDK_BUTTON_PRESS &&
      event->button == GDK_BUTTON_SECONDARY) {
    gtk_menu_popup_at_pointer(GTK_MENU(ctx->context_menu), (GdkEvent *)event);
    return TRUE;
  }
  return FALSE;
}

// Définissez un callback pour gérer la fin du processus
static void on_terminal_spawn_async_callback(VteTerminal *terminal, GPid pid,
                                             GError *error,
                                             gpointer user_data) {
  // Récupération du contexte depuis user_data
  AppContext *ctx = (AppContext *)user_data;

  // Vérification de cohérence (optionnel, pour le debug)
  g_assert(terminal == VTE_TERMINAL(ctx->terminal));

  if (error != NULL) {
    g_critical(
        "Erreur lors du lancement de Neomutt dans le terminal (PID: %d) : %s",
        pid, error->message);
    g_error_free(error);

    // Exemple : Désactiver le terminal ou afficher une erreur dans l'UI
    if (ctx && ctx->terminal) {
      // Vous pouvez par exemple désactiver le terminal ou afficher un message
      gtk_widget_set_sensitive(ctx->terminal, FALSE);
    }
  } else {
    g_message("Neomutt lancé avec succès dans le terminal (PID: %d)", pid);

    // Stocker le PID dans une variable de ctx
    ctx->terminal_pid = pid;

    // Exemple : Configurer le terminal après le lancement
    if (ctx && ctx->terminal) {
      vte_terminal_set_scroll_on_output(terminal, TRUE);
    }
  }
}

// Fonction pour vider la corbeille locale

/* --- INITIALISATION UI --- */
int init_gui(AppContext *ctx, GtkBuilder *builder) {
  GError *error = NULL;

  /* 1. Chargement du XML depuis les ressources */
  if (!gtk_builder_add_from_resource(
          builder, "/com/monprojet/icons/interface.ui", &error)) {
    g_critical("Erreur lors du chargement de l'interface : %s", error->message);
    g_error_free(error);
    return FALSE; // Utilisez FALSE pour les fonctions GLib/GTK
  }

  // Supprimer les fichiers eml et html
  // Vérifiez que les chemins sont valides
  if (LOCAL_HTML && *LOCAL_HTML != '\0') {
    if (remove(LOCAL_HTML) != 0 &&
        errno != ENOENT) { // ENOENT = fichier inexistant
      g_warning("Impossible de supprimer %s : %s", LOCAL_HTML, strerror(errno));
    }
  }
  if (LOCAL_EML && *LOCAL_EML != '\0') {
    if (remove(LOCAL_EML) != 0 && errno != ENOENT) {
      g_warning("Impossible de supprimer %s : %s", LOCAL_EML, strerror(errno));
    }
  }
  if (LOCAL_ASSETS && *LOCAL_ASSETS != '\0') {
    vider_repertoire(LOCAL_ASSETS);
  } else {
    g_warning("LOCAL_ASSETS est NULL ou vide.");
  }

  /* 2. Récupération des widgets principaux */
  ctx->window = GTK_WIDGET(gtk_builder_get_object(builder, "window"));
  ctx->terminal = GTK_WIDGET(gtk_builder_get_object(builder, "terminal"));

  // Récupération et vérification explicite de main_stack;
  GtkStack *main_stack_widget =
      GTK_STACK(gtk_builder_get_object(builder, "main_stack"));
  if (main_stack_widget && GTK_IS_STACK(main_stack_widget)) {
    ctx->main_stack = GTK_STACK(main_stack_widget);
  } else {
    g_critical("Le widget 'main_stack' n'est pas un GtkStack valide dans "
               "interface.ui !");
    return FALSE;
  }

  if (!ctx->window || !ctx->main_stack || !ctx->terminal) {
    g_critical("Un ou plusieurs widgets manquants dans interface.ui");
    return FALSE;
  }

  // Si le stack meurt, GTK mettra ctx->main_stack à NULL automatiquement.
  g_object_add_weak_pointer(G_OBJECT(ctx->main_stack),
                            (gpointer *)&(ctx->main_stack));

  /* 3. Configuration de WebKit (Injection dans le conteneur du XML) */
  GtkWidget *web_container =
      GTK_WIDGET(gtk_builder_get_object(builder, "web_container"));
  if (web_container) {
    // AJOUTEZ CES DEUX LIGNES POUR FORCER L'EXPANSION
    gtk_widget_set_vexpand(web_container, TRUE);
    gtk_widget_set_valign(web_container, GTK_ALIGN_FILL);

    ctx->web_view = webkit_web_view_new();
    if (!ctx->web_view) {
      g_critical("Impossible de créer le WebKitWebView.");
      return FALSE;
    }

    // 1. Initialiser l'objet settings AVANT de l'utiliser
    ctx->web_settings = webkit_settings_new();
    if (!ctx->web_settings) {
      g_critical("Impossible de créer les WebKitSettings.");
      g_object_unref(ctx->web_view);
      return FALSE;
    }

    // Initialisation des réglages
    webkit_settings_set_allow_file_access_from_file_urls(ctx->web_settings,
                                                         TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls(ctx->web_settings,
                                                              TRUE);

    // 2. Autoriser l'affichage des images
    webkit_settings_set_auto_load_images(ctx->web_settings, TRUE);

    // 3. Forcer l'encodage UTF-8 pour les accents
    webkit_settings_set_default_charset(ctx->web_settings, "UTF-8");

    // 4. Activer les outils de dev (clic droit -> inspecter) pour déboguer
    webkit_settings_set_enable_developer_extras(ctx->web_settings, TRUE);

    // Appliquer les réglages
    webkit_web_view_set_settings(WEBKIT_WEB_VIEW(ctx->web_view),
                                 ctx->web_settings);
    // 1. On autorise la vue à s'étendre verticalement et horizontalement
    gtk_widget_set_hexpand(ctx->web_view, TRUE);
    gtk_widget_set_vexpand(ctx->web_view, TRUE);
    // 2. On l'ajoute au conteneur
    gtk_container_add(GTK_CONTAINER(web_container), ctx->web_view);
    // 3. On peut enlever le size_request ou le mettre à une valeur minimale
    // gtk_widget_set_size_request(ctx->web_view, 100, 100);
    gtk_widget_show_all(web_container);
  } else {
    g_critical("'web_container' introuvable dans interface.ui");
    return FALSE; // Quittez la fonction si le conteneur est manquant
  }

  /* 4. Style CSS */
  GtkCssProvider *provider = gtk_css_provider_new();
  if (!provider) {
    g_critical("Impossible de créer le GtkCssProvider.");
    return FALSE;
  }

  GError *css_error = NULL;
  if (!gtk_css_provider_load_from_data(
          provider,
          ".folder-active { background-color: #3584e4; color: white; "
          "border-radius: 5px; }",
          -1, &css_error)) {
    g_critical("Erreur lors du chargement du CSS : %s", css_error->message);
    g_error_free(css_error);
    g_object_unref(provider);
    return FALSE;
  }

  gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  /* 5. Configuration et Lancement du Terminal VTE */
  if (ctx->terminal) {
    vte_terminal_set_scroll_on_output(VTE_TERMINAL(ctx->terminal), TRUE);

    // Vérifiez que CMD_NEOMUTT est valide
    if (!CMD_NEOMUTT || *CMD_NEOMUTT == '\0') {
      g_critical("CMD_NEOMUTT est NULL ou vide.");
      return FALSE;
    }

    vte_terminal_spawn_async(VTE_TERMINAL(ctx->terminal),
                             VTE_PTY_DEFAULT,               // pty_flags
                             NULL,                          // working_directory
                             (char *[]){CMD_NEOMUTT, NULL}, // argv
                             NULL,                          // envv
                             G_SPAWN_SEARCH_PATH,           // spawn_flags
                             NULL,                          // child_setup
                             NULL,                          // child_setup_data
                             NULL, // child_setup_data_destroy
                             -1,   // timeout
                             NULL, // cancellable
                             on_terminal_spawn_async_callback, // callback
                             ctx                               // user_data
    );

    // ====================================================================
    // 🛑 LE CORRECTIF : FORCER LE DÉBLOCAGE DES FLUX ET DE LA BOUCLE
    // ====================================================================
    fflush(stdout);
    fflush(stderr);

    // On laisse le temps à GTK et GIO de traiter la création du moniteur
    while (gtk_events_pending()) {
      gtk_main_iteration();
    }

    // Vérifiez que le signal est bien connecté
    gulong signal_id =
        g_signal_connect(ctx->terminal, "child-exited",
                         G_CALLBACK(on_terminal_child_exited), ctx);
    if (signal_id == 0) {
      g_critical("Impossible de connecter le signal 'child-exited'.");
      return FALSE;
    }
    // Stockez signal_id dans ctx si vous voulez le déconnecter plus tard
    ctx->terminal_child_exited_signal_id = signal_id;
  }

  /* 6. Widgets de Recherche */
  ctx->search_entry =
      GTK_WIDGET(gtk_builder_get_object(builder, "main_search_entry"));
  ctx->search_combo =
      GTK_WIDGET(gtk_builder_get_object(builder, "search_options_combo"));
  ctx->date_combo =
      GTK_WIDGET(gtk_builder_get_object(builder, "date_search_combo"));

  GtkWidget *btn_search =
      GTK_WIDGET(gtk_builder_get_object(builder, "btn_execute_search"));
  if (btn_search) {
    g_signal_connect(btn_search, "clicked", G_CALLBACK(on_search_clicked), ctx);
    g_signal_connect(ctx->search_entry, "activate",
                     G_CALLBACK(on_search_clicked), ctx);
  }

  /* 7. Gestion des Dossiers (Sidebar) */
  /* La définition des boutons "folders" est au début */
  for (size_t i = 0; i < G_N_ELEMENTS(folders); i++) {
    GtkWidget *b = GTK_WIDGET(gtk_builder_get_object(builder, folders[i].id));
    ctx->folder_buttons[i] = b;
    if (b) {
      g_object_set_data(G_OBJECT(b), "ctx", ctx);
      g_signal_connect(b, "clicked", G_CALLBACK(on_folder_clicked),
                       (gpointer)folders[i].macro);
    }
  }

  /* 8. Boutons d'Action (Raccourcis clavier) */
  /* La définition des boutons "shortcuts" est au début */
  for (size_t i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
    GtkWidget *obj =
        GTK_WIDGET(gtk_builder_get_object(builder, shortcuts[i].id));
    if (obj) {
      g_object_set_data(G_OBJECT(obj), "key-to-send",
                        (gpointer)shortcuts[i].key);
      g_signal_connect(obj, "clicked", G_CALLBACK(on_action_clicked), ctx);
    }
  }

  /* 9. Boutons de navigation/outils */
  /* La définition des boutons "special" un peu partout */
  struct {
    const char *id;
    GCallback cb;
  } special[] = {{"btn_help", G_CALLBACK(on_help_clicked)},
                 {"btn_stop", G_CALLBACK(on_stop_clicked)},
                 {"btn_sync", G_CALLBACK(on_refresh_clicked)}};
  for (size_t i = 0; i < G_N_ELEMENTS(special); i++) {
    GtkWidget *btn = GTK_WIDGET(gtk_builder_get_object(builder, special[i].id));
    if (btn)
      g_signal_connect(btn, "clicked", special[i].cb, ctx);
  }

  /*--- RACCOURCIS CLAVIER GLOBAUX (À l'échelle de la fenêtre) ---*/
  GtkAccelGroup *accel_group = gtk_accel_group_new();
  gtk_window_add_accel_group(GTK_WINDOW(ctx->window), accel_group);

  // Associer Ctrl+C à notre fonction de copie globale
  gtk_accel_group_connect(
      accel_group, GDK_KEY_c, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE,
      g_cclosure_new(G_CALLBACK(on_global_copy_activated), ctx, NULL));

  // Associer Ctrl+V à notre fonction de collage globale
  gtk_accel_group_connect(
      accel_group, GDK_KEY_v, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE,
      g_cclosure_new(G_CALLBACK(on_global_paste_activated), ctx, NULL));

  /*--- CRÉATION DU MENU CONTEXTUEL UNIQUE --*/
  ctx->context_menu = gtk_menu_new();

  GtkWidget *menu_item_copy = gtk_menu_item_new_with_label("Copier");
  g_signal_connect(menu_item_copy, "activate",
                   G_CALLBACK(on_global_copy_activated), ctx);
  gtk_menu_shell_append(GTK_MENU_SHELL(ctx->context_menu), menu_item_copy);

  GtkWidget *menu_item_paste = gtk_menu_item_new_with_label("Coller");
  g_signal_connect(menu_item_paste, "activate",
                   G_CALLBACK(on_global_paste_activated), ctx);
  gtk_menu_shell_append(GTK_MENU_SHELL(ctx->context_menu), menu_item_paste);

  // Rendre les éléments du menu visibles
  gtk_widget_show_all(ctx->context_menu);

  /*--- CAPTURER LE CLIC DROIT PARTOUT DANS LA FENÊTRE --*/
  /*--- On autorise la fenêtre à intercepter les clics de souris ---*/
  gtk_widget_add_events(ctx->window, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(ctx->window, "button-press-event",
                   G_CALLBACK(on_window_button_press), ctx);

  /* --- NOUVEAU : Gestion des images distantes --- */
  struct {
    const char *id;
    gboolean allow;
  } img_btns[] = {{"btn_img_on", TRUE}, {"btn_img_off", FALSE}};

  for (size_t i = 0; i < G_N_ELEMENTS(img_btns); i++) {
    GtkWidget *b = GTK_WIDGET(gtk_builder_get_object(builder, img_btns[i].id));
    if (b) {
      // On stocke la valeur TRUE/FALSE directement dans l'objet bouton
      g_object_set_data(G_OBJECT(b), "allow-images",
                        GINT_TO_POINTER(img_btns[i].allow));
      g_signal_connect(b, "clicked", G_CALLBACK(on_toggle_images_clicked), ctx);
    }
  }

  /* --- connexion manuelle du signal */
  GtkWidget *btn_view = GTK_WIDGET(gtk_builder_get_object(builder, "btn_view"));
  if (btn_view) {
    g_signal_connect(btn_view, "clicked", G_CALLBACK(on_view_html_clicked),
                     ctx);
  } else {
    g_critical("Impossible de lier le callback : le bouton 'btn_view' "
               "n'existe pas !");
  }

  GtkWidget *btn_img = GTK_WIDGET(gtk_builder_get_object(builder, "btn_img"));
  if (btn_img) {
    g_signal_connect(btn_img, "clicked", G_CALLBACK(on_toggle_images_clicked),
                     ctx);
  } else {
    g_critical("Impossible de lier le callback : le bouton 'btn_img' "
               "n'existe pas !");
  }

  GtkWidget *btn_pdf = GTK_WIDGET(gtk_builder_get_object(builder, "btn_pdf"));
  if (btn_pdf) {
    g_signal_connect(btn_pdf, "clicked", G_CALLBACK(on_print_pdf_clicked), ctx);
  } else {
    g_critical("Impossible de lier le callback : le bouton 'btn_pdf' "
               "n'existe pas !");
  }

  GtkWidget *btn_back = GTK_WIDGET(gtk_builder_get_object(builder, "btn_back"));
  if (btn_back) {
    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_clicked), ctx);
  } else {
    g_critical("Impossible de lier le callback : le bouton 'btn_back' "
               "n'existe pas !");
  }

  GtkWidget *btn_refresh =
      GTK_WIDGET(gtk_builder_get_object(builder, "btn_refresh"));
  if (btn_refresh) {
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_viewhtml),
                     ctx);
  } else {
    g_critical("Impossible de lier le callback : le bouton 'btn_refresh' "
               "n'existe pas !");
  }

  /* 10. Finalisation et Affichage */
  if (ctx->window) {
    g_signal_connect(ctx->window, "key-press-event", G_CALLBACK(on_key_press),
                     ctx);
    gtk_widget_show_all(ctx->window);
    gtk_window_maximize(GTK_WINDOW(ctx->window));
  }

  if (ctx->terminal) {
    gtk_widget_grab_focus(ctx->terminal);
  }

  return 1;
}

/* --- MAIN --- */
int main(int argc, char *argv[]) {
  // 1. Nommer le processus (utile pour 'top' ou 'ps')
  prctl(PR_SET_NAME, PROGRAMME_NAME, 0, 0, 0);

  // 2. Initialisation de GTK
  gtk_init(&argc, &argv);

  // 3. Initialisation du contexte
  // Il est conseillé de mettre la structure à zéro pour éviter les pointeurs
  // sauvages
  /*AppContext ctx;
  memset(&ctx, 0, sizeof(AppContext)); */

  AppContext *ctx = g_new0(AppContext, 1);

  // 4. Création du Builder
  GtkBuilder *builder = gtk_builder_new();

  // 5. Chargement de l'interface
  if (!init_gui(ctx, builder)) {
    g_printerr(ERR_BUILDER);
    g_object_unref(builder);
    g_free(ctx);
    return 1;
  }

  /** Libération du builder MAINTENANT
   * L'interface GTK reste en vie
   * car ctx contient les pointeurs directs. */
  g_object_unref(builder);

  // 6. Boucle principale
  gtk_main();

  // Nettoyage final avant de quitter
  g_free(ctx);

  return 0;
}
