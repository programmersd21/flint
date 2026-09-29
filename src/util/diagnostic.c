/* SPDX-License-Identifier: MIT */
#include "diagnostic.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

FlSpan fl_span(size_t start, size_t end)
{
	FlSpan span = {(uint32_t)start, (uint32_t)end};
	return span;
}

void fl_source_init(FlSource *source, const char *name, const char *text)
{
	source->name = name != NULL ? name : "<source>";
	source->text = text != NULL ? text : "";
	source->length = strlen(source->text);
	source->line_count = 1;
	for (size_t i = 0; i < source->length; i++)
		if (source->text[i] == '\n')
			source->line_count++;
	source->line_starts = malloc(source->line_count * sizeof(size_t));
	if (source->line_starts == NULL) {
		source->line_count = 0;
		return;
	}
	source->line_starts[0] = 0;
	size_t line = 1;
	for (size_t i = 0; i < source->length; i++) {
		if (source->text[i] == '\n')
			source->line_starts[line++] = i + 1;
	}
}

void fl_source_free(FlSource *source)
{
	free(source->line_starts);
	source->line_starts = NULL;
	source->line_count = 0;
}

size_t fl_source_line(const FlSource *source, size_t offset)
{
	if (source->line_count == 0)
		return 0;
	if (offset > source->length)
		offset = source->length;
	size_t lo = 0;
	size_t hi = source->line_count;
	while (lo + 1 < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (source->line_starts[mid] <= offset)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

size_t fl_source_column(const FlSource *source, size_t offset)
{
	if (source->line_count == 0)
		return 0;
	size_t line = fl_source_line(source, offset);
	size_t col = 0;
	if (offset > source->length)
		offset = source->length;
	for (size_t i = source->line_starts[line]; i < offset;) {
		unsigned char c = (unsigned char)source->text[i];
		if (c == '\t') {
			col += 4 - col % 4;
			i++;
		} else {
			col++;
			size_t width = c < 0x80             ? 1
			               : (c & 0xe0) == 0xc0 ? 2
			               : (c & 0xf0) == 0xe0 ? 3
			                                    : 4;
			if (i + width > offset)
				break;
			i += width;
		}
	}
	return col;
}

static size_t visual_column(const FlSource *source, size_t line, size_t offset)
{
	if (source->line_count == 0 || line >= source->line_count)
		return 0;
	if (offset > source->length)
		offset = source->length;
	size_t at = source->line_starts[line];
	size_t col = 0;
	while (at < offset) {
		unsigned char c = (unsigned char)source->text[at];
		if (c == '\t') {
			col += 4 - col % 4;
			at++;
		} else {
			size_t width = c < 0x80             ? 1
			               : (c & 0xe0) == 0xc0 ? 2
			               : (c & 0xf0) == 0xe0 ? 3
			                                    : 4;
			if (at + width > offset)
				break;
			col++;
			at += width;
		}
	}
	return col;
}

static void write_source_line(FILE *out, const FlSource *source, size_t line)
{
	if (source->line_count == 0 || line >= source->line_count)
		return;
	size_t end = source->line_starts[line];
	while (end < source->length && source->text[end] != '\n')
		end++;
	if (end > source->line_starts[line] && source->text[end - 1] == '\r')
		end--;
	for (size_t i = source->line_starts[line]; i < end; i++) {
		if (source->text[i] == '\t') {
			fputs("    ", out);
		} else {
			fputc((unsigned char)source->text[i], out);
		}
	}
}

const char *fl_diag_applicability(FlApplicability applicability)
{
	switch (applicability) {
	case FL_APPLICABILITY_MACHINE:
		return "machine-applicable";
	case FL_APPLICABILITY_MAYBE_INCORRECT:
		return "maybe-incorrect";
	case FL_APPLICABILITY_PLACEHOLDERS:
		return "has-placeholders";
	default:
		return "unspecified";
	}
}

static const char *severity_name(FlDiagSeverity severity)
{
	switch (severity) {
	case FL_DIAG_WARNING:
		return "warning";
	case FL_DIAG_NOTE:
		return "note";
	case FL_DIAG_HELP:
		return "help";
	default:
		return "error";
	}
}

static void json_string(FILE *out, const char *s)
{
	fputc('"', out);
	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			fputc('\\', out);
			fputc(c, out);
		} else if (c == '\n') {
			fputs("\\n", out);
		} else if (c == '\r') {
			fputs("\\r", out);
		} else if (c == '\t') {
			fputs("\\t", out);
		} else if (c < 0x20) {
			fprintf(out, "\\u%04x", c);
		} else {
			fputc(c, out);
		}
	}
	fputc('"', out);
}

static void json_string_array(FILE *out, const char *const *items, size_t n)
{
	fputc('[', out);
	for (size_t i = 0; i < n; i++) {
		if (i != 0)
			fputc(',', out);
		json_string(out, items[i]);
	}
	fputc(']', out);
}

static void emit_json(FILE *out, const FlDiagnostic *d, const FlSource *s)
{
	fputs("{\"severity\":", out);
	json_string(out, severity_name(d->severity));
	fputs(",\"code\":", out);
	json_string(out, d->code != NULL ? d->code : "");
	fputs(",\"message\":", out);
	json_string(out, d->message);
	fputs(",\"spans\":[", out);
	bool comma = false;
	for (size_t i = 0; i <= d->label_count; i++) {
		bool primary = i == 0;
		if ((primary && !d->has_primary) || (!primary && s == NULL))
			continue;
		FlSpan span = primary ? d->primary : d->labels[i - 1].span;
		const char *label =
		        primary ? d->primary_label : d->labels[i - 1].label;
		if (comma)
			fputc(',', out);
		comma = true;
		size_t start = span.start;
		size_t end = span.end;
		if (s != NULL && start > s->length)
			start = s->length;
		if (s != NULL && end > s->length)
			end = s->length;
		fprintf(out, "{\"file\":");
		json_string(out, s != NULL ? s->name : "<source>");
		fprintf(out,
		        ",\"start\":%u,\"end\":%u,\"line_start\":%zu"
		        ",\"column_start\":%zu,\"line_end\":%zu"
		        ",\"column_end\":%zu,\"is_primary\":%s,\"label\":",
		        (unsigned)start,
		        (unsigned)end,
		        s != NULL ? fl_source_line(s, start) + 1 : 0,
		        s != NULL ? fl_source_column(s, start) + 1 : 0,
		        s != NULL ? fl_source_line(s, end) + 1 : 0,
		        s != NULL ? fl_source_column(s, end) + 1 : 0,
		        primary ? "true" : "false");
		if (label != NULL)
			json_string(out, label);
		else
			fputs("null", out);
		fputc('}', out);
	}
	fputs("],\"notes\":", out);
	json_string_array(out, d->notes, d->note_count);
	fputs(",\"help\":", out);
	json_string_array(out, d->help, d->help_count);
	fputs(",\"suggestions\":[", out);
	for (size_t i = 0; i < d->suggestion_count; i++) {
		const FlDiagSuggestion *fix = &d->suggestions[i];
		if (i != 0)
			fputc(',', out);
		fprintf(out,
		        "{\"start\":%u,\"end\":%u,\"message\":",
		        (unsigned)fix->span.start,
		        (unsigned)fix->span.end);
		json_string(out, fix->message);
		fputs(",\"replacement\":", out);
		json_string(out, fix->replacement);
		fputs(",\"applicability\":", out);
		json_string(out, fl_diag_applicability(fix->applicability));
		fputc('}', out);
	}
	fputs("]}\n", out);
}

static void emit_short(FILE *out, const FlDiagnostic *d, const FlSource *s)
{
	fprintf(out,
	        "%s[%s]: %s",
	        severity_name(d->severity),
	        d->code != NULL ? d->code : "",
	        d->message);
	if (d->has_primary && s != NULL && s->line_count != 0) {
		size_t line = fl_source_line(s, d->primary.start) + 1;
		size_t col = fl_source_column(s, d->primary.start) + 1;
		fprintf(out, " at %s:%zu:%zu", s->name, line, col);
	}
	fputc('\n', out);
}

static void emit_human(
        FILE *out, const FlDiagnostic *d, const FlSource *s, bool color)
{
	const char *paint =
	        color ? (d->severity == FL_DIAG_ERROR ? "\033[31m" : "\033[33m")
	              : "";
	const char *reset = color ? "\033[0m" : "";
	/* secondary spans get a dim cyan rather than the severity colour:
	 * the severity belongs to the thing being complained about, and a
	 * second span in the same colour reads as a second complaint */
	const char *muted = color ? "\033[2;36m" : "";
	fprintf(out, "%s%s%s", paint, severity_name(d->severity), reset);
	if (d->code != NULL)
		fprintf(out, "[%s]", d->code);
	fprintf(out, ": %s\n", d->message);
	if (d->has_primary && s != NULL && s->line_count != 0) {
		size_t start = d->primary.start;
		if (start > s->length)
			start = s->length;
		size_t line = fl_source_line(s, start);
		size_t col = visual_column(s, line, start);
		fprintf(out,
		        " --> %s:%zu:%zu\n  |\n",
		        s->name,
		        line + 1,
		        col + 1);
		size_t width = 1;
		for (size_t n = line + 1; n >= 10; n /= 10)
			width++;
		fprintf(out, "%*zu | ", (int)width, line + 1);
		write_source_line(out, s, line);
		fputc('\n', out);
		fprintf(out,
		        "%*s | %*s%s^",
		        (int)width,
		        "",
		        (int)col,
		        "",
		        paint);
		size_t end = d->primary.end;
		if (end > s->length)
			end = s->length;
		while (end > d->primary.start &&
		        (s->text[end - 1] == '\n' || s->text[end - 1] == '\r'))
			end--;
		size_t underline = fl_source_line(s, end) == line
		                           ? visual_column(s, line, end) - col
		                           : 1;
		if (underline == 0)
			underline = 1;
		if (underline > 40)
			underline = 40;
		for (size_t i = 1; i < underline; i++)
			fputc('~', out);
		fprintf(out, "%s", reset);
		if (d->primary_label != NULL)
			fprintf(out, " %s", d->primary_label);
		fputs("\n  |\n", out);
	}
	/*
	 * Secondary labels, each on its own source line with a muted caret.
	 *
	 * A label is only useful if it points at something, and a label with
	 * no line of source under it is just a sentence. "first defined here"
	 * printed on its own is a claim the reader has to go and verify; with
	 * the line and a caret under the name it is a fact they can check at
	 * a glance.
	 *
	 * Only labels that land on a different line from the primary are
	 * drawn this way. One on the same line is already inside the span
	 * being pointed at, and drawing it again would just be noise.
	 */
	for (size_t i = 0; i < d->label_count; i++) {
		const FlDiagLabel *lab = &d->labels[i];
		if (lab->label == NULL || s == NULL)
			continue;
		if (lab->span.end > s->length)
			continue;

		size_t lline = fl_source_line(s, lab->span.start);
		size_t lcol = visual_column(s, lline, lab->span.start);
		if (d->has_primary &&
		        lline == fl_source_line(s, d->primary.start))
			continue;

		size_t lwidth = 1;
		for (size_t n = lline + 1; n >= 10; n /= 10)
			lwidth++;

		fprintf(out, "  |\n");
		fprintf(out, "%*zu | ", (int)lwidth, lline + 1);
		write_source_line(out, s, lline);
		fputc('\n', out);
		fprintf(out,
		        "%*s | %*s%s-",
		        (int)lwidth,
		        "",
		        (int)lcol,
		        "",
		        muted);
		size_t lend = lab->span.end;
		while (lend > lab->span.start &&
		        (s->text[lend - 1] == '\n' ||
		                s->text[lend - 1] == '\r'))
			lend--;
		size_t lunderline =
		        fl_source_line(s, lend) == lline
		                ? visual_column(s, lline, lend) - lcol
		                : 1;
		if (lunderline == 0)
			lunderline = 1;
		if (lunderline > 40)
			lunderline = 40;
		for (size_t k = 1; k < lunderline; k++)
			fputc('~', out);
		fprintf(out, "%s", reset);
		fprintf(out, " %s\n", lab->label);
	}
	for (size_t i = 0; i < d->note_count; i++)
		fprintf(out, "  = note: %s\n", d->notes[i]);
	for (size_t i = 0; i < d->help_count; i++)
		fprintf(out, "  = help: %s\n", d->help[i]);
	for (size_t i = 0; i < d->suggestion_count; i++) {
		const FlDiagSuggestion *fix = &d->suggestions[i];

		/*
		 * A suggestion carries its replacement separately from its
		 * message, so the renderer decides what to show. The
		 * replacement goes on the help line, because a bare "|" and
		 * then the delimiter in a block of its own, with no line
		 * number and no source, told the reader nothing they could
		 * act on.
		 */
		if (fix->replacement != NULL) {
			fprintf(out,
			        "  = help: %s: %s\n",
			        fix->message != NULL ? fix->message : "try",
			        fix->replacement);
		} else {
			fprintf(out,
			        "  = help: %s\n",
			        fix->message != NULL ? fix->message : "");
		}

		/*
		 * Applicability is metadata, and it only earns a line when
		 * it changes what will happen. A machine-applicable fix is
		 * applied silently by --fix, so saying so on every error
		 * would be noise. Anything else is worth saying, because
		 * --fix will leave it alone and the reader would otherwise
		 * wonder why.
		 */
		if (fix->applicability != FL_APPLICABILITY_MACHINE) {
			fprintf(out,
			        "  = note: --fix will not apply this "
			        "automatically\n");
		}
	}
}

void fl_diag_emit(FILE *out,
        const FlDiagnostic *diag,
        const FlSource *source,
        FlDiagFormat format,
        FlColorMode color)
{
	if (format == FL_DIAG_JSON) {
		emit_json(out, diag, source);
	} else if (format == FL_DIAG_SHORT) {
		emit_short(out, diag, source);
	} else {
		bool enabled = color == FL_COLOR_ALWAYS ||
		               (color == FL_COLOR_AUTO && out == stderr &&
		                       isatty(STDERR_FILENO));
		emit_human(out, diag, source, enabled);
	}
}
