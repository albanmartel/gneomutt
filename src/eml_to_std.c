#include <ctype.h>
#include <gmime/gmime.h>
#include <gumbo.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

#define PROGRAMME_NAME "eml_to_std"

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
  if (!arena || !arena->buffer)
    return NULL;
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

// ============================================================================
// LOGIQUE DE NETTOYAGE DES ESPACES UNICODE ET DES SAUTS DE LIGNES
// ============================================================================
typedef struct {
  unsigned char bytes[3];
  int length;
} EspaceUnicode;

static const EspaceUnicode espaces_table[] = {
    {{0xC2, 0xA0, 0x00}, 2}, {{0xE1, 0x9A, 0x80}, 3}, {{0xE2, 0x80, 0x80}, 3},
    {{0xE2, 0x80, 0x81}, 3}, {{0xE2, 0x80, 0x82}, 3}, {{0xE2, 0x80, 0x83}, 3},
    {{0xE2, 0x80, 0x84}, 3}, {{0xE2, 0x80, 0x85}, 3}, {{0xE2, 0x80, 0x86}, 3},
    {{0xE2, 0x80, 0x87}, 3}, {{0xE2, 0x80, 0x88}, 3}, {{0xE2, 0x80, 0x89}, 3},
    {{0xE2, 0x80, 0x8A}, 3}, {{0xE2, 0x80, 0xAF}, 3}, {{0xE2, 0x81, 0x9F}, 3},
    {{0xE3, 0x80, 0x80}, 3}};
#define NB_ESPACES (sizeof(espaces_table) / sizeof(EspaceUnicode))

static int get_match_length(const unsigned char *ptr, size_t remaining) {
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

  // 1. Remplacer d'abord tous les espaces UTF-8 exotiques par de l'espace
  // standard 0x20
  while (i < src_len) {
    int skip = get_match_length(&src_ptr[i], src_len - i);
    if (skip > 0) {
      *dst_ptr++ = ' ';
      i += skip;
    } else {
      *dst_ptr++ = src[i];
      i++;
    }
  }
  *dst_ptr = '\0';

  // 2. Deuxième passe CORRIGÉE : Supprimer les espaces de début de ligne SANS
  // perdre les lignes vides (\n\n)
  char *final_dst = arena_alloc(arena, src_len + 1);
  char *f_ptr = final_dst;
  const char *p = dst;
  int debut_de_ligne = 1;

  while (*p != '\0') {
    if (debut_de_ligne) {
      // Sauter les espaces et tabulations au tout début de chaque ligne
      while (*p == ' ' || *p == '\t' || *p == '\r') {
        p++;
      }
      debut_de_ligne = 0;
    }

    if (*p == '\0')
      break;

    // On recopie le caractère actuel
    *f_ptr++ = *p;

    // Si on croise un retour à la ligne, la ligne suivante recommencera à zéro
    if (*p == '\n') {
      debut_de_ligne = 1;
    }
    p++;
  }
  *f_ptr = '\0';

  return final_dst;
}

static char *condenser_sauts_lignes(Arena *arena, const char *src) {
  if (!arena || !src)
    return NULL;
  size_t src_len = strlen(src);
  char *dst = arena_alloc(arena, src_len + 1);
  if (!dst)
    return NULL;

  char *dst_ptr = dst;
  size_t newline_count = 0;

  for (size_t i = 0; i < src_len; i++) {
    if (src[i] == '\n') {
      newline_count++;
      if (newline_count <= 2) {
        *dst_ptr++ = src[i];
      }
    } else {
      newline_count = 0;
      *dst_ptr++ = src[i];
    }
  }
  *dst_ptr = '\0';
  return dst;
}

// ============================================================================
// PARSING GUMBO DIRECT
// ============================================================================
typedef struct {
  char *urls_txt;
  size_t urls_len;
  char *temp_eml_text;
  Arena *arena;
} ParserContext;

static void extraire_gumbo_texte_et_urls(GumboNode *node, ParserContext *ctx,
                                         char **corps_txt, size_t *corps_len) {
  if (!node)
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
  for (unsigned int i = 0; i < children->length; ++i) {
    extraire_gumbo_texte_et_urls((GumboNode *)children->data[i], ctx, corps_txt,
                                 corps_len);
  }

  // Amélioration structurelle : Gestion complète des paragraphes et structures
  // tabulaires de mailings
  if (element->tag == GUMBO_TAG_P || element->tag == GUMBO_TAG_DIV ||
      element->tag == GUMBO_TAG_TR) {
    arena_string_append(ctx->arena, corps_txt, corps_len, "\n\n");
  } else if (element->tag == GUMBO_TAG_BR || element->tag == GUMBO_TAG_TD ||
             element->tag == GUMBO_TAG_TH) {
    arena_string_append(ctx->arena, corps_txt, corps_len, "\n");
  }
}

// ============================================================================
// CALLBACK FOR GMIME (CONVERSION UTF-8 ET DÉCODAGE EXPLICITE VIA FILTRE)
// ============================================================================
static void process_mime_part(G_GNUC_UNUSED GMimeObject *parent,
                              GMimeObject *part, gpointer user_data) {
  if (!part || !user_data)
    return;
  ParserContext *ctx = (ParserContext *)user_data;

  GMimeContentType *content_type = g_mime_object_get_content_type(part);
  const char *disposition = g_mime_object_get_disposition(part);

  if (GMIME_IS_PART(part)) {
    GMimePart *mime_part = GMIME_PART(part);
    const char *filename = g_mime_part_get_filename(mime_part);

    if (filename == NULL &&
        (!disposition || strcasecmp(disposition, "attachment") != 0)) {
      if (GMIME_IS_TEXT_PART(part)) {
        GMimeTextPart *text_part = GMIME_TEXT_PART(part);

        // 1. On récupère le charset d'origine défini dans l'en-tête de cette
        // partie (ex: "iso-8859-1")
        const char *charset = g_mime_text_part_get_charset(text_part);
        if (!charset)
          charset = "us-ascii";

        // 2. On extrait le wrapper de contenu de GMime
        GMimeDataWrapper *wrapper = g_mime_part_get_content(mime_part);
        if (wrapper) {
          // On crée un flux mémoire pour intercepter le texte décodé
          GMimeStream *mem_stream = g_mime_stream_mem_new();
          // Un flux de filtrage pour appliquer la conversion de charset
          GMimeStream *filter_stream = g_mime_stream_filter_new(mem_stream);

          // On configure le filtre : Source (charset du mail) -> Destination
          // (UTF-8 obligatoire)
          GMimeFilter *filter = g_mime_filter_charset_new(charset, "UTF-8");
          g_mime_stream_filter_add(GMIME_STREAM_FILTER(filter_stream), filter);
          g_object_unref(filter);

          // On écrit le contenu à travers le filtre (ce qui décode le
          // Quoted-Printable/Base64 ET convertit le charset)
          g_mime_data_wrapper_write_to_stream(wrapper, filter_stream);
          g_mime_stream_flush(filter_stream);

          // On récupère la chaîne UTF-8 finale convertie
          GByteArray *buffer = GMIME_STREAM_MEM(mem_stream)->buffer;
          char *gm_text = malloc(buffer->len + 1);
          if (gm_text) {
            memcpy(gm_text, buffer->data, buffer->len);
            gm_text[buffer->len] = '\0';

            // Priorité au HTML s'il y en a un, sinon on se rabat sur le Plain
            if (content_type &&
                g_mime_content_type_is_type(content_type, "text", "html")) {
              ctx->temp_eml_text = arena_strdup(ctx->arena, gm_text);
            } else if (content_type &&
                       g_mime_content_type_is_type(content_type, "text",
                                                   "plain") &&
                       strlen(ctx->temp_eml_text) == 0) {
              ctx->temp_eml_text = arena_strdup(ctx->arena, gm_text);
            }
            free(gm_text);
          }

          g_object_unref(filter_stream);
          g_object_unref(mem_stream);
        }
      }
    }
  }
}

// ============================================================================
// MAIN FUNCTION
// ============================================================================
int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  setlocale(LC_ALL, "C.UTF-8");
  prctl(PR_SET_NAME, PROGRAMME_NAME, 0, 0, 0);

  Arena *arena = arena_create(16 * 1024 * 1024);

  size_t input_capacity = 1024 * 1024;
  char *input_buf = arena_alloc(arena, input_capacity);
  size_t input_len = 0;
  int c;

  while ((c = fgetc(stdin)) != EOF) {
    if (input_len + 1 >= input_capacity) {
      fprintf(stderr, "Erreur : Contenu de stdin trop volumineux.\n");
      return 1;
    }
    input_buf[input_len++] = (char)c;
  }
  input_buf[input_len] = '\0';

  char *corps_final = arena_strdup(arena, "");
  size_t corps_final_len = 0;
  ParserContext ctx = {.urls_txt = arena_strdup(arena, ""),
                       .urls_len = 0,
                       .temp_eml_text = arena_strdup(arena, ""),
                       .arena = arena};

  // ============================================================================
  // DEBUT DE LA ZONE MODIFIÉE
  // ============================================================================
  int is_eml = (strstr(input_buf, "From:") != NULL || strstr(input_buf, "MIME-Version:") != NULL);

  if (is_eml) {
    g_mime_init();
    GMimeStream *stream = g_mime_stream_mem_new_with_buffer(input_buf, input_len);
    GMimeParser *parser = g_mime_parser_new_with_stream(stream);
    GMimeMessage *message = g_mime_parser_construct_message(parser, NULL);

    if (message) {
      g_mime_message_foreach(message, process_mime_part, &ctx);
      g_object_unref(message);
    }
    g_object_unref(parser);
    g_object_unref(stream);
    g_mime_shutdown();
  }

  // Choix de la source HTML : soit l'extraction GMime, soit le buffer brut
  const char *html_to_parse = NULL;
  if (is_eml && ctx.temp_eml_text && strlen(ctx.temp_eml_text) > 0) {
    html_to_parse = ctx.temp_eml_text;
  } else if (!is_eml) {
    html_to_parse = input_buf;
  }

  if (html_to_parse) {
    GumboOutput *gumbo_out = gumbo_parse(html_to_parse);
    if (gumbo_out) {
      extraire_gumbo_texte_et_urls(gumbo_out->root, &ctx, &corps_final, &corps_final_len);
      gumbo_destroy_output(&kGumboDefaultOptions, gumbo_out);
    }
  }
  // ============================================================================
  // FIN DE LA ZONE MODIFIÉE
  // ============================================================================

  // Application des nettoyages de mise en page (Espaces décalés + Sauts excessifs)
  if (corps_final_len > 0) {
    corps_final = nettoyer_espaces_texte(arena, corps_final);
    corps_final = condenser_sauts_lignes(arena, corps_final);
  }

  // Rendu sur stdout pour NeoMutt
  fprintf(stdout, "%s\n", corps_final ? corps_final : "");
  if (ctx.urls_len > 0) {
    fprintf(stdout, "\n---\nURLs détectées :\n%s", ctx.urls_txt);
  }

  free(arena);
  return 0;
}
