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

#define PROGRAMME_NAME "eml_to_txt"

// ============================================================================
// ENGINE DE L'ARENA MEMOIRE
// ============================================================================
typedef struct {
  char *buffer;
  size_t capacity;
  size_t offset;
} Arena;

Arena *arena_create(size_t capacity) {
  Arena *arena = malloc(sizeof(Arena));
  if (!arena) {
    fprintf(stderr,
            "Erreur critique : Échec de l'allocation de la structure Arena\n");
    exit(EXIT_FAILURE);
  }

  arena->buffer = malloc(capacity);
  if (!arena->buffer) {
    fprintf(stderr,
            "Erreur critique : Échec de l'allocation du buffer de l'Arena (%zu "
            "octets)\n",
            capacity);
    free(arena);
    exit(EXIT_FAILURE);
  }

  arena->capacity = capacity;
  arena->offset = 0;
  return arena;
}

void *arena_alloc(Arena *arena, size_t size) {
  if (!arena || !arena->buffer) {
    fprintf(stderr,
            "Erreur : Tentative d'allocation sur une Arena invalide ou NULL\n");
    return NULL;
  }

  size_t aligned_size = (size + 7) & ~7;
  if (arena->offset + aligned_size > arena->capacity) {
    fprintf(stderr, "Erreur critique : Arena saturée !\n");
    exit(EXIT_FAILURE);
  }
  void *ptr = &arena->buffer[arena->offset];
  arena->offset += aligned_size;
  return ptr;
}

char *arena_strdup(Arena *arena, const char *src) {
  if (!arena || !src)
    return NULL;

  size_t len = strlen(src);
  char *dst = arena_alloc(arena, len + 1);
  if (dst)
    memcpy(dst, src, len + 1);
  return dst;
}

char *arena_asprintf(Arena *arena, const char *format, ...) {
  if (!arena || !format)
    return NULL;

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

void arena_string_append(Arena *arena, char **dest, size_t *current_len,
                         const char *src) {
  if (!arena || !dest || !current_len || !src)
    return;

  size_t src_len = strlen(src);
  char *new_buf = arena_alloc(arena, *current_len + src_len + 1);
  if (!new_buf)
    return;

  if (*current_len > 0 && *dest) {
    memcpy(new_buf, *dest, *current_len);
  }
  memcpy(new_buf + *current_len, src, src_len + 1);
  *dest = new_buf;
  *current_len += src_len;
}

void arena_string_append_printf(Arena *arena, char **dest, size_t *current_len,
                                const char *format, ...) {
  if (!arena || !dest || !current_len || !format)
    return;

  va_list args;
  va_start(args, format);
  size_t needed = vsnprintf(NULL, 0, format, args) + 1;
  va_end(args);

  char *formatted = arena_alloc(arena, needed);
  if (!formatted)
    return;

  va_start(args, format);
  vsnprintf(formatted, needed, format, args);
  va_end(args);

  arena_string_append(arena, dest, current_len, formatted);
}

void arena_destroy(Arena *arena) {
  if (!arena)
    return;
  if (arena->buffer) {
    free(arena->buffer);
  }
  free(arena);
}

// ============================================================================
// STRUCTURES ET LOGIQUE DE NETTOYAGE DES ESPACES UNICODE
// ============================================================================
typedef struct {
  unsigned char bytes[3];
  int length;
} EspaceUnicode;

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

static int get_match_length(const unsigned char *ptr, size_t remaining) {
  if (!ptr || remaining == 0)
    return 0;
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

static char *nettoyer_espaces_texte(Arena *arena, const char *src) {
  if (!arena || !src)
    return NULL;

  size_t src_len = strlen(src);
  char *dst = arena_alloc(arena, src_len + 1);
  if (!dst)
    return NULL;

  char *dst_ptr = dst;
  const unsigned char *src_ptr = (const unsigned char *)src;

  size_t i = 0;
  while (i < src_len) {
    int skip = get_match_length(&src_ptr[i], src_len - i);
    if (skip > 0) {
      *dst_ptr++ = 0x20;
      i += skip;
    } else {
      *dst_ptr++ = src[i];
      i++;
    }
  }
  *dst_ptr = '\0';
  return dst;
}

// ============================================================================
// STRUCTURES DU PARSER
// ============================================================================
typedef struct {
  char *plain_content;
  size_t plain_len;
  char *html_content;
  size_t html_len;
  char *attachments_txt;
  size_t attach_len;
  char *urls_txt;
  size_t urls_len;
  Arena *arena;
} ParserContext;

// ============================================================================
// FONCTIONS UTILITAIRES, JSON ET EXTRACTION DE LIENS
// ============================================================================
static char *echapper_json_string(Arena *arena, const char *str) {
  if (!arena)
    return NULL;
  if (!str)
    return arena_strdup(arena, "");

  size_t len = 0;
  for (const char *p = str; *p; p++) {
    if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t')
      len += 2;
    else
      len++;
  }
  char *res = arena_alloc(arena, len + 1);
  if (!res)
    return NULL;

  char *dst = res;
  for (const char *p = str; *p; p++) {
    switch (*p) {
    case '"':
      *dst++ = '\\';
      *dst++ = '"';
      break;
    case '\\':
      *dst++ = '\\';
      *dst++ = '\\';
      break;
    case '\n':
      *dst++ = '\\';
      *dst++ = 'n';
      break;
    case '\r':
      *dst++ = '\\';
      *dst++ = 'r';
      break;
    case '\t':
      *dst++ = '\\';
      *dst++ = 't';
      break;
    default:
      *dst++ = *p;
      break;
    }
  }
  *dst = '\0';
  return res;
}

static void generer_headers_json(const char *output_dir, const char *msg_id,
                                 const char *subject, const char *from,
                                 const char *to, const char *cc,
                                 const char *bcc, const char *raw_date,
                                 const char *in_reply_to,
                                 const char *references, Arena *arena) {
  if (!output_dir || !arena) {
    fprintf(stderr,
            "Erreur : Paramètres invalides fournis à generer_headers_json\n");
    return;
  }

  char json_path[1024];
  snprintf(json_path, sizeof(json_path), "%s/headers.json", output_dir);

  FILE *f = fopen(json_path, "wb");
  if (!f) {
    fprintf(stderr, "Erreur : Impossible de créer le fichier %s\n", json_path);
    return;
  }

  fprintf(f, "{\n");
  fprintf(f, "  \"Message-ID\": \"%s\",\n",
          echapper_json_string(arena, msg_id));
  fprintf(f, "  \"Subject\": \"%s\",\n", echapper_json_string(arena, subject));
  fprintf(f, "  \"From\": \"%s\",\n", echapper_json_string(arena, from));
  fprintf(f, "  \"To\": \"%s\",\n", echapper_json_string(arena, to));
  fprintf(f, "  \"Cc\": \"%s\",\n", echapper_json_string(arena, cc));
  fprintf(f, "  \"Bcc\": \"%s\",\n", echapper_json_string(arena, bcc));
  fprintf(f, "  \"Date\": \"%s\",\n", echapper_json_string(arena, raw_date));
  fprintf(f, "  \"In-Reply-To\": \"%s\",\n",
          echapper_json_string(arena, in_reply_to));
  fprintf(f, "  \"References\": \"%s\"\n",
          echapper_json_string(arena, references));
  fprintf(f, "}\n");
  fclose(f);
}

static void extraire_urls_depuis_texte(Arena *arena, const char *text,
                                       char **urls_txt, size_t *urls_len) {
  if (!arena || !text || !urls_txt || !urls_len)
    return;

  const char *p = text;
  while (*p) {
    const char *p1 = strstr(p, "http://");
    const char *p2 = strstr(p, "https://");
    const char *next = NULL;

    if (p1 && p2) {
      next = (p1 < p2) ? p1 : p2;
    } else {
      next = p1 ? p1 : p2;
    }

    if (!next)
      break;

    p = next;
    const char *start = p;
    while (*p && !isspace((unsigned char)*p) && *p != '"' && *p != '\'' &&
           *p != '<' && *p != '>') {
      p++;
    }
    size_t len = p - start;
    if (len > 0) {
      char *url = arena_alloc(arena, len + 1);
      if (url) {
        memcpy(url, start, len);
        url[len] = '\0';
        arena_string_append_printf(arena, urls_txt, urls_len, "%s\n", url);
      }
    }
  }
}

static void extraire_gumbo_texte_et_urls(GumboNode *node, ParserContext *ctx,
                                         char **corps_txt, size_t *corps_len) {
  if (!node || !ctx || !corps_txt || !corps_len)
    return;

  if (node->type == GUMBO_NODE_TEXT) {
    arena_string_append(ctx->arena, corps_txt, corps_len, node->v.text.text);
    return;
  }

  if (node->type != GUMBO_NODE_ELEMENT)
    return;

  GumboElement *element = &node->v.element;
  if (element->tag == GUMBO_TAG_SCRIPT || element->tag == GUMBO_TAG_STYLE)
    return;

  if (element->tag == GUMBO_TAG_A) {
    GumboAttribute *href = gumbo_get_attribute(&element->attributes, "href");
    if (href && href->value &&
        (strncmp(href->value, "http://", 7) == 0 ||
         strncmp(href->value, "https://", 8) == 0)) {
      arena_string_append_printf(ctx->arena, &ctx->urls_txt, &ctx->urls_len,
                                 "%s\n", href->value);
    }
  }

  GumboVector *children = &element->children;
  if (children && children->data) {
    for (unsigned int i = 0; i < children->length; ++i) {
      extraire_gumbo_texte_et_urls((GumboNode *)children->data[i], ctx,
                                   corps_txt, corps_len);
    }
  }

  if (element->tag == GUMBO_TAG_P || element->tag == GUMBO_TAG_BR ||
      element->tag == GUMBO_TAG_DIV || element->tag == GUMBO_TAG_TR) {
    arena_string_append(ctx->arena, corps_txt, corps_len, "\n");
  }
}

static char *parse_email_date_to_french(Arena *arena, GMimeMessage *message) {
  if (!arena)
    return NULL;
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

// ============================================================================
// TRAITEMENT DES PIECES ET DU CONTENU MIME
// ============================================================================
static void process_mime_part(G_GNUC_UNUSED GMimeObject *parent,
                              GMimeObject *part, gpointer user_data) {
  if (!part || !user_data)
    return;
  ParserContext *ctx = (ParserContext *)user_data;
  if (!ctx->arena)
    return;

  GMimeContentType *content_type = g_mime_object_get_content_type(part);
  const char *disposition = g_mime_object_get_disposition(part);

  if (GMIME_IS_PART(part)) {
    GMimePart *mime_part = GMIME_PART(part);
    const char *filename = g_mime_part_get_filename(mime_part);

    if (filename == NULL &&
        (!disposition || strcasecmp(disposition, "attachment") != 0)) {
      if (GMIME_IS_TEXT_PART(part)) {
        GMimeTextPart *text_part = GMIME_TEXT_PART(part);
        char *gm_text = g_mime_text_part_get_text(text_part);
        if (gm_text) {
          char *text = arena_strdup(ctx->arena, gm_text);
          g_free(gm_text); // Libération du texte GMime
          if (text) {
            if (content_type &&
                g_mime_content_type_is_type(content_type, "text", "plain")) {
              arena_string_append(ctx->arena, &ctx->plain_content,
                                  &ctx->plain_len, text);
            } else if (content_type && g_mime_content_type_is_type(
                                           content_type, "text", "html")) {
              arena_string_append(ctx->arena, &ctx->html_content,
                                  &ctx->html_len, text);
            }
          }
        }
      }
    } else if (filename != NULL) {
      size_t size = 0;
      GMimeDataWrapper *content = g_mime_part_get_content(mime_part);
      if (content) {
        GMimeStream *stream = g_mime_data_wrapper_get_stream(content);
        if (stream) {
          ssize_t len = g_mime_stream_length(stream);
          if (len > 0)
            size = (size_t)len;
        }
      }
      arena_string_append_printf(ctx->arena, &ctx->attachments_txt,
                                 &ctx->attach_len, "- %s (%zu octets)\n",
                                 filename, size);
    }
  }
}

// ============================================================================
// COEUR DE L'APPLICATION
// ============================================================================
void eml_to_txt(const char *eml_path, const char *output_txt_path) {
  if (!eml_path || !output_txt_path) {
    fprintf(stderr,
            "Erreur : Chemins d'entrée ou de sortie invalides (NULL).\n");
    return;
  }

  prctl(PR_SET_NAME, PROGRAMME_NAME, 0, 0, 0);
  Arena *arena = arena_create(16 * 1024 * 1024);

  GMimeStream *stream = g_mime_stream_file_open(eml_path, "r", NULL);
  if (!stream) {
    fprintf(stderr, "Erreur : Impossible d'ouvrir %s\n", eml_path);
    arena_destroy(arena);
    return;
  }

  GMimeParser *parser = g_mime_parser_new();
  if (!parser) {
    fprintf(stderr, "Erreur : Échec de l'allocation du parser GMime.\n");
    g_object_unref(stream);
    arena_destroy(arena);
    return;
  }

  g_mime_parser_init_with_stream(parser, stream);
  GMimeMessage *message = g_mime_parser_construct_message(parser, NULL);

  if (!message) {
    fprintf(stderr, "Erreur : Échec du parsing GMime.\n");
    g_object_unref(parser);
    g_object_unref(stream);
    arena_destroy(arena);
    return;
  }

  const char *subject = g_mime_message_get_subject(message);
  if (!subject)
    subject = "";

  InternetAddressList *from_list = g_mime_message_get_from(message);
  InternetAddressList *to_list = g_mime_message_get_to(message);
  InternetAddressList *cc_list = g_mime_message_get_cc(message);
  InternetAddressList *bcc_list = g_mime_message_get_bcc(message);

  char *from_str = from_list
                       ? internet_address_list_to_string(from_list, NULL, FALSE)
                       : NULL;
  char *to_str =
      to_list ? internet_address_list_to_string(to_list, NULL, FALSE) : NULL;
  char *cc_str =
      cc_list ? internet_address_list_to_string(cc_list, NULL, FALSE) : NULL;
  char *bcc_str =
      bcc_list ? internet_address_list_to_string(bcc_list, NULL, FALSE) : NULL;

  const char *msg_id = g_mime_message_get_message_id(message);
  if (!msg_id)
    msg_id = "";

  const char *raw_date =
      g_mime_object_get_header(GMIME_OBJECT(message), "Date");
  if (!raw_date)
    raw_date = "";
  const char *in_reply_to =
      g_mime_object_get_header(GMIME_OBJECT(message), "In-Reply-To");
  if (!in_reply_to)
    in_reply_to = "";
  const char *references =
      g_mime_object_get_header(GMIME_OBJECT(message), "References");
  if (!references)
    references = "";

  char *french_date = parse_email_date_to_french(arena, message);

  char *txt_path_copy = arena_strdup(arena, output_txt_path);
  char *output_dir = txt_path_copy ? dirname(txt_path_copy) : ".";

  generer_headers_json(output_dir, msg_id, subject, from_str ? from_str : "",
                       to_str ? to_str : "", cc_str ? cc_str : "",
                       bcc_str ? bcc_str : "", raw_date, in_reply_to,
                       references, arena);

  ParserContext ctx = {.plain_content = arena_strdup(arena, ""),
                       .plain_len = 0,
                       .html_content = arena_strdup(arena, ""),
                       .html_len = 0,
                       .attachments_txt = arena_strdup(arena, ""),
                       .attach_len = 0,
                       .urls_txt = arena_strdup(arena, ""),
                       .urls_len = 0,
                       .arena = arena};

  g_mime_message_foreach(message, process_mime_part, &ctx);

  char *corps_final = arena_strdup(arena, "");
  size_t corps_final_len = 0;

  if (ctx.plain_len > 0 && ctx.plain_content) {
    arena_string_append(arena, &corps_final, &corps_final_len,
                        ctx.plain_content);
    extraire_urls_depuis_texte(arena, corps_final, &ctx.urls_txt,
                               &ctx.urls_len);
  } else if (ctx.html_len > 0 && ctx.html_content) {
    GumboOutput *gumbo_out = gumbo_parse(ctx.html_content);
    if (gumbo_out) {
      extraire_gumbo_texte_et_urls(gumbo_out->root, &ctx, &corps_final,
                                   &corps_final_len);
      gumbo_destroy_output(&kGumboDefaultOptions, gumbo_out);
    }
  }

  if (corps_final_len > 0 && corps_final) {
    corps_final = nettoyer_espaces_texte(arena, corps_final);
  }

  FILE *f = fopen(output_txt_path, "wb");
  if (f) {
    fprintf(f, "Date : %s\n", french_date ? french_date : "");
    if (from_str)
      fprintf(f, "De : %s\n", from_str);
    if (to_str)
      fprintf(f, "À : %s\n", to_str);
    if (cc_str)
      fprintf(f, "Copie : %s\n", cc_str);
    if (bcc_str)
      fprintf(f, "Copie Cachée : %s\n", bcc_str);
    fprintf(f, "Sujet : %s\n", subject);
    if (msg_id && strlen(msg_id) > 0)
      fprintf(f, "Identifiant : %s\n", msg_id);

    fprintf(f, "---\n");
    fprintf(f, "Pièces jointes :\n");
    if (ctx.attach_len > 0 && ctx.attachments_txt)
      fprintf(f, "%s", ctx.attachments_txt);
    else
      fprintf(f, "Aucune\n");

    fprintf(f, "---\n");
    fprintf(f, "%s\n", corps_final ? corps_final : "");

    fprintf(f, "---\n");
    fprintf(f, "URLs détectées :\n");
    if (ctx.urls_len > 0 && ctx.urls_txt)
      fprintf(f, "%s", ctx.urls_txt);
    else
      fprintf(f, "Aucune\n");

    fclose(f);
    printf("Export texte brut réussi : %s\n", output_txt_path);
  } else {
    fprintf(stderr, "Erreur : Impossible d'écrire le fichier %s\n",
            output_txt_path);
  }

  if (from_str)
    g_free(from_str);
  if (to_str)
    g_free(to_str);
  if (cc_str)
    g_free(cc_str);
  if (bcc_str)
    g_free(bcc_str);

  g_object_unref(message);
  g_object_unref(parser);
  g_object_unref(stream);

  arena_destroy(arena);
}

int main(int argc, char *argv[]) {
  setlocale(LC_ALL, "C.UTF-8");
  g_mime_init();

  if (argc < 3 || !argv[1] || !argv[2]) {
    printf("Usage : %s <eml_path> <output_txt>\n",
           argv[0] ? argv[0] : PROGRAMME_NAME);
    g_mime_shutdown();
    return 1;
  }

  eml_to_txt(argv[1], argv[2]);
  g_mime_shutdown();
  return 0;
}