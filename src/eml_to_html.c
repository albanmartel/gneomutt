#include <glib.h>
#include <gmime/gmime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PROGRAMME_NAME "eml_to_html"

// Structure pour accumuler les données lors du parcours MIME
typedef struct {
  GString *html_content;
  GString *text_content;
  GString *attachments_html;
  GHashTable *inline_images; // Clé: CID, Valeur: Chemin relatif
  const char *folder_name;
  const char *final_assets_dir;
} ParserContext;

// Échappement des caractères HTML (<, >, &, ")
static gchar *html_escape(const gchar *text) {
  if (!text)
    return g_strdup("");
  return g_markup_escape_text(text, -1);
}

// Extraction propre des en-têtes textuels
static gchar *get_clean_header_value(GMimeMessage *message,
                                     const char *header_name) {
  const char *value =
      g_mime_object_get_header(GMIME_OBJECT(message), header_name);
  if (!value)
    return g_strdup("");

  // Décodage des fragments RFC 2047 (Ex: =?utf-8?Q?...)
  gchar *decoded = g_mime_utils_header_decode_text(NULL, value);
  if (!decoded)
    return g_strdup(value);

  gchar *trimmed = g_strstrip(decoded);
  return g_strdup(trimmed);
}

// Traduction de la date du message en français
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

  GDateWeekday wd = g_date_time_get_day_of_week(gdt); // 1 = lundi, 7 = dimanche
  int day_idx = (wd == 7) ? 0 : wd; // Ajustement pour dimanche = index 0
  int month_idx = g_date_time_get_month(gdt);

  return g_strdup_printf("%s %02d %s %04d à %02d:%02d", jours_fr[day_idx],
                         g_date_time_get_day_of_month(gdt), mois_fr[month_idx],
                         g_date_time_get_year(gdt), g_date_time_get_hour(gdt),
                         g_date_time_get_minute(gdt));
}

// Génération du bloc HTML regroupant les en-têtes principaux
static gchar *generate_headers_html(GMimeMessage *message) {
  GString *sb = g_string_new("");
  gchar *french_date = parse_email_date_to_french(message);

  struct {
    const char *label;
    gchar *value;
  } headers[] = {{"Date", french_date},
                 {"De", get_clean_header_value(message, "From")},
                 {"À", get_clean_header_value(message, "To")},
                 {"Copie", get_clean_header_value(message, "Cc")},
                 {"Copie Cachée", get_clean_header_value(message, "Bcc")},
                 {"Identifiant", get_clean_header_value(message, "Message-ID")},
                 {"Réponse à", get_clean_header_value(message, "In-Reply-To")},
                 {"Références", get_clean_header_value(message, "References")}};

  for (size_t i = 0; i < 8; i++) {
    if (headers[i].value && strlen(headers[i].value) > 0 &&
        g_ascii_strcasecmp(headers[i].value, "none") != 0) {
      gchar *safe_val = html_escape(headers[i].value);
      if (sb->len > 0)
        g_string_append(sb, "<br>\n");
      g_string_append_printf(sb, "<strong>%s :</strong> %s", headers[i].label,
                             safe_val);
      g_free(safe_val);
    }
    g_free(headers[i].value);
  }
  return g_string_free(sb, FALSE);
}

// Traitement récursif de chaque composant du mail (MIME part)
static void process_mime_part(G_GNUC_UNUSED GMimeObject *parent,
                              GMimeObject *part, gpointer user_data) {
  ParserContext *ctx = (ParserContext *)user_data;

  if (!GMIME_IS_PART(part))
    return;

  GMimePart *mime_part = GMIME_PART(part);
  GMimeContentType *content_type = g_mime_object_get_content_type(part);
  const char *disposition = g_mime_object_get_disposition(part);
  const char *filename = g_mime_part_get_filename(mime_part);

  // 1. Extraction textuelle brute ou HTML (Si pas marqué explicitement comme
  // pièce jointe)
  if (filename == NULL &&
      (!disposition || g_ascii_strcasecmp(disposition, "attachment") != 0)) {

    // Vérifier si c'est bien une partie texte
    if (GMIME_IS_TEXT_PART(part)) {
      GMimeTextPart *text_part = GMIME_TEXT_PART(part);
      char *text = g_mime_text_part_get_text(text_part);

      if (text) {
        if (g_mime_content_type_is_type(content_type, "text", "html")) {
          g_string_append(ctx->html_content, text);
        } else if (g_mime_content_type_is_type(content_type, "text", "plain")) {
          g_string_append(ctx->text_content, text);
        }
        g_free(text);
      }
    }
  }
  // 2. Traitement des fichiers (Pièces jointes ou Images intégrées)
  else if (filename != NULL) {
    gchar *filepath = g_build_filename(ctx->final_assets_dir, filename, NULL);
    gchar *relative_filepath =
        g_strdup_printf("%s/%s", ctx->folder_name, filename);

    // Sauvegarde sur le disque
    GMimeDataWrapper *content = g_mime_part_get_content(mime_part);
    if (content) {
      GMimeStream *file_stream = g_mime_stream_file_open(filepath, "wb", NULL);
      if (file_stream) {
        g_mime_data_wrapper_write_to_stream(content, file_stream);
        g_object_unref(file_stream);
      }
    }

    const char *cid = g_mime_object_get_header(part, "Content-ID");
    if (cid && disposition && g_ascii_strcasecmp(disposition, "inline") == 0) {
      // Nettoyage des caractères '<' et '>' du Content-ID
      GRegex *regex = g_regex_new("[<>]", 0, 0, NULL);
      gchar *cid_clean =
          g_regex_replace_literal(regex, cid, -1, 0, "", 0, NULL);

      g_hash_table_insert(ctx->inline_images, g_strdup(cid_clean),
                          g_strdup(relative_filepath));

      g_free(cid_clean);
      g_regex_unref(regex);
    } else {
      // Construction de la liste HTML des pièces jointes standard
      g_string_append_printf(ctx->attachments_html,
                             "<li><a href=\"%s\" download>%s</a></li>\n",
                             relative_filepath, filename);
    }
    g_free(filepath);
    g_free(relative_filepath);
  }
}

// Remplacement des balises src="cid:..." par le chemin local du fichier extrait
static void replace_cid_references(GString *html, const gchar *cid,
                                   const gchar *local_path) {
  gchar *pattern = g_strdup_printf("src=[\"']cid:%s[\"']", cid);
  gchar *replacement = g_strdup_printf("src=\"%s\"", local_path);

  GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
  gchar *result = g_regex_replace_literal(regex, html->str, html->len, 0,
                                          replacement, 0, NULL);

  g_string_assign(html, result);

  g_free(result);
  g_regex_unref(regex);
  g_free(pattern);
  g_free(replacement);
}

// Traitement minimal de substitution au format Markdown vers HTML (text/plain
// de secours)
static gchar *convert_text_to_html_fallback(const gchar *text) {
  GString *sb = g_string_new("");
  gchar *escaped = html_escape(text);

  gchar **lines = g_strsplit(escaped, "\n", -1);
  g_string_append(sb, "<p>");
  for (int i = 0; lines[i] != NULL; i++) {
    if (strlen(lines[i]) == 0) {
      g_string_append(sb, "</p>\n<p>");
    } else {
      if (i > 0 && strlen(lines[i - 1]) > 0)
        g_string_append(sb, "<br>\n");
      g_string_append(sb, lines[i]);
    }
  }
  g_string_append(sb, "</p>");

  g_strfreev(lines);
  g_free(escaped);
  return g_string_free(sb, FALSE);
}

void eml_to_html(const char *eml_path, const char *output_html_path,
                 const char *output_dir) {
  prctl(PR_SET_NAME, PROGRAMME_NAME, 0, 0, 0);

  // Détermination des dossiers
  gchar *html_parent_dir = g_path_get_dirname(output_html_path);
  const char *folder_name =
      output_dir ? g_path_get_basename(output_dir) : "eml_assets";
  gchar *final_assets_dir =
      g_build_filename(html_parent_dir, folder_name, NULL);

  // Création du répertoire d'extraction
  g_mkdir_with_parents(final_assets_dir, 0755);

  // Lecture de l'EML via GMime
  GMimeStream *stream = g_mime_stream_file_open(eml_path, "r", NULL);
  if (!stream) {
    g_printerr("Erreur : Impossible d'ouvrir le fichier %s\n", eml_path);
    g_free(html_parent_dir);
    g_free(final_assets_dir);
    return;
  }

  GMimeParser *parser = g_mime_parser_new_with_stream(stream);
  GMimeMessage *message = g_mime_parser_construct_message(parser, NULL);

  gchar *headers_html = generate_headers_html(message);
  const char *subject = g_mime_message_get_subject(message);
  if (!subject)
    subject = "Sans titre";

  // Initialisation du contexte d'analyse
  ParserContext ctx;
  ctx.html_content = g_string_new("");
  ctx.text_content = g_string_new("");
  ctx.attachments_html = g_string_new("");
  ctx.inline_images =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  ctx.folder_name = folder_name;
  ctx.final_assets_dir = final_assets_dir;

  // Analyse récursive de toutes les sections
  g_mime_message_foreach(message, process_mime_part, &ctx);

  // Choix de la structure du corps (Priorité au HTML)
  GString *final_body = g_string_new("");
  if (ctx.html_content->len > 0) {
    g_string_assign(final_body, ctx.html_content->str);

    // Remplacement des références d'images CID par les fichiers locaux
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, ctx.inline_images);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      replace_cid_references(final_body, (const gchar *)key,
                             (const gchar *)value);
    }
  } else {
    gchar *fallback = convert_text_to_html_fallback(ctx.text_content->str);
    g_string_assign(final_body, fallback);
    g_free(fallback);
  }

  // Traitement final de la zone des pièces jointes
  GString *attachments_section = g_string_new("");
  if (ctx.attachments_html->len > 0) {
    g_string_append(attachments_section, "<hr><h3>Pièces jointes :</h3><ul>\n");
    g_string_append(attachments_section, ctx.attachments_html->str);
    g_string_append(attachments_section, "</ul>\n");
  }

  // Construction du document HTML final complet
  GString *full_html = g_string_new("");
  g_string_append_printf(
      full_html,
      "<!DOCTYPE html>\n<html lang=\"fr\">\n<head>\n"
      "    <meta charset=\"UTF-8\">\n    <title>%s</title>\n"
      "    <style>\n"
      "        body { font-family: Arial, sans-serif; line-height: 1.6; "
      "margin: 20px; color: #333; }\n"
      "        h3 { color: #555; border-bottom: 1px solid #ccc; "
      "padding-bottom: 5px; }\n"
      "        ul { list-style-type: none; padding-left: 0; }\n"
      "        li { margin: 5px 0; }\n"
      "        a { color: #0066cc; text-decoration: none; }\n"
      "        a:hover { text-decoration: underline; }\n"
      "    </style>\n</head>\n<body>\n"
      "    <h2>%s</h2>\n    <p>\n        %s\n    </p>\n    <hr>\n"
      "    <div class=\"email-body\">\n        %s\n    </div>\n"
      "    %s\n</body>\n</html>",
      subject, subject, headers_html, final_body->str,
      attachments_section->str);

  // Écriture finale sur le disque
  GError *error = NULL;
  g_file_set_contents(output_html_path, full_html->str, full_html->len, &error);
  if (error) {
    g_printerr("Erreur d'écriture : %s\n", error->message);
    g_error_free(error);
  } else {
    g_print("Conversion réussie ! Fichier généré : %s\n", output_html_path);
  }

  // Libération globale de la mémoire
  g_string_free(full_html, TRUE);
  g_string_free(final_body, TRUE);
  g_string_free(attachments_section, TRUE);
  g_string_free(ctx.html_content, TRUE);
  g_string_free(ctx.text_content, TRUE);
  g_string_free(ctx.attachments_html, TRUE);
  g_hash_table_destroy(ctx.inline_images);
  g_free(headers_html);
  g_object_unref(message);
  g_object_unref(parser);
  g_object_unref(stream);
  g_free(html_parent_dir);
  g_free(final_assets_dir);
}

int main(int argc, char *argv[]) {
  g_mime_init();

  if (argc < 3) {
    g_print("Convertit un fichier d'e-mail (.eml) en page HTML.\n");
    g_print("Usage : %s <eml_path> <output_html> [<assets_dir>]\n", argv[0]);
    g_mime_shutdown();
    return 0;
  }

  const char *eml_path = argv[1];
  const char *output_html = argv[2];
  const char *assets_dir = (argc >= 4) ? argv[3] : NULL;

  if (!g_file_test(eml_path, G_FILE_TEST_EXISTS)) {
    g_printerr("Erreur : Le fichier source '%s' n'existe pas.\n", eml_path);
    g_mime_shutdown();
    return 1;
  }

  eml_to_html(eml_path, output_html, assets_dir);

  g_mime_shutdown();
  return 0;
}