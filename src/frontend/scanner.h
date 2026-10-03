/* SPDX-License-Identifier: MIT */
/*
 * Token types produced by the scanner.
 */
#ifndef FL_SCANNER_H
#define FL_SCANNER_H

#include "common.h"
#include "../util/diagnostic.h"

typedef enum {
	/* single character */
	TOKEN_LEFT_PAREN,
	TOKEN_RIGHT_PAREN,
	TOKEN_LEFT_BRACE,
	TOKEN_RIGHT_BRACE,
	TOKEN_LEFT_BRACKET,
	TOKEN_RIGHT_BRACKET,
	TOKEN_COMMA,
	TOKEN_DOT,
	TOKEN_SEMICOLON,
	TOKEN_COLON,
	TOKEN_PLUS,
	TOKEN_MINUS,
	TOKEN_STAR,
	TOKEN_SLASH,
	TOKEN_PERCENT,

	/* one or two characters */
	TOKEN_BANG,
	TOKEN_BANG_EQUAL,
	TOKEN_EQUAL,
	TOKEN_EQUAL_EQUAL,
	TOKEN_GREATER,
	TOKEN_GREATER_EQUAL,
	TOKEN_LESS,
	TOKEN_LESS_EQUAL,
	TOKEN_DOT_DOT,
	TOKEN_QUESTION_QUESTION,
	TOKEN_PLUS_EQUAL,
	TOKEN_MINUS_EQUAL,
	TOKEN_STAR_EQUAL,
	TOKEN_SLASH_EQUAL,

	/* literals */
	TOKEN_IDENTIFIER,
	TOKEN_STRING,
	TOKEN_NUMBER,

	/* keywords */
	TOKEN_AND,
	TOKEN_AS,
	TOKEN_BREAK,
	TOKEN_CONST,
	TOKEN_CONTINUE,
	TOKEN_ELSE,
	TOKEN_EXPORT,
	TOKEN_FALSE,
	TOKEN_FN,
	TOKEN_FOR,
	TOKEN_IF,
	TOKEN_IMPORT,
	TOKEN_IN,
	TOKEN_LET,
	TOKEN_NIL,
	TOKEN_NOT,
	TOKEN_OR,
	TOKEN_PRINT,
	TOKEN_RETURN,
	TOKEN_TRUE,
	TOKEN_WHILE,
	TOKEN_TRY,
	TOKEN_CATCH,
	TOKEN_THROW,

	TOKEN_ERROR,
	TOKEN_EOF
} TokenType;

/*
 * A token is a window into the source, not a copy. start and length are the
 * bytes; the compiler interns them when it needs a string object.
 */
typedef struct {
	TokenType type;
	const char *start;
	int length;
	int line;
	/* a newline appeared before this token, which ends a statement */
	bool newline_before;
	uint32_t offset;
} Token;

/* point the scanner at a new buffer. resets the line to 1. */
void scanner_init(const char *source);

/* next token. TOKEN_EOF is returned forever once the input runs out. */
Token scan_token(void);

#endif /* FL_SCANNER_H */
