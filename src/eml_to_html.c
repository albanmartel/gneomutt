#include <ctype.h>
#include <gmime/gmime.h>
#include <gumbo.h>
#include <libgen.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PROGRAMME_NAME "eml_to_html"

// ============================================================================
// ENGINE ET CONVERTISSEUR D'ESPACES UNICODE
// ============================================================================
typedef struct {
  unsigned char bytes[3];
  int length;
} EspaceUnicode;

// Table contenant les séquences UTF-8 des espaces à convertir
static const EspaceUnicode espaces_table[] = {
    {{0xC2, 0xA0, 0x00}, 2}, // U+00A0 (NBSP)
    {{0xE1, 0x9A, 0x80}, 3}, // U+1680
    {{0xE2, 0x80, 0x80}, 3}, // U+2000 à U+200A
    {{0xE2, 0x80, 0x81}, 3}, {{0xE2, 0x80, 0x82}, 3}, {{0xE2, 0x80, 0x83}, 3},
    {{0xE2, 0x80, 0x84}, 3}, {{0xE2, 0x80, 0x85}, 3}, {{0xE2, 0x80, 0x86}, 3},
    {{0xE2, 0x80, 0x87}, 3}, // U+2007 (Figure Space)
    {{0xE2, 0x80, 0x88}, 3}, {{0xE2, 0x80, 0x89}, 3}, {{0xE2, 0x80, 0x8A}, 3},
    {{0xE2, 0x80, 0xAF}, 3}, // U+202F (Narrow NBSP)
    {{0xE2, 0x81, 0x9F}, 3}, // U+205F
    {{0xE3, 0x80, 0x80}, 3}  // U+3000
};

#define NB_ESPACES (sizeof(espaces_table) / sizeof(EspaceUnicode))

// Vérifie si la séquence actuelle correspond à un espace insécable ou spécial
static int get_space_match_length(const unsigned char *ptr) {
  if (!ptr || *ptr == '\0')
    return 0;

  size_t remaining = strlen((const char *)ptr);
  for (size_t i = 0; i < NB_ESPACES; i++) {
    if (remaining >= (size_t)espaces_table[i].length) {
      int match = 1;
      for (int j = 0; j < espaces_table[i].length; j++) {
        if (ptr[j] != espaces_table[i].bytes[j]) {
          match = 0;
          break;
        }
      }
      if (match)
        return espaces_table[i].length;
    }
  }
  return 0;
}

// ============================================================================
// STRUCTURE & ENGINE DE L'ARENA MEMOIRE
// ============================================================================
typedef struct {
  char *buffer;
  size_t capacity;
  size_t offset;
} Arena;

Arena *arena_create(size_t capacity) {
  Arena *arena = malloc(sizeof(Arena));
  arena->buffer = malloc(capacity);
  arena->capacity = capacity;
  arena->offset = 0;
  return arena;
}

void *arena_alloc(Arena *arena, size_t size) {
  size_t aligned_size = (size + 7) & ~7;
  if (arena->offset + aligned_size > arena->capacity) {
    fprintf(stderr,
            "Erreur critique : Arena saturée ! Plus de mémoire disponible.\n");
    exit(EXIT_FAILURE);
  }
  void *ptr = &arena->buffer[arena->offset];
  arena->offset += aligned_size;
  return ptr;
}

char *arena_strdup(Arena *arena, const char *src) {
  if (!src)
    return NULL;
  size_t len = strlen(src);
  char *dst = arena_alloc(arena, len + 1);
  if (dst)
    memcpy(dst, src, len + 1);
  return dst;
}

char *arena_asprintf(Arena *arena, const char *format, ...) {
  va_list args;
  va_start(args, format);
  size_t needed = vsnprintf(NULL, 0, format, args) + 1;
  va_end(args);

  char *dst = arena_alloc(arena, needed);
  if (dst) {
    va_start(args, format);
    vsnprintf(dst, needed, format, args);
    va_end(args);
  }
  return dst;
}

void arena_destroy(Arena *arena) {
  free(arena->buffer);
  free(arena);
}

// ============================================================================
// STRUCTURES DU PARSER
// ============================================================================
typedef struct {
  char *html_content;
  size_t html_len;
  char *text_content;
  size_t text_len;
  char *attachments_html;
  size_t attach_len;
  GHashTable *inline_images;
  const char *folder_name;
  const char *final_assets_dir;
  Arena *arena;
} ParserContext;

// ============================================================================
// STRUCTURES DES HEADERS HTML
// ============================================================================
typedef struct {
  GMimeMessage *message;
  const char *subject;
  char *safe_page_title;
  char *headers_html;
  char *from_str;
  char *to_str;
  char *cc_str;
  char *bcc_str;
} EmailHeaders;

void export_metadata_json(Arena *arena, const char *msg_id, const char *subject,
                          const char *from_str, const char *to_str,
                          const char *cc_str, const char *bcc_str,
                          const char *raw_date, const char *in_reply_to,
                          const char *references, const char *output_html_path);

static char *generate_headers_html(Arena *arena, const char *french_date,
                                   const char *from_str, const char *to_str,
                                   const char *cc_str, const char *bcc_str,
                                   const char *subject, const char *msg_id,
                                   const char *in_reply_to,
                                   const char *references);

static char *parse_email_date_to_french(Arena *arena, GMimeMessage *message);
static char *generer_titre_page(Arena *arena, GMimeMessage *message);

void extraire_et_exporter_headers(Arena *arena, GMimeStream *stream,
                                  GMimeParser *parser,
                                  const char *output_html_path,
                                  EmailHeaders *out_headers);

void extraire_et_exporter_headers(Arena *arena, GMimeStream *stream,
                                  GMimeParser *parser,
                                  const char *output_html_path,
                                  EmailHeaders *out_headers) {
  if (!arena || !stream || !parser || !out_headers)
    return;

  g_mime_parser_init_with_stream(parser, stream);
  GMimeMessage *message = g_mime_parser_construct_message(parser, NULL);
  out_headers->message = message;

  if (!message)
    return;

  const char *msg_id = g_mime_message_get_message_id(message);
  const char *subject = g_mime_message_get_subject(message);
  if (!subject)
    subject = "Sans titre";
  out_headers->subject = subject;

  InternetAddressList *from_list = g_mime_message_get_from(message);
  InternetAddressList *to_list = g_mime_message_get_to(message);
  InternetAddressList *cc_list = g_mime_message_get_cc(message);
  InternetAddressList *bcc_list = g_mime_message_get_bcc(message);

  out_headers->from_str =
      from_list ? internet_address_list_to_string(from_list, NULL, FALSE)
                : NULL;
  out_headers->to_str =
      to_list ? internet_address_list_to_string(to_list, NULL, FALSE) : NULL;
  out_headers->cc_str =
      cc_list ? internet_address_list_to_string(cc_list, NULL, FALSE) : NULL;
  out_headers->bcc_str =
      bcc_list ? internet_address_list_to_string(bcc_list, NULL, FALSE) : NULL;

  const char *in_reply_to =
      g_mime_object_get_header(GMIME_OBJECT(message), "In-Reply-To");
  const char *references =
      g_mime_object_get_header(GMIME_OBJECT(message), "References");
  const char *raw_date =
      g_mime_object_get_header(GMIME_OBJECT(message), "Date");

  char *french_date = parse_email_date_to_french(arena, message);

  export_metadata_json(arena, msg_id, subject, out_headers->from_str,
                       out_headers->to_str, out_headers->cc_str,
                       out_headers->bcc_str, raw_date, in_reply_to, references,
                       output_html_path);

  out_headers->safe_page_title = generer_titre_page(arena, message);
  out_headers->headers_html = generate_headers_html(
      arena, french_date, out_headers->from_str, out_headers->to_str,
      out_headers->cc_str, out_headers->bcc_str, subject, msg_id, in_reply_to,
      references);
}

static void arena_string_append(Arena *arena, char **dest, size_t *current_len,
                                const char *src) {
  if (!src)
    return;
  size_t src_len = strlen(src);

  char *new_buf = arena_alloc(arena, *current_len + src_len + 1);
  if (*current_len > 0 && *dest) {
    memcpy(new_buf, *dest, *current_len);
  }
  memcpy(new_buf + *current_len, src, src_len + 1);
  *dest = new_buf;
  *current_len += src_len;
}

static void arena_string_append_printf(Arena *arena, char **dest,
                                       size_t *current_len, const char *format,
                                       ...) {
  va_list args;
  va_start(args, format);
  size_t needed = vsnprintf(NULL, 0, format, args) + 1;
  va_end(args);

  char *formatted = arena_alloc(arena, needed);
  va_start(args, format);
  vsnprintf(formatted, needed, format, args);
  va_end(args);

  arena_string_append(arena, dest, current_len, formatted);
}

static void c_strstrip(char *s) {
  char *start = s;
  while (isspace((unsigned char)*start))
    start++;
  if (start != s) {
    memmove(s, start, strlen(start) + 1);
  }
  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1]))
    len--;
  s[len] = '\0';
}

static int get_utf8_length(const unsigned char *utf8_bytes) {
  if (!utf8_bytes)
    return 0;

  if (utf8_bytes[0] < 0x80)
    return 1;
  if ((utf8_bytes[0] & 0xE0) == 0xC0 && (utf8_bytes[1] & 0xC0) == 0x80)
    return 2;
  if ((utf8_bytes[0] & 0xF0) == 0xE0 && (utf8_bytes[1] & 0xC0) == 0x80 &&
      (utf8_bytes[2] & 0xC0) == 0x80)
    return 3;
  if ((utf8_bytes[0] & 0xF8) == 0xF0 && (utf8_bytes[1] & 0xC0) == 0x80 &&
      (utf8_bytes[2] & 0xC0) == 0x80 && (utf8_bytes[3] & 0xC0) == 0x80)
    return 4;
  return 0;
}

static size_t get_html_entity_length(const unsigned char *reader) {
  if (!reader || *reader == '\0')
    return 0;
  if (reader[0] != '&')
    return 0;

  const unsigned char *checker = reader + 1;

  if (*checker == '#') {
    checker++;
    if (*checker == 'x' || *checker == 'X') {
      checker++;
      while (*checker && isxdigit(*checker))
        checker++;
    } else {
      while (*checker && isdigit(*checker))
        checker++;
    }
    if (*checker == ';')
      return (checker - reader) + 1;
    return 0;
  }

  if (isalpha(*checker)) {
    while (*checker && isalnum(*checker))
      checker++;
    if (*checker == ';')
      return (checker - reader) + 1;
  }

  return 0;
}

void en_minuscules(char *str) {
  if (!str)
    return;

  for (int i = 0; str[i]; i++) {
    if (str[i] >= 'A' && str[i] <= 'Z') {
      str[i] = str[i] + 32;
    }
  }
}

void indent_html(int profondeur, FILE *output) {
  if (profondeur < 0 || !output)
    return;
  for (int i = 0; i < profondeur; ++i) {
    fputs("  ", output);
  }
}

static char *html_escape(Arena *arena, const char *text) {
  if (!arena)
    return NULL;
  if (!text)
    return arena_strdup(arena, "");

  const unsigned char *reader = (const unsigned char *)text;
  size_t required_len = 0;

  while (*reader) {
    size_t entity_len = get_html_entity_length(reader);
    if (entity_len > 0) {
      required_len += entity_len;
      reader += entity_len;
      continue;
    }

    int utf8_len = get_utf8_length(reader);
    if (utf8_len == 0) {
      required_len += 3;
      reader++;
    } else if (utf8_len == 1) {
      if (*reader == '<' || *reader == '>')
        required_len += 4;
      else if (*reader == '&')
        required_len += 5;
      else if (*reader == '"')
        required_len += 6;
      else
        required_len++;
      reader++;
    } else {
      required_len += utf8_len;
      reader += utf8_len;
    }
  }

  char *result = arena_alloc(arena, required_len + 1);
  char *writer = result;
  reader = (const unsigned char *)text;

  while (*reader) {
    size_t entity_len = get_html_entity_length(reader);
    if (entity_len > 0) {
      memcpy(writer, reader, entity_len);
      writer += entity_len;
      reader += entity_len;
      continue;
    }

    int utf8_len = get_utf8_length(reader);
    if (utf8_len == 0) {
      *writer++ = 0xEF;
      *writer++ = 0xBF;
      *writer++ = 0xBD;
      reader++;
    } else if (utf8_len == 1) {
      if (*reader == '<') {
        memcpy(writer, "&lt;", 4);
        writer += 4;
      } else if (*reader == '>') {
        memcpy(writer, "&gt;", 4);
        writer += 4;
      } else if (*reader == '&') {
        memcpy(writer, "&amp;", 5);
        writer += 5;
      } else if (*reader == '"') {
        memcpy(writer, "&quot;", 6);
        writer += 6;
      } else {
        *writer++ = *reader;
      }
      reader++;
    } else {
      memcpy(writer, reader, utf8_len);
      writer += utf8_len;
      reader += utf8_len;
    }
  }
  *writer = '\0';
  return result;
}

static void css_prettifier_to_file(const char *text, FILE *output) {
  if (!text || !output)
    return;

  fputc('\n', output);

  while (*text) {
    if (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t') {
      text++;
      continue;
    }
    if (*text == '{') {
      fputs(" {\n    ", output);
    } else if (*text == ';') {
      fputs(";\n    ", output);
    } else if (*text == '}') {
      fputs("\n}\n\n", output);
    } else {
      fputc(*text, output);
    }
    text++;
  }
}

void html_prettifier_to_file(Arena *arena, GumboNode *node, int profondeur,
                             FILE *output) {
  if (!arena || !node || !output || profondeur < 0)
    return;

  // --- 1. GESTION DES NŒUDS DE TEXTE ---
  if (node->type == GUMBO_NODE_TEXT) {
    if (node->parent && node->parent->type == GUMBO_NODE_ELEMENT &&
        node->parent->v.element.tag == GUMBO_TAG_STYLE) {
      css_prettifier_to_file(node->v.text.text, output);
    } else {
      const char *text = node->v.text.text;

      // On vérifie si on doit préserver les espaces (ex: balise <pre>)
      int preserver_espaces = 0;
      if (node->parent && node->parent->type == GUMBO_NODE_ELEMENT) {
        if (node->parent->v.element.tag == GUMBO_TAG_PRE) {
          preserver_espaces = 1;
        }
      }

      if (preserver_espaces) {
        // Mode brut : on écrit tout tel quel
        while (*text) {
          fputc(*text, output);
          text++;
        }
      } else {
        // Mode HTML standard : effondrement des espaces et des sauts de ligne
        int espace_recurent = 0;
        while (*text) {
          int skip_space = get_space_match_length((const unsigned char *)text);
          if (skip_space > 0) {
            if (!espace_recurent) {
              fputc(0x20, output);
              espace_recurent = 1;
            }
            text += skip_space;
            continue;
          }

          if (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t') {
            if (!espace_recurent) {
              fputc(0x20, output);
              espace_recurent = 1;
            }
            text++;
          } else {
            espace_recurent = 0; // On a du vrai texte, on reset le flag

            if (*text == '<')
              fputs("&lt;", output);
            else if (*text == '>')
              fputs("&gt;", output);
            else if (*text == '&') {
              if (get_html_entity_length((const unsigned char *)text) > 0) {
                fputc(*text, output);
              } else {
                fputs("&amp;", output);
              }
            } else if (*text == '"')
              fputs("&quot;", output);
            else
              fputc(*text, output);
            text++;
          }
        }
      }
    }
    return;
  }

  // --- 2. GESTION DES ÉLÉMENTS (BALISES) ---
  if (node->type != GUMBO_NODE_ELEMENT)
    return;

  GumboElement *element = &node->v.element;

  if (element->tag == GUMBO_TAG_SCRIPT || element->tag == GUMBO_TAG_NOSCRIPT)
    return;

  fputc('\n', output);
  indent_html(profondeur, output);

  const char *tag_name = gumbo_normalized_tagname(element->tag);
  char tag_custom[128] = {0};

  if (!tag_name || strlen(tag_name) == 0) {
    gumbo_tag_from_original_text(&element->original_tag);
    if (element->original_tag.data && element->original_tag.length > 0) {
      size_t len = element->original_tag.length;
      if (len > sizeof(tag_custom) - 1)
        len = sizeof(tag_custom) - 1;

      size_t idx = 0;
      for (size_t i = 0; i < len; i++) {
        char c = element->original_tag.data[i];
        if (c != '<' && c != '>' && c != '/' && c != ' ' && c != '\t' &&
            c != '\n' && c != '\r') {
          tag_custom[idx++] = c;
        } else if (idx > 0) {
          break;
        }
      }
      tag_custom[idx] = '\0';
      tag_name = tag_custom;
    }
  }

  if (!tag_name || strlen(tag_name) == 0) {
    tag_name = "span";
  }

  fprintf(output, "<%s", tag_name);

  GumboVector *attributes = &element->attributes;
  for (unsigned int i = 0; i < attributes->length; ++i) {
    GumboAttribute *attr = (GumboAttribute *)attributes->data[i];
    if (strlen(attr->value) > 0) {
      fprintf(output, " %s=\"%s\"", attr->name, attr->value);
    }
  }
  fputc('>', output);

  GumboVector *children = &element->children;
  int a_des_enfants_balises = 0;

  for (unsigned int i = 0; i < children->length; ++i) {
    GumboNode *child = (GumboNode *)children->data[i];
    if (child->type == GUMBO_NODE_ELEMENT)
      a_des_enfants_balises = 1;
    html_prettifier_to_file(arena, child, profondeur + 1, output);
  }

  if (element->tag != GUMBO_TAG_IMG && element->tag != GUMBO_TAG_BR &&
      element->tag != GUMBO_TAG_INPUT) {
    if (a_des_enfants_balises) {
      fputc('\n', output);
      indent_html(profondeur, output);
    }
    fprintf(output, "</%s>", tag_name);
  }
}

void nettoyer_arbre(GumboNode *node) {
  if (!node)
    return;
  if (node->type != GUMBO_NODE_ELEMENT) {
    return;
  }

  GumboElement *element = &node->v.element;

  if (element->tag == GUMBO_TAG_SCRIPT || element->tag == GUMBO_TAG_NOSCRIPT) {
    return;
  }

  GumboVector *attributes = &element->attributes;
  for (unsigned int i = 0; i < attributes->length; ++i) {
    GumboAttribute *attr = (GumboAttribute *)attributes->data[i];

    char attr_name[256];
    strncpy(attr_name, attr->name, sizeof(attr_name) - 1);
    en_minuscules(attr_name);

    if (strncmp(attr_name, "on", 2) == 0) {
      printf("Attribut dangereux détecté et supprimé : %s\n", attr->name);
    }

    if (strcmp(attr_name, "href") == 0 || strcmp(attr_name, "src") == 0) {
      char attr_value[512];
      strncpy(attr_value, attr->value, sizeof(attr_value) - 1);
      en_minuscules(attr_value);

      if (strncmp(attr_value, "javascript:", 11) == 0) {
        printf("Lien JavaScript détecté et neutralisé dans : %s\n", attr->name);
      }
    }
  }

  GumboVector *children = &element->children;
  for (unsigned int i = 0; i < children->length; ++i) {
    nettoyer_arbre((GumboNode *)children->data[i]);
  }
}

static char *get_clean_header_value(Arena *arena, GMimeMessage *message,
                                    const char *header_name) {
  if (!message || !header_name)
    return arena_strdup(arena, "");

  const char *value =
      g_mime_object_get_header(GMIME_OBJECT(message), header_name);
  if (!value)
    return arena_strdup(arena, "");

  char *decoded = g_mime_utils_header_decode_text(NULL, value);
  if (!decoded)
    return arena_strdup(arena, value);

  c_strstrip(decoded);
  char *arena_val = arena_strdup(arena, decoded);
  g_free(decoded);
  return arena_val;
}

static char *parse_email_date_to_french(Arena *arena, GMimeMessage *message) {
  if (!message)
    return arena_strdup(arena, "");

  GDateTime *gdt = g_mime_message_get_date(message);
  if (!gdt)
    return arena_strdup(arena, "");

  const char *jours_fr[] = {"dimanche", "lundi",    "mardi", "mercredi",
                            "jeudi",    "vendredi", "samedi"};
  const char *mois_fr[] = {"",        "janvier",   "février", "mars",
                           "avril",   "mai",       "juin",    "juillet",
                           "août",    "septembre", "octobre", "novembre",
                           "décembre"};

  int day_idx = (g_date_time_get_day_of_week(gdt) == 7)
                    ? 0
                    : g_date_time_get_day_of_week(gdt);
  int month_idx = g_date_time_get_month(gdt);

  if (day_idx < 0 || day_idx > 6 || month_idx < 1 || month_idx > 12)
    return arena_strdup(arena, "");

  return arena_asprintf(arena, "%s %02d %s %04d à %02d:%02d", jours_fr[day_idx],
                        g_date_time_get_day_of_month(gdt), mois_fr[month_idx],
                        g_date_time_get_year(gdt), g_date_time_get_hour(gdt),
                        g_date_time_get_minute(gdt));
}

static char *generate_headers_html(Arena *arena, const char *french_date,
                                   const char *from_str, const char *to_str,
                                   const char *cc_str, const char *bcc_str,
                                   const char *subject, const char *msg_id,
                                   const char *in_reply_to,
                                   const char *references) {
  if (!arena)
    return "";

  char *sb = arena_strdup(arena, "");
  size_t sb_len = 0;

  struct {
    const char *label;
    const char *value;
  } headers[] = {{"Date", french_date ? french_date : ""},
                 {"De", from_str ? from_str : ""},
                 {"À", to_str ? to_str : ""},
                 {"Copie", cc_str ? cc_str : ""},
                 {"Copie Cachée", bcc_str ? bcc_str : ""},
                 {"Sujet", subject ? subject : ""},
                 {"Identifiant", msg_id ? msg_id : ""},
                 {"Réponse à", in_reply_to ? in_reply_to : ""},
                 {"Références", references ? references : ""}};

  for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
    if (headers[i].value && strlen(headers[i].value) > 0) {
      char *val_copy = arena_strdup(arena, headers[i].value);
      c_strstrip(val_copy);

      if (strcasecmp(val_copy, "none") != 0 && strlen(val_copy) > 0) {
        char *safe_val = html_escape(arena, val_copy);
        if (sb_len > 0) {
          arena_string_append(arena, &sb, &sb_len, "<br>\n");
        }
        arena_string_append_printf(arena, &sb, &sb_len,
                                   "<strong>%s :</strong> %s", headers[i].label,
                                   safe_val);
      }
    }
  }
  return sb;
}

static char *json_escape(Arena *arena, const char *text) {
  if (!arena)
    return NULL;
  if (!text)
    return arena_strdup(arena, "");

  size_t len_requise = 0;
  const char *lecteur_source = text;

  while (*lecteur_source) {
    char caractere_actuel = *lecteur_source;
    if (caractere_actuel == '"' || caractere_actuel == '\\' ||
        caractere_actuel == '\n' || caractere_actuel == '\r' ||
        caractere_actuel == '\t') {
      len_requise += 2;
    } else {
      len_requise++;
    }
    lecteur_source++;
  }

  char *chaine_destination = arena_alloc(arena, len_requise + 1);
  char *curseur_ecriture = chaine_destination;
  lecteur_source = text;

  while (*lecteur_source) {
    char caractere_actuel = *lecteur_source;

    if (caractere_actuel == '"') {
      *curseur_ecriture++ = '\\';
      *curseur_ecriture++ = '"';
    } else if (caractere_actuel == '\\') {
      *curseur_ecriture++ = '\\';
      *curseur_ecriture++ = '\\';
    } else if (caractere_actuel == '\n') {
      *curseur_ecriture++ = '\\';
      *curseur_ecriture++ = 'n';
    } else if (caractere_actuel == '\r') {
      *curseur_ecriture++ = '\\';
      *curseur_ecriture++ = 'r';
    } else if (caractere_actuel == '\t') {
      *curseur_ecriture++ = '\\';
      *curseur_ecriture++ = 't';
    } else {
      *curseur_ecriture++ = caractere_actuel;
    }

    lecteur_source++;
  }

  *curseur_ecriture = '\0';
  return chaine_destination;
}

void export_metadata_json(Arena *arena, const char *msg_id, const char *subject,
                          const char *from_str, const char *to_str,
                          const char *cc_str, const char *bcc_str,
                          const char *raw_date, const char *in_reply_to,
                          const char *references,
                          const char *output_html_path) {
  if (!arena || !output_html_path)
    return;

  const char *val_msg_id = msg_id ? msg_id : "";
  const char *val_subject = subject ? subject : "";
  const char *val_from = from_str ? from_str : "";
  const char *val_to = to_str ? to_str : "";
  const char *val_cc = cc_str ? cc_str : "";
  const char *val_bcc = bcc_str ? bcc_str : "";
  const char *val_date = raw_date ? raw_date : "";
  const char *val_in_reply_to = in_reply_to ? in_reply_to : "";
  const char *val_references = references ? references : "";

  char *path_copy = arena_strdup(arena, output_html_path);
  char *dossier_parent = dirname(path_copy);

  char output_json_path[1024];
  snprintf(output_json_path, sizeof(output_json_path), "%s/headers.json",
           dossier_parent);

  FILE *f_json = fopen(output_json_path, "wb");
  if (!f_json) {
    fprintf(stderr, "Erreur : Impossible de créer le fichier %s\n",
            output_json_path);
    return;
  }

  char *safe_msg_id = json_escape(arena, val_msg_id);
  char *safe_subject = json_escape(arena, val_subject);
  char *safe_from = json_escape(arena, val_from);
  char *safe_to = json_escape(arena, val_to);
  char *safe_cc = json_escape(arena, val_cc);
  char *safe_bcc = json_escape(arena, val_bcc);
  char *safe_date = json_escape(arena, val_date);
  char *safe_in_reply_to = json_escape(arena, val_in_reply_to);
  char *safe_references = json_escape(arena, val_references);

  fprintf(f_json, "{\n");
  fprintf(f_json, "  \"Message-ID\": \"%s\",\n", safe_msg_id);
  fprintf(f_json, "  \"Subject\": \"%s\",\n", safe_subject);
  fprintf(f_json, "  \"From\": \"%s\",\n", safe_from);
  fprintf(f_json, "  \"To\": \"%s\",\n", safe_to);
  fprintf(f_json, "  \"Cc\": \"%s\",\n", safe_cc);
  fprintf(f_json, "  \"Bcc\": \"%s\",\n", safe_bcc);
  fprintf(f_json, "  \"Date\": \"%s\",\n", safe_date);
  fprintf(f_json, "  \"In-Reply-To\": \"%s\",\n", safe_in_reply_to);
  fprintf(f_json, "  \"References\": \"%s\"\n", safe_references);
  fprintf(f_json, "}\n");

  fclose(f_json);
  printf("All raw headers exported successfully to JSON: %s\n",
         output_json_path);
}

static void process_mime_part(G_GNUC_UNUSED GMimeObject *parent,
                              GMimeObject *part, gpointer user_data) {
  if (!part || !user_data)
    return;

  ParserContext *ctx = (ParserContext *)user_data;
  if (!GMIME_IS_PART(part))
    return;

  GMimePart *mime_part = GMIME_PART(part);
  GMimeContentType *content_type = g_mime_object_get_content_type(part);
  const char *disposition = g_mime_object_get_disposition(part);
  const char *filename = g_mime_part_get_filename(mime_part);

  if (filename == NULL &&
      (!disposition || strcasecmp(disposition, "attachment") != 0)) {
    if (GMIME_IS_TEXT_PART(part)) {
      GMimeTextPart *text_part = GMIME_TEXT_PART(part);
      char *gm_text = g_mime_text_part_get_text(text_part);

      if (gm_text) {
        char *text = arena_strdup(ctx->arena, gm_text);

        if (content_type &&
            g_mime_content_type_is_type(content_type, "text", "html")) {
          char *conflict = strstr(text, "charset=");
          if (conflict) {
            memcpy(conflict, "disable=", 8);
          }
          arena_string_append(ctx->arena, &ctx->html_content, &ctx->html_len,
                              text);
        } else if (content_type &&
                   g_mime_content_type_is_type(content_type, "text", "plain")) {
          char *safe_text = html_escape(ctx->arena, text);
          arena_string_append(ctx->arena, &ctx->text_content, &ctx->text_len,
                              safe_text);
        }
      }
    }
  } else if (filename != NULL) {
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", ctx->final_assets_dir,
             filename);

    char *relative_filepath =
        arena_asprintf(ctx->arena, "%s/%s", ctx->folder_name, filename);

    GMimeDataWrapper *content = g_mime_part_get_content(mime_part);
    if (content) {
      GMimeStream *file_stream = g_mime_stream_file_open(filepath, "wb", NULL);
      if (file_stream) {
        g_mime_data_wrapper_write_to_stream(content, file_stream);
        g_object_unref(file_stream);
      }
    }

    const char *cid = g_mime_object_get_header(part, "Content-ID");
    if (cid && disposition && strcasecmp(disposition, "inline") == 0) {
      char *cid_clean = arena_strdup(ctx->arena, cid);
      char *src = cid_clean, *dst = cid_clean;
      while (*src) {
        if (*src != '<' && *src != '>')
          *dst++ = *src;
        src++;
      }
      *dst = '\0';

      g_hash_table_insert(ctx->inline_images, g_strdup(cid_clean),
                          g_strdup(relative_filepath));
    } else {
      char *safe_filename = html_escape(ctx->arena, filename);
      char *safe_relative_path = html_escape(ctx->arena, relative_filepath);

      arena_string_append_printf(ctx->arena, &ctx->attachments_html,
                                 &ctx->attach_len,
                                 "<li><a href=\"%s\" download>%s</a></li>\n",
                                 safe_relative_path, safe_filename);
    }
  }
}

static void replace_cid_references(Arena *arena, char **html, size_t *html_len,
                                   const char *cid, const char *local_path) {
  if (!html || !*html || !cid || !local_path)
    return;

  char target[512];
  snprintf(target, sizeof(target), "src=\"cid:%s\"", cid);
  char replacement[512];
  snprintf(replacement, sizeof(replacement), "src=\"%s\"", local_path);

  char *pos;
  while ((pos = strstr(*html, target)) != NULL) {
    size_t offset = pos - *html;
    size_t target_len = strlen(target);
    size_t rep_len = strlen(replacement);
    size_t tail_len = strlen(pos + target_len);

    char *new_html = arena_alloc(arena, offset + rep_len + tail_len + 1);
    memcpy(new_html, *html, offset);
    memcpy(new_html + offset, replacement, rep_len);
    memcpy(new_html + offset + rep_len, pos + target_len, tail_len + 1);

    *html = new_html;
    *html_len = offset + rep_len + tail_len;
  }
}

static char *convert_text_to_html_fallback(Arena *arena, const char *text) {
  if (!text || *text == '\0')
    return arena_strdup(arena, "<p></p>");

  char *escaped = html_escape(arena, text);
  char *sb = arena_strdup(arena, "<p>");
  size_t sb_len = 3;

  const char *p = escaped;
  while (*p) {
    if (*p == '\n') {
      if (*(p + 1) == '\n') {
        arena_string_append(arena, &sb, &sb_len, "</p>\n<p>");
        p++;
      } else {
        arena_string_append(arena, &sb, &sb_len, "<br>\n");
      }
    } else if (*p != '\r') {
      char temp[2] = {*p, '\0'};
      arena_string_append(arena, &sb, &sb_len, temp);
    }
    p++;
  }

  arena_string_append(arena, &sb, &sb_len, "</p>");
  return sb;
}

static char *find_tag_case_insensitive(const char *haystack, const char *tag) {
  if (!haystack || !tag)
    return NULL;
  size_t tag_len = strlen(tag);
  while (*haystack) {
    if (strncasecmp(haystack, tag, tag_len) == 0)
      return (char *)haystack;
    haystack++;
  }
  return NULL;
}

static char *extraire_et_fusionner_styles(Arena *arena, const char *html_source,
                                          char **css_extrait) {
  if (!html_source || !css_extrait) {
    if (css_extrait)
      *css_extrait = arena_strdup(arena, "");
    return arena_strdup(arena, "");
  }

  size_t src_len = strlen(html_source);
  char *html_nettoye = arena_alloc(arena, src_len + 1);
  char *css_buf = arena_alloc(arena, src_len + 1);

  size_t h_idx = 0, c_idx = 0, i = 0;
  css_buf[0] = '\0';

  while (i < src_len) {
    if (i < src_len - 6 && strncasecmp(&html_source[i], "<style", 6) == 0) {
      i += 6;
      while (i < src_len && html_source[i] != '>')
        i++;
      if (i < src_len)
        i++;

      char *style_end = find_tag_case_insensitive(&html_source[i], "</style>");
      if (style_end) {
        size_t css_chunk_len = style_end - &html_source[i];
        memcpy(&css_buf[c_idx], &html_source[i], css_chunk_len);
        c_idx += css_chunk_len;
        css_buf[c_idx++] = '\n';
        i += css_chunk_len + 8;
        continue;
      }
    }
    html_nettoye[h_idx++] = html_source[i++];
  }

  html_nettoye[h_idx] = '\0';
  css_buf[c_idx] = '\0';
  *css_extrait = css_buf;
  return html_nettoye;
}

static int preparer_repertoires(Arena *arena, const char *eml_path,
                                const char *output_html_path,
                                const char *output_dir, char *final_assets_dir,
                                size_t max_len, char **folder_name_out,
                                char **path_copy_out) {
  if (!eml_path || !output_html_path || !final_assets_dir || max_len == 0 ||
      !folder_name_out || !path_copy_out) {
    return 0;
  }

  if (!*eml_path || access(eml_path, R_OK) != 0) {
    fprintf(stderr, "Erreur : Fichier source invalide ou illisible.\n");
    return 0;
  }

  char *path_copy = arena_strdup(arena, output_html_path);
  char *html_parent_dir = dirname(path_copy);

  if (access(html_parent_dir, W_OK) != 0) {
    fprintf(stderr, "Erreur : Répertoire parent non accessible en écriture.\n");
    return 0;
  }

  *folder_name_out = output_dir
                         ? arena_strdup(arena, basename((char *)output_dir))
                         : arena_strdup(arena, "eml_assets");
  snprintf(final_assets_dir, max_len, "%s/%s", html_parent_dir,
           *folder_name_out);
  mkdir(final_assets_dir, 0755);

  *path_copy_out = path_copy;
  return 1;
}

static char *generer_titre_page(Arena *arena, GMimeMessage *message) {
  if (!message)
    return arena_strdup(arena, "");

  char *meta_date = parse_email_date_to_french(arena, message);
  char *meta_from = get_clean_header_value(arena, message, "From");
  char *meta_subject = get_clean_header_value(arena, message, "Subject");

  if (!meta_subject || strlen(meta_subject) == 0) {
    meta_subject = arena_strdup(arena, "Sans titre");
  }

  char *page_title =
      arena_asprintf(arena, "%s - %s - %s", meta_date, meta_from, meta_subject);
  return html_escape(arena, page_title);
}

static char *extraire_corps_email(Arena *arena, GMimeMessage *message,
                                  ParserContext *ctx, int *is_pure_html) {
  if (!message || !ctx || !is_pure_html)
    return arena_strdup(arena, "");

  g_mime_message_foreach(message, process_mime_part, ctx);

  char *final_body = arena_strdup(arena, "");
  size_t final_body_len = 0;
  *is_pure_html = 0;

  if (ctx->html_len > 0) {
    arena_string_append(arena, &final_body, &final_body_len, ctx->html_content);
    *is_pure_html = 1;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, ctx->inline_images);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      replace_cid_references(arena, &final_body, &final_body_len,
                             (const char *)key, (const char *)value);
    }
  } else {
    char *fallback = convert_text_to_html_fallback(arena, ctx->text_content);
    arena_string_append(arena, &final_body, &final_body_len, fallback);
  }

  return final_body;
}

static char *generer_section_pieces_jointes(Arena *arena, ParserContext *ctx) {
  if (!ctx)
    return arena_strdup(arena, "");

  char *attachments_section = arena_strdup(arena, "");
  size_t attach_sec_len = 0;

  if (ctx->attach_len > 0) {
    arena_string_append(arena, &attachments_section, &attach_sec_len,
                        "<hr><h3>Pièces jointes :</h3><ul>\n");
    arena_string_append(arena, &attachments_section, &attach_sec_len,
                        ctx->attachments_html);
    arena_string_append(arena, &attachments_section, &attach_sec_len,
                        "</ul>\n");
  }

  return attachments_section;
}

static char *generer_html_fallback(Arena *arena, const char *title,
                                   const char *styles, const char *subject,
                                   const char *headers, const char *body,
                                   const char *attachments) {
  if (!title || !styles || !subject || !headers || !body || !attachments)
    return arena_strdup(arena, "");

  return arena_asprintf(
      arena,
      "<!DOCTYPE html>\n<html lang=\"fr\">\n<head>\n<meta charset=\"UTF-8\">\n"
      "<title>%s</title>\n%s</head>\n"
      "<body>\n<div "
      "class=\"eml-header-block\">\n<h2>%s</h2>\n<p>%s</p>\n<hr>\n</div>\n"
      "<div class=\"email-body\">\n%s\n</div>\n<div "
      "class=\"eml-attachments\">\n%s\n</div>\n</body>\n</html>",
      title, styles, subject, headers, body, attachments);
}

static char *assembler_html_final(Arena *arena, const char *final_body,
                                  const char *safe_page_title,
                                  const char *custom_styles,
                                  const char *subject, const char *headers_html,
                                  const char *attachments_section) {
  if (!final_body || !safe_page_title || !custom_styles || !subject ||
      !headers_html || !attachments_section)
    return arena_strdup(arena, "");

  char *body_tag = find_tag_case_insensitive(final_body, "<body");
  if (!body_tag)
    return NULL;

  char *body_tag_close = strchr(body_tag, '>');
  if (!body_tag_close)
    return NULL;
  body_tag_close++;

  size_t head_part_len = body_tag_close - final_body;
  size_t total_needed = head_part_len + strlen(custom_styles) +
                        strlen(subject) + strlen(headers_html) +
                        strlen(body_tag_close) + strlen(attachments_section) +
                        strlen(safe_page_title) + 1500;

  char *full_html = arena_alloc(arena, total_needed);
  memcpy(full_html, final_body, head_part_len);
  full_html[head_part_len] = '\0';

  char *existing_title_open = find_tag_case_insensitive(full_html, "<title>");
  char *existing_title_close = find_tag_case_insensitive(full_html, "</title>");
  if (existing_title_open && existing_title_close &&
      existing_title_close > existing_title_open) {
    size_t open_offset = (existing_title_open + 7) - full_html;
    size_t close_offset = existing_title_close - full_html;

    char *temp_title_buf = arena_alloc(arena, total_needed);
    memcpy(temp_title_buf, full_html, open_offset);
    strcpy(temp_title_buf + open_offset, safe_page_title);
    strcat(temp_title_buf, full_html + close_offset);
    full_html = temp_title_buf;
  } else {
    char *head_tag = find_tag_case_insensitive(full_html, "</head>");
    if (head_tag) {
      size_t offset = head_tag - full_html;
      char *temp_html = arena_alloc(arena, total_needed);
      memcpy(temp_html, full_html, offset);
      sprintf(temp_html + offset, "<title>%s</title>\n%s", safe_page_title,
              custom_styles);
      strcat(temp_html, head_tag);
      full_html = temp_html;
    }
  }

  if (!find_tag_case_insensitive(full_html, "</head>")) {
    char *temp_html = arena_alloc(arena, total_needed);
    sprintf(temp_html, "<title>%s</title>\n%s%s", safe_page_title,
            custom_styles, full_html);
    full_html = temp_html;
  }

  char *dest = full_html + strlen(full_html);
  sprintf(dest,
          "\n<div "
          "class=\"eml-header-block\">\n<h2>%s</h2>\n<p>%s</p>\n<hr>\n</div>\n",
          subject, headers_html);
  strcat(full_html, body_tag_close);

  char *body_close_tag = find_tag_case_insensitive(full_html, "</body>");
  if (body_close_tag) {
    size_t tail_offset = body_close_tag - full_html;
    char *final_buffer = arena_alloc(arena, total_needed);
    memcpy(final_buffer, full_html, tail_offset);
    sprintf(final_buffer + tail_offset,
            "<div class=\"eml-attachments\">\n%s\n</div>\n</body>\n</html>",
            attachments_section);
    full_html = final_buffer;
  } else {
    size_t current_full_html_len = strlen(full_html);
    arena_string_append_printf(arena, &full_html, &current_full_html_len,
                               "<div class=\"eml-attachments\">\n%s\n</div>",
                               attachments_section);
  }

  return full_html;
}

void eml_to_html(const char *eml_path, const char *output_html_path,
                 const char *output_dir) {
  if (!eml_path || !output_html_path)
    return;

  prctl(PR_SET_NAME, PROGRAMME_NAME, 0, 0, 0);

  Arena *arena = arena_create(16 * 1024 * 1024);

  char final_assets_dir[1024];
  char *folder_name = NULL;
  char *path_copy = NULL;

  if (!preparer_repertoires(arena, eml_path, output_html_path, output_dir,
                            final_assets_dir, sizeof(final_assets_dir),
                            &folder_name, &path_copy)) {
    arena_destroy(arena);
    return;
  }

  GMimeStream *stream = g_mime_stream_file_open(eml_path, "r", NULL);
  if (!stream) {
    arena_destroy(arena);
    return;
  }

  GMimeParser *parser = g_mime_parser_new();
  EmailHeaders hdrs = {0};

  extraire_et_exporter_headers(arena, stream, parser, output_html_path, &hdrs);

  ParserContext ctx = {.html_content = arena_strdup(arena, ""),
                       .html_len = 0,
                       .text_content = arena_strdup(arena, ""),
                       .text_len = 0,
                       .attachments_html = arena_strdup(arena, ""),
                       .attach_len = 0,
                       .inline_images = g_hash_table_new_full(
                           g_str_hash, g_str_equal, g_free, g_free),
                       .folder_name = folder_name,
                       .final_assets_dir = final_assets_dir,
                       .arena = arena};

  int is_pure_html = 0;
  char *final_body =
      extraire_corps_email(arena, hdrs.message, &ctx, &is_pure_html);
  char *attachments_section = generer_section_pieces_jointes(arena, &ctx);

  char *css_interne = NULL;
  final_body = extraire_et_fusionner_styles(arena, final_body, &css_interne);

  char *custom_styles = arena_asprintf(
      arena,
      "<style>\n"
      "  .eml-header-block { font-family: Arial, sans-serif; line-height: 1.6; "
      "margin: 20px; color: #333; }\n"
      "  .eml-header-block h2 { margin-top: 0; }\n"
      "  .email-body p { margin: 1em 0; }\n"
      "  .eml-attachments h3 { color: #555; border-bottom: 1px solid #ccc; "
      "padding-bottom: 5px; }\n"
      "  .eml-attachments ul { list-style-type: none; padding-left: 0; }\n"
      "  .eml-attachments li { margin: 5px 0; }\n"
      "  .eml-attachments a { color: #0066cc; text-decoration: none; }\n"
      "  .eml-attachments a:hover { text-decoration: underline; }\n"
      "  /* Styles extraits de l'e-mail d'origine : */\n%s"
      "</style>\n",
      css_interne);

  char *full_html = NULL;
  if (is_pure_html) {
    full_html = assembler_html_final(arena, final_body, hdrs.safe_page_title,
                                     custom_styles, hdrs.subject,
                                     hdrs.headers_html, attachments_section);
  }

  if (!full_html) {
    full_html = generer_html_fallback(
        arena, hdrs.safe_page_title, custom_styles, hdrs.subject,
        hdrs.headers_html, final_body, attachments_section);
  }

  if (!hdrs.message) {
    fprintf(stderr, "Erreur : Échec du parsing du message GMime.\n");
    g_hash_table_destroy(ctx.inline_images);
    g_object_unref(parser);
    g_object_unref(stream);
    arena_destroy(arena);
    return;
  }

  GumboOutput *gumbo_out = gumbo_parse(full_html);
  nettoyer_arbre(gumbo_out->root);

  FILE *f = fopen(output_html_path, "wb");
  if (f) {
    html_prettifier_to_file(arena, gumbo_out->root, 0, f);
    fclose(f);
  } else {
    fprintf(stderr, "Erreur : Impossible d'ouvrir le fichier de sortie %s\n",
            output_html_path);
  }

  if (hdrs.from_str)
    g_free(hdrs.from_str);
  if (hdrs.to_str)
    g_free(hdrs.to_str);
  if (hdrs.cc_str)
    g_free(hdrs.cc_str);
  if (hdrs.bcc_str)
    g_free(hdrs.bcc_str);

  gumbo_destroy_output(&kGumboDefaultOptions, gumbo_out);
  g_hash_table_destroy(ctx.inline_images);
  g_object_unref(hdrs.message);
  g_object_unref(parser);
  g_object_unref(stream);

  arena_destroy(arena);
}

int main(int argc, char *argv[]) {
  setlocale(LC_ALL, "C.UTF-8");
  g_mime_init();

  if (argc < 3) {
    printf("Usage : %s <eml_path> <output_html> [<assets_dir>]\n", argv[0]);
    g_mime_shutdown();
    return 1;
  }

  eml_to_html(argv[1], argv[2], (argc >= 4) ? argv[3] : NULL);
  g_mime_shutdown();
  return 0;
}