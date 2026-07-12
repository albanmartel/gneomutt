#include <dirent.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <webkit2/webkit2.h>
// 2. Parser le fichier headers.json (Exemple simplifié ou via GJson)
#include <json-glib/json-glib.h>

/* --- CONFIGURATION --- */
#define PROGRAMME_NAME "eml_to_html_reply"
#define PATH_EDITOR "/tmp/mutt_render/editor.html"
#define PATH_INDEX_HTML "/tmp/mutt_render/index.html"
#define PATH_HEADERS "/tmp/mutt_render/headers.json"
#define ASSETS_DIR "/tmp/mutt_render/eml_assets"
#define PATH_DRAFT "/tmp/mutt_render/draft.eml"

typedef struct {
  char *from;
  char *subject;
  char *msg_id;
  GList *attachments; // Liste de chaînes (chemins absolus des PJs)
} EmailContext;

WebKitWebView *web_view;
EmailContext email_ctx;

// 1. Charger les pièces jointes existantes depuis le dossier assets
void charger_pieces_jointes(EmailContext *ctx) {
  // Vérification de sécurité pour éviter un crash si le contexte est invalide
  if (!ctx)
    return;

  ctx->attachments = NULL;
  DIR *d = opendir(ASSETS_DIR);
  if (!d)
    return;

  struct dirent *dir;
  while ((dir = readdir(d)) != NULL) {
    if (dir->d_name[0] != '.') { // Ignorer . et ..
      char *full_path = g_strdup_printf("%s/%s", ASSETS_DIR, dir->d_name);
      ctx->attachments = g_list_append(ctx->attachments, full_path);
    }
  }
  closedir(d);
}

// 2. Parser le fichier headers.json dynamiquement
void charger_headers(EmailContext *ctx) {
  // Vérification de sécurité pour éviter un crash si le contexte est invalide
  if (!ctx)
    return;

  ctx->from = NULL;
  ctx->subject = NULL;
  ctx->msg_id = NULL;

  GError *error = NULL;
  JsonParser *parser = json_parser_new();

  // Charger le fichier JSON
  if (!json_parser_load_from_file(parser, PATH_HEADERS, &error)) {
    g_printerr("Erreur lors du parsing de headers.json : %s\n", error->message);
    g_clear_error(&error);
    g_object_unref(parser);

    // Valeurs par défaut si le fichier échoue
    ctx->from = g_strdup("");
    ctx->subject = g_strdup("Sans titre");
    return;
  }

  // Récupérer l'objet racine
  JsonNode *root = json_parser_get_root(parser);
  if (JSON_NODE_HOLDS_OBJECT(root)) {
    JsonObject *obj = json_node_get_object(root);

    // Extraction dynamique des champs
    if (json_object_has_member(obj, "From")) {
      ctx->from = g_strdup(json_object_get_string_member(obj, "From"));
    }
    if (json_object_has_member(obj, "Subject")) {
      ctx->subject = g_strdup(json_object_get_string_member(obj, "Subject"));
    }
    if (json_object_has_member(obj, "Message-ID")) {
      ctx->msg_id = g_strdup(json_object_get_string_member(obj, "Message-ID"));
    }
  }

  // Libérer le parser
  g_object_unref(parser);

  // Sécurité si les clés n'existaient pas dans le JSON
  if (!ctx->from)
    ctx->from = g_strdup("");
  if (!ctx->subject)
    ctx->subject = g_strdup("");
}

// 3. Fonction pour échapper le HTML pour une chaîne de caractères JavaScript
char *echapper_html_pour_js(const char *chemin) {
  // Sécurité : si le chemin est NULL, on s'arrête tout de suite
  if (!chemin)
    return NULL;

  char *contenu = NULL;
  gsize longueur;
  if (!g_file_get_contents(chemin, &contenu, &longueur, NULL))
    return NULL;

  GString *escaped = g_string_new("");
  for (size_t i = 0; i < longueur; i++) {
    char c = contenu[i];
    if (c == '\\')
      g_string_append(escaped, "\\\\");
    else if (c == '\'')
      g_string_append(escaped, "\\'");
    else if (c == '"')
      g_string_append(escaped, "\\\"");
    else if (c == '\n')
      g_string_append(escaped, "\\n");
    else if (c == '\r') {
    } else
      g_string_append_c(escaped, c);
  }
  g_free(contenu);
  return g_string_free(escaped, FALSE);
}

// 5. Callback de réception du HTML final depuis l'éditeur Quill
void on_javascript_finished(GObject *object, GAsyncResult *result,
                            gpointer user_data) {
  // Supprime le warning proprement
  (void)user_data;

  // Sécurité : on vérifie que l'objet émetteur et le résultat asynchrone sont
  // valides
  if (!object || !result) {
    g_printerr(
        "Erreur : on_javascript_finished appelé avec des pointeurs NULL.\n");
    return;
  }

  GError *error = NULL;
  JSCValue *value = webkit_web_view_evaluate_javascript_finish(
      WEBKIT_WEB_VIEW(object), result, &error);

  if (error) {
    g_printerr("Erreur JavaScript : %s\n", error->message);
    g_error_free(error);
    return;
  }

  if (!value)
    return;

  // Récupération directe de la chaîne de caractères HTML
  char *html_final = jsc_value_to_string(value);
  g_object_unref(value); // Libère la JSCValue

  // 6. Écriture du fichier Draft pour NeoMutt
  FILE *f = fopen(PATH_DRAFT, "w");
  if (f) {
    fprintf(f, "To: %s\n", email_ctx.from);
    fprintf(f, "Subject: Re: %s\n", email_ctx.subject);
    if (email_ctx.msg_id) {
      fprintf(f, "In-Reply-To: %s\n", email_ctx.msg_id);
    }
    fprintf(f, "MIME-Version: 1.0\n");
    fprintf(f, "Content-Type: text/html; charset=utf-8\n");

    for (GList *l = email_ctx.attachments; l != NULL; l = l->next) {
      fprintf(f, "Attach: %s\n", (char *)l->data);
    }

    fprintf(f, "\n");
    fprintf(f, "%s\n", html_final);
    fclose(f);

    g_print("Brouillon sauvegardé dans %s. Prêt pour NeoMutt.\n", PATH_DRAFT);
    gtk_main_quit();
  }

  g_free(html_final);
}

// Clic sur le bouton GTK "Envoyer"
void on_bouton_envoyer_clicked(GtkButton *bouton, gpointer user_data) {
  (void)bouton;
  (void)user_data;

  webkit_web_view_evaluate_javascript(web_view, "getMailContent();", -1, NULL,
                                      NULL, NULL, on_javascript_finished, NULL);
}

// Callback quand la page editor.html est complètement chargée
void on_page_load_changed(WebKitWebView *view_signal, WebKitLoadEvent event,
                          gpointer user_data) {
  (void)user_data;

  // Sécurité : Vérification du pointeur de la WebKitWebView émettrice
  if (!view_signal) {
    g_printerr("Erreur : on_page_load_changed appelé avec un pointeur WebView "
               "NULL.\n");
    return;
  }

  // Optionnel : s'assurer que la valeur de l'event est valide dans l'enum
  if (event < WEBKIT_LOAD_STARTED || event > WEBKIT_LOAD_FINISHED) {
    return;
  }

  if (event == WEBKIT_LOAD_FINISHED) {
    char *html_origine = echapper_html_pour_js(PATH_INDEX_HTML);
    if (html_origine) {
      char *js_cmd = g_strdup_printf(
          "setMailContent('<blockquote>%s</blockquote>');", html_origine);

      // On utilise view_signal reçu par le callback plutôt que la variable
      // globale web_view
      webkit_web_view_evaluate_javascript(view_signal, js_cmd, -1, NULL, NULL,
                                          NULL, NULL, NULL);

      g_free(html_origine);
      g_free(js_cmd);
    }
  }
}

int main(int argc, char *argv[]) {
  // 1. Nommer le processus (utile pour 'top' ou 'ps')
  prctl(PR_SET_NAME, PROGRAMME_NAME, 0, 0, 0);

  gtk_init(&argc, &argv);

  charger_headers(&email_ctx);
  charger_pieces_jointes(&email_ctx);

  // Fenêtre principale GTK
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 700);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

  // Layout Vertical (Barre d'outils / Pièces jointes en haut, WebKit en
  // dessous)
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  // Bouton Envoyer en GTK
  GtkWidget *btn_envoyer =
      gtk_button_new_with_label("Enregistrer le brouillon pour NeoMutt");
  g_signal_connect(btn_envoyer, "clicked",
                   G_CALLBACK(on_bouton_envoyer_clicked), NULL);
  gtk_box_pack_start(GTK_BOX(vbox), btn_envoyer, FALSE, FALSE, 0);

  // Label pour afficher le nombre de pièces jointes capturées
  char *lbl_text = g_strdup_printf("Pièces jointes détectées (%d)",
                                   g_list_length(email_ctx.attachments));
  GtkWidget *lbl_pj = gtk_label_new(lbl_text);
  gtk_box_pack_start(GTK_BOX(vbox), lbl_pj, FALSE, FALSE, 0);
  g_free(lbl_text);

  // Initialisation WebKitGTK
  web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(web_view), TRUE, TRUE, 0);
  g_signal_connect(web_view, "load-changed", G_CALLBACK(on_page_load_changed),
                   NULL);

  // Charger le fichier editor.html local
  char *uri = g_strdup_printf("file://%s", PATH_EDITOR);
  webkit_web_view_load_uri(web_view, uri);
  g_free(uri);

  gtk_widget_show_all(window);
  gtk_main();

  return 0;
}
