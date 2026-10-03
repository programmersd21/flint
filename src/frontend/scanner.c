/* SPDX-License-Identifier: MIT */
/*
 * The scanner: source text in, one token at a time out.
 *
 * There is no token array. The compiler asks for a token when it wants one
 * and the scanner hands it over. That is possible because the whole scanner
 * is a single global, which is also why this cannot be reentrant.
 */
#include "scanner.h"

#include <stdint.h>
#include <string.h>

typedef struct {
	const char *source;
	const char *start; /* start of the current token */
	const char *current; /* next byte to look at */
	int line;
	bool newline_seen; /* a newline was crossed since the last token */
} Scanner;

static Scanner scanner;

void scanner_init(const char *source)
{
	scanner.source = source;
	scanner.start = source;
	scanner.current = source;
	scanner.line = 1;
	scanner.newline_seen = false;
}

static bool is_at_end(void) { return *scanner.current == '\0'; }

/* consume one byte and return the one before current. */
static char advance(void)
{
	scanner.current++;
	return scanner.current[-1];
}

static char peek(void) { return *scanner.current; }

static char peek_next(void)
{
	if (is_at_end())
		return '\0';
	return scanner.current[1];
}

/* consume a byte if it is the one we expect. used for two-character tokens. */
static bool match(char expected)
{
	if (is_at_end() || *scanner.current != expected)
		return false;
	scanner.current++;
	return true;
}

/*
 * Build a token from what we have consumed. The token points into the source
 * buffer rather than copying, which is safe because the compiler never holds
 * a token past the string it came from.
 */
static Token make_token(TokenType type)
{
	Token token;
	token.type = type;
	token.start = scanner.start;
	token.length = (int)(scanner.current - scanner.start);
	token.line = scanner.line;
	token.newline_before = scanner.newline_seen;
	token.offset = (uint32_t)(scanner.start - scanner.source);

	/* a newline belongs to the token that follows it, not this one */
	scanner.newline_seen = false;
	return token;
}

/*
 * Errors are tokens too. start points at the message rather than at the
 * source, so error_at() can print it with the usual %.*s format.
 */
static Token error_token(const char *message)
{
	Token token;
	token.type = TOKEN_ERROR;
	token.start = message;
	token.length = (int)strlen(message);
	token.line = scanner.line;
	token.newline_before = scanner.newline_seen;
	token.offset = (uint32_t)(scanner.start - scanner.source);
	scanner.newline_seen = false;
	return token;
}

static void skip_whitespace(void)
{
	for (;;) {
		char c = peek();
		switch (c) {
		case ' ':
		case '\r':
		case '\t':
			advance();
			break;
		case '\n':
			/*
			 * Newline is whitespace, but it also terminates a
			 * statement, so record it instead of dropping it.
			 */
			scanner.newline_seen = true;
			scanner.line++;
			advance();
			break;
		case '#':
			/* comments run to the newline, or to end of input */
			while (peek() != '\n' && !is_at_end())
				advance();
			break;
		default:
			return;
		}
	}
}

/*
 * Compare the tail of the current identifier. The length is part of the
 * match, which is what stops "andy" from being scanned as "and" plus junk.
 */
static TokenType check_keyword(
        int start, int length, const char *rest, TokenType type)
{
	if (scanner.current - scanner.start == start + length &&
	        memcmp(scanner.start + start, rest, length) == 0)
		return type;
	return TOKEN_IDENTIFIER;
}

/*
 * Keywords, as a hand-written prefix trie on the first two characters.
 *
 * Running strcmp against all twenty keywords for every identifier would mean
 * twenty memcmp calls on the most common token in the language. Dispatching
 * on the first letter first means most identifiers cost one switch.
 */
static TokenType identifier_type(void)
{
	switch (scanner.start[0]) {
	case 'a':
		if (scanner.current - scanner.start > 1) {
			switch (scanner.start[1]) {
			case 'n':
				return check_keyword(2, 1, "d", TOKEN_AND);
			case 's':
				return check_keyword(2, 0, "", TOKEN_AS);
			}
		}
		break;
	case 'b':
		return check_keyword(1, 4, "reak", TOKEN_BREAK);
	case 'c':
		if (scanner.current - scanner.start > 1) {
			switch (scanner.start[1]) {
			case 'o':
				if (scanner.current - scanner.start > 2) {
					switch (scanner.start[2]) {
					case 'n':
						if (scanner.current -
						                scanner.start >
						        3) {
							if (scanner.start[3] ==
							        's')
								return check_keyword(
								        4,
								        1,
								        "t",
								        TOKEN_CONST);
							if (scanner.start[3] ==
							        't')
								return check_keyword(
								        4,
								        4,
								        "inue",
								        TOKEN_CONTINUE);
						}
						break;
					}
				}
				break;
			case 'a':
				return check_keyword(2, 3, "tch", TOKEN_CATCH);
			}
		}
		break;
	case 'e':
		if (scanner.current - scanner.start > 1) {
			switch (scanner.start[1]) {
			case 'l':
				return check_keyword(2, 2, "se", TOKEN_ELSE);
			case 'x':
				return check_keyword(
				        2, 4, "port", TOKEN_EXPORT);
			}
		}
		break;
	case 'f':
		if (scanner.current - scanner.start > 1) {
			switch (scanner.start[1]) {
			case 'a':
				return check_keyword(2, 3, "lse", TOKEN_FALSE);
			case 'n':
				return check_keyword(2, 0, "", TOKEN_FN);
			case 'o':
				return check_keyword(2, 1, "r", TOKEN_FOR);
			}
		}
		break;
	case 'i':
		if (scanner.current - scanner.start > 1) {
			switch (scanner.start[1]) {
			case 'f':
				return check_keyword(2, 0, "", TOKEN_IF);
			case 'm':
				return check_keyword(
				        2, 4, "port", TOKEN_IMPORT);
			case 'n':
				return check_keyword(2, 0, "", TOKEN_IN);
			}
		}
		break;
	case 'l':
		return check_keyword(1, 2, "et", TOKEN_LET);
	case 'n':
		if (scanner.current - scanner.start > 1) {
			switch (scanner.start[1]) {
			case 'i':
				return check_keyword(2, 1, "l", TOKEN_NIL);
			case 'o':
				return check_keyword(2, 1, "t", TOKEN_NOT);
			}
		}
		break;
	case 'o':
		return check_keyword(1, 1, "r", TOKEN_OR);
	case 'p':
		return check_keyword(1, 4, "rint", TOKEN_PRINT);
	case 'r':
		return check_keyword(1, 5, "eturn", TOKEN_RETURN);
	case 't':
		if (scanner.current - scanner.start > 1) {
			switch (scanner.start[1]) {
			case 'h':
				return check_keyword(2, 3, "row", TOKEN_THROW);
			case 'r':
				if (scanner.current - scanner.start > 2) {
					switch (scanner.start[2]) {
					case 'u':
						return check_keyword(1,
						        3,
						        "rue",
						        TOKEN_TRUE);
					case 'y':
						return check_keyword(
						        2, 1, "y", TOKEN_TRY);
					}
				}
				break;
			}
		}
		return check_keyword(1, 3, "rue", TOKEN_TRUE);
	case 'w':
		return check_keyword(1, 4, "hile", TOKEN_WHILE);
	}
	return TOKEN_IDENTIFIER;
}

static bool is_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* scan an identifier, then decide whether it is a keyword. */
static Token identifier(void)
{
	while (is_alpha(peek()) || is_digit(peek()))
		advance();
	return make_token(identifier_type());
}

/*
 * numbers: digits, an optional fraction, an optional exponent.
 *
 * the fraction requires a digit after the dot, so "1..5" scans as 1, .., 5
 * and not as 1. followed by a malformed fraction. getting that wrong makes
 * ranges impossible to write.
 */
static Token number(void)
{
	while (is_digit(peek()))
		advance();

	if (peek() == '.' && is_digit(peek_next())) {
		advance();
		while (is_digit(peek()))
			advance();
	}

	if (peek() == 'e' || peek() == 'E') {
		advance();
		if (peek() == '+' || peek() == '-')
			advance();
		if (!is_digit(peek()))
			return error_token("unterminated scientific notation.");
		while (is_digit(peek()))
			advance();
	}

	return make_token(TOKEN_NUMBER);
}

/*
 * A string is raw bytes between double quotes. Escape processing happens in
 * the compiler, not here, because the compiler is the thing that allocates.
 * Multibyte utf-8 is never inspected, so it passes through unchanged.
 */
static Token string(void)
{
	while (peek() != '"' && !is_at_end()) {
		if (peek() == '\n')
			scanner.line++;
		/* skip the escaped byte so \" does not end the string */
		if (peek() == '\\') {
			advance();
			if (is_at_end())
				return error_token("unterminated string.");
		}
		advance();
	}

	if (is_at_end())
		return error_token("unterminated string.");

	advance();
	return make_token(TOKEN_STRING);
}

/* one token. the compiler drives this; nothing buffers ahead. */
Token scan_token(void)
{
	skip_whitespace();
	scanner.start = scanner.current;

	if (is_at_end())
		return make_token(TOKEN_EOF);

	char c = advance();

	if (is_alpha(c))
		return identifier();
	if (is_digit(c))
		return number();

	switch (c) {
	case '(':
		return make_token(TOKEN_LEFT_PAREN);
	case ')':
		return make_token(TOKEN_RIGHT_PAREN);
	case '{':
		return make_token(TOKEN_LEFT_BRACE);
	case '}':
		return make_token(TOKEN_RIGHT_BRACE);
	case '[':
		return make_token(TOKEN_LEFT_BRACKET);
	case ']':
		return make_token(TOKEN_RIGHT_BRACKET);
	case ';':
		return make_token(TOKEN_SEMICOLON);
	case ':':
		return make_token(TOKEN_COLON);
	case ',':
		return make_token(TOKEN_COMMA);
	/* ".." is a range, "." is field access */
	case '.':
		if (match('.'))
			return make_token(TOKEN_DOT_DOT);
		return make_token(TOKEN_DOT);
	/* "??" is nil-coalescing. a lone "?" is not valid flint. */
	case '?':
		if (match('?'))
			return make_token(TOKEN_QUESTION_QUESTION);
		return error_token("unexpected character '?'.");
	case '-':
		if (match('='))
			return make_token(TOKEN_MINUS_EQUAL);
		return make_token(TOKEN_MINUS);
	case '+':
		if (match('='))
			return make_token(TOKEN_PLUS_EQUAL);
		return make_token(TOKEN_PLUS);
	case '/':
		if (match('='))
			return make_token(TOKEN_SLASH_EQUAL);
		return make_token(TOKEN_SLASH);
	case '*':
		if (match('='))
			return make_token(TOKEN_STAR_EQUAL);
		return make_token(TOKEN_STAR);
	case '%':
		return make_token(TOKEN_PERCENT);
	case '!':
		return make_token(match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
	case '=':
		return make_token(match('=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
	case '<':
		return make_token(match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
	case '>':
		return make_token(
		        match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
	case '"':
		return string();
	}

	return error_token("unexpected character.");
}
