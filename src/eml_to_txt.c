#include <gio/gio.h> // Requis pour g_input_stream et g_output_stream
#include <glib.h>
#include <gmime/gmime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h> // Requis pour waitpid
#include <unistd.h>   // Requis pour close()

#define PROGRAMME_NAME "eml_to_txt"

// Structure de contexte passée à GMime pour accumuler les textes (bruts et
// HTML) lors du scan des sections du mail
typedef struct {
  GString *text_content;
  GString *html_content;
} TextParserContext;

// Extraction et décodage propre des en-têtes textuels (Sujet, From, etc.)
static gchar *get_clean_header_value(GMimeMessage *message,
                                     const char *header_name) {
  const char *value =
      g_mime_object_get_header(GMIME_OBJECT(message), header_name);
  if (!value)
    return g_strdup("");

  // Décodage des fragments encodés selon la RFC 2047 (Ex: =?utf-8?Q?...)
  gchar *decoded = g_mime_utils_header_decode_text(NULL, value);
  if (!decoded)
    return g_strdup(value);

  return g_strdup(g_strstrip(decoded));
}

// Traduction de la date du courriel en français, indépendamment des locales du
// système hôte
static gchar *parse_email_date_to_french(GMimeMessage *message) {
  GDateTime *gdt = g_mime_message_get_date(message);
  if (!gdt)
    return g_strdup("");

  const char *jours_fr[] = {"dimanche", "lundi",    "mardi", "mercredi",
                            "jeudi",    "vendredi", "samedi"};
  const char *mois_fr[] = {"",        "janvier",   "février", "mars",
                           "avril",   "mai",       "juin",    "juillet",
                           "août",    "septembre", "octobre", "novembre",
                           "décembre"};

  GDateWeekday wd = g_date_time_get_day_of_week(gdt);
  int day_idx =
      (wd == 7) ? 0
                : wd; // Ajustement si le dimanche est indexé à 7 par la GLib
  int month_idx = g_date_time_get_month(gdt);

  return g_strdup_printf("%s %02d %s %04d à %02d:%02d", jours_fr[day_idx],
                         g_date_time_get_day_of_month(gdt), mois_fr[month_idx],
                         g_date_time_get_year(gdt), g_date_time_get_hour(gdt),
                         g_date_time_get_minute(gdt));
}

// Génération du bloc textuel regroupant les en-têtes principaux (Date, De, À,
// Cc)
static gchar *generate_headers_text(GMimeMessage *message) {
  GString *sb = g_string_new("");
  gchar *french_date = parse_email_date_to_french(message);

  // Tableau de structures temporaire pour boucler proprement sur les en-têtes
  // essentiels
  struct {
    const char *label;
    gchar *value;
  } headers[] = {{"Date ", french_date},
                 {"De   ", get_clean_header_value(message, "From")},
                 {"À    ", get_clean_header_value(message, "To")},
                 {"Cc   ", get_clean_header_value(message, "Cc")}};

  for (size_t i = 0; i < 4; i++) {
    if (headers[i].value && strlen(headers[i].value) > 0 &&
        g_ascii_strcasecmp(headers[i].value, "none") != 0) {
      g_string_append_printf(sb, "%s: %s\n", headers[i].label,
                             headers[i].value);
    }
    g_free(headers[i].value); // Libération de la mémoire allouée par g_strdup
  }
  return g_string_free(sb, FALSE);
}

// Fonction de rappel (callback) récursive pour extraire le texte ou le HTML
// (hors pièces jointes nommées)
static void process_text_part(G_GNUC_UNUSED GMimeObject *parent,
                              GMimeObject *part, gpointer user_data) {
  TextParserContext *ctx = (TextParserContext *)user_data;

  if (!GMIME_IS_PART(part))
    return;

  GMimePart *mime_part = GMIME_PART(part);
  GMimeContentType *content_type = g_mime_object_get_content_type(part);
  const char *filename = g_mime_part_get_filename(mime_part);

  // On ignore le traitement si la section MIME actuelle possède un nom de
  // fichier (c'est une vraie pièce jointe)
  if (filename == NULL) {
    GMimeDataWrapper *content = g_mime_part_get_content(mime_part);
    if (content) {
      // Extraction du flux de données de la section vers la mémoire RAM
      GMimeStream *mem_stream = g_mime_stream_mem_new();
      g_mime_data_wrapper_write_to_stream(content, mem_stream);
      g_mime_stream_seek(mem_stream, 0, GMIME_STREAM_SEEK_SET);

      GByteArray *bytes =
          g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(mem_stream));
      gchar *text = g_strndup((const gchar *)bytes->data, bytes->len);

      // Tri et accumulation du contenu textuel selon son type MIME réel
      if (g_mime_content_type_is_type(content_type, "text", "plain")) {
        g_string_append(ctx->text_content, text);
      } else if (g_mime_content_type_is_type(content_type, "text", "html")) {
        g_string_append(ctx->html_content, text);
      }
      g_free(text);
      g_object_unref(mem_stream);
    }
  }
}

// MOTEUR DE CONVERSION : Utilisation du rendu de w3m pour un affichage terminal
// parfait
static gchar *strip_html_tags(const gchar *html_or_path) {
  if (!html_or_path || strlen(html_or_path) == 0)
    return g_strdup("");

  gchar *w3m_result = NULL;
  GError *error = NULL;
  gint exit_status;
  gchar *command = NULL;

  // Détection : Si la chaîne reçue commence par "<", c'est du code HTML brut
  // (venant de GMime)
  if (g_str_has_prefix(g_strstrip((gchar *)html_or_path), "<")) {
    gchar *tmp_path = NULL;
    gint tmp_fd = g_file_open_tmp("eml_to_txt_XXXXXX.html", &tmp_path, NULL);
    if (tmp_fd != -1) {
      write(tmp_fd, html_or_path, strlen(html_or_path));
      close(tmp_fd);

      // -T text/html : force le type
      // -dump : affiche le rendu textuel formaté sur stdout
      // -cols 80 : calibre le rendu sur 80 colonnes standard pour NeoMutt
      command =
          g_strdup_printf("w3m -T text/html -dump -cols 80 \"%s\"", tmp_path);
      g_spawn_command_line_sync(command, &w3m_result, NULL, &exit_status,
                                &error);
      unlink(tmp_path);
      g_free(tmp_path);
      g_free(command);
    }
  } else {
    // Cas NeoMutt direct : w3m lit directement le fichier temporaire de Mutt
    command =
        g_strdup_printf("w3m -T text/html -dump -cols 80 \"%s\"", html_or_path);
    g_spawn_command_line_sync(command, &w3m_result, NULL, &exit_status, &error);
    g_free(command);
  }

  if (!error && exit_status == 0 && w3m_result != NULL) {
    // Nettoyage des sauts de lignes multiples pour garder un affichage compact
    GRegex *regex_newlines = g_regex_new("\n{3,}", G_REGEX_MULTILINE, 0, NULL);
    gchar *final_text = g_regex_replace_literal(regex_newlines, w3m_result, -1,
                                                0, "\n\n", 0, NULL);
    g_regex_unref(regex_newlines);
    g_free(w3m_result);

    return g_strstrip(final_text);
  }

  if (error) {
    g_printerr("Erreur d'exécution de w3m : %s\n", error->message);
    g_error_free(error);
  }

  return g_strdup("(Erreur lors du rendu HTML via w3m)");
}
// Fonction maîtresse de traitement du fichier
void eml_to_txt(const char *eml_path, const char *output_txt_path) {
  // Modification du nom de thread pour faciliter le debug système
  prctl(PR_SET_NAME, PROGRAMME_NAME, 0, 0, 0);

  gchar *full_txt_result = NULL;
  gchar *file_content = NULL;
  gsize file_length = 0;

  // ÉTAPE 1 : Lecture brute de l'intégralité du fichier d'entrée
  if (!g_file_get_contents(eml_path, &file_content, &file_length, NULL)) {
    g_printerr("Erreur : Impossible de lire le fichier source %s\n", eml_path);
    return;
  }

  // ÉTAPE 2 : Détection adaptative du format d'entrée (Cas NeoMutt Mailcap vs
  // Fichier EML entier) Si le fichier s'ouvre directement par une balise '<' ou
  // qu'il ne comporte aucune mention de version MIME, c'est que NeoMutt a déjà
  // isolé la pièce jointe HTML brute et nous la pousse via son pager.
  gboolean is_pure_html = FALSE;
  if (g_str_has_prefix(g_strstrip(file_content), "<") ||
      strstr(file_content, "MIME-Version:") == NULL) {
    is_pure_html = TRUE;
  }

  if (is_pure_html) {
    // --- LOGIQUE A : CAS MUTT / MAILCAP ---
    // Le fichier est déjà du HTML pur. On le passe directement à Pandoc pour
    // obtenir du Markdown.
    gchar *body = strip_html_tags(file_content);
    full_txt_result = g_strdup_printf("%s\n", body);
    g_free(body);
  } else {
    // --- LOGIQUE B : CAS STANDARD (FICHIER .EML ENTIER) ---
    // Utilisation de la bibliothèque GMime pour analyser la structure du mail
    // complet
    GMimeStream *stream = g_mime_stream_file_open(eml_path, "r", NULL);
    if (stream) {
      GMimeParser *parser = g_mime_parser_new_with_stream(stream);
      GMimeMessage *message = g_mime_parser_construct_message(parser, NULL);

      if (message) {
        gchar *headers_txt = generate_headers_text(message);
        const char *subject = g_mime_message_get_subject(message);
        if (!subject)
          subject = "Sans titre";

        TextParserContext ctx;
        ctx.text_content = g_string_new("");
        ctx.html_content = g_string_new("");

        // Scan récursif de toutes les sous-sections du courriel
        g_mime_message_foreach(message, process_text_part, &ctx);

        // ARBITRAGE INTELLIGENT DE RENDU :
        gchar *body = NULL;
        if (ctx.text_content->len > 0) {
          // S'il y a du texte brut natif fourni par l'expéditeur, on le
          // privilégie (rapidité absolue)
          body = g_strdup(g_strstrip(ctx.text_content->str));
        } else if (ctx.html_content->len > 0) {
          // S'il n'y a QUE du HTML (cas des newsletters), on invoque Pandoc
          // pour créer un Markdown propre
          body = strip_html_tags(ctx.html_content->str);
        } else {
          body = g_strdup("(Message vide)");
        }

        // Assemblage final du document textuel structuré (Sujet + En-têtes +
        // Corps converti)
        full_txt_result = g_strdup_printf(
            "SUJET: "
            "%s\n%s--------------------------------------------------\n\n%s\n",
            subject, headers_txt, body);

        g_free(body);
        g_string_free(ctx.text_content, TRUE);
        g_string_free(ctx.html_content, TRUE);
        g_free(headers_txt);
        g_object_unref(message);
      }
      g_object_unref(parser);
      g_object_unref(stream);
    }
  }
  g_free(file_content);

  // Sécurité anti-crash globale
  if (!full_txt_result) {
    full_txt_result = g_strdup("(Erreur lors du traitement du message)");
  }

  // ÉTAPE 3 : Écriture intelligente et contournement du bug GLib sur
  // /dev/stdout g_file_set_contents écrit d'abord dans un fichier temporaire
  // masqué (ex: /dev/stdout.TMP) puis le renomme. Cela provoque un crash
  // "Permission Denied" sur la console. On l'esquive en repérant le
  // périphérique virtuel.
  if (g_strcmp0(output_txt_path, "/dev/stdout") == 0 ||
      g_strcmp0(output_txt_path, "-") == 0) {
    // Envoi brut et direct sur la console (Afficheur interactif de NeoMutt)
    g_print("%s", full_txt_result);
  } else {
    // Écriture standard sécurisée dans un vrai fichier physique (.txt) sur
    // l'espace disque
    GError *error = NULL;
    g_file_set_contents(output_txt_path, full_txt_result, -1, &error);
    if (error) {
      g_printerr("Erreur d'écriture du fichier texte : %s\n", error->message);
      g_error_free(error);
    }
  }
  g_free(full_txt_result);
}

int main(int argc, char *argv[]) {
  // Initialisation globale de l'écosystème GMime
  g_mime_init();

  if (argc < 3) {
    g_print("Usage : %s <eml_path> <output_txt_path>\n", argv[0]);
    g_mime_shutdown();
    return 0;
  }

  // Sécurité : On s'assure que le fichier source existe avant de lancer le
  // traitement
  if (!g_file_test(argv[1], G_FILE_TEST_EXISTS)) {
    g_printerr("Erreur : Le fichier source '%s' n'existe pas.\n", argv[1]);
    g_mime_shutdown();
    return 1;
  }

  // Lancement de la moulinette
  eml_to_txt(argv[1], argv[2]);

  // Désallocation propre des structures GMime de la mémoire
  g_mime_shutdown();
  return 0;
}