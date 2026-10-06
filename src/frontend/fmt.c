/* SPDX-License-Identifier: MIT */
/*
 * `flint fmt`: canonical layout for flint sources.
 *
 * What it does, and nothing else:
 *   - strips trailing whitespace on every line,
 *   - re-indents by brace/paren/bracket depth, four spaces a level,
 *   - ends the file with exactly one newline.
 *
 * What it does not do, on purpose: join or split lines, respacing
 * operators, reorder anything, or touch a line that sits inside a
 * multi-line string. Indentation is layout; the rest is the program,
 * and a formatter that rewrites the program is a compiler with no
 * business being run by accident.
 *
 * Depth walks the text the way the repl's completeness check does:
 * strings skipped with escapes honoured, `#` comments skipped to the
 * newline, single quotes not strings. A line inside a string keeps its
 * bytes exactly, because those bytes are the value, not the layout.
 */
#include "fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FMT_INDENT 4

typedef struct {
	int depth;
	bool in_string;
} FmtState;

/* walk one line, updating depth and string state. returns the count of
 * leading closers, so the caller can dedent the line's own indent. */
static int fmt_scan_line(const char *line, FmtState *state)
{
	int leading_closers = 0;
	bool leading = true;
	bool in_string = state->in_string;
	int depth = state->depth;

	for (const char *p = line; *p != '\0' && *p != '\n'; p++) {
		if (in_string) {
			if (*p == '\\' && p[1] != '\0')
				p++;
			else if (*p == '"')
				in_string = false;
			continue;
		}
		if (*p == '"') {
			in_string = true;
			leading = false;
		} else if (*p == '#') {
			break;
		} else if (*p == '{' || *p == '(' || *p == '[') {
			depth++;
			leading = false;
		} else if (*p == '}' || *p == ')' || *p == ']') {
			depth--;
			if (depth < 0)
				depth = 0;
			if (leading)
				leading_closers++;
		} else if (*p != ' ' && *p != '\t') {
			leading = false;
		}
	}

	state->depth = depth;
	state->in_string = in_string;
	return leading_closers;
}

static char *fmt_format_text(const char *text, bool *changed)
{
	size_t cap = strlen(text) + 128;
	size_t used = 0;
	char *out = malloc(cap);
	if (out == NULL)
		return NULL;

	FmtState state = {0, false};
	const char *p = text;

	while (*p != '\0') {
		const char *eol = strchr(p, '\n');
		size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
		while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' ||
		                          p[len - 1] == '\r'))
			len--;

		char *line = malloc(len + 1);
		if (line == NULL) {
			free(out);
			return NULL;
		}
		memcpy(line, p, len);
		line[len] = '\0';

		bool blank = true;
		for (size_t i = 0; i < len; i++) {
			if (line[i] != ' ' && line[i] != '\t') {
				blank = false;
				break;
			}
		}

		const char *body = line;
		if (!blank && !state.in_string) {
			/* indent from the depth above this line: an opening
			 * bracket indents what follows, a leading closer
			 * belongs to the line above and comes back off. */
			int before = state.depth;
			int closers = fmt_scan_line(line, &state);
			int indent = before - closers;
			if (indent < 0)
				indent = 0;
			while (*body == ' ' || *body == '\t')
				body++;
			size_t need =
			        (size_t)indent * FMT_INDENT + strlen(body) + 2;
			while (used + need > cap) {
				cap *= 2;
				char *grown = realloc(out, cap);
				if (grown == NULL) {
					free(line);
					free(out);
					return NULL;
				}
				out = grown;
			}
			for (int i = 0; i < indent * FMT_INDENT; i++)
				out[used++] = ' ';
			size_t bl = strlen(body);
			memcpy(out + used, body, bl);
			used += bl;
			out[used++] = '\n';
		} else {
			if (state.in_string)
				fmt_scan_line(line, &state);
			size_t need = blank ? 1 : strlen(body) + 1;
			while (used + need > cap) {
				cap *= 2;
				char *grown = realloc(out, cap);
				if (grown == NULL) {
					free(line);
					free(out);
					return NULL;
				}
				out = grown;
			}
			if (!blank) {
				size_t bl = strlen(body);
				memcpy(out + used, body, bl);
				used += bl;
			}
			out[used++] = '\n';
		}
		free(line);
		p = eol != NULL ? eol + 1 : p + len;
	}

	out[used] = '\0';
	if (changed != NULL)
		*changed = strcmp(out, text) != 0;
	return out;
}

static char *fmt_read_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size < 0) {
		fclose(file);
		return NULL;
	}
	char *text = malloc((size_t)size + 1);
	if (text == NULL) {
		fclose(file);
		return NULL;
	}
	size_t read = fread(text, 1, (size_t)size, file);
	fclose(file);
	if (read < (size_t)size) {
		/* a short read is a directory, a pipe, or a race. handing
		 * a truncated buffer to the formatter would be worse. */
		free(text);
		return NULL;
	}
	/*
	 * In bounds: the allocation above is `size + 1`. The analyzer
	 * cannot tie `size` back to it across the ftell/fseek sequence,
	 * which is the same complaint it makes about the identical
	 * read_file() in main.c, where the same annotation is already
	 * in place.
	 */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	text[size] = '\0';
	return text;
}

int flint_fmt(int argc, char **argv, int start, bool check)
{
	int worst = 0;
	bool any = false;
	for (int i = start; i < argc; i++) {
		if (strcmp(argv[i], "--check") == 0) {
			check = true;
			continue;
		}
		any = true;
		char *text = fmt_read_file(argv[i]);
		if (text == NULL) {
			fprintf(stderr,
			        "flint fmt: cannot read '%s'\n",
			        argv[i]);
			worst = 74;
			continue;
		}
		bool changed = false;
		char *formatted = fmt_format_text(text, &changed);
		if (formatted == NULL) {
			fprintf(stderr, "flint fmt: out of memory\n");
			free(text);
			return 74;
		}
		if (check) {
			if (changed) {
				printf("%s\n", argv[i]);
				worst = 1;
			}
		} else if (changed) {
			FILE *file = fopen(argv[i], "wb");
			if (file == NULL) {
				fprintf(stderr,
				        "flint fmt: cannot write '%s'\n",
				        argv[i]);
				worst = 74;
			} else {
				fwrite(formatted, 1, strlen(formatted), file);
				fclose(file);
			}
		}
		free(formatted);
		free(text);
	}
	if (!any) {
		fprintf(stderr, "flint fmt: no files given\n");
		return 64;
	}
	return worst;
}
