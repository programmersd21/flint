/* SPDX-License-Identifier: MIT */
/*
 * __json_parse(text) -> a flint value, or nil on malformed input.
 *
 * A recursive-descent parser over the existing value model: objects become
 * flint tables, arrays become lists, and there is no third kind of thing.
 * The mapping is the whole design decision and it is the obvious one:
 * JSON has two container types and flint has two.
 *
 * A pointer into the source, with a small skip-whitespace helper, and an error
 * string that says how far it got. flint has no exceptions, so failure is a
 * nil return plus a message on the VM, which is what every other native does.
 */
#include "jsonp.h"

#include "object.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const char *p;
	const char *end;
	VM *vm;
	char err[192];
} Json;

static void json_fail_at(Json *j, const char *base, const char *what)
{
	if (j->err[0] == '\0')
		snprintf(j->err,
		        sizeof(j->err),
		        "%s at offset %d",
		        what,
		        (int)(j->p - base));
}

static const char *json_base;

static void skip_ws(Json *j)
{
	while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' ||
	                                *j->p == '\n' || *j->p == '\r'))
		j->p++;
}

static bool parse_value(Json *j, Value *out);

/* one \uXXXX escape, encoded to utf-8. the surrogate pair case is the part
 * everybody gets wrong; a lone high surrogate becomes U+FFFD rather than
 * invalid utf-8, because a json file with a broken pair in it should still
 * parse into something a string can hold. */
static void append_utf8(char *buf, int *len, unsigned long cp)
{
	if (cp < 0x80) {
		buf[(*len)++] = (char)cp;
	} else if (cp < 0x800) {
		buf[(*len)++] = (char)(0xC0 | (cp >> 6));
		buf[(*len)++] = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		buf[(*len)++] = (char)(0xE0 | (cp >> 12));
		buf[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		buf[(*len)++] = (char)(0x80 | (cp & 0x3F));
	} else {
		buf[(*len)++] = (char)(0xF0 | (cp >> 18));
		buf[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
		buf[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		buf[(*len)++] = (char)(0x80 | (cp & 0x3F));
	}
}

static unsigned long hex4(Json *j)
{
	unsigned long v = 0;
	for (int i = 0; i < 4; i++) {
		if (j->p >= j->end) {
			json_fail_at(j, json_base, "truncated \\u escape");
			return 0;
		}
		char c = *j->p++;
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned long)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned long)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned long)(c - 'A' + 10);
		else {
			json_fail_at(j, json_base, "bad hex digit");
			return 0;
		}
	}
	return v;
}

static bool parse_string(Json *j, Value *out)
{
	if (j->p >= j->end || *j->p != '"') {
		json_fail_at(j, json_base, "expected a string");
		return false;
	}
	j->p++;

	/* a json string can be a lot bigger than the stack, so this is a heap
	 * buffer. the source is already a flint string, so the common case
	 * has no escapes at all and can point straight at it. */
	size_t cap = 128;
	while (cap < (size_t)(j->end - j->p) * 6 + 8)
		cap *= 2;
	char *buf = malloc(cap);
	if (buf == NULL) {
		json_fail_at(j, json_base, "out of memory");
		return false;
	}
	int len = 0;

	while (j->p < j->end && *j->p != '"') {
		if (len + 8 >= (int)cap) {
			free(buf);
			json_fail_at(j, json_base, "string too long");
			return false;
		}
		unsigned char c = (unsigned char)*j->p;
		if (c == '\\') {
			j->p++;
			if (j->p >= j->end) {
				free(buf);
				json_fail_at(j, json_base, "truncated escape");
				return false;
			}
			char e = *j->p++;
			switch (e) {
			case 'n':
				buf[len++] = '\n';
				break;
			case 't':
				buf[len++] = '\t';
				break;
			case 'r':
				buf[len++] = '\r';
				break;
			case 'b':
				buf[len++] = '\b';
				break;
			case 'f':
				buf[len++] = '\f';
				break;
			case '/':
				buf[len++] = '/';
				break;
			case '"':
				buf[len++] = '"';
				break;
			case '\\':
				buf[len++] = '\\';
				break;
			case 'u': {
				unsigned long cp = hex4(j);
				if (j->err[0]) {
					free(buf);
					return false;
				}
				/* a high surrogate followed by \uDCxx is one
				 * code point, and joining them here is the
				 * difference between emoji and mojibake */
				if (cp >= 0xD800 && cp <= 0xDBFF &&
				        j->p + 1 < j->end && j->p[0] == '\\' &&
				        j->p[1] == 'u') {
					const char *save = j->p;
					j->p += 2;
					unsigned long lo = hex4(j);
					if (j->err[0]) {
						free(buf);
						return false;
					}
					if (lo >= 0xDC00 && lo <= 0xDFFF)
						cp = 0x10000 +
						     ((cp - 0xD800) << 10) +
						     (lo - 0xDC00);
					else
						j->p = save;
				}
				/* an unpaired surrogate is not a valid code
				 * point. U+FFFD is what json consumers
				 * generally do and it keeps the
				 * output valid utf-8, which a lone
				 * surrogate would not be. */
				if (cp >= 0xD800 && cp <= 0xDFFF)
					cp = 0xFFFD;
				append_utf8(buf, &len, cp);
				break;
			}
			default:
				free(buf);
				json_fail_at(j, json_base, "unknown escape");
				return false;
			}
		} else if (c < 0x20) {
			free(buf);
			json_fail_at(j,
			        json_base,
			        "raw control character in string");
			return false;
		} else {
			buf[len++] = (char)c;
			j->p++;
		}
	}

	if (j->p >= j->end) {
		free(buf);
		json_fail_at(j, json_base, "unterminated string");
		return false;
	}
	j->p++; /* the closing quote */
	buf[len] = '\0';
	*out = OBJ_VAL(copy_string(j->vm, buf, len));
	free(buf);
	return true;
}

static bool parse_object(Json *j, Value *out)
{
	j->p++; /* { */
	ObjTable *t = new_flint_table(j->vm);
	j->vm->stack_top++; /* a slot is the root; the table is not on the stack */
	/* stack discipline: the table is rooted by the frame's value stack, and
	 * every allocation below can collect. the raw slot is the cheapest root
	 * that exists. */
	j->vm->stack_top[-1] = OBJ_VAL(t);

	skip_ws(j);
	if (j->p < j->end && *j->p == '}') {
		j->p++;
		*out = OBJ_VAL(t);
		j->vm->stack_top--;
		return true;
	}

	for (;;) {
		skip_ws(j);
		Value key;
		if (!parse_string(j, &key)) {
			j->vm->stack_top--;
			return false;
		}
		skip_ws(j);
		if (j->p >= j->end || *j->p != ':') {
			json_fail_at(j, json_base, "expected ':'");
			j->vm->stack_top--;
			return false;
		}
		j->p++;
		skip_ws(j);
		Value v;
		if (!parse_value(j, &v)) {
			j->vm->stack_top--;
			return false;
		}

		if (t->count == t->capacity) {
			int old = t->capacity;
			t->capacity = old > 0 ? old * 2 : 8;
			t->keys = realloc(t->keys,
			        sizeof(ObjString *) * (size_t)t->capacity);
			t->values = realloc(
			        t->values, sizeof(Value) * (size_t)t->capacity);
			if (t->keys == NULL || t->values == NULL) {
				j->vm->stack_top--;
				return false;
			}
		}
		t->keys[t->count] = AS_STRING(key);
		t->values[t->count] = v;
		t->count++;

		skip_ws(j);
		if (j->p < j->end && *j->p == ',') {
			j->p++;
			continue;
		}
		if (j->p < j->end && *j->p == '}') {
			j->p++;
			break;
		}
		json_fail_at(j, json_base, "expected ',' or '}'");
		j->vm->stack_top--;
		return false;
	}

	*out = OBJ_VAL(t);
	j->vm->stack_top--;
	return true;
}

static bool parse_array(Json *j, Value *out)
{
	j->p++; /* [ */
	ObjList *l = new_list(j->vm);
	j->vm->stack_top++;
	j->vm->stack_top[-1] = OBJ_VAL(l);

	skip_ws(j);
	if (j->p < j->end && *j->p == ']') {
		j->p++;
		*out = OBJ_VAL(l);
		j->vm->stack_top--;
		return true;
	}

	for (;;) {
		skip_ws(j);
		Value v;
		if (!parse_value(j, &v)) {
			j->vm->stack_top--;
			return false;
		}
		if (l->count == l->capacity) {
			int old = l->capacity;
			l->capacity = old > 0 ? old * 2 : 8;
			l->items = realloc(
			        l->items, sizeof(Value) * (size_t)l->capacity);
			if (l->items == NULL) {
				j->vm->stack_top--;
				return false;
			}
		}
		l->items[l->count++] = v;

		skip_ws(j);
		if (j->p < j->end && *j->p == ',') {
			j->p++;
			continue;
		}
		if (j->p < j->end && *j->p == ']') {
			j->p++;
			break;
		}
		json_fail_at(j, json_base, "expected ',' or ']'");
		j->vm->stack_top--;
		return false;
	}

	*out = OBJ_VAL(l);
	j->vm->stack_top--;
	return true;
}

static bool parse_number(Json *j, Value *out)
{
	const char *start = j->p;
	if (j->p < j->end && (*j->p == '-' || *j->p == '+'))
		j->p++;
	while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
		j->p++;
	if (j->p < j->end && *j->p == '.') {
		j->p++;
		while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
			j->p++;
	}
	if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
		j->p++;
		if (j->p < j->end && (*j->p == '-' || *j->p == '+'))
			j->p++;
		while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
			j->p++;
	}
	if (j->p == start) {
		json_fail_at(j, json_base, "expected a value");
		return false;
	}
	char buf[64];
	size_t n = (size_t)(j->p - start);
	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	memcpy(buf, start, n);
	buf[n] = '\0';
	*out = NUMBER_VAL(strtod(buf, NULL));
	return true;
}

static bool parse_value(Json *j, Value *out)
{
	skip_ws(j);
	if (j->p >= j->end) {
		json_fail_at(j, json_base, "unexpected end of input");
		return false;
	}

	char c = *j->p;

	/* these four are matched before the generic "starts with" test, so
	 * `null` cannot be read as a bare identifier `nulls`. json has no
	 * bare words other than these three. */
	if (c == 'n' && (size_t)(j->end - j->p) >= 4 &&
	        memcmp(j->p, "null", 4) == 0) {
		j->p += 4;
		*out = NIL_VAL;
		return true;
	}
	if (c == 't' && (size_t)(j->end - j->p) >= 4 &&
	        memcmp(j->p, "true", 4) == 0) {
		j->p += 4;
		*out = TRUE_VAL;
		return true;
	}
	if (c == 'f' && (size_t)(j->end - j->p) >= 5 &&
	        memcmp(j->p, "false", 5) == 0) {
		j->p += 5;
		*out = FALSE_VAL;
		return true;
	}

	if (c == '"')
		return parse_string(j, out);
	if (c == '{')
		return parse_object(j, out);
	if (c == '[')
		return parse_array(j, out);
	if (c == '-' || c == '+' || (c >= '0' && c <= '9'))
		return parse_number(j, out);

	json_fail_at(j, json_base, "expected a value");
	return false;
}

static Value json_parse_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	if (!IS_STRING(argv[0])) {
		vm_runtime_error(
		        vm, "Argument to json.parse() must be a string.");
		return NIL_VAL;
	}

	ObjString *text = AS_STRING(argv[0]);
	json_base = text->chars;

	Json j = {text->chars, text->chars + text->length, vm, {0}};
	Value v;
	if (!parse_value(&j, &v)) {
		vm_runtime_error(vm, "invalid json: %s", j.err);
		return NIL_VAL;
	}
	skip_ws(&j);
	if (j.p != j.end) {
		vm_runtime_error(vm,
		        "invalid json: trailing data at offset %d",
		        (int)(j.p - json_base));
		return NIL_VAL;
	}
	return v;
}

/*
 * stringify. the mirror of the parser, and the part that has to get escaping
 * right, because a json document with a raw quote in a string is not a json
 * document.
 */
static bool str_grow(char **buf, size_t *len, size_t *cap, size_t need)
{
	if (*len + need + 1 <= *cap)
		return true;
	while (*cap < *len + need + 1)
		*cap = *cap < 256 ? 256 : *cap * 2;
	char *bigger = realloc(*buf, *cap);
	if (bigger == NULL)
		return false;
	*buf = bigger;
	return true;
}

static bool str_put(
        char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
	if (!str_grow(buf, len, cap, n))
		return false;
	memcpy(*buf + *len, s, n);
	*len += n;
	(*buf)[*len] = '\0';
	return true;
}

static bool str_putc(char **buf, size_t *len, size_t *cap, char c)
{
	return str_put(buf, len, cap, &c, 1);
}

static bool json_write(VM *vm,
        char **buf,
        size_t *len,
        size_t *cap,
        Value v,
        int depth,
        int pretty);

static bool write_string(
        VM *vm, char **buf, size_t *len, size_t *cap, ObjString *s)
{
	(void)vm;
	if (!str_putc(buf, len, cap, '"'))
		return false;
	for (int i = 0; i < s->length; i++) {
		unsigned char c = (unsigned char)s->chars[i];
		switch (c) {
		case '"':
			if (!str_put(buf, len, cap, "\\\"", 2))
				return false;
			break;
		case '\\':
			if (!str_put(buf, len, cap, "\\\\", 2))
				return false;
			break;
		case '\n':
			if (!str_put(buf, len, cap, "\\n", 2))
				return false;
			break;
		case '\r':
			if (!str_put(buf, len, cap, "\\r", 2))
				return false;
			break;
		case '\t':
			if (!str_put(buf, len, cap, "\\t", 2))
				return false;
			break;
		case '\b':
			if (!str_put(buf, len, cap, "\\b", 2))
				return false;
			break;
		case '\f':
			if (!str_put(buf, len, cap, "\\f", 2))
				return false;
			break;
		default:
			if (c < 0x20) {
				/* the rest of the control range has to be
				 * escaped as \u00xx. a raw control character
				 * inside a json string is malformed and
				 * the reader that produced it was wrong. */
				char esc[7] = {'\\',
				        'u',
				        '0',
				        '0',
				        "0123456789abcdef"[c >> 4],
				        "0123456789abcdef"[c & 15],
				        0};
				if (!str_put(buf, len, cap, esc, 6))
					return false;
			} else if (!str_putc(buf, len, cap, (char)c)) {
				return false;
			}
		}
	}
	return str_putc(buf, len, cap, '"');
}

static bool indent(char **buf, size_t *len, size_t *cap, int depth)
{
	for (int i = 0; i < depth; i++)
		if (!str_put(buf, len, cap, "  ", 2))
			return false;
	return true;
}

static bool json_write(VM *vm,
        char **buf,
        size_t *len,
        size_t *cap,
        Value v,
        int depth,
        int pretty)
{
	if (IS_NIL(v))
		return str_put(buf, len, cap, "null", 4);
	if (IS_TRUE(v))
		return str_put(buf, len, cap, "true", 4);
	if (IS_FALSE(v))
		return str_put(buf, len, cap, "false", 5);
	if (IS_NUMBER(v)) {
		char num[64];
		double d = AS_NUMBER(v);
		/* %.17g round-trips every double, and the shortest
		 * representation of a flint number is what a reader expects
		 * to see. %.15g loses the last bit on some values. */
		snprintf(num, sizeof(num), "%.17g", d);
		return str_put(buf, len, cap, num, strlen(num));
	}
	if (IS_STRING(v))
		return write_string(vm, buf, len, cap, AS_STRING(v));

	if (IS_LIST(v)) {
		ObjList *l = AS_LIST(v);
		if (!str_putc(buf, len, cap, '['))
			return false;
		for (int i = 0; i < l->count; i++) {
			if (i > 0 && !str_putc(buf, len, cap, ','))
				return false;
			if (pretty) {
				if (!str_putc(buf, len, cap, '\n') ||
				        !indent(buf, len, cap, depth + 1))
					return false;
			}
			if (!json_write(vm,
			            buf,
			            len,
			            cap,
			            l->items[i],
			            depth + 1,
			            pretty))
				return false;
		}
		if (pretty && l->count > 0) {
			if (!str_putc(buf, len, cap, '\n') ||
			        !indent(buf, len, cap, depth))
				return false;
		}
		return str_putc(buf, len, cap, ']');
	}

	if (IS_FLINT_TABLE(v)) {
		ObjTable *t = AS_FLINT_TABLE(v);
		if (!str_putc(buf, len, cap, '{'))
			return false;
		for (int i = 0; i < t->count; i++) {
			if (i > 0 && !str_putc(buf, len, cap, ','))
				return false;
			if (pretty) {
				if (!str_putc(buf, len, cap, '\n') ||
				        !indent(buf, len, cap, depth + 1))
					return false;
			}
			if (!write_string(vm, buf, len, cap, t->keys[i]) ||
			        !str_putc(buf, len, cap, ':'))
				return false;
			if (pretty && !str_putc(buf, len, cap, ' '))
				return false;
			if (!json_write(vm,
			            buf,
			            len,
			            cap,
			            t->values[i],
			            depth + 1,
			            pretty))
				return false;
		}
		if (pretty && t->count > 0) {
			if (!str_putc(buf, len, cap, '\n') ||
			        !indent(buf, len, cap, depth))
				return false;
		}
		return str_putc(buf, len, cap, '}');
	}

	/* a function is not a value json can describe. saying so beats
	 * writing `null` and having the reader wonder why. */
	vm_runtime_error(vm, "json: cannot stringify a function");
	return false;
}

static Value stringify_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	size_t cap = 256, len = 0;
	char *buf = malloc(cap);
	if (buf == NULL) {
		vm_runtime_error(vm, "Out of memory in json.stringify.");
		return NIL_VAL;
	}
	buf[0] = '\0';
	if (!json_write(vm, &buf, &len, &cap, argv[0], 0, 0)) {
		free(buf);
		return NIL_VAL;
	}
	ObjString *out = copy_string(vm, buf, (int)len);
	free(buf);
	return OBJ_VAL(out);
}

static Value stringify_pretty_native(VM *vm, int argc, Value *argv)
{
	(void)argc;
	size_t cap = 512, len = 0;
	char *buf = malloc(cap);
	if (buf == NULL) {
		vm_runtime_error(vm, "Out of memory in json.pretty.");
		return NIL_VAL;
	}
	buf[0] = '\0';
	if (!json_write(vm, &buf, &len, &cap, argv[0], 0, 1)) {
		free(buf);
		return NIL_VAL;
	}
	ObjString *out = copy_string(vm, buf, (int)len);
	free(buf);
	return OBJ_VAL(out);
}

void register_json_natives(VM *vm)
{
	vm_define_native(vm, "__json_parse", json_parse_native, 1);
	vm_define_native(vm, "__json_stringify", stringify_native, 1);
	vm_define_native(
	        vm, "__json_stringify_pretty", stringify_pretty_native, 1);
}
