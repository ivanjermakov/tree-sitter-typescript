#include "tree_sitter/parser.h"

#include <wctype.h>

enum TokenType {
  AUTOMATIC_SEMICOLON,
  TEMPLATE_CHARS,
  TERNARY_QMARK,
  HTML_COMMENT,
  LOGICAL_OR,
  ESCAPE_SEQUENCE,
  FUNCTION_SIGNATURE_AUTOMATIC_SEMICOLON,
};

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static bool scan_template_chars(TSLexer *lexer) {
  lexer->result_symbol = TEMPLATE_CHARS;
  for (bool has_content = false;; has_content = true) {
    lexer->mark_end(lexer);
    switch (lexer->lookahead) {
    case '`':
      return has_content;
    case '\0':
      return false;
    case '$':
      advance(lexer);
      if (lexer->lookahead == '{') return has_content;
      break;
    case '\\':
      return has_content;
    default:
      advance(lexer);
    }
  }
}

static bool scan_whitespace_and_comments(TSLexer *lexer, bool *scanned_comment) {
  for (;;) {
    while (iswspace(lexer->lookahead)) {
      skip(lexer);
    }

    if (lexer->lookahead == '/') {
      skip(lexer);

      if (lexer->lookahead == '/') {
        skip(lexer);
        while (lexer->lookahead != 0 && lexer->lookahead != '\n') {
          skip(lexer);
        }
        *scanned_comment = true;
      } else if (lexer->lookahead == '*') {
        skip(lexer);
        while (lexer->lookahead != 0) {
          if (lexer->lookahead == '*') {
            skip(lexer);
            if (lexer->lookahead == '/') {
              skip(lexer);
              break;
            }
          } else {
            skip(lexer);
          }
        }
      } else {
        return false;
      }
    } else {
      return true;
    }
  }
}

static bool scan_automatic_semicolon(TSLexer *lexer, const bool *valid_symbols, bool *scanned_comment) {
  lexer->result_symbol = AUTOMATIC_SEMICOLON;
  lexer->mark_end(lexer);

  for (;;) {
    if (lexer->lookahead == 0) return true;
    if (lexer->lookahead == '}') {
      // Automatic semicolon insertion breaks detection of object patterns
      // in a typed context:
      //   type F = ({a}: {a: number}) => number;
      // Therefore, disable automatic semicolons when followed by typing
      do {
        skip(lexer);
      } while (iswspace(lexer->lookahead));
      if (lexer->lookahead == ':') return false;
      return true;
    }
    if (!iswspace(lexer->lookahead)) return false;
    if (lexer->lookahead == '\n') break;
    skip(lexer);
  }

  skip(lexer);

  if (!scan_whitespace_and_comments(lexer, scanned_comment)) return false;

  switch (lexer->lookahead) {
  case ',':
  case '.':
  case ';':
  case '*':
  case '%':
  case '>':
  case '<':
  case '=':
  case '?':
  case '^':
  case '|':
  case '&':
  case '/':
  case ':':
    return false;

  case '{':
    if (valid_symbols[FUNCTION_SIGNATURE_AUTOMATIC_SEMICOLON]) return false;
    break;

    // Don't insert a semicolon before a '[' or '(', unless we're parsing
    // a type. Detect whether we're parsing a type or an expression using
    // the validity of a binary operator token.
  case '(':
  case '[':
    if (valid_symbols[LOGICAL_OR]) return false;
    break;

    // Insert a semicolon before `--` and `++`, but not before binary `+` or `-`.
  case '+':
    skip(lexer);
    return lexer->lookahead == '+';
  case '-':
    skip(lexer);
    return lexer->lookahead == '-';

    // Don't insert a semicolon before `!=`, but do insert one before a unary `!`.
  case '!':
    skip(lexer);
    return lexer->lookahead != '=';

    // Don't insert a semicolon before `in` or `instanceof`, but do insert one
    // before an identifier.
  case 'i':
    skip(lexer);

    if (lexer->lookahead != 'n') return true;
    skip(lexer);

    if (!iswalpha(lexer->lookahead)) return false;

    for (unsigned i = 0; i < 8; i++) {
      if (lexer->lookahead != "stanceof"[i]) return true;
      skip(lexer);
    }

    if (!iswalpha(lexer->lookahead)) return false;
    break;
  }

  return true;
}

static bool scan_ternary_qmark(TSLexer *lexer) {
  for(;;) {
    if (!iswspace(lexer->lookahead)) break;
    skip(lexer);
  }

  if (lexer->lookahead == '?') {
    advance(lexer);

    if (lexer->lookahead == '?') return false;
    /* Optional chaining. */
    if (lexer->lookahead == '.') return false;

    lexer->mark_end(lexer);
    lexer->result_symbol = TERNARY_QMARK;

    /* TypeScript optional arguments contain the ?: sequence, possibly
       with whitespace. */
    for(;;) {
      if (!iswspace(lexer->lookahead)) break;
      advance(lexer);
    }
    if (lexer->lookahead == ':') return false;
    if (lexer->lookahead == ')') return false;
    if (lexer->lookahead == ',') return false;

    if (lexer->lookahead == '.') {
      advance(lexer);
      if (iswdigit(lexer->lookahead)) return true;
      return false;
    }
    return true;
  }
  return false;
}

static bool scan_closing_comment(TSLexer *lexer) {
    while (iswspace(lexer->lookahead) || lexer->lookahead == 0x2028 || lexer->lookahead == 0x2029) {
        skip(lexer);
    }

    const char *comment_start = "<!--";
    const char *comment_end = "-->";

    if (lexer->lookahead == '<') {
        for (unsigned i = 0; i < 4; i++) {
            if (lexer->lookahead != comment_start[i]) {
                return false;
            }
            advance(lexer);
        }
    } else if (lexer->lookahead == '-') {
        for (unsigned i = 0; i < 3; i++) {
            if (lexer->lookahead != comment_end[i]) {
                return false;
            }
            advance(lexer);
        }
    } else {
        return false;
    }

    while (lexer->lookahead != 0 && lexer->lookahead != '\n' && lexer->lookahead != 0x2028 &&
           lexer->lookahead != 0x2029) {
        advance(lexer);
    }

    lexer->result_symbol = HTML_COMMENT;
    lexer->mark_end(lexer);

    return true;
}


static inline bool external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  if (valid_symbols[TEMPLATE_CHARS]) {
    if (valid_symbols[AUTOMATIC_SEMICOLON]) return false;
    return scan_template_chars(lexer);
  } else if (
    valid_symbols[AUTOMATIC_SEMICOLON] ||
    valid_symbols[FUNCTION_SIGNATURE_AUTOMATIC_SEMICOLON]
             ) {
    bool scanned_comment = false;
    bool ret = scan_automatic_semicolon(lexer, valid_symbols, &scanned_comment);
    if (!ret && !scanned_comment && valid_symbols[TERNARY_QMARK] && lexer->lookahead == '?')
      return scan_ternary_qmark(lexer);
    return ret;
  }
  if (valid_symbols[TERNARY_QMARK]) {
    return scan_ternary_qmark(lexer);
  }

  if (valid_symbols[HTML_COMMENT] && !valid_symbols[LOGICAL_OR] && !valid_symbols[ESCAPE_SEQUENCE]) {
    return scan_closing_comment(lexer);
  }

  return false;

}
