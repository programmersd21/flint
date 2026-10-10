/* SPDX-License-Identifier: MIT */
#ifndef FL_DIAGNOSTIC_H
#define FL_DIAGNOSTIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
	uint32_t start;
	uint32_t end;
} FlSpan;

typedef enum {
	FL_DIAG_ERROR,
	FL_DIAG_WARNING,
	FL_DIAG_NOTE,
	FL_DIAG_HELP
} FlDiagSeverity;

typedef enum {
	FL_APPLICABILITY_MACHINE,
	FL_APPLICABILITY_MAYBE_INCORRECT,
	FL_APPLICABILITY_PLACEHOLDERS,
	FL_APPLICABILITY_UNSPECIFIED
} FlApplicability;

typedef enum {
	FL_DIAG_LEGACY,
	FL_DIAG_HUMAN,
	FL_DIAG_SHORT,
	FL_DIAG_JSON
} FlDiagFormat;

typedef enum { FL_COLOR_AUTO, FL_COLOR_ALWAYS, FL_COLOR_NEVER } FlColorMode;

typedef struct {
	FlSpan span;
	const char *label;
} FlDiagLabel;

typedef struct {
	FlSpan span;
	const char *message;
	const char *replacement;
	FlApplicability applicability;
} FlDiagSuggestion;

typedef struct {
	FlDiagSeverity severity;
	const char *code;
	const char *message;
	FlSpan primary;
	bool has_primary;
	const char *primary_label;
	const FlDiagLabel *labels;
	size_t label_count;
	const char *const *notes;
	size_t note_count;
	const char *const *help;
	size_t help_count;
	const FlDiagSuggestion *suggestions;
	size_t suggestion_count;
} FlDiagnostic;

typedef struct {
	const char *name;
	const char *text;
	size_t length;
	size_t *line_starts;
	size_t line_count;
} FlSource;

void fl_source_init(FlSource *source, const char *name, const char *text);
void fl_source_free(FlSource *source);
size_t fl_source_line(const FlSource *source, size_t offset);
size_t fl_source_column(const FlSource *source, size_t offset);
FlSpan fl_span(size_t start, size_t end);
void fl_diag_emit(FILE *out,
        const FlDiagnostic *diag,
        const FlSource *source,
        FlDiagFormat format,
        FlColorMode color);
const char *fl_diag_applicability(FlApplicability applicability);

#endif
