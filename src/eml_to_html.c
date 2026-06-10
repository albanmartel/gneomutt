#include <glib.h>
#include <gmime/gmime.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

  // Si le texte contient des caractères UTF-8 invalides, on nettoie
  if (!g_utf8_validate(text, -1, NULL)) {
    // Reconstruit une chaîne en remplaçant les caractères invalides par '?'
    return g_utf8_make_valid(text, -1);
  }

  return g_markup_escape_text(text, -1);
}

// Extraction propre des en-têtes textuels
static gchar *get_clean_header_value(GMimeMessage *message,
                                     const char *header_name) {
  // Style GLib : Vérifie la validité et logue une erreur si c'est NULL
  g_return_val_if_fail(message != NULL, g_strdup(""));
  g_return_val_if_fail(header_name != NULL, g_strdup(""));

  const char *value =
      g_mime_object_get_header(GMIME_OBJECT(message), header_name);
  if (!value)
    return g_strdup("");

  // Décodage des fragments RFC 2047 (Ex: =?utf-8?Q?...)
  gchar *decoded = g_mime_utils_header_decode_text(NULL, value);
  if (!decoded)
    return g_strdup(value);

  g_strstrip(decoded);
  return decoded;
}

// Traduction de la date du message en français
static gchar *parse_email_date_to_french(GMimeMessage *message) {
  // 1. Sécurité : Vérification du pointeur d'entrée
  g_return_val_if_fail(message != NULL, g_strdup(""));

  GDateTime *gdt = g_mime_message_get_date(message);
  if (!gdt)
    return g_strdup("");

  const char *jours_fr[] = {"dimanche", "lundi",    "mardi", "mercredi",
                            "jeudi",    "vendredi", "samedi"};
  const char *mois_fr[] = {"",        "janvier",   "février", "mars",
                           "avril",   "mai",       "juin",    "juillet",
                           "août",    "septembre", "octobre", "novembre",
                           "décembre"};

  // 1 = lundi, 7 = dimanche selon GLib
  GDateWeekday wd = g_date_time_get_day_of_week(gdt);
  // Ajustement pour dimanche = index 0
  int day_idx = (wd == 7) ? 0 : wd;
  int month_idx =
      g_mime_message_get_date(message) ? g_date_time_get_month(gdt) : 0;

  // 2. Sécurité : Validation des indices des tableaux (au cas où)
  if (day_idx < 0 || day_idx > 6 || month_idx < 1 || month_idx > 12) {
    // Si la date extraite est invalide ou corrompue, on évite le crash
    return g_strdup("");
  }

  return g_strdup_printf("%s %02d %s %04d à %02d:%02d", jours_fr[day_idx],
                         g_date_time_get_day_of_month(gdt), mois_fr[month_idx],
                         g_date_time_get_year(gdt), g_date_time_get_hour(gdt),
                         g_date_time_get_minute(gdt));
}

// Génération du bloc HTML regroupant les en-têtes principaux
static gchar *generate_headers_html(GMimeMessage *message) {
  // 1. Sécurité : Vérification du pointeur d'entrée
  g_return_val_if_fail(message != NULL, g_strdup(""));

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

  for (size_t i = 0; i < G_N_ELEMENTS(headers); i++) {
    // 1. On vérifie que la valeur existe et n'est pas vide
    if (headers[i].value && strlen(headers[i].value) > 0) {

      // 2. On nettoie les espaces pour le test du "none"
      gchar *trimmed_val = g_strdup(headers[i].value);
      g_strstrip(trimmed_val);

      if (g_ascii_strcasecmp(trimmed_val, "none") != 0) {
        // 3. PROTECTION ABSOLUE : On applique html_escape ICI
        // Qu'il s'agisse d'un nom, d'un mail, ou d'une date falsifiée, tout est
        // neutralisé.
        gchar *safe_val = html_escape(headers[i].value);

        if (sb->len > 0)
          g_string_append(sb, "<br>\n");

        g_string_append_printf(sb, "<strong>%s :</strong> %s", headers[i].label,
                               safe_val);

        g_free(safe_val);
      }
      g_free(trimmed_val);
    }
    g_free(headers[i].value); // Nettoyage de la mémoire
  }
  return g_string_free(sb, FALSE);
}

// Traitement récursif de chaque composant du mail (MIME part)
static void process_mime_part(G_GNUC_UNUSED GMimeObject *parent,
                              GMimeObject *part, gpointer user_data) {
  // 1. Sécurité : Vérification des pointeurs de base
  g_return_if_fail(part != NULL);
  g_return_if_fail(user_data != NULL);

  ParserContext *ctx = (ParserContext *)user_data;

  if (!GMIME_IS_PART(part))
    return;

  GMimePart *mime_part = GMIME_PART(part);
  GMimeContentType *content_type = g_mime_object_get_content_type(part);
  const char *disposition = g_mime_object_get_disposition(part);
  const char *filename = g_mime_part_get_filename(mime_part);

  // 2. Extraction textuelle brute ou HTML (Si pas marqué explicitement comme
  // pièce jointe)
  if (filename == NULL &&
      (!disposition || g_ascii_strcasecmp(disposition, "attachment") != 0)) {

    // Vérifier si c'est bien une partie texte
    if (GMIME_IS_TEXT_PART(part)) {
      GMimeTextPart *text_part = GMIME_TEXT_PART(part);
      char *text = g_mime_text_part_get_text(text_part);

      if (text) {
        // Sécurité : On s'assure que content_type n'est pas NULL
        if (content_type &&
            g_mime_content_type_is_type(content_type, "text", "html")) {
          // /!\ ATTENTION : 'text' contient du HTML brut provenant du mail
          // (Risque XSS si non sanitisé en amont)
          g_string_append(ctx->html_content, text);
        } else if (content_type &&
                   g_mime_content_type_is_type(content_type, "text", "plain")) {
          // Pour le texte brut, on l'échappe en HTML avant de l'ajouter au
          // contenu HTML global
          gchar *safe_text = html_escape(text);
          g_string_append(ctx->text_content, safe_text);
          g_free(safe_text);
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
      // PROTECTION XSS : On échappe le nom du fichier et son chemin relatif
      gchar *safe_filename = html_escape(filename);
      gchar *safe_relative_path = html_escape(relative_filepath);

      // Construction sécurisée de la liste HTML des pièces jointes
      g_string_append_printf(ctx->attachments_html,
                             "<li><a href=\"%s\" download>%s</a></li>\n",
                             safe_relative_path, safe_filename);

      g_free(safe_filename);
      g_free(safe_relative_path);
    }
    g_free(filepath);
    g_free(relative_filepath);
  }
}

// Remplacement des balises src="cid:..." par le chemin local du fichier extrait
static void replace_cid_references(GString *html, const gchar *cid,
                                   const gchar *local_path) {

  // 1. Sécurité : Vérification stricte des pointeurs d'entrée
  g_return_if_fail(html != NULL);
  g_return_if_fail(html->str != NULL);
  g_return_if_fail(cid != NULL);
  g_return_if_fail(local_path != NULL);

  // 2. Sécurité : On échappe le CID au cas où il contiendrait des caractères
  // spéciaux de Regex (ex: '.', '+')
  gchar *escaped_cid = g_regex_escape_string(cid, -1);

  // Amélioration de la Regex pour gérer les espaces potentiels :
  // src=\s*["']cid:...["']
  gchar *pattern = g_strdup_printf("src\\s*=\\s*[\"']cid:%s[\"']", escaped_cid);
  gchar *replacement = g_strdup_printf("src=\"%s\"", local_path);

  // 3. Sécurité : On vérifie si la compilation de la Regex réussit
  GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
  if (regex) {
    gchar *result = g_regex_replace_literal(regex, html->str, html->len, 0,
                                            replacement, 0, NULL);
    if (result) {
      g_string_assign(html, result);
      g_free(result);
    }
    g_regex_unref(regex);
  }

  // Nettoyage de la mémoire
  g_free(pattern);
  g_free(replacement);
  g_free(escaped_cid);
}

// Traitement minimal de substitution au format Markdown vers HTML (text/plain
// de secours)
static gchar *convert_text_to_html_fallback(const gchar *text) {
  // 1. Vérification de sécurité du pointeur
  g_return_val_if_fail(text != NULL, NULL);

  // 2. Optionnel : Gestion rapide si la chaîne est vide
  if (*text == '\0') {
    return g_strdup("<p></p>");
  }

  GString *sb = g_string_new("");
  gchar *escaped = html_escape(text);

  // Sécurité additionnelle : vérifier si html_escape n'a pas échoué
  if (escaped == NULL) {
    g_string_free(sb, TRUE);
    return NULL;
  }

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

  // --- Vérifications des paramètres ---
  // 1. Vérification de eml_path
  if (!eml_path || !*eml_path) {
    g_printerr("Erreur : eml_path est NULL ou vide.\n");
    return;
  }

  if (g_file_test(eml_path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR) ==
      FALSE) {
    g_printerr("Erreur : Le fichier %s n'existe pas ou n'est pas un fichier "
               "régulier.\n",
               eml_path);
    return;
  }

  if (access(eml_path, R_OK) != 0) {
    g_printerr("Erreur : Le fichier %s n'est pas lisible.\n", eml_path);
    return;
  }

  // 2. Vérification de output_html_path
  if (!output_html_path || !*output_html_path) {
    g_printerr("Erreur : output_html_path est NULL ou vide.\n");
    return;
  }

  // Récupération du répertoire parent de output_html_path
  gchar *html_parent_dir = g_path_get_dirname(output_html_path);
  if (!html_parent_dir || !*html_parent_dir) {
    g_printerr(
        "Erreur : Impossible de déterminer le répertoire parent de %s.\n",
        output_html_path);
    g_free(html_parent_dir);
    return;
  }

  // Vérification que le répertoire parent existe et est accessible en écriture
  if (g_file_test(html_parent_dir, G_FILE_TEST_IS_DIR) == FALSE) {
    g_printerr("Erreur : Le répertoire parent %s n'existe pas ou n'est pas un "
               "dossier.\n",
               html_parent_dir);
    g_free(html_parent_dir);
    return;
  }

  if (access(html_parent_dir, W_OK) != 0) {
    g_printerr(
        "Erreur : Le répertoire parent %s n'est pas accessible en écriture.\n",
        html_parent_dir);
    g_free(html_parent_dir);
    return;
  }

  // 3. Vérification de output_dir (si non-NULL)
  if (output_dir && !*output_dir) {
    g_printerr("Erreur : output_dir est une chaîne vide.\n");
    g_free(html_parent_dir);
    return;
  }
  // --- Fin des vérifications ---

  // Détermination des dossiers
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
    g_print("Conversion successful! File generated: %s\n", output_html_path);
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
  // Forcer la locale à UTF-8
  setlocale(LC_ALL, "fr_FR.UTF-8");
  g_setenv("LANG", "fr_FR.UTF-8", TRUE);
  g_setenv("LC_ALL", "fr_FR.UTF-8", TRUE);
  g_mime_init();

  if (argc < 3) {
    g_print("Convertit un fichier d'e-mail (.eml) en page HTML.\n");
    g_print("Usage : %s <eml_path> <output_html> [<assets_dir>]\n", argv[0]);
    g_mime_shutdown();
    return 1;
  }

  const char *eml_path = argv[1];
  const char *output_html = argv[2];
  const char *assets_dir = (argc >= 4) ? argv[3] : NULL;

  if (!g_file_test(eml_path, G_FILE_TEST_EXISTS)) {
    g_printerr("Erreur : Le fichier source '%s' n'existe pas.\n", eml_path);
    g_mime_shutdown();
    return 1;
  }

  if (access(eml_path, R_OK) != 0) {
    g_printerr("Erreur : Le fichier '%s' n'est pas lisible.\n", eml_path);
    g_mime_shutdown();
    return 1;
  }

  // Vérification du répertoire parent de output_html
  gchar *output_dir = g_path_get_dirname(output_html);
  if (!output_dir) {
    g_printerr(
        "Erreur : Impossible de déterminer le répertoire parent de '%s'.\n",
        output_html);
    g_mime_shutdown();
    return 1;
  }

  if (!g_file_test(output_dir, G_FILE_TEST_IS_DIR)) {
    g_printerr("Erreur : Le répertoire parent '%s' n'existe pas ou n'est pas "
               "un dossier.\n",
               output_dir);
    g_free(output_dir);
    g_mime_shutdown();
    return 1;
  }

  if (access(output_dir, W_OK) != 0) {
    g_printerr("Erreur : Le répertoire parent '%s' n'est pas accessible en "
               "écriture.\n",
               output_dir);
    g_free(output_dir);
    g_mime_shutdown();
    return 1;
  }
  g_free(output_dir);

  // Appel de la fonction de conversion
  eml_to_html(eml_path, output_html, assets_dir);

  g_mime_shutdown();
  return 0;
}