/* SPDX-License-Identifier: MIT */
/*
 * `flint pkg`: path dependencies, end to end, and nothing else.
 *
 * Three commands. `pkg add <path>` records a dependency in flint.toml.
 * `pkg install` copies every dependency into flint_modules/ and writes
 * flint.lock. `pkg list` shows what the lock says is installed. Imports
 * resolve from flint_modules/ (see sys_resolve_module), so there is one
 * module loader, not two.
 *
 * The manifest format is a documented subset of TOML, parsed here by hand:
 * tables, string values, and one level of inline tables. Anything else is
 * an error naming the line, not a silent guess. A strict little parser
 * beats a lenient big one: the file is written by this tool as often as by
 * hand, and both writers should agree on what is legal.
 *
 * What this is not, on purpose: there is no registry, no network, and no
 * version solver beyond exact and caret requirements. A package manager
 * that downloads from nowhere would be fiction; path dependencies are the
 * foundation the registry phase inherits.
 */
#include "pkg.h"
#include "sys.h"

#include "sha256.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#	include <sys/wait.h>
#else
#	include <windows.h>
#endif

/* forward: defined with the file operations below, used by the git
 * layer above it. */
static char *pkg_join(const char *a, const char *b);
static bool pkg_remove_tree(const char *path);
static char *pkg_dup(const char *text);

/* a manifest in memory. values are heap strings the manifest owns. */
typedef struct {
	char *key;
	char *value;
} PkgPair;

typedef struct {
	char *name; /* "dependencies", "packages[2]", ... */
	PkgPair *pairs;
	int count;
	int capacity;
} PkgTable;

typedef struct {
	PkgTable *tables;
	int count;
	int capacity;
} PkgManifest;

typedef struct {
	char *name;
	char *path; /* path dependency, or NULL */
	char *git; /* git URL dependency, or NULL */
	char *rev; /* requested rev, or NULL for the default branch */
	char *version; /* version requirement, may be NULL for "any" */
} PkgDep;

/* defined at the bottom of this file, next to the other graph walking;
 * declared here because install calls it before its definition */
static PkgDep *pkg_expand_deps(PkgDep *list,
        int count,
        int *capacity,
        int *filled,
        const char *base_dir,
        int depth,
        char *error,
        size_t error_size,
        int from);

/*
 * Run git and capture its stdout. argv[0] is "git", the rest are plain
 * arguments -- no shell anywhere, so a URL full of metacharacters is
 * data, not code. stdout is truncated to cap - 1 bytes with a NUL after
 * it; stderr inherits ours, because git's own errors read better
 * unfiltered than wrapped. Returns the exit status, or -1 when the
 * child could not be started or waited for.
 */
#ifndef _WIN32
static int pkg_git(char *const argv[], char *out, size_t cap)
{
	int fd[2];
	if (pipe(fd) != 0)
		return -1;
	fflush(stdout);
	fflush(stderr);
	pid_t pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return -1;
	}
	if (pid == 0) {
		close(fd[0]);
		if (dup2(fd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(fd[1]);
		execvp("git", argv);
		_exit(127);
	}
	close(fd[1]);
	/* NULL output drains: the caller wants the status, not the text. */
	char chunk[1024];
	size_t total = 0;
	for (;;) {
		ssize_t n = read(fd[0], chunk, sizeof(chunk));
		if (n <= 0)
			break;
		if (out != NULL && cap > 0 && total < cap - 1) {
			size_t room = cap - 1 - total;
			size_t take = (size_t)n < room ? (size_t)n : room;
			memcpy(out + total, chunk, take);
			total += take;
		}
	}
	if (out != NULL && cap > 0)
		out[total] = '\0';
	close(fd[0]);
	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		/* EINTR is the one errno worth retrying on. */
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		if (errno != EINTR)
			return -1;
	}
	if (!WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}
#endif

/* whether git exists at all. checked once per process, at first use:
 * every git failure after that is about the repository, not the tool.
 * POSIX-only: without fork there is no git to check for. */
#ifndef _WIN32
static int pkg_git_present = -1;
#endif

static bool pkg_have_git(void)
{
#ifdef _WIN32
	return false;
#else
	if (pkg_git_present >= 0)
		return pkg_git_present != 0;
	char *argv[] = {"git", "--version", NULL};
	char out[128];
	pkg_git_present = pkg_git(argv, out, sizeof(out)) == 0;
	return pkg_git_present != 0;
#endif
}

/* fnv-1a over the URL, eight hex digits. not a checksum, just a
 * directory name that cannot collide by spelling: two different URLs
 * for the "same" package stay two different mirrors. */
static void pkg_url_hash(const char *url, char out[9])
{
	unsigned long hash = 2166136261UL;
	for (const unsigned char *p = (const unsigned char *)url; *p != '\0';
	        p++) {
		hash ^= *p;
		hash *= 16777619UL;
	}
	snprintf(out, 9, "%08lx", hash & 0xffffffffUL);
}

/* last URL component, sanitized for a directory name. strips a trailing
 * slash and a .git suffix; anything outside letters, digits, _ and -
 * becomes _. empty stays empty, which the caller treats as an error. */
static void pkg_url_slug(
        const char *url, const char *fallback, char *out, size_t size)
{
	const char *base = strrchr(url, '/');
	const char *colon = strrchr(url, ':');
	if (colon != NULL && (base == NULL || colon > base))
		base = colon;
	base = base != NULL ? base + 1 : url;
	if (base[0] == '\0')
		base = fallback;
	size_t len = strlen(base);
	while (len > 0 && base[len - 1] == '/')
		len--;
	if (len > 4 && memcmp(base + len - 4, ".git", 4) == 0)
		len -= 4;
	size_t i = 0;
	for (size_t j = 0; j < len && i + 1 < size; j++) {
		char c = base[j];
		out[i++] = isalnum((unsigned char)c) || c == '_' || c == '-'
		                   ? c
		                   : '_';
	}
	out[i] = '\0';
}

/* the mirror for a URL: ~/.flint/git/<slug>-<hash8>/. created on demand.
 * NULL when HOME is missing or the directory cannot be made -- git
 * dependencies need a cache, and there is nowhere honest to put one. */
static char *pkg_mirror_dir(const char *url, const char *name)
{
	const char *home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return NULL;
	char slug[128];
	pkg_url_slug(url, name, slug, sizeof(slug));
	if (slug[0] == '\0')
		return NULL;
	char hash[9];
	pkg_url_hash(url, hash);
	size_t hlen = strlen(home);
	size_t slen = strlen(slug);
	if (hlen > (size_t)-1 - slen - 24)
		return NULL;
	size_t need = hlen + 13 + slen + 1 + 8 + 1;
	char *out = malloc(need);
	if (out == NULL)
		return NULL;
	snprintf(out, need, "%s/.flint/git/%s-%s", home, slug, hash);
	if (!sys_make_dirs(out)) {
		free(out);
		return NULL;
	}
	return out;
}

/* read one file at a rev out of a mirror, for the add-time identity
 * probe: the package's real name lives in its manifest, not its URL. */
static char *pkg_git_show(const char *mirror, const char *rev, const char *path)
{
#ifndef _WIN32
	char spec[4352];
	if (snprintf(spec, sizeof(spec), "%s:%s", rev, path) >=
	        (int)sizeof(spec))
		return NULL;
	char *argv[] = {"git",
	        "-c",
	        "protocol.file.allow=always",
	        "--git-dir",
	        (char *)mirror,
	        "show",
	        spec,
	        NULL};
	/* manifests are small; anything bigger is not a manifest. */
	char *out = malloc(65536);
	if (out == NULL)
		return NULL;
	if (pkg_git(argv, out, 65536) != 0) {
		free(out);
		return NULL;
	}
	return out;
#else
	(void)mirror;
	(void)rev;
	(void)path;
	return NULL;
#endif
}

/* resolve a rev to a commit inside a mirror. empty rev means HEAD, the
 * default branch, whatever it is this week. */
static bool pkg_resolve_commit(
        const char *mirror, const char *rev, char sha[41])
{
#ifndef _WIN32
	char *argv[] = {"git",
	        "-c",
	        "protocol.file.allow=always",
	        "--git-dir",
	        (char *)mirror,
	        "rev-parse",
	        "--verify",
	        NULL,
	        NULL};
	char want[256];
	const char *r = (rev != NULL && rev[0] != '\0') ? rev : "HEAD";
	if (snprintf(want, sizeof(want), "%s^{commit}", r) >= (int)sizeof(want))
		return NULL;
	argv[7] = want;
	char out[128];
	if (pkg_git(argv, out, sizeof(out)) != 0)
		return NULL;
	size_t len = strlen(out);
	while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
		out[--len] = '\0';
	if (len != 40)
		return NULL;
	memcpy(sha, out, 41);
	return true;
#else
	(void)mirror;
	(void)rev;
	(void)sha;
	return false;
#endif
}

static void pkg_free_manifest(PkgManifest *manifest)
{
	for (int i = 0; i < manifest->count; i++) {
		PkgTable *table = &manifest->tables[i];
		free(table->name);
		for (int j = 0; j < table->count; j++) {
			free(table->pairs[j].key);
			free(table->pairs[j].value);
		}
		free(table->pairs);
	}
	free(manifest->tables);
	memset(manifest, 0, sizeof(*manifest));
}

static PkgTable *pkg_table(PkgManifest *manifest, const char *name, bool create)
{
	for (int i = 0; i < manifest->count; i++) {
		if (strcmp(manifest->tables[i].name, name) == 0)
			return &manifest->tables[i];
	}
	if (!create)
		return NULL;
	if (manifest->count >= 256)
		return NULL;
	PkgTable *grown = realloc(manifest->tables,
	        (size_t)(manifest->count + 1) * sizeof(PkgTable));
	if (grown == NULL)
		return NULL;
	manifest->tables = grown;
	PkgTable *table = &manifest->tables[manifest->count++];
	memset(table, 0, sizeof(*table));
	table->name = malloc(strlen(name) + 1);
	if (table->name == NULL) {
		manifest->count--;
		return NULL;
	}
	memcpy(table->name, name, strlen(name) + 1);
	return table;
}

static bool pkg_set(PkgTable *table, const char *key, const char *value)
{
	for (int i = 0; i < table->count; i++) {
		if (strcmp(table->pairs[i].key, key) == 0) {
			char *copy = malloc(strlen(value) + 1);
			if (copy == NULL)
				return NULL;
			memcpy(copy, value, strlen(value) + 1);
			free(table->pairs[i].value);
			table->pairs[i].value = copy;
			return true;
		}
	}
	if (table->count >= 256)
		return NULL;
	PkgPair *grown = realloc(
	        table->pairs, (size_t)(table->count + 1) * sizeof(PkgPair));
	if (grown == NULL)
		return NULL;
	table->pairs = grown;
	PkgPair *pair = &table->pairs[table->count++];
	pair->key = malloc(strlen(key) + 1);
	pair->value = malloc(strlen(value) + 1);
	if (pair->key == NULL || pair->value == NULL) {
		free(pair->key);
		free(pair->value);
		table->count--;
		return NULL;
	}
	memcpy(pair->key, key, strlen(key) + 1);
	memcpy(pair->value, value, strlen(value) + 1);
	return true;
}

static const char *pkg_get(
        const PkgManifest *manifest, const char *table, const char *key)
{
	for (int i = 0; i < manifest->count; i++) {
		if (strcmp(manifest->tables[i].name, table) != 0)
			continue;
		for (int j = 0; j < manifest->tables[i].count; j++) {
			if (strcmp(manifest->tables[i].pairs[j].key, key) == 0)
				return manifest->tables[i].pairs[j].value;
		}
	}
	return NULL;
}

/* TOML subset parser state. errors name the file and line. */
typedef struct {
	const char *path;
	const char *text;
	int lineno;
	char error[256];
} PkgParse;

static void pkg_fail(PkgParse *state, const char *message)
{
	snprintf(state->error,
	        sizeof(state->error),
	        "%s:%d: %s",
	        state->path,
	        state->lineno,
	        message);
}

/* skip blanks. comments end the line everywhere but inside strings, and
 * string parsing never calls this, so a # inside quotes is safe. */
static const char *pkg_skip(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	return p;
}

/* parse a basic string at *pp. handles \\ \" \n \t; anything else after
 * a backslash is the character itself, like the language. */
static char *pkg_string(PkgParse *state, const char **pp, const char *end)
{
	const char *p = *pp;
	if (p >= end || *p != '"') {
		pkg_fail(state, "expected a quoted string here");
		return NULL;
	}
	p++;
	size_t cap = 32;
	size_t len = 0;
	char *out = malloc(cap);
	if (out == NULL)
		return NULL;
	while (p < end && *p != '"') {
		char c = *p++;
		if (c == '\n') {
			free(out);
			pkg_fail(state,
			        "strings here stay on one line; "
			        "a value that needs more lines does not "
			        "belong in a manifest");
			return NULL;
		}
		if (c == '\\' && p < end) {
			char e = *p++;
			switch (e) {
			case 'n':
				c = '\n';
				break;
			case 't':
				c = '\t';
				break;
			default:
				c = e;
				break;
			}
		}
		if (len + 1 >= cap) {
			cap *= 2;
			char *grown = realloc(out, cap);
			if (grown == NULL) {
				free(out);
				return NULL;
			}
			out = grown;
		}
		out[len++] = c;
	}
	if (p >= end) {
		free(out);
		pkg_fail(state, "unterminated string");
		return NULL;
	}
	out[len] = '\0';
	*pp = p + 1;
	return out;
}

static char *pkg_bare(const char **pp, const char *end)
{
	const char *start = *pp;
	while (*pp < end &&
	        (isalnum((unsigned char)**pp) || **pp == '_' || **pp == '-'))
		(*pp)++;
	size_t len = (size_t)(*pp - start);
	char *out = malloc(len + 1);
	if (out == NULL)
		return NULL;
	memcpy(out, start, len);
	out[len] = '\0';
	return out;
}

/* one `key = value` line inside table. value is a string or a single-line
 * inline table of strings; the table is flattened as key.sub, so
 * `utils = { path = "../utils" }` reads back as utils.path. */
static bool pkg_line(
        PkgParse *state, PkgTable *table, const char *line, const char *end)
{
	const char *p = pkg_skip(line, end);
	if (p >= end || *p == '#' || *p == '\n')
		return true;
	char *key = pkg_bare(&p, end);
	if (key == NULL || key[0] == '\0') {
		free(key);
		pkg_fail(state, "expected key = value here");
		return NULL;
	}
	p = pkg_skip(p, end);
	if (p >= end || *p != '=') {
		free(key);
		pkg_fail(state, "expected = after the key");
		return NULL;
	}
	p = pkg_skip(p + 1, end);
	bool ok = false;
	if (p < end && *p == '{') {
		p++;
		for (;;) {
			p = pkg_skip(p, end);
			if (p < end && *p == '}') {
				p++;
				ok = true;
				break;
			}
			char *sub = pkg_bare(&p, end);
			if (sub == NULL || sub[0] == '\0') {
				free(sub);
				break;
			}
			p = pkg_skip(p, end);
			if (p >= end || *p != '=') {
				free(sub);
				break;
			}
			p = pkg_skip(p + 1, end);
			char *value = pkg_string(state, &p, end);
			if (value == NULL) {
				free(sub);
				break;
			}
			size_t need = strlen(key) + 1 + strlen(sub) + 1;
			char *flat = malloc(need);
			if (flat == NULL) {
				free(sub);
				free(value);
				break;
			}
			snprintf(flat, need, "%s.%s", key, sub);
			free(sub);
			if (!pkg_set(table, flat, value)) {
				free(flat);
				free(value);
				break;
			}
			free(flat);
			free(value);
			p = pkg_skip(p, end);
			if (p < end && *p == ',') {
				p++;
				continue;
			}
			if (p < end && *p == '}') {
				p++;
				ok = true;
				break;
			}
			break;
		}
		free(key);
		if (!ok) {
			pkg_fail(state,
			        "inline tables hold string values: "
			        "{ path = \"...\", version = \"...\" }");
			return NULL;
		}
	} else {
		char *value = pkg_string(state, &p, end);
		if (value == NULL) {
			free(key);
			return NULL;
		}
		ok = pkg_set(table, key, value);
		free(key);
		free(value);
		if (!ok) {
			pkg_fail(state, "out of memory");
			return NULL;
		}
	}
	p = pkg_skip(p, end);
	if (p < end && *p != '#' && *p != '\n') {
		pkg_fail(state, "unexpected text after the value");
		return NULL;
	}
	return true;
}

static bool pkg_parse_text(
        PkgParse *state, PkgManifest *manifest, const char *text)
{
	PkgTable *table = NULL;
	const char *p = text;
	state->lineno = 1;
	while (*p != '\0') {
		const char *eol = strchr(p, '\n');
		const char *end = eol != NULL ? eol : p + strlen(p);
		const char *q = pkg_skip(p, end);
		if (q < end && *q == '[') {
			bool array = (q + 1 < end && q[1] == '[');
			const char *name_start = q + (array ? 2 : 1);
			const char *name_end = strchr(name_start, ']');
			if (name_end == NULL || name_end >= end) {
				pkg_fail(state, "unterminated [table] header");
				return NULL;
			}
			size_t len = (size_t)(name_end - name_start);
			char *name = malloc(len + 1);
			if (name == NULL)
				return NULL;
			memcpy(name, name_start, len);
			name[len] = '\0';
			if (array) {
				/* [[packages]] sections number themselves:
				 * packages[0], packages[1], in file order. */
				int n = 0;
				for (int i = 0; i < manifest->count; i++) {
					if (strncmp(manifest->tables[i].name,
					            name,
					            len) == 0 &&
					        manifest->tables[i].name[len] ==
					                '[')
						n++;
				}
				char numbered[256];
				snprintf(numbered,
				        sizeof(numbered),
				        "%s[%d]",
				        name,
				        n);
				free(name);
				name = malloc(strlen(numbered) + 1);
				if (name == NULL)
					return NULL;
				memcpy(name, numbered, strlen(numbered) + 1);
			}
			table = pkg_table(manifest, name, true);
			free(name);
			if (table == NULL) {
				pkg_fail(state, "out of memory");
				return NULL;
			}
		} else if (q < end && *q != '#' && *q != '\n') {
			if (table == NULL) {
				pkg_fail(state,
				        "a value before any [table] header");
				return NULL;
			}
			if (!pkg_line(state, table, p, end))
				return NULL;
		} else {
			state->lineno += (eol != NULL);
			p = eol != NULL ? eol + 1 : end;
			continue;
		}
		state->lineno += (eol != NULL);
		p = eol != NULL ? eol + 1 : end;
	}
	return true;
}

static char *pkg_read_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return NULL;
	}
	long size = ftell(file);
	if (size < 0) {
		fclose(file);
		return NULL;
	}
	if (fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	char *text = malloc((size_t)size + 1);
	if (text == NULL) {
		fclose(file);
		return NULL;
	}
	size_t got = fread(text, 1, (size_t)size, file);
	fclose(file);
	if (got < (size_t)size) {
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

static bool pkg_load(
        const char *path, PkgManifest *manifest, char *error, size_t error_size)
{
	memset(manifest, 0, sizeof(*manifest));
	char *text = pkg_read_file(path);
	if (text == NULL) {
		snprintf(error, error_size, "cannot read '%s'", path);
		return NULL;
	}
	PkgParse state;
	memset(&state, 0, sizeof(state));
	state.path = path;
	state.text = text;
	bool ok = pkg_parse_text(&state, manifest, text);
	if (!ok) {
		snprintf(error, error_size, "%s", state.error);
		pkg_free_manifest(manifest);
	}
	free(text);
	return ok;
}

/* semantic versions: three nonnegative integers. anything else is not a
 * version, and comparing it would be guessing. */
typedef struct {
	long major;
	long minor;
	long patch;
} PkgVersion;

static bool pkg_version_parse(const char *text, PkgVersion *version)
{
	char *end = NULL;
	errno = 0;
	long parts[3];
	const char *p = text;
	for (int i = 0; i < 3; i++) {
		if (i > 0) {
			if (*p != '.')
				return NULL;
			p++;
		}
		if (!isdigit((unsigned char)*p))
			return NULL;
		errno = 0;
		long v = strtol(p, &end, 10);
		if (errno != 0 || end == p || v < 0)
			return NULL;
		parts[i] = v;
		p = end;
	}
	if (*p != '\0')
		return NULL;
	version->major = parts[0];
	version->minor = parts[1];
	version->patch = parts[2];
	return true;
}

static int pkg_version_compare(const PkgVersion *a, const PkgVersion *b)
{
	if (a->major != b->major)
		return a->major < b->major ? -1 : 1;
	if (a->minor != b->minor)
		return a->minor < b->minor ? -1 : 1;
	if (a->patch != b->patch)
		return a->patch < b->patch ? -1 : 1;
	return 0;
}

/* a requirement is exact ("1.2.3") or caret ("^1.2.3": same major,
 * at least this version). anything else is an error, because a
 * requirement the tool cannot check is a promise it cannot keep. */
static bool pkg_satisfies(const char *requirement,
        const PkgVersion *have,
        char *error,
        size_t error_size)
{
	bool caret = requirement[0] == '^';
	const char *text = caret ? requirement + 1 : requirement;
	PkgVersion want;
	if (!pkg_version_parse(text, &want)) {
		snprintf(error,
		        error_size,
		        "bad version requirement '%s': exact "
		        "\"1.2.3\" or caret \"^1.2.3\"",
		        requirement);
		return NULL;
	}
	if (!caret) {
		if (pkg_version_compare(have, &want) != 0) {
			snprintf(error,
			        error_size,
			        "does not satisfy '%s'",
			        requirement);
			return NULL;
		}
		return true;
	}
	if (have->major != want.major || pkg_version_compare(have, &want) < 0) {
		snprintf(error,
		        error_size,
		        "does not satisfy '%s'",
		        requirement);
		return NULL;
	}
	return true;
}

/*
 * The mirror exists and is usable: cloned once, fetched when asked, or
 * when the commit we need is not in it. A mirror that will not clone or
 * fetch is reported with git's own complaint on stderr plus one line of
 * ours saying which URL it was for -- git already explains itself, and
 * wrapping that twice helps nobody.
 */
static bool pkg_ensure_mirror_unlocked(const char *mirror,
        const char *url,
        bool fetch,
        const char *commit,
        char *error,
        size_t error_size)
{
#ifndef _WIN32
	char head[4352];
	if (snprintf(head, sizeof(head), "%s/HEAD", mirror) >=
	        (int)sizeof(head)) {
		snprintf(error, error_size, "path too long");
		return false;
	}
	if (access(head, F_OK) != 0) {
		char staging[4352];
		bool made_staging = false;
		for (unsigned int attempt = 0; attempt < 100; attempt++) {
			int n = snprintf(staging,
			        sizeof(staging),
			        "%s.tmp.%ld.%u",
			        mirror,
			        (long)getpid(),
			        attempt);
			if (n < 0 || (size_t)n >= sizeof(staging)) {
				snprintf(error, error_size, "path too long");
				return false;
			}
			if (mkdir(staging, 0700) == 0) {
				made_staging = true;
				break;
			}
			if (errno != EEXIST) {
				snprintf(error, error_size, "cannot create mirror staging directory");
				return false;
			}
		}
		if (!made_staging) {
			snprintf(error, error_size, "cannot create mirror staging directory");
			return false;
		}
		char *argv[] = {"git",
		        "-c",
		        "protocol.file.allow=always",
		        "clone",
		        "--mirror",
		        "--",
		        (char *)url,
		        staging,
		        NULL};
		if (pkg_git(argv, NULL, 1) != 0) {
			(void)pkg_remove_tree(staging);
			snprintf(error, error_size, "cannot clone '%s'", url);
			return false;
		}

		struct stat st;
		if (lstat(mirror, &st) == 0) {
			if (!pkg_remove_tree(mirror)) {
				(void)pkg_remove_tree(staging);
				snprintf(error, error_size, "cannot remove damaged mirror '%s'", mirror);
				return false;
			}
		} else if (errno != ENOENT) {
			(void)pkg_remove_tree(staging);
			snprintf(error, error_size, "cannot inspect mirror '%s'", mirror);
			return false;
		}
		if (rename(staging, mirror) != 0) {
			(void)pkg_remove_tree(staging);
			snprintf(error, error_size, "cannot publish mirror '%s'", mirror);
			return false;
		}
		return true;
	}
	bool need_fetch = fetch;
	if (!need_fetch && commit != NULL) {
		char sha[41];
		need_fetch = !pkg_resolve_commit(mirror, commit, sha) ||
		             strcmp(sha, commit) != 0;
	}
	if (need_fetch) {
		char *argv[] = {"git",
		        "-c",
		        "protocol.file.allow=always",
		        "--git-dir",
		        (char *)mirror,
		        "fetch",
		        "origin",
		        NULL};
		if (pkg_git(argv, NULL, 1) != 0) {
			snprintf(error, error_size, "cannot fetch '%s'", url);
			return false;
		}
	}
	return true;
#else
	(void)mirror;
	(void)url;
	(void)fetch;
	(void)commit;
	snprintf(error, error_size, "git dependencies need a POSIX system");
	return false;
#endif
}

static bool pkg_ensure_mirror(const char *mirror,
        const char *url,
        bool fetch,
        const char *commit,
        char *error,
        size_t error_size)
{
#ifndef _WIN32
	char lock_path[4352];
	if (snprintf(lock_path, sizeof(lock_path), "%s.lock", mirror) >=
	        (int)sizeof(lock_path)) {
		snprintf(error, error_size, "path too long");
		return false;
	}
	int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
	if (lock_fd < 0) {
		snprintf(error, error_size, "cannot open mirror lock '%s'", lock_path);
		return false;
	}
	struct flock lock = {0};
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	while (fcntl(lock_fd, F_SETLKW, &lock) != 0) {
		if (errno == EINTR)
			continue;
		close(lock_fd);
		snprintf(error, error_size, "cannot lock mirror '%s'", mirror);
		return false;
	}
	bool ok = pkg_ensure_mirror_unlocked(
	        mirror, url, fetch, commit, error, error_size);
	lock.l_type = F_UNLCK;
	(void)fcntl(lock_fd, F_SETLK, &lock);
	close(lock_fd);
	return ok;
#else
	(void)mirror;
	(void)url;
	(void)fetch;
	(void)commit;
	snprintf(error, error_size, "git dependencies need a POSIX system");
	return false;
#endif
}

/* copy a commit out of a mirror into dest: clone, detach at the pin,
 * drop the .git, so what lands in flint_modules/ is source, not a
 * repository. dest must not exist; the caller clears it first. */
static bool pkg_materialize(const char *mirror,
        const char *sha,
        const char *dest,
        char *error,
        size_t error_size)
{
#ifndef _WIN32
	char *clone_argv[] = {"git",
	        "-c",
	        "protocol.file.allow=always",
	        "clone",
	        "--quiet",
	        "--",
	        (char *)mirror,
	        (char *)dest,
	        NULL};
	if (pkg_git(clone_argv, NULL, 1) != 0) {
		snprintf(error, error_size, "cannot check out '%s'", sha);
		return NULL;
	}
	char *checkout_argv[] = {"git",
	        "-C",
	        (char *)dest,
	        "checkout",
	        "--quiet",
	        "--detach",
	        (char *)sha,
	        NULL};
	if (pkg_git(checkout_argv, NULL, 1) != 0) {
		snprintf(error, error_size, "cannot check out '%s'", sha);
		return NULL;
	}
	char *dotgit = pkg_join(dest, ".git");
	if (dotgit == NULL) {
		snprintf(error, error_size, "out of memory");
		return NULL;
	}
	bool ok = pkg_remove_tree(dotgit);
	free(dotgit);
	if (!ok) {
		snprintf(error, error_size, "cannot clean '%s'", dest);
		return NULL;
	}
	return true;
#else
	(void)mirror;
	(void)sha;
	(void)dest;
	snprintf(error, error_size, "git dependencies need a POSIX system");
	return false;
#endif
}

/* stat a tree entry: 0 with st filled, -1 on failure, 1 when the
 * entry is a link and must be skipped rather than followed. Links are
 * symlinks on POSIX (lstat) and reparse points on Windows (attributes:
 * stat would follow them). Copying or hashing through one would read
 * outside the tree being installed, so the walks skip them exactly as
 * they skip anything that is not a regular file or directory. */
static int pkg_stat_entry(const char *path, struct stat *st)
{
#ifdef _WIN32
	DWORD attrs = GetFileAttributesA(path);
	if (attrs == INVALID_FILE_ATTRIBUTES)
		return -1;
	if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		return 1;
	return stat(path, st) == 0 ? 0 : -1;
#else
	if (lstat(path, st) != 0)
		return -1;
	if (S_ISLNK(st->st_mode))
		return 1;
	return 0;
#endif
}

/* join two path components. NULL on overflow or allocation failure. */
static char *pkg_join(const char *a, const char *b)
{
	size_t alen = strlen(a);
	size_t blen = strlen(b);
	if (alen > (size_t)-1 - blen - 2)
		return NULL;
	char *out = malloc(alen + 1 + blen + 1);
	if (out == NULL)
		return NULL;
	if (snprintf(out, alen + 1 + blen + 1, "%s/%s", a, b) < 0) {
		free(out);
		return NULL;
	}
	return out;
}

/* copy one regular file. short writes and read errors fail loudly:
 * a truncated dependency is worse than none. */
static bool pkg_copy_file(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb");
	if (in == NULL)
		return NULL;
	FILE *out = fopen(to, "wb");
	if (out == NULL) {
		fclose(in);
		return NULL;
	}
	if (fseek(in, 0, SEEK_END) != 0) {
		fclose(in);
		fclose(out);
		return NULL;
	}
	long size = ftell(in);
	if (size < 0 || fseek(in, 0, SEEK_SET) != 0) {
		fclose(in);
		fclose(out);
		return NULL;
	}
	bool ok = true;
	char buf[8192];
	while (size > 0) {
		size_t want =
		        (size_t)size < sizeof(buf) ? (size_t)size : sizeof(buf);
		size_t n = 0;
		/* NOLINTNEXTLINE(clang-analyzer-unix.Stream) */
		n = fread(buf, 1, want, in);
		if (n == 0) {
			ok = false;
			break;
		}
		if (fwrite(buf, 1, n, out) != n) {
			ok = false;
			break;
		}
		size -= (long)n;
	}
	fclose(in);
	/* the loop above only re-reads when the stream had a shorter
	 * item than wanted; after it, feof-state re-reads are guarded
	 * by the size counter, but the analyzer cannot see that. */
	/* NOLINTNEXTLINE(clang-analyzer-unix.Stream) */
	if (ok && size != 0)
		ok = false;
	if (fclose(out) != 0)
		ok = false;
	return ok;
}

/* copy a tree: regular files, directories, and nothing else. dotfiles
 * and dot-directories are skipped, so a .git beside the sources does
 * not ride along -- that history is not the dependency. symlinks are
 * skipped for the same reason a guess is not a copy. */
static bool pkg_copy_tree(const char *from, const char *to)
{
	DIR *dir = opendir(from);
	if (dir == NULL)
		return NULL;
	if (!sys_make_dirs(to)) {
		closedir(dir);
		return NULL;
	}
	bool ok = true;
	struct dirent *entry;
	while (ok && (entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		        strcmp(entry->d_name, "..") == 0 ||
		        entry->d_name[0] == '.')
			continue;
		char *src = pkg_join(from, entry->d_name);
		char *dst = pkg_join(to, entry->d_name);
		if (src == NULL || dst == NULL) {
			free(src);
			free(dst);
			ok = false;
			break;
		}
		struct stat st;
		int sr = pkg_stat_entry(src, &st);
		if (sr < 0) {
			free(src);
			free(dst);
			ok = false;
			break;
		}
		if (sr == 0 && S_ISDIR(st.st_mode)) {
			ok = pkg_copy_tree(src, dst);
		} else if (sr == 0 && S_ISREG(st.st_mode)) {
			ok = pkg_copy_file(src, dst);
		}
		/* anything else -- links, fifos, sockets -- is left out,
		 * for the same reason dotfiles are: that history is not
		 * the dependency. */
		free(src);
		free(dst);
	}
	closedir(dir);
	return ok;
}

/* remove a tree, for reinstalling over a previous copy. errors fail
 * loudly: a half-removed directory followed by a copy is a merged
 * one, and merged dependencies are how stale files haunt builds. */
static bool pkg_remove_entry(const char *child)
{
	struct stat st;
	int sr = pkg_stat_entry(child, &st);
	if (sr < 0)
		return false;
	if (sr == 0 && S_ISDIR(st.st_mode))
		return pkg_remove_tree(child);
	if (remove(child) == 0)
		return true;
#ifdef _WIN32
	/* a reparse point is removed as a link, never followed: it may be
	 * a junction, which rmdir unlinks without recursing. remove()
	 * first for symlinks to files. */
	return rmdir(child) == 0;
#else
	return false;
#endif
}

static bool pkg_remove_tree(const char *path)
{
	struct stat st;
	int sr = pkg_stat_entry(path, &st);
	if (sr < 0)
		return false;
	if (sr != 0 || !S_ISDIR(st.st_mode))
		return remove(path) == 0;

	DIR *dir = opendir(path);
	if (dir == NULL)
		return false;
	bool ok = true;
	struct dirent *entry;
	while (ok && (entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		        strcmp(entry->d_name, "..") == 0)
			continue;
		char *child = pkg_join(path, entry->d_name);
		if (child == NULL) {
			ok = false;
			break;
		}
		ok = pkg_remove_entry(child);
		free(child);
	}
	closedir(dir);
	return ok && rmdir(path) == 0;
}

/* hash an installed tree into 64 hex characters: the `content` the lock
 * records beside a pinned commit. what it covers, exactly:
 *
 *   - every regular file under dir, by path relative to dir
 *   - paths joined with '/', so the hash is identical on every OS
 *   - paths sorted byte-wise, because readdir order is the filesystem's
 *     business, not ours
 *   - file bytes, framed as path + NUL + u64-LE size + bytes, so two
 *     different trees cannot frame the same stream
 *
 * what it skips: dotfiles and dot-directories, symlinks, and anything
 * that is not a regular file -- the same filter pkg_copy_tree applies,
 * because hashing what the copy would not have copied makes the hash a
 * function of the source directory rather than of the installed tree.
 * metadata (mtimes, modes, owners) is deliberately excluded: a git
 * checkout restores bytes, not timestamps, so the same pin must hash
 * the same on every machine.
 *
 * false on any I/O error or allocation failure. */
static int pkg_name_compare(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool pkg_hash_file(FlSha256 *ctx, const char *relative, const char *full)
{
	FILE *in = fopen(full, "rb");
	if (in == NULL)
		return false;
	if (fseek(in, 0, SEEK_END) != 0) {
		fclose(in);
		return false;
	}
	long size = ftell(in);
	if (size < 0 || fseek(in, 0, SEEK_SET) != 0) {
		fclose(in);
		return false;
	}
	fl_sha256_update(ctx, relative, strlen(relative) + 1);
	uint64_t n = (uint64_t)size;
	uint8_t framed[8];
	for (int i = 0; i < 8; i++)
		framed[i] = (uint8_t)(n >> (8 * i));
	fl_sha256_update(ctx, framed, sizeof(framed));
	bool ok = true;
	uint8_t buf[8192];
	while (size > 0) {
		size_t want =
		        (size_t)size < sizeof(buf) ? (size_t)size : sizeof(buf);
		size_t got = fread(buf, 1, want, in);
		if (got == 0) {
			ok = false;
			break;
		}
		fl_sha256_update(ctx, buf, got);
		size -= (long)got;
	}
	if (fclose(in) != 0)
		ok = false;
	return ok && size == 0;
}

static bool pkg_hash_walk(FlSha256 *ctx, const char *dir, const char *relative)
{
	DIR *stream = opendir(dir);
	if (stream == NULL)
		return false;
	/* collected first, then sorted: readdir order must not leak into
	 * the digest. */
	char **names = NULL;
	int count = 0;
	int capacity = 0;
	bool ok = true;
	struct dirent *entry;
	while (ok && (entry = readdir(stream)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		        strcmp(entry->d_name, "..") == 0 ||
		        entry->d_name[0] == '.')
			continue;
		if (count == capacity) {
			int grown = capacity == 0 ? 16 : capacity * 2;
			char **bigger =
			        realloc(names, (size_t)grown * sizeof(*names));
			if (bigger == NULL) {
				ok = false;
				break;
			}
			names = bigger;
			capacity = grown;
		}
		names[count] = pkg_dup(entry->d_name);
		if (names[count] == NULL) {
			ok = false;
			break;
		}
		count++;
	}
	closedir(stream);
	if (ok && count > 0) {
		qsort(names, (size_t)count, sizeof(*names), pkg_name_compare);
		for (int i = 0; i < count && ok; i++) {
			char child_relative[4096];
			if (relative[0] == '\0')
				snprintf(child_relative,
				        sizeof(child_relative),
				        "%s",
				        names[i]);
			else
				snprintf(child_relative,
				        sizeof(child_relative),
				        "%s/%s",
				        relative,
				        names[i]);
			/* a relative path that does not fit is refused, not
			 * truncated: a truncated path hashes a different tree
			 * than the one installed. */
			if (strlen(child_relative) >=
			        sizeof(child_relative) - 1) {
				ok = false;
				break;
			}
			char *full = pkg_join(dir, names[i]);
			if (full == NULL) {
				ok = false;
				break;
			}
			struct stat st;
			int sr = pkg_stat_entry(full, &st);
			if (sr < 0) {
				free(full);
				ok = false;
				break;
			}
			if (sr == 0 && S_ISDIR(st.st_mode))
				ok = pkg_hash_walk(ctx, full, child_relative);
			else if (sr == 0 && S_ISREG(st.st_mode))
				ok = pkg_hash_file(ctx, child_relative, full);
			free(full);
		}
	}
	for (int i = 0; i < count; i++)
		free(names[i]);
	free(names);
	return ok;
}

static bool pkg_hash_tree(const char *dir, char out[65])
{
	FlSha256 ctx;
	fl_sha256_init(&ctx);
	if (!pkg_hash_walk(&ctx, dir, ""))
		return false;
	uint8_t digest[32];
	fl_sha256_final(&ctx, digest);
	char *hex = out;
	for (int i = 0; i < 32; i++) {
		snprintf(hex, 3, "%02x", digest[i]);
		hex += 2;
	}
	*hex = '\0';
	return true;
}

static void pkg_usage(FILE *stream)
{
	bool c = isatty(fileno(stream));

	if (c) {
		fprintf(stream,
		        "usage: \x1b[1;36mflint pkg\x1b[0m "
			"\x1b[32m<command>\x1b[0m "
			"\x1b[33m[options]\x1b[0m\n\n");
		fprintf(stream,
		        "manage dependencies declared in flint.toml.\n\n");

		fprintf(stream, "\x1b[1mcommands:\x1b[0m\n");
		fprintf(stream,
		        "  \x1b[32minstall\x1b[0m            resolve manifest "
			"dependencies into flint_modules/\n");
		fprintf(stream,
		        "  \x1b[32madd\x1b[0m \x1b[33m<path|url>\x1b[0m     "
			"add dependency to flint.toml and install\n");
		fprintf(stream,
		        "  \x1b[32mupdate\x1b[0m \x1b[33m[name]\x1b[0m      "
			"re-resolve git pins and reinstall\n");
		fprintf(stream,
		        "  \x1b[32mlist\x1b[0m               list installed "
			"packages from flint.lock\n\n");

		fprintf(stream,
		        "\x1b[1mmanifest format (flint.toml):\x1b[0m\n");
		fprintf(stream, "  name = \"../path\"\n");
		fprintf(stream,
		        "  name = { path = \"../path\", version = \"^1.0.0\" "
			"}\n");
		fprintf(stream,
		        "  name = { git = \"https://...\", rev = \"v1.0.0\" "
			"}\n");
	} else {
		fprintf(stream, "usage: flint pkg <command> [options]\n\n");
		fprintf(stream,
		        "manage dependencies declared in flint.toml.\n\n");

		fprintf(stream, "commands:\n");
		fprintf(stream,
		        "  install           resolve manifest dependencies "
			"into flint_modules/\n");
		fprintf(stream,
		        "  add <path|url>    add dependency to flint.toml and "
			"install\n");
		fprintf(stream,
		        "  update [name]     re-resolve git pins and "
			"reinstall\n");
		fprintf(stream,
		        "  list              list installed packages from "
			"flint.lock\n\n");

		fprintf(stream, "manifest format (flint.toml):\n");
		fprintf(stream, "  name = \"../path\"\n");
		fprintf(stream,
		        "  name = { path = \"../path\", version = \"^1.0.0\" "
			"}\n");
		fprintf(stream,
		        "  name = { git = \"https://...\", rev = \"v1.0.0\" "
			"}\n");
	}
}

/* read the [package] name and version out of a dependency's manifest. */
static bool pkg_read_identity(const char *manifest_path,
        char **name,
        char **version,
        char *error,
        size_t error_size)
{
	PkgManifest manifest;
	if (!pkg_load(manifest_path, &manifest, error, error_size))
		return NULL;
	const char *n = pkg_get(&manifest, "package", "name");
	const char *v = pkg_get(&manifest, "package", "version");
	bool ok = true;
	if (n == NULL || n[0] == '\0') {
		snprintf(error,
		        error_size,
		        "%s: [package] needs a name",
		        manifest_path);
		ok = false;
	} else if (v == NULL || v[0] == '\0') {
		snprintf(error,
		        error_size,
		        "%s: [package] needs a version",
		        manifest_path);
		ok = false;
	} else {
		PkgVersion parsed;
		if (!pkg_version_parse(v, &parsed)) {
			snprintf(error,
			        error_size,
			        "%s: version '%s' is not X.Y.Z",
			        manifest_path,
			        v);
			ok = false;
		}
	}
	if (ok) {
		*name = malloc(strlen(n) + 1);
		*version = malloc(strlen(v) + 1);
		if (*name == NULL || *version == NULL) {
			free(*name);
			free(*version);
			/* the caller frees on failure too, and frees
			 * NULL harmlessly: leave the slots clean, so
			 * nothing here can be freed twice. */
			*name = *version = NULL;
			ok = false;
		} else {
			memcpy(*name, n, strlen(n) + 1);
			memcpy(*version, v, strlen(v) + 1);
		}
	}
	pkg_free_manifest(&manifest);
	return ok;
}

/*
 * Collect [dependencies] entries. The parser flattens inline tables,
 * so each entry is found by its source key, and the rest is looked up:
 *
 *   name = "../path"                     bare string: path, no requirement
 *   name = { path, version? }            name.path, requirement at name.version
 *   name = { git, rev?, version? }       name.git, rev at name.rev
 *
 * name.path, name.rev and name.version keys never start an entry; a
 * dependency with both path and git is a manifest that disagrees with
 * itself.
 */
static bool pkg_collect_deps(const PkgManifest *manifest,
        PkgDep **deps,
        int *count,
        char *error,
        size_t error_size)
{
	*deps = NULL;
	*count = 0;
	for (int i = 0; i < manifest->count; i++) {
		if (strcmp(manifest->tables[i].name, "dependencies") != 0)
			continue;
		for (int j = 0; j < manifest->tables[i].count; j++) {
			const char *raw = manifest->tables[i].pairs[j].key;
			const char *dot = strchr(raw, '.');
			char key[512];
			const char *source;
			const char *git_url = NULL;
			const char *git_rev = NULL;
			const char *requirement = NULL;
			if (dot != NULL) {
				if (strcmp(dot, ".path") != 0 &&
				        strcmp(dot, ".git") != 0)
					continue;
				if ((size_t)(dot - raw) == 0 ||
				        (size_t)(dot - raw) >= sizeof(key))
					continue;
				memcpy(key, raw, (size_t)(dot - raw));
				key[dot - raw] = '\0';
				char flat[512];
				const char *other = strcmp(dot, ".path") == 0
				                            ? ".git"
				                            : ".path";
				if (snprintf(flat,
				            sizeof(flat),
				            "%s%s",
				            key,
				            other) >= (int)sizeof(flat))
					continue;
				if (pkg_get(manifest, "dependencies", flat) !=
				        NULL) {
					snprintf(error,
					        error_size,
					        "dependency '%s' has both "
					        "path and git -- pick one",
					        key);
					return NULL;
				}
				if (strcmp(dot, ".git") == 0) {
					git_url = manifest->tables[i]
					                  .pairs[j]
					                  .value;
					char flat_rev[512];
					char flat_req[512];
					if (snprintf(flat_rev,
					            sizeof(flat_rev),
					            "%s.rev",
					            key) >=
					                (int)sizeof(flat_rev) ||
					        snprintf(flat_req,
					                sizeof(flat_req),
					                "%s.version",
					                key) >=
					                (int)sizeof(flat_req))
						continue;
					git_rev = pkg_get(manifest,
					        "dependencies",
					        flat_rev);
					requirement = pkg_get(manifest,
					        "dependencies",
					        flat_req);
					source = git_url;
				} else {
					source = manifest->tables[i]
					                 .pairs[j]
					                 .value;
					char flat_req[512];
					if (snprintf(flat_req,
					            sizeof(flat_req),
					            "%s.version",
					            key) >=
					        (int)sizeof(flat_req))
						continue;
					requirement = pkg_get(manifest,
					        "dependencies",
					        flat_req);
				}
			} else {
				/* a bare string is a path with no
				 * requirement. */
				if (strlen(raw) == 0 ||
				        strlen(raw) >= sizeof(key))
					continue;
				memcpy(key, raw, strlen(raw) + 1);
				source = manifest->tables[i].pairs[j].value;
				char flat_git[512];
				if (snprintf(flat_git,
				            sizeof(flat_git),
				            "%s.git",
				            key) < (int)sizeof(flat_git))
					git_url = pkg_get(manifest,
					        "dependencies",
					        flat_git);
				if (git_url != NULL) {
					snprintf(error,
					        error_size,
					        "dependency '%s' has both "
					        "path and git -- pick one",
					        key);
					return NULL;
				}
			}
			if (strlen(source) >= 4096) {
				snprintf(error,
				        error_size,
				        "source for '%s' is too long",
				        key);
				return NULL;
			}
			char path[4096];
			memcpy(path, source, strlen(source) + 1);
			PkgDep *grown = realloc(
			        *deps, (size_t)(*count + 1) * sizeof(PkgDep));
			if (grown == NULL)
				return NULL;
			*deps = grown;
			PkgDep *dep = &(*deps)[(*count)++];
			memset(dep, 0, sizeof(*dep));
			dep->name = malloc(strlen(key) + 1);
			if (git_url != NULL) {
				dep->git = malloc(strlen(git_url) + 1);
				if (git_rev != NULL) {
					dep->rev = malloc(strlen(git_rev) + 1);
					if (dep->rev != NULL)
						memcpy(dep->rev,
						        git_rev,
						        strlen(git_rev) + 1);
				}
			} else {
				dep->path = malloc(strlen(path) + 1);
			}
			dep->version = requirement != NULL
			                       ? malloc(strlen(requirement) + 1)
			                       : NULL;
			if (dep->name == NULL ||
			        (git_url != NULL
			                        ? (dep->git == NULL ||
			                                  (git_rev != NULL &&
			                                          dep->rev ==
			                                                  NULL))
						: dep->path == NULL) ||
			        (requirement != NULL && dep->version == NULL)) {
				free(dep->name);
				free(dep->path);
				free(dep->git);
				free(dep->rev);
				free(dep->version);
				(*count)--;
				return NULL;
			}
			memcpy(dep->name, key, strlen(key) + 1);
			if (git_url != NULL)
				memcpy(dep->git, git_url, strlen(git_url) + 1);
			else
				memcpy(dep->path, path, strlen(path) + 1);
			if (requirement != NULL)
				memcpy(dep->version,
				        requirement,
				        strlen(requirement) + 1);
		}
	}
	return true;
}

static void pkg_free_deps(PkgDep *deps, int count)
{
	for (int i = 0; i < count; i++) {
		free(deps[i].name);
		free(deps[i].path);
		free(deps[i].git);
		free(deps[i].rev);
		free(deps[i].version);
	}
	free(deps);
}

/* names become directory names under flint_modules/, so they are
 * fenced in: letters, digits, underscore, hyphen, and nothing else.
 * a slash here would be a path escape, not a name. */
static bool pkg_sane_name(const char *name)
{
	if (name[0] == '\0')
		return NULL;
	for (const char *c = name; *c != '\0'; c++) {
		if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-')
			return NULL;
	}
	return true;
}

/* a lock entry: what install resolved last time. commit is the pinned
 * git commit, or NULL for path dependencies, which are live source.
 * content is the sha256 of the installed tree, or NULL when the lock
 * predates content hashes -- the next install records it. */
typedef struct {
	char *name;
	char *version;
	char *source;
	char *rev;
	char *commit;
	char *content;
} PkgLocked;

static void pkg_free_locked(PkgLocked *locked, int count)
{
	for (int i = 0; i < count; i++) {
		free(locked[i].name);
		free(locked[i].version);
		free(locked[i].source);
		free(locked[i].rev);
		free(locked[i].commit);
		free(locked[i].content);
	}
	free(locked);
}

static char *pkg_dup(const char *text)
{
	if (text == NULL)
		return NULL;
	char *out = malloc(strlen(text) + 1);
	if (out != NULL)
		memcpy(out, text, strlen(text) + 1);
	return out;
}

/* read flint.lock. missing is not an error -- it means nothing was ever
 * installed, and the first install writes it. malformed is. */
static bool pkg_read_lock(
        PkgLocked **locked, int *count, char *error, size_t error_size)
{
	*locked = NULL;
	*count = 0;
	if (access("flint.lock", F_OK) != 0)
		return true;
	PkgManifest manifest;
	if (!pkg_load("flint.lock", &manifest, error, error_size))
		return NULL;
	bool ok = true;
	for (int i = 0; i < manifest.count && ok; i++) {
		if (strncmp(manifest.tables[i].name, "packages[", 9) != 0)
			continue;
		const char *name =
		        pkg_get(&manifest, manifest.tables[i].name, "name");
		const char *version =
		        pkg_get(&manifest, manifest.tables[i].name, "version");
		const char *source =
		        pkg_get(&manifest, manifest.tables[i].name, "source");
		if (name == NULL || version == NULL || source == NULL) {
			snprintf(error,
			        error_size,
			        "flint.lock: a package is missing name, "
			        "version or source -- delete it and "
			        "reinstall");
			ok = false;
			break;
		}
		PkgLocked *grown = realloc(
		        *locked, (size_t)(*count + 1) * sizeof(PkgLocked));
		if (grown == NULL) {
			ok = false;
			break;
		}
		*locked = grown;
		PkgLocked *entry = &(*locked)[(*count)++];
		memset(entry, 0, sizeof(*entry));
		entry->name = pkg_dup(name);
		entry->version = pkg_dup(version);
		entry->source = pkg_dup(source);
		/* empty rev or commit reads back as unset: the lock
		 * writes rev = "" when there is none, and an empty pin
		 * must compare equal to no pin, not to a request. */
		const char *rev =
		        pkg_get(&manifest, manifest.tables[i].name, "rev");
		const char *commit =
		        pkg_get(&manifest, manifest.tables[i].name, "commit");
		entry->rev =
		        (rev != NULL && rev[0] != '\0') ? pkg_dup(rev) : NULL;
		entry->commit = (commit != NULL && commit[0] != '\0')
		                        ? pkg_dup(commit)
		                        : NULL;
		/* a lock written before content hashes has no `content` key,
		 * and a mangled one is not a hash: both read back as
		 * unknown, and the next install records the real one. */
		const char *content =
		        pkg_get(&manifest, manifest.tables[i].name, "content");
		entry->content = NULL;
		if (content != NULL && strlen(content) == 64) {
			bool hex = true;
			for (int h = 0; h < 64 && hex; h++)
				hex = isxdigit((unsigned char)content[h]) != 0;
			if (hex)
				entry->content = pkg_dup(content);
		}
		if (entry->name == NULL || entry->version == NULL ||
		        entry->source == NULL) {
			ok = false;
			break;
		}
	}
	pkg_free_manifest(&manifest);
	if (!ok)
		pkg_free_locked(*locked, *count);
	return ok;
}

/* a resolved dependency: owned name/version, borrowed source tree. for
 * git the tree is a commit pin; for path it is the live directory.
 * content is the installed tree's hash once known, "" until then. */
typedef struct {
	char *name;
	char *version;
	const char *source;
	const char *rev;
	char commit[41];
	char content[65];
	bool is_git;
	bool have_commit;
} PkgResolved;

/*
 * Join a base directory with a relative path and normalize it.
 *
 * Two jobs. First, a dependency's nested path is written relative to the
 * package that declares it, so `../base` inside `../mid` means something
 * different from `../base` in the project: joining first and normalizing
 * after is what makes the two agree.
 *
 * Second, and the reason this is not a one-liner: a dependency path is
 * untrusted input the moment a manifest comes from somewhere else, and
 * "copy this tree somewhere" with an unchecked path writes outside the
 * directory it was told to write in. `..` is collapsed here rather than
 * trusted.
 *
 * Returns NULL on overflow or allocation failure; the caller frees.
 */
static char *pkg_normalize_path(const char *base, const char *relative)
{
	size_t blen = strlen(base);
	size_t rlen = strlen(relative);
	if (blen > (size_t)-1 - rlen - 2)
		return NULL;

	size_t cap = blen + rlen + 2;
	char *out = malloc(cap);
	if (out == NULL)
		return NULL;
	size_t len = 0;
	/* a base of "." contributes nothing: emitting it produces
	 * "./../base", which is the same place by a different spelling and
	 * makes the lockfile confusing to read and to compare. */
	if (blen > 0 && strcmp(base, ".") != 0) {
		memcpy(out, base, blen);
		len = blen;
	}

	size_t i = 0;
	while (i < rlen) {
		while (i < rlen && relative[i] == '/')
			i++;
		size_t start = i;
		while (i < rlen && relative[i] != '/')
			i++;
		size_t seg = i - start;
		if (seg == 0)
			continue;
		if (seg == 1 && relative[start] == '.')
			continue;
		if (seg == 2 && relative[start] == '.' &&
		        relative[start + 1] == '.') {
			/* pop one component, but never past the start of the
			 * base: a path that escapes the project is clamped
			 * here, and what is left is refused by the caller
			 * rather than followed. */
			while (len > 0 && out[len - 1] != '/')
				len--;
			if (len > 0)
				len--;
			continue;
		}
		if (len + 1 + seg + 1 > cap) {
			free(out);
			return NULL;
		}
		out[len++] = '/';
		memcpy(out + len, relative + start, seg);
		len += seg;
	}
	out[len] = '\0';
	return out;
}

/* resolve one path dependency: identity, name checks, requirement. */
static bool pkg_resolve_path(
        const PkgDep *dep, PkgResolved *out, char *error, size_t error_size)
{
	char *manifest_path = pkg_join(dep->path, "flint.toml");
	if (manifest_path == NULL)
		return NULL;
	char *name = NULL;
	char *version = NULL;
	bool ok = pkg_read_identity(
	        manifest_path, &name, &version, error, error_size);
	if (!ok) {
		size_t n = strlen(error);
		snprintf(error + n,
		        error_size - n,
		        " '%s': %s",
		        access(dep->path, F_OK) == 0 ? "not a package"
			                             : "no such directory",
		        dep->path);
	} else if (strcmp(name, dep->name) != 0) {
		snprintf(error,
		        error_size,
		        "'%s' calls itself '%s' -- rename one of them",
		        dep->path,
		        name);
		ok = false;
	} else if (!pkg_sane_name(name)) {
		snprintf(error,
		        error_size,
		        "'%s' is not a package name (letters, digits, _ "
		        "and -)",
		        name);
		ok = false;
	} else if (sys_stdlib_has(name)) {
		snprintf(error,
		        error_size,
		        "'%s' is a standard library module -- pick a name "
		        "that does not shadow it",
		        name);
		ok = false;
	} else {
		PkgVersion have;
		if (!pkg_version_parse(version, &have)) {
			snprintf(error,
			        error_size,
			        "version '%s' is not X.Y.Z",
			        version);
			ok = false;
		} else if (dep->version != NULL &&
		           !pkg_satisfies(
		                   dep->version, &have, error, error_size)) {
			size_t n = strlen(error);
			snprintf(error + n,
			        error_size,
			        " (%s has %s)",
			        name,
			        version);
			ok = false;
		}
	}
	if (ok) {
		out->name = name;
		out->version = version;
		out->source = dep->path;
		out->rev = NULL;
		out->is_git = false;
		out->have_commit = false;
		name = version = NULL;
	}
	free(name);
	free(version);
	free(manifest_path);
	return ok;
}

/*
 * Resolve one git dependency: mirror, pin, materialize.
 *
 * locked_commit is the pin from flint.lock, used when the manifest did
 * not change for this dependency and no update was asked: reinstalling
 * the pin needs no network when the mirror already has it, and the
 * second install of a project is byte-identical to the first. A fetch
 * happens on update, or when the pin is missing from the mirror --
 * a fresh machine with a lockfile but no cache still converges.
 */
static bool pkg_resolve_git(const PkgDep *dep,
        const char *locked_commit,
        bool update,
        PkgResolved *out,
        char **workdir,
        char *error,
        size_t error_size)
{
	*workdir = NULL;
	if (!pkg_have_git()) {
		/* one message per platform, chosen outside the call: a
		 * preprocessor conditional inside a macro's argument list
		 * is undefined behaviour, and clang says so. */
#ifdef _WIN32
		snprintf(error,
		        error_size,
		        "git dependencies need a POSIX system");
#else
		snprintf(error,
		        error_size,
		        "git is not installed -- git dependencies need it");
#endif
		return false;
	}
	char *mirror = pkg_mirror_dir(dep->git, dep->name);
	if (mirror == NULL) {
		snprintf(error, error_size, "no HOME to keep the git cache in");
		return NULL;
	}
	const char *pin =
	        (!update && locked_commit != NULL && locked_commit[0] != '\0')
	                ? locked_commit
	                : NULL;
	bool ok = false;
	char sha[41];
	if (pin != NULL) {
		if (!pkg_ensure_mirror(
		            mirror, dep->git, false, pin, error, error_size))
			goto done;
		if (!pkg_resolve_commit(mirror, pin, sha)) {
			snprintf(error,
			        error_size,
			        "locked commit is gone from '%s' -- run "
			        "`flint pkg update %s`",
			        dep->git,
			        dep->name);
			goto done;
		}
	} else {
		if (!pkg_ensure_mirror(
		            mirror, dep->git, update, NULL, error, error_size))
			goto done;
		if (!pkg_resolve_commit(mirror, dep->rev, sha)) {
			snprintf(error,
			        error_size,
			        "cannot resolve '%s' in '%s'",
			        dep->rev != NULL ? dep->rev : "HEAD",
			        dep->git);
			goto done;
		}
	}
	char *dest = pkg_join("flint_modules", dep->name);
	if (dest == NULL)
		goto done;
	if (access(dest, F_OK) == 0 && !pkg_remove_tree(dest)) {
		snprintf(error,
		        error_size,
		        "cannot clear '%s' -- remove it by hand and retry",
		        dest);
		free(dest);
		goto done;
	}
	if (!pkg_materialize(mirror, sha, dest, error, error_size)) {
		free(dest);
		goto done;
	}
	char *manifest_path = pkg_join(dest, "flint.toml");
	if (manifest_path == NULL) {
		free(dest);
		goto done;
	}
	char *name = NULL;
	char *version = NULL;
	if (!pkg_read_identity(
	            manifest_path, &name, &version, error, error_size)) {
		pkg_remove_tree(dest);
	} else if (strcmp(name, dep->name) != 0) {
		snprintf(error,
		        error_size,
		        "'%s' calls itself '%s' -- rename one of them",
		        dep->git,
		        name);
		pkg_remove_tree(dest);
		free(name);
		free(version);
		name = version = NULL;
	} else if (!pkg_sane_name(name) || sys_stdlib_has(name)) {
		snprintf(error,
		        error_size,
		        "'%s' is not an installable "
		        "package name",
		        name);
		pkg_remove_tree(dest);
		free(name);
		free(version);
		name = version = NULL;
	} else {
		PkgVersion have;
		if (!pkg_version_parse(version, &have)) {
			snprintf(error,
			        error_size,
			        "version '%s' is not X.Y.Z",
			        version);
			pkg_remove_tree(dest);
			free(name);
			free(version);
			name = version = NULL;
		} else if (dep->version != NULL &&
		           !pkg_satisfies(
		                   dep->version, &have, error, error_size)) {
			size_t n = strlen(error);
			snprintf(error + n,
			        error_size,
			        " (%s has %s)",
			        name,
			        version);
			pkg_remove_tree(dest);
			free(name);
			free(version);
			name = version = NULL;
		} else {
			out->name = name;
			out->version = version;
			out->source = dep->git;
			out->rev = dep->rev;
			memcpy(out->commit, sha, sizeof(out->commit));
			out->is_git = true;
			out->have_commit = true;
			*workdir = dest;
			dest = NULL;
			name = version = NULL;
		}
	}
	free(name);
	free(version);
	free(manifest_path);
	free(dest);
	ok = *workdir != NULL;
done:
	free(mirror);
	return ok;
}

/*
 * Install (and update): resolve the manifest against the lock, copy
 * trees into flint_modules/, write flint.lock.
 *
 * update_one names one dependency to re-resolve, update_all re-resolves
 * every git dependency. otherwise a git dependency whose manifest entry
 * matches the lock reinstalls its pinned commit -- no network, identical
 * bytes. path dependencies are live source and always re-read.
 *
 * Content hashes make "identical bytes" checkable rather than assumed.
 * when the lock records a hash for a dependency and the tree already in
 * flint_modules/ hashes to it, the dependency is verified in place and
 * left alone. otherwise the tree is re-materialized and hashed again,
 * and a pinned git commit whose fresh bytes do not match the recorded
 * hash stops the install: the source changed under a pin, and blessing
 * it silently is what a checksum is for. path sources are live, so
 * their hash is recorded, not enforced across installs.
 */
static int pkg_lock_index(
        const PkgLocked *locked, int locked_count, const char *name)
{
	for (int k = 0; k < locked_count; k++) {
		if (strcmp(locked[k].name, name) == 0)
			return k;
	}
	return -1;
}
static int pkg_install_core(bool update_all, const char *update_one)
{
	char error[512];
	PkgManifest manifest;
	if (!pkg_load("flint.toml", &manifest, error, sizeof(error))) {
		if (access("flint.toml", F_OK) != 0)
			fprintf(stderr,
			        "flint pkg: no flint.toml here. `flint pkg "
			        "add <path>` starts one.\n");
		else
			fprintf(stderr, "flint pkg: %s\n", error);
		return 74;
	}
	PkgDep *deps = NULL;
	int count = 0;
	if (!pkg_collect_deps(&manifest, &deps, &count, error, sizeof(error))) {
		fprintf(stderr, "flint pkg: %s\n", error);
		pkg_free_manifest(&manifest);
		return 65;
	}
	pkg_free_manifest(&manifest);

	PkgLocked *locked = NULL;
	int locked_count = 0;
	if (!pkg_read_lock(&locked, &locked_count, error, sizeof(error))) {
		fprintf(stderr, "flint pkg: %s\n", error);
		pkg_free_deps(deps, count);
		return 65;
	}

	/* duplicate names declare the same package twice, which is a
	 * typo, not a merge. */
	for (int i = 0; i < count; i++) {
		for (int j = i + 1; j < count; j++) {
			if (strcmp(deps[i].name, deps[j].name) == 0) {
				fprintf(stderr,
				        "flint pkg: '%s' listed twice\n",
				        deps[i].name);
				pkg_free_deps(deps, count);
				pkg_free_locked(locked, locked_count);
				return 65;
			}
		}
	}

	if (update_one != NULL) {
		bool known = false;
		for (int i = 0; i < count; i++) {
			if (strcmp(deps[i].name, update_one) == 0)
				known = true;
		}
		if (!known) {
			fprintf(stderr,
			        "flint pkg: no dependency '%s' in "
			        "flint.toml\n",
			        update_one);
			pkg_free_deps(deps, count);
			pkg_free_locked(locked, locked_count);
			return 65;
		}
	}

	/*
	 * Transitive dependencies, before anything is resolved. A
	 * dependency's own [dependencies] are followed relative to that
	 * package's directory, and everything lands in the one flat
	 * flint_modules/ the language can express. Done first, so a missing
	 * or cyclic transitive dependency is reported before any directory
	 * is written.
	 */
	if (count > 0) {
		int capacity = count;
		/* the returned pointer replaces the old one: expansion may
		 * realloc, and reading the old pointer afterwards is the
		 * use-after-free that produced a garbage path here. */
		int filled = count;
		PkgDep *expanded = pkg_expand_deps(deps,
		        count,
		        &capacity,
		        &filled,
		        ".",
		        0,
		        error,
		        sizeof(error),
		        0);
		if (expanded == NULL) {
			fprintf(stderr, "flint pkg: %s\n", error);
			pkg_free_deps(deps, count);
			pkg_free_locked(locked, locked_count);
			return 65;
		}
		deps = expanded;
		count = filled;

		/* dedupe by name: two packages can reach the same one by
		 * different routes, and it should be installed once. A name
		 * reaching two *different* versions is a conflict rather than
		 * a merge -- reported below rather than resolved by picking
		 * one, because a lockfile that silently chose is not
		 * trustworthy. */
		int kept = 0;
		for (int i = 0; i < count; i++) {
			bool seen = false;
			for (int j = 0; j < i && !seen; j++)
				seen = strcmp(deps[j].name, deps[i].name) == 0;
			if (seen) {
				free(deps[i].name);
				free(deps[i].path);
				free(deps[i].git);
				free(deps[i].rev);
				free(deps[i].version);
				continue;
			}
			deps[kept++] = deps[i];
		}
		count = kept;
	}

	PkgResolved *resolved = NULL;
	char **workdirs = NULL;
	bool *verified = NULL;
	if (count > 0) {
		resolved = calloc((size_t)count, sizeof(PkgResolved));
		workdirs = calloc((size_t)count, sizeof(char *));
		verified = calloc((size_t)count, sizeof(bool));
		if (resolved == NULL || workdirs == NULL || verified == NULL) {
			fprintf(stderr, "flint pkg: out of memory\n");
			free(resolved);
			free(workdirs);
			free(verified);
			pkg_free_deps(deps, count);
			pkg_free_locked(locked, locked_count);
			return 74;
		}
	}
	int code = 0;
	for (int i = 0; i < count && code == 0; i++) {
		bool updating = update_all ||
		                (update_one != NULL &&
		                        strcmp(update_one, deps[i].name) == 0);
		/* Verified in place: the lock records a content hash for
		 * this pin, and the tree already in flint_modules/
		 * hashes to it. Nothing is cloned, copied, or written;
		 * the mirror is not even touched. Workdirs stay NULL so
		 * a later failure cannot sweep a directory this run did
		 * not create. */
		if (!updating && deps[i].git != NULL) {
			int li = pkg_lock_index(
			        locked, locked_count, deps[i].name);
			if (li >= 0 && locked[li].commit != NULL &&
			        locked[li].content != NULL &&
			        strcmp(locked[li].source, deps[i].git) == 0 &&
			        ((locked[li].rev == NULL &&
			                 deps[i].rev == NULL) ||
			                (locked[li].rev != NULL &&
			                        deps[i].rev != NULL &&
			                        strcmp(locked[li].rev,
			                                deps[i].rev) == 0))) {
				char *dest =
				        pkg_join("flint_modules", deps[i].name);
				char have[65];
				if (dest != NULL && access(dest, F_OK) == 0 &&
				        pkg_hash_tree(dest, have) &&
				        strcmp(have, locked[li].content) == 0) {
					resolved[i].name =
					        pkg_dup(locked[li].name);
					resolved[i].version =
					        pkg_dup(locked[li].version);
					if (resolved[i].name == NULL ||
					        resolved[i].version == NULL) {
						fprintf(stderr,
						        "flint pkg: out of "
						        "memory\n");
						free(resolved[i].name);
						free(resolved[i].version);
						free(dest);
						code = 74;
						break;
					}
					resolved[i].source = locked[li].source;
					resolved[i].rev = locked[li].rev;
					memcpy(resolved[i].commit,
					        locked[li].commit,
					        sizeof(resolved[i].commit));
					memcpy(resolved[i].content,
					        locked[li].content,
					        sizeof(resolved[i].content));
					resolved[i].is_git = true;
					resolved[i].have_commit = true;
					verified[i] = true;
				}
				free(dest);
				if (verified[i])
					continue;
			}
		}
		if (deps[i].git != NULL) {
			const char *pin = NULL;
			for (int k = 0; k < locked_count; k++) {
				if (strcmp(locked[k].name, deps[i].name) != 0)
					continue;
				bool same_source = strcmp(locked[k].source,
				                           deps[i].git) == 0;
				bool same_rev =
				        (locked[k].rev == NULL &&
				                deps[i].rev == NULL) ||
				        (locked[k].rev != NULL &&
				                deps[i].rev != NULL &&
				                strcmp(locked[k].rev,
				                        deps[i].rev) == 0);
				if (same_source && same_rev)
					pin = locked[k].commit;
				break;
			}
			if (!pkg_resolve_git(&deps[i],
			            pin,
			            updating,
			            &resolved[i],
			            &workdirs[i],
			            error,
			            sizeof(error))) {
				fprintf(stderr,
				        "flint pkg: %s '%s': %s\n",
				        updating ? "cannot update"
					         : "cannot install",
				        deps[i].name,
				        error);
				code = 74;
			}
		} else {
			if (!pkg_resolve_path(&deps[i],
			            &resolved[i],
			            error,
			            sizeof(error))) {
				fprintf(stderr, "flint pkg: %s\n", error);
				code = 65;
			}
		}
	}

	FILE *lock = NULL;
	if (code == 0) {
		/* A reinstalled pin must byte-match the recorded hash.
		 * Checked here, before flint.lock is opened for writing,
		 * so a mismatch leaves the old lock -- the record of what
		 * was trusted -- exactly as it was. Only the suspect trees
		 * go; the next install retries them from the mirror. */
		for (int i = 0; i < count && code == 0; i++) {
			if (verified[i] || !resolved[i].is_git)
				continue;
			bool updating =
			        update_all ||
			        (update_one != NULL &&
			                strcmp(update_one, deps[i].name) == 0);
			if (updating)
				continue;
			int li = pkg_lock_index(
			        locked, locked_count, deps[i].name);
			if (li < 0 || locked[li].content == NULL ||
			        locked[li].commit == NULL)
				continue;
			if (strcmp(locked[li].commit, resolved[i].commit) != 0)
				continue;
			char fresh[65];
			if (workdirs[i] == NULL ||
			        !pkg_hash_tree(workdirs[i], fresh)) {
				fprintf(stderr,
				        "flint pkg: cannot hash '%s'\n",
				        deps[i].name);
				code = 74;
				break;
			}
			if (strcmp(fresh, locked[li].content) != 0) {
				fprintf(stderr,
				        "flint pkg: '%s' does not match "
				        "flint.lock: commit %s installed "
				        "different bytes than recorded -- "
				        "check '%s', then `flint pkg update "
				        "%s` to re-pin\n",
				        deps[i].name,
				        resolved[i].commit,
				        deps[i].git,
				        deps[i].name);
				code = 74;
				break;
			}
			memcpy(resolved[i].content,
			        fresh,
			        sizeof(resolved[i].content));
		}
	}
	if (code != 0) {
		for (int i = 0; i < count; i++) {
			if (workdirs[i] != NULL)
				pkg_remove_tree(workdirs[i]);
			free(workdirs[i]);
		}
		for (int i = 0; i < count; i++) {
			free(resolved[i].name);
			free(resolved[i].version);
		}
		free(resolved);
		free(workdirs);
		free(verified);
		pkg_free_deps(deps, count);
		pkg_free_locked(locked, locked_count);
		return code;
	}
	if (code == 0) {
		if (!sys_make_dirs("flint_modules")) {
			fprintf(stderr,
			        "flint pkg: cannot create flint_modules/\n");
			code = 74;
		} else {
			lock = fopen("flint.lock", "w");
			if (lock == NULL) {
				fprintf(stderr,
				        "flint pkg: cannot write flint.lock\n");
				code = 74;
			}
		}
	}
	for (int i = 0; i < count && code == 0; i++) {
		/* git trees materialized during resolve; path trees copy
		 * now, after everything resolved. either way a failure
		 * leaves no half-written directory behind. */
		if (!resolved[i].is_git) {
			char *dest =
			        pkg_join("flint_modules", resolved[i].name);
			if (dest == NULL) {
				fprintf(stderr, "flint pkg: out of memory\n");
				code = 74;
				break;
			}
			if (access(dest, F_OK) == 0 && !pkg_remove_tree(dest)) {
				fprintf(stderr,
				        "flint pkg: cannot clear '%s' -- "
				        "remove it by hand and retry\n",
				        dest);
				free(dest);
				code = 74;
				break;
			}
			if (!pkg_copy_tree(resolved[i].source, dest)) {
				fprintf(stderr,
				        "flint pkg: cannot copy '%s'\n",
				        resolved[i].source);
				free(dest);
				code = 74;
				break;
			}
			free(workdirs[i]);
			workdirs[i] = dest;
		}
		/* what lands in the lock is hashed after it lands: the
		 * digest describes the installed tree, not the source it
		 * came from. verified-in-place trees carry theirs already;
		 * everything else is hashed here, and an unhashable tree
		 * is not recorded. */
		if (resolved[i].content[0] == '\0') {
			if (workdirs[i] == NULL ||
			        !pkg_hash_tree(
			                workdirs[i], resolved[i].content)) {
				fprintf(stderr,
				        "flint pkg: cannot hash '%s'\n",
				        resolved[i].name);
				code = 74;
				break;
			}
		}
		if (resolved[i].is_git)
			fprintf(lock,
			        "[[packages]]\nname = \"%s\"\nversion = "
			        "\"%s\"\nsource = \"%s\"\nrev = \"%s\"\n"
			        "commit = \"%s\"\ncontent = \"%s\"\n\n",
			        resolved[i].name,
			        resolved[i].version,
			        resolved[i].source,
			        resolved[i].rev != NULL ? resolved[i].rev : "",
			        resolved[i].commit,
			        resolved[i].content);
		else
			fprintf(lock,
			        "[[packages]]\nname = \"%s\"\nversion = "
			        "\"%s\"\nsource = \"%s\"\ncontent = "
			        "\"%s\"\n\n",
			        resolved[i].name,
			        resolved[i].version,
			        resolved[i].source,
			        resolved[i].content);
		printf("%s %s %s\n",
		        verified[i] ? "verified" : "installed",
		        resolved[i].name,
		        resolved[i].version);
	}
	/* entries in the lock but not the manifest left with the old
	 * manifest: flint_modules/ is owned by this tool, so their
	 * directories go too. anything else in there is the user's. */
	if (code == 0) {
		for (int k = 0; k < locked_count; k++) {
			bool kept = false;
			for (int i = 0; i < count; i++) {
				if (strcmp(locked[k].name, resolved[i].name) ==
				        0) {
					kept = true;
					break;
				}
			}
			if (!kept) {
				char *stale = pkg_join(
				        "flint_modules", locked[k].name);
				if (stale != NULL) {
					pkg_remove_tree(stale);
					free(stale);
				}
			}
		}
	}
	if (lock != NULL)
		fclose(lock);
	if (code != 0) {
		/* a failed install writes no lock and keeps no trees it
		 * put there: every materialized or copied directory goes,
		 * and the next install starts from the manifest alone. */
		for (int i = 0; i < count; i++) {
			if (workdirs[i] != NULL)
				pkg_remove_tree(workdirs[i]);
			free(workdirs[i]);
		}
		remove("flint.lock");
	} else {
		for (int i = 0; i < count; i++)
			free(workdirs[i]);
	}
	for (int i = 0; i < count; i++) {
		free(resolved[i].name);
		free(resolved[i].version);
	}
	free(resolved);
	free(workdirs);
	free(verified);
	pkg_free_deps(deps, count);
	pkg_free_locked(locked, locked_count);
	return code;
}

static int pkg_install(void) { return pkg_install_core(false, NULL); }

/* a git URL rather than a path: a scheme, an scp-like git@ host, or
 * a .git suffix. anything else is a filesystem path. */
static bool pkg_is_url(const char *text)
{
	if (strstr(text, "://") != NULL)
		return true;
	if (strncmp(text, "git@", 4) == 0)
		return true;
	size_t len = strlen(text);
	return len > 4 && strcmp(text + len - 4, ".git") == 0;
}

/* write one dependency entry into flint.toml, creating the manifest
 * (named after the directory) and the [dependencies] table as needed.
 * the entry is already formatted: { path = "..." } or { git = "..." }. */
static int pkg_write_dep(const char *name, const char *entry)
{
	char error[512];
	PkgManifest manifest;
	memset(&manifest, 0, sizeof(manifest));
	if (access("flint.toml", F_OK) == 0) {
		if (!pkg_load("flint.toml", &manifest, error, sizeof(error))) {
			fprintf(stderr, "flint pkg: %s\n", error);
			return 65;
		}
	} else {
		/* no manifest yet: start one, named after the directory. */
		char cwd[4096];
		const char *base = "app";
		if (getcwd(cwd, sizeof(cwd)) != NULL) {
			char *slash = strrchr(cwd, '/');
			if (slash != NULL && slash[1] != '\0')
				base = slash + 1;
		}
		PkgTable *package = pkg_table(&manifest, "package", true);
		if (package == NULL || !pkg_set(package, "name", base) ||
		        !pkg_set(package, "version", "0.1.0")) {
			fprintf(stderr, "flint pkg: out of memory\n");
			pkg_free_manifest(&manifest);
			return 74;
		}
	}
	PkgTable *deps = pkg_table(&manifest, "dependencies", true);
	if (deps == NULL || !pkg_set(deps, name, entry)) {
		fprintf(stderr, "flint pkg: out of memory\n");
		pkg_free_manifest(&manifest);
		return 74;
	}
	FILE *file = fopen("flint.toml", "w");
	if (file == NULL) {
		fprintf(stderr, "flint pkg: cannot write flint.toml\n");
		pkg_free_manifest(&manifest);
		return 74;
	}
	for (int i = 0; i < manifest.count; i++) {
		fprintf(file, "[%s]\n", manifest.tables[i].name);
		/*
		 * The parser flattens `name = { ... }` into name.sub
		 * keys, so writing pairs back verbatim would corrupt the
		 * manifest with dotted keys it cannot read. Dependency
		 * entries regroup by prefix here; anything already bare
		 * writes as it was read.
		 */
		bool grouped =
		        strcmp(manifest.tables[i].name, "dependencies") == 0;
		for (int j = 0; j < manifest.tables[i].count; j++) {
			const char *key = manifest.tables[i].pairs[j].key;
			const char *value = manifest.tables[i].pairs[j].value;
			if (!grouped || strchr(key, '.') == NULL) {
				if (value[0] == '{')
					fprintf(file, "%s = %s\n", key, value);
				else
					fprintf(file,
					        "%s = \"%s\"\n",
					        key,
					        value);
				continue;
			}
			const char *dot = strchr(key, '.');
			size_t prefix = (size_t)(dot - key);
			/* one inline table per prefix: the first dotted
			 * key emits the group, the rest ride along. bare
			 * keys never count, so a broken manifest with
			 * both writes both and install rejects it. */
			bool seen = false;
			for (int k = 0; k < j && !seen; k++) {
				const char *earlier =
				        manifest.tables[i].pairs[k].key;
				const char *edot = strchr(earlier, '.');
				seen = edot != NULL &&
				       (size_t)(edot - earlier) == prefix &&
				       memcmp(earlier, key, prefix) == 0;
			}
			if (seen)
				continue;
			fprintf(file, "%.*s = {", (int)prefix, key);
			const char *sep = " ";
			for (int k = j; k < manifest.tables[i].count; k++) {
				const char *other =
				        manifest.tables[i].pairs[k].key;
				const char *odot = strchr(other, '.');
				if (odot == NULL ||
				        (size_t)(odot - other) != prefix ||
				        memcmp(other, key, prefix) != 0)
					continue;
				fprintf(file,
				        "%s%s = \"%s\"",
				        sep,
				        odot + 1,
				        manifest.tables[i].pairs[k].value);
				sep = ", ";
			}
			fprintf(file, " }\n");
		}
		fprintf(file, "\n");
	}
	fclose(file);
	pkg_free_manifest(&manifest);
	return 0;
}

static int pkg_add(const char *arg)
{
	char error[512];
	if (strchr(arg, '"') != NULL || strchr(arg, '\n') != NULL) {
		fprintf(stderr, "flint pkg: that cannot go in a manifest\n");
		return 65;
	}
	char name[128];
	char entry[8192];
	if (pkg_is_url(arg)) {
		/* validated before anything is written: add probes the
		 * repository (clone once, read its manifest at HEAD), so
		 * a dead URL fails here with git's own complaint rather
		 * than as a manifest entry install chokes on. the entry
		 * uses the package's real name, not the URL's last
		 * component, which is how `add` and `install` agree. */
		if (!pkg_have_git()) {
			fprintf(stderr,
			        "flint pkg: git is not installed -- git "
			        "dependencies need it\n");
			return 74;
		}
		char probe[128];
		pkg_url_slug(arg, "", probe, sizeof(probe));
		char *mirror = pkg_mirror_dir(arg, probe);
		if (mirror == NULL) {
			fprintf(stderr,
			        "flint pkg: no HOME to keep the git cache "
			        "in\n");
			return 74;
		}
		if (!pkg_ensure_mirror(
		            mirror, arg, false, NULL, error, sizeof(error))) {
			fprintf(stderr, "flint pkg: %s\n", error);
			free(mirror);
			return 74;
		}
		char *probe_text = pkg_git_show(mirror, "HEAD", "flint.toml");
		free(mirror);
		if (probe_text == NULL) {
			fprintf(stderr,
			        "flint pkg: '%s' has no flint.toml at its "
			        "default branch\n",
			        arg);
			return 74;
		}
		PkgParse probe_state;
		memset(&probe_state, 0, sizeof(probe_state));
		probe_state.path = arg;
		PkgManifest probe_manifest;
		memset(&probe_manifest, 0, sizeof(probe_manifest));
		bool probe_ok = pkg_parse_text(
		        &probe_state, &probe_manifest, probe_text);
		const char *probe_name =
		        probe_ok ? pkg_get(&probe_manifest, "package", "name")
			         : NULL;
		if (!probe_ok || probe_name == NULL || probe_name[0] == '\0') {
			fprintf(stderr,
			        "flint pkg: '%s': %s\n",
			        arg,
			        probe_ok ? "no [package] name"
				         : probe_state.error);
			free(probe_text);
			pkg_free_manifest(&probe_manifest);
			return 74;
		}
		if (snprintf(name, sizeof(name), "%s", probe_name) >=
		        (int)sizeof(name)) {
			fprintf(stderr, "flint pkg: that name is too long\n");
			free(probe_text);
			pkg_free_manifest(&probe_manifest);
			return 65;
		}
		free(probe_text);
		pkg_free_manifest(&probe_manifest);
		if (!pkg_sane_name(name)) {
			fprintf(stderr,
			        "flint pkg: '%s' is not a package name "
			        "(letters, digits, _ and -)\n",
			        name);
			return 65;
		}
		if (sys_stdlib_has(name)) {
			fprintf(stderr,
			        "flint pkg: '%s' is a standard library "
			        "module -- pick a name that does not "
			        "shadow it\n",
			        name);
			return 65;
		}
		if (snprintf(entry, sizeof(entry), "{ git = \"%s\" }", arg) >=
		        (int)sizeof(entry)) {
			fprintf(stderr, "flint pkg: that URL is too long\n");
			return 65;
		}
	} else {
		char *manifest_path = pkg_join(arg, "flint.toml");
		if (manifest_path == NULL)
			return 74;
		char *found = NULL;
		char *found_version = NULL;
		if (!pkg_read_identity(manifest_path,
		            &found,
		            &found_version,
		            error,
		            sizeof(error))) {
			fprintf(stderr, "flint pkg: '%s': %s\n", arg, error);
			free(manifest_path);
			return 74;
		}
		free(manifest_path);
		/* validated here, not just at install: add writes the
		 * manifest, and a name that cannot be installed must
		 * not be written. */
		if (!pkg_sane_name(found)) {
			fprintf(stderr,
			        "flint pkg: '%s' is not a package name "
			        "(letters, digits, _ and -)\n",
			        found);
			free(found);
			free(found_version);
			return 65;
		}
		if (sys_stdlib_has(found)) {
			fprintf(stderr,
			        "flint pkg: '%s' is a standard library "
			        "module -- pick a name that does not "
			        "shadow it\n",
			        found);
			free(found);
			free(found_version);
			return 65;
		}
		if (snprintf(name, sizeof(name), "%s", found) >=
		        (int)sizeof(name)) {
			fprintf(stderr, "flint pkg: that name is too long\n");
			free(found);
			free(found_version);
			return 65;
		}
		free(found);
		free(found_version);
		if (snprintf(entry, sizeof(entry), "{ path = \"%s\" }", arg) >=
		        (int)sizeof(entry)) {
			fprintf(stderr, "flint pkg: that path is too long\n");
			return 65;
		}
	}
	int code = pkg_write_dep(name, entry);
	if (code != 0)
		return code;
	code = pkg_install();
	/* "added" only when the install behind it worked: the manifest
	 * entry without the files is a promise, not a dependency. */
	if (code == 0)
		printf("added %s\n", name);
	return code;
}

static int pkg_list(void)
{
	char error[512];
	PkgManifest lock;
	if (!pkg_load("flint.lock", &lock, error, sizeof(error))) {
		fprintf(stderr,
		        "flint pkg: no flint.lock here. `flint pkg "
		        "install` writes one.\n");
		return 74;
	}
	int n = 0;
	for (int i = 0; i < lock.count; i++) {
		if (strncmp(lock.tables[i].name, "packages[", 9) != 0)
			continue;
		const char *name = pkg_get(&lock, lock.tables[i].name, "name");
		const char *version =
		        pkg_get(&lock, lock.tables[i].name, "version");
		const char *source =
		        pkg_get(&lock, lock.tables[i].name, "source");
		const char *commit =
		        pkg_get(&lock, lock.tables[i].name, "commit");
		if (name == NULL || version == NULL)
			continue;
		if (commit != NULL && commit[0] != '\0')
			printf("%s %s (%s @ %.8s)\n",
			        name,
			        version,
			        source != NULL ? source : "unknown source",
			        commit);
		else
			printf("%s %s (%s)\n",
			        name,
			        version,
			        source != NULL ? source : "unknown source");
		n++;
	}
	if (n == 0)
		printf("no packages installed.\n");
	pkg_free_manifest(&lock);
	return 0;
}

int flint_pkg(int argc, char **argv, int start)
{
	if (start >= argc) {
		pkg_usage(stderr);
		return 64;
	}
	if (strcmp(argv[start], "install") == 0)
		return pkg_install();
	if (strcmp(argv[start], "add") == 0) {
		if (start + 1 >= argc) {
			fprintf(stderr,
			        "flint pkg add: a path or URL is required\n");
			return 64;
		}
		if (strcmp(argv[start + 1], "--help") == 0 ||
		        strcmp(argv[start + 1], "-h") == 0) {
			pkg_usage(stdout);
			return 0;
		}
		return pkg_add(argv[start + 1]);
	}
	if (strcmp(argv[start], "list") == 0)
		return pkg_list();
	if (strcmp(argv[start], "update") == 0) {
		if (start + 1 < argc &&
		        (strcmp(argv[start + 1], "--help") == 0 ||
		                strcmp(argv[start + 1], "-h") == 0)) {
			pkg_usage(stdout);
			return 0;
		}
		/* `pkg update` re-resolves everything; `pkg update name`
		 * re-resolves one entry and leaves the other pins alone.
		 * path dependencies are live source and unaffected. */
		if (start + 1 < argc)
			return pkg_install_core(false, argv[start + 1]);
		return pkg_install_core(true, NULL);
	}
	if (strcmp(argv[start], "--help") == 0 ||
	        strcmp(argv[start], "-h") == 0) {
		pkg_usage(stdout);
		return 0;
	}
	fprintf(stderr, "flint pkg: unknown command '%s'\n", argv[start]);
	pkg_usage(stderr);
	return 64;
}

/* ---------------------------------------------------------------------- */
/* transitive dependencies                                                  */
/* ---------------------------------------------------------------------- */

/*
 * Resolve a package's own dependencies, recursively, relative to where
 * that package lives.
 *
 * A dependency is a directory, and its flint.toml names its own
 * dependencies with paths relative to *it*, not to the project being
 * installed. So a transitive edge is resolved against the dependency's
 * directory: ../../base from inside ../mid is ../base from here. Getting
 * that wrong installs a package that cannot import what it declared, and
 * the failure surfaces at import time rather than at install -- which is
 * why this walks the graph rather than trusting the top level.
 *
 * Transitive dependencies land in the same flat flint_modules/ directory as
 * direct ones. That is the one flattening the language can express:
 * imports are a flat name space, so two packages that both depend on
 * different versions of something is a conflict, not something to nest.
 *
 * Conflicts are reported, not resolved by picking one. A solver that takes
 * the first conflict it finds makes a lockfile nobody can trust; saying
 * "two versions of X" is more useful than choosing.
 */
static PkgDep *pkg_expand_deps(PkgDep *list,
        int count,
        int *capacity,
        int *filled,
        const char *base_dir,
        int depth,
        char *error,
        size_t error_size,
        int from)
{
	if (from == 0 && depth > 32) {
		snprintf(error,
		        error_size,
		        "dependency chain is deeper than 32 -- is there a "
		        "cycle?");
		return NULL;
	}

	/* count first: every append can move the array, so the loop below
	 * would otherwise read a freed pointer */
	int added = 0;
	for (int i = from; i < count; i++) {
		if (list[i].git != NULL)
			continue;
		PkgManifest child;
		char manifest_path[4096];
		/*
		 * base_dir is "." at the top level, so the join is spelled
		 * out rather than done with a trailing separator: "." +
		 * "/../mid" is a path with a ".." in it that exists but
		 * reads as though it does not, and the lockfile should not
		 * carry that spelling either.
		 */
		if (strcmp(base_dir, ".") == 0)
			snprintf(manifest_path,
			        sizeof(manifest_path),
			        "%s/flint.toml",
			        list[i].path);
		else
			snprintf(manifest_path,
			        sizeof(manifest_path),
			        "%s/%s/flint.toml",
			        base_dir,
			        list[i].path);
		if (!pkg_load(manifest_path, &child, error, error_size))
			return NULL;
		PkgDep *nested = NULL;
		int nested_count = 0;
		bool ok = pkg_collect_deps(
		        &child, &nested, &nested_count, error, error_size);
		pkg_free_manifest(&child);
		if (!ok) {
			pkg_free_deps(nested, nested_count);
			return NULL;
		}
		added += nested_count;
		pkg_free_deps(nested, nested_count);
	}
	*filled = count;
	if (added == 0)
		return list;
	if (count + added > *capacity) {
		int grown = *capacity;
		if (grown < count) {
			grown = count;
		}
		grown = grown < 8 ? 8 : grown * 2;
		while (grown < count + added)
			grown *= 2;
		PkgDep *bigger = realloc(list, (size_t)grown * sizeof(PkgDep));
		if (bigger == NULL) {
			snprintf(error, error_size, "out of memory");
			return NULL;
		}
		list = bigger;
		*capacity = grown;
	}

	/*
	 * the same walk again, this time appending rather than counting.
	 * `start` is where the originals end and the additions begin, and
	 * it is what bounds the two walks -- without it each pass would
	 * re-expand what the previous pass appended, which never
	 * terminates.
	 */
	const int start = count;
	for (int i = from; i < start; i++) {
		if (list[i].git != NULL)
			continue;
		PkgManifest child;
		char manifest_path[4096];
		/*
		 * base_dir is "." at the top level, so the join is spelled
		 * out rather than done with a trailing separator: "." +
		 * "/../mid" is a path with a ".." in it that exists but
		 * reads as though it does not, and the lockfile should not
		 * carry that spelling either.
		 */
		if (strcmp(base_dir, ".") == 0)
			snprintf(manifest_path,
			        sizeof(manifest_path),
			        "%s/flint.toml",
			        list[i].path);
		else
			snprintf(manifest_path,
			        sizeof(manifest_path),
			        "%s/%s/flint.toml",
			        base_dir,
			        list[i].path);
		if (!pkg_load(manifest_path, &child, error, error_size))
			return NULL;
		PkgDep *nested = NULL;
		int nested_count = 0;
		bool ok = pkg_collect_deps(
		        &child, &nested, &nested_count, error, error_size);
		pkg_free_manifest(&child);
		if (!ok) {
			pkg_free_deps(nested, nested_count);
			return NULL;
		}
		for (int k = 0; k < nested_count; k++) {
			/* a nested path is relative to its parent package */
			if (nested[k].path != NULL) {
				char joined[4096];
				/* the parent package's own directory, not the
				 * project's: a nested path is written
				 * relative to the package that declares
				 * it, and "." must not contribute a
				 * component or the lockfile carries a
				 * spelling nobody wrote. */
				if (strcmp(base_dir, ".") == 0)
					snprintf(joined,
					        sizeof(joined),
					        "%s",
					        list[i].path);
				else
					snprintf(joined,
					        sizeof(joined),
					        "%s/%s",
					        base_dir,
					        list[i].path);
				char *absolute = pkg_normalize_path(
				        joined, nested[k].path);
				free(nested[k].path);
				nested[k].path = absolute;
			}
			list[count++] = nested[k];
		}
	}

	*filled = count;
	/* recurse into what was just added, not into the whole list */
	return pkg_expand_deps(list,
	        count,
	        capacity,
	        filled,
	        base_dir,
	        depth + 1,
	        error,
	        error_size,
	        start);
}
