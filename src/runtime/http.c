/* SPDX-License-Identifier: MIT */
/*
 * HTTP request natives.
 *
 * Implements http/https requests without external libraries by using
 * native TCP sockets for http:// and curl(1) for https://.
 */
#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "memory.h"
#include "object.h"
#include "value.h"
#include "vm.h"

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} Buf;

static void buf_init(Buf *b)
{
	b->cap = 8192;
	b->len = 0;
	b->data = malloc(b->cap);
	if (b->data != NULL)
		b->data[0] = '\0';
}

static void buf_free(Buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static bool buf_reserve(Buf *b, size_t need)
{
	if (b->len + need + 1 <= b->cap)
		return true;
	size_t ncap = b->cap * 2;
	while (ncap < b->len + need + 1)
		ncap *= 2;
	char *nb = realloc(b->data, ncap);
	if (nb == NULL)
		return false;
	b->data = nb;
	b->cap = ncap;
	return true;
}

static bool buf_append(Buf *b, const void *data, size_t len)
{
	if (!buf_reserve(b, len))
		return false;
	memcpy(b->data + b->len, data, len);
	b->len += len;
	/* the grow loop above guarantees capacity for the NUL; the
	 * analyzer cannot see that, the same opacity as main.c's
	 * read_stdin. the annotation is the honest version. */
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
	b->data[b->len] = '\0';
	return true;
}

typedef struct {
	char *scheme;
	char *host;
	char *port;
	char *path;
} Url;

static void url_free(Url *u)
{
	free(u->scheme);
	free(u->host);
	free(u->port);
	free(u->path);
}

static bool ascii_case_eq(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char ca = a[i];
		char cb = b[i];
		if (ca >= 'A' && ca <= 'Z')
			ca += 32;
		if (cb >= 'A' && cb <= 'Z')
			cb += 32;
		if (ca != cb)
			return false;
		if (ca == '\0')
			break;
	}
	return true;
}

static bool ascii_ci(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		char ca = *a;
		char cb = *b;
		if (ca >= 'A' && ca <= 'Z')
			ca += 32;
		if (cb >= 'A' && cb <= 'Z')
			cb += 32;
		if (ca != cb)
			return false;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static bool url_parse(const char *text, Url *u, char *err, size_t errlen)
{
	memset(u, 0, sizeof(*u));
	if (strchr(text, ' ') != NULL || strchr(text, '\t') != NULL ||
	        strchr(text, '\r') != NULL || strchr(text, '\n') != NULL) {
		snprintf(err,
		        errlen,
		        "URL contains spaces or control characters.");
		return false;
	}

	const char *rest = text;
	if (ascii_case_eq(rest, "http://", 7)) {
		u->scheme = strdup("http");
		rest += 7;
		u->port = strdup("80");
	} else if (ascii_case_eq(rest, "https://", 8)) {
		u->scheme = strdup("https");
		rest += 8;
		u->port = strdup("443");
	} else {
		snprintf(err,
		        errlen,
		        "URL must start with http:// or https://.");
		return false;
	}

	const char *path_start = strchr(rest, '/');
	const char *host_end =
	        path_start != NULL ? path_start : rest + strlen(rest);

	const char *port_colon = memchr(rest, ':', (size_t)(host_end - rest));
	if (port_colon != NULL) {
		size_t hostlen = (size_t)(port_colon - rest);
		u->host = strndup(rest, hostlen);
		size_t portlen = (size_t)(host_end - (port_colon + 1));
		free(u->port);
		u->port = strndup(port_colon + 1, portlen);
	} else {
		size_t hostlen = (size_t)(host_end - rest);
		if (hostlen == 0) {
			snprintf(err, errlen, "URL has empty host.");
			url_free(u);
			return false;
		}
		u->host = strndup(rest, hostlen);
	}

	if (path_start != NULL) {
		u->path = strdup(path_start);
	} else {
		u->path = strdup("/");
	}

	if (u->host[0] == '\0') {
		snprintf(err, errlen, "URL has empty host.");
		url_free(u);
		return false;
	}
	return true;
}

typedef struct {
	char *name;
	char *value;
} Header;

typedef struct {
	int status;
	char *reason;
	Header *headers;
	size_t nheaders;
	char *body;
	size_t body_len;
} Response;

static void response_free(Response *r)
{
	free(r->reason);
	for (size_t i = 0; i < r->nheaders; i++) {
		free(r->headers[i].name);
		free(r->headers[i].value);
	}
	free(r->headers);
	free(r->body);
}

static bool parse_response_headers(const char *raw,
        size_t raw_len,
        Response *resp,
        const char **body_start)
{
	memset(resp, 0, sizeof(*resp));
	const char *p = raw;
	const char *end = raw + raw_len;

	const char *line_end = strstr(p, "\r\n");
	size_t line_len;
	if (line_end != NULL) {
		line_len = (size_t)(line_end - p);
	} else {
		line_end = strchr(p, '\n');
		if (line_end == NULL)
			return false;
		line_len = (size_t)(line_end - p);
	}

	char line[1024];
	if (line_len >= sizeof(line))
		return false;
	memcpy(line, p, line_len);
	line[line_len] = '\0';

	char *sp1 = strchr(line, ' ');
	if (sp1 == NULL)
		return false;
	*sp1 = '\0';
	char *status_str = sp1 + 1;
	char *sp2 = strchr(status_str, ' ');
	if (sp2 != NULL) {
		*sp2 = '\0';
		resp->reason = strdup(sp2 + 1);
	} else {
		resp->reason = strdup("");
	}
	resp->status = (int)strtol(status_str, NULL, 10);

	p = line_end + (line_end[1] == '\n' ? 2 : 1);

	size_t h_cap = 8;
	resp->headers = malloc(sizeof(Header) * h_cap);

	for (;;) {
		if (p >= end)
			return false;
		const char *next_eol = strstr(p, "\r\n");
		size_t llen;
		int eol_len = 2;
		if (next_eol != NULL) {
			llen = (size_t)(next_eol - p);
		} else {
			next_eol = strchr(p, '\n');
			if (next_eol == NULL)
				return false;
			llen = (size_t)(next_eol - p);
			eol_len = 1;
		}

		if (llen == 0) {
			p = next_eol + eol_len;
			break;
		}

		char hline[4096];
		if (llen >= sizeof(hline)) {
			p = next_eol + eol_len;
			continue;
		}
		memcpy(hline, p, llen);
		hline[llen] = '\0';
		p = next_eol + eol_len;

		char *colon = strchr(hline, ':');
		if (colon == NULL)
			continue;
		*colon = '\0';
		char *val = colon + 1;
		while (*val == ' ' || *val == '\t')
			val++;

		char *name = hline;
		for (char *c = name; *c; c++) {
			if (*c >= 'A' && *c <= 'Z')
				*c += 32;
		}

		if (resp->nheaders >= h_cap) {
			h_cap *= 2;
			Header *nh =
			        realloc(resp->headers, sizeof(Header) * h_cap);
			if (nh == NULL)
				return false;
			resp->headers = nh;
		}

		resp->headers[resp->nheaders].name = strdup(name);
		resp->headers[resp->nheaders].value = strdup(val);
		resp->nheaders++;
	}

	*body_start = p;
	return true;
}

static bool dechunk(
        const char *src, size_t src_len, char **out, size_t *out_len)
/* NOLINTNEXTLINE(misc-no-recursion) */
{
	Buf b;
	buf_init(&b);
	const char *p = src;
	const char *end = src + src_len;

	while (p < end) {
		char *line_end = strstr(p, "\r\n");
		if (line_end == NULL) {
			buf_free(&b);
			return false;
		}
		size_t hex_len = (size_t)(line_end - p);
		char hex_buf[64];
		if (hex_len >= sizeof(hex_buf)) {
			buf_free(&b);
			return false;
		}
		memcpy(hex_buf, p, hex_len);
		hex_buf[hex_len] = '\0';

		unsigned long chunk_size = strtoul(hex_buf, NULL, 16);
		p = line_end + 2;

		if (chunk_size == 0)
			break;

		if ((size_t)(end - p) < chunk_size) {
			buf_free(&b);
			return false;
		}

		if (!buf_append(&b, p, chunk_size)) {
			buf_free(&b);
			return false;
		}
		p += chunk_size;
		if (p + 2 <= end && p[0] == '\r' && p[1] == '\n')
			p += 2;
	}

	*out = b.data;
	*out_len = b.len;
	return true;
}

static char *socket_fetch(const Url *u,
        const char *method,
        const char *body,
        size_t body_len,
        const Header *headers,
        size_t nheaders,
        int timeout_ms,
        size_t *resp_len,
        char *err,
        size_t errlen)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = NULL;
	int gai = getaddrinfo(u->host, u->port, &hints, &res);
	if (gai != 0) {
		snprintf(err,
		        errlen,
		        "DNS resolution failed for %s: %s",
		        u->host,
		        gai_strerror(gai));
		return NULL;
	}

	int sock = -1;
	struct addrinfo *rp = NULL;
	uint64_t start = (uint64_t)time(NULL) * 1000;

	for (rp = res; rp != NULL; rp = rp->ai_next) {
		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sock < 0)
			continue;

		int flags = fcntl(sock, F_GETFL, 0);
		fcntl(sock, F_SETFL, flags | O_NONBLOCK);

		int conn = connect(sock, rp->ai_addr, rp->ai_addrlen);
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		if (conn < 0 && errno == EINPROGRESS) {
			/* NOLINTNEXTLINE(misc-include-cleaner) */
			struct pollfd pfd;
			pfd.fd = sock;
			/* NOLINTNEXTLINE(misc-include-cleaner) */
			pfd.events = POLLOUT;
			int rem = timeout_ms;
			/* NOLINTNEXTLINE(misc-include-cleaner) */
			int r = poll(&pfd, 1, rem);
			if (r > 0) {
				int err_code = 0;
				socklen_t elen = sizeof(err_code);
				if (getsockopt(sock,
				            SOL_SOCKET,
				            SO_ERROR,
				            &err_code,
				            &elen) == 0 &&
				        err_code == 0) {
					break;
				}
			}
		} else if (conn == 0) {
			break;
		}
		close(sock);
		sock = -1;
	}
	freeaddrinfo(res);

	if (sock < 0) {
		snprintf(err,
		        errlen,
		        "Connection failed to %s:%s",
		        u->host,
		        u->port);
		return NULL;
	}

	int flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);

	Buf req;
	buf_init(&req);
	buf_append(&req, method, strlen(method));
	buf_append(&req, " ", 1);
	buf_append(&req, u->path, strlen(u->path));
	buf_append(&req, " HTTP/1.1\r\n", 11);

	bool has_host = false;
	bool has_ua = false;
	bool has_accept = false;
	bool has_conn = false;
	bool has_cl = false;

	for (size_t i = 0; i < nheaders; i++) {
		if (ascii_ci(headers[i].name, "host") == 0)
			has_host = true;
		else if (ascii_ci(headers[i].name, "user-agent") == 0)
			has_ua = true;
		else if (ascii_ci(headers[i].name, "accept") == 0)
			has_accept = true;
		else if (ascii_ci(headers[i].name, "connection") == 0)
			has_conn = true;
		else if (ascii_ci(headers[i].name, "content-length") == 0)
			has_cl = true;
	}

	if (!has_host) {
		char hbuf[512];
		snprintf(hbuf, sizeof(hbuf), "Host: %s\r\n", u->host);
		buf_append(&req, hbuf, strlen(hbuf));
	}
	if (!has_ua) {
		buf_append(&req, "User-Agent: flint/0.7.0\r\n", 25);
	}
	if (!has_accept) {
		buf_append(&req, "Accept: */*\r\n", 13);
	}
	if (!has_conn) {
		buf_append(&req, "Connection: close\r\n", 19);
	}
	if (!has_cl && body != NULL) {
		char clbuf[64];
		snprintf(clbuf,
		        sizeof(clbuf),
		        "Content-Length: %zu\r\n",
		        body_len);
		buf_append(&req, clbuf, strlen(clbuf));
	}

	for (size_t i = 0; i < nheaders; i++) {
		buf_append(&req, headers[i].name, strlen(headers[i].name));
		buf_append(&req, ": ", 2);
		buf_append(&req, headers[i].value, strlen(headers[i].value));
		buf_append(&req, "\r\n", 2);
	}
	buf_append(&req, "\r\n", 2);
	if (body != NULL && body_len > 0) {
		buf_append(&req, body, body_len);
	}

	size_t sent = 0;
	while (sent < req.len) {
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		struct pollfd pfd;
		pfd.fd = sock;
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		pfd.events = POLLOUT;
		uint64_t now = (uint64_t)time(NULL) * 1000;
		int elapsed = (int)(now - start);
		int rem = timeout_ms - elapsed;
		if (rem <= 0) {
			snprintf(err, errlen, "HTTP request timeout.");
			close(sock);
			buf_free(&req);
			return NULL;
		}
		if (poll(&pfd, 1, rem) <= 0) {
			snprintf(err, errlen, "HTTP send timeout.");
			close(sock);
			buf_free(&req);
			return NULL;
		}
		ssize_t n = send(sock, req.data + sent, req.len - sent, 0);
		if (n < 0) {
			/* NOLINTNEXTLINE(misc-include-cleaner) */
			if (errno == EINTR)
				continue;
			snprintf(err,
			        errlen,
			        "HTTP send failed: %s",
			        strerror(errno));
			close(sock);
			buf_free(&req);
			return NULL;
		}
		sent += (size_t)n;
	}
	buf_free(&req);

	Buf resp;
	buf_init(&resp);
	for (;;) {
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		struct pollfd pfd;
		pfd.fd = sock;
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		pfd.events = POLLIN;
		uint64_t now = (uint64_t)time(NULL) * 1000;
		int elapsed = (int)(now - start);
		int rem = timeout_ms - elapsed;
		if (rem <= 0) {
			break;
		}
		/* NOLINTNEXTLINE(misc-include-cleaner) */
		int r = poll(&pfd, 1, rem);
		if (r < 0) {
			/* NOLINTNEXTLINE(misc-include-cleaner) */
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break;

		char rbuf[8192];
		ssize_t n = recv(sock, rbuf, sizeof(rbuf), 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		if (!buf_append(&resp, rbuf, (size_t)n))
			break;
		if (resp.len > (size_t)64 * 1024 * 1024) {
			snprintf(err, errlen, "Response too large.");
			close(sock);
			buf_free(&resp);
			return NULL;
		}
	}
	close(sock);

	*resp_len = resp.len;
	return resp.data;
}

static char *curl_fetch(const Url *u,
        const char *method,
        const char *body,
        size_t body_len,
        const Header *headers,
        size_t nheaders,
        int timeout_ms,
        size_t *resp_len,
        char *err,
        size_t errlen)
{
	int pipe_in[2];
	int pipe_out[2];
	if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) {
		snprintf(err, errlen, "pipe() failed.");
		return NULL;
	}

	fflush(stdout);
	fflush(stderr);

	pid_t pid = fork();
	if (pid < 0) {
		close(pipe_in[0]);
		close(pipe_in[1]);
		close(pipe_out[0]);
		close(pipe_out[1]);
		snprintf(err, errlen, "fork() failed.");
		return NULL;
	}

	if (pid == 0) {
		close(pipe_in[1]);
		close(pipe_out[0]);
		dup2(pipe_in[0], STDIN_FILENO);
		dup2(pipe_out[1], STDOUT_FILENO);
		dup2(pipe_out[1], STDERR_FILENO);
		close(pipe_in[0]);
		close(pipe_out[1]);

		char tbuf[32];
		snprintf(tbuf,
		        sizeof(tbuf),
		        "%d",
		        timeout_ms / 1000 > 0 ? timeout_ms / 1000 : 1);

		char *argv[64];
		int ac = 0;
		argv[ac++] = "curl";
		argv[ac++] = "-sS";
		argv[ac++] = "-i";
		argv[ac++] = "--proto";
		argv[ac++] = "=https";
		argv[ac++] = "--connect-timeout";
		argv[ac++] = tbuf;
		argv[ac++] = "--max-time";
		argv[ac++] = tbuf;
		argv[ac++] = "-X";
		argv[ac++] = (char *)method;

		/* each header needs its own stable buffer: all argv entries
		 * must be distinct, because exec loads them later. one shared
		 * buffer would silently repeat the last header value. */
		static char hbufs[64][1024];
		for (size_t i = 0; i < nheaders && ac < 60 && i < 64; i++) {
			snprintf(hbufs[i],
			        sizeof(hbufs[i]),
			        "%s: %s",
			        headers[i].name,
			        headers[i].value);
			argv[ac++] = hbufs[i];
		}

		if (body != NULL) {
			argv[ac++] = "--data-binary";
			argv[ac++] = "@-";
		}
		char full_url[2048];
		snprintf(full_url,
		        sizeof(full_url),
		        "%s://%s:%s%s",
		        u->scheme,
		        u->host,
		        u->port,
		        u->path);
		argv[ac++] = full_url;
		argv[ac] = NULL;

		execvp("curl", argv);
		_exit(127);
	}

	close(pipe_in[0]);
	close(pipe_out[1]);

	if (body != NULL && body_len > 0) {
		size_t written = 0;
		while (written < body_len) {
			ssize_t n = write(
			        pipe_in[1], body + written, body_len - written);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			written += (size_t)n;
		}
	}
	close(pipe_in[1]);

	Buf b;
	buf_init(&b);
	for (;;) {
		char rbuf[4096];
		ssize_t n = read(pipe_out[0], rbuf, sizeof(rbuf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		if (!buf_append(&b, rbuf, (size_t)n))
			break;
	}
	close(pipe_out[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			break;
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		snprintf(err,
		        errlen,
		        "curl failed: %s",
		        b.len > 0 ? b.data : "unknown error");
		buf_free(&b);
		return NULL;
	}

	*resp_len = b.len;
	return b.data;
}

static void set_table_field(VM *vm, ObjTable *t, const char *name, Value val)
{
	ObjString *key = copy_string(vm, name, (int)strlen(name));
	vm_push(vm, STR_VAL(key));
	if (t->capacity < t->count + 1) {
		int old_cap = t->capacity;
		t->capacity = GROW_CAPACITY(old_cap);
		t->keys = GROW_ARRAY(
		        vm, ObjString *, t->keys, old_cap, t->capacity);
		t->values =
		        GROW_ARRAY(vm, Value, t->values, old_cap, t->capacity);
	}
	t->keys[t->count] = key;
	t->values[t->count] = val;
	t->count++;
	vm_pop(vm);
}

static Value http_request_native(VM *vm, int argc, Value *argv)
{
	if (argc != 6 || !IS_STRING(argv[0]) || !IS_STRING(argv[1]) ||
	        (!IS_NIL(argv[2]) && !IS_STRING(argv[2])) ||
	        (!IS_NIL(argv[3]) && !IS_FLINT_TABLE(argv[3])) ||
	        !IS_NUMBER(argv[4]) || !IS_BOOL(argv[5])) {
		vm_runtime_error(vm,
		        "http_request() takes (url, method, body, headers, "
		        "timeout_ms, follow).");
		return NIL_VAL;
	}

	const char *url_str = AS_CSTRING(argv[0]);
	const char *method = AS_CSTRING(argv[1]);
	const char *body = IS_NIL(argv[2]) ? NULL : AS_CSTRING(argv[2]);
	size_t body_len = body != NULL ? (size_t)AS_STRING(argv[2])->length : 0;
	int timeout_ms = (int)AS_NUMBER(argv[4]);
	bool follow = AS_BOOL(argv[5]);

	Header *headers = NULL;
	size_t nheaders = 0;
	if (!IS_NIL(argv[3])) {
		ObjTable *ht = AS_FLINT_TABLE(argv[3]);
		headers = malloc(sizeof(Header) *
		                 (size_t)(ht->count > 0 ? ht->count : 1));
		for (int i = 0; i < ht->count; i++) {
			if (IS_STRING(OBJ_VAL(ht->keys[i])) &&
			        IS_STRING(ht->values[i])) {
				headers[nheaders].name =
				        strdup(ht->keys[i]->chars);
				headers[nheaders].value =
				        strdup(AS_CSTRING(ht->values[i]));
				nheaders++;
			}
		}
	}

	char current_url[2048];
	snprintf(current_url, sizeof(current_url), "%s", url_str);
	int redirects = 0;
	char err_buf[512] = "";

	Response resp;
	memset(&resp, 0, sizeof(resp));
	char *raw_resp = NULL;

	for (;;) {
		Url u;
		if (!url_parse(current_url, &u, err_buf, sizeof(err_buf))) {
			for (size_t i = 0; i < nheaders; i++) {
				free(headers[i].name);
				free(headers[i].value);
			}
			free(headers);
			vm_runtime_error(vm, "http error: %s", err_buf);
			return NIL_VAL;
		}

		size_t raw_len = 0;
		if (strcmp(u.scheme, "https") == 0) {
			raw_resp = curl_fetch(&u,
			        method,
			        body,
			        body_len,
			        headers,
			        nheaders,
			        timeout_ms,
			        &raw_len,
			        err_buf,
			        sizeof(err_buf));
		} else {
			raw_resp = socket_fetch(&u,
			        method,
			        body,
			        body_len,
			        headers,
			        nheaders,
			        timeout_ms,
			        &raw_len,
			        err_buf,
			        sizeof(err_buf));
		}

		url_free(&u);

		if (raw_resp == NULL) {
			for (size_t i = 0; i < nheaders; i++) {
				free(headers[i].name);
				free(headers[i].value);
			}
			free(headers);

			ObjTable *res = new_flint_table(vm);
			vm_push(vm, OBJ_VAL(res));
			set_table_field(vm, res, "ok", FALSE_VAL);
			set_table_field(vm, res, "status", NUMBER_VAL(0));
			set_table_field(vm,
			        res,
			        "status_text",
			        STR_VAL(new_string(vm, "", 0)));
			set_table_field(vm,
			        res,
			        "body",
			        STR_VAL(new_string(vm, "", 0)));
			set_table_field(vm,
			        res,
			        "url",
			        STR_VAL(new_string(vm,
			                current_url,
			                (int)strlen(current_url))));
			set_table_field(
			        vm, res, "redirects", NUMBER_VAL(redirects));
			set_table_field(vm,
			        res,
			        "error",
			        STR_VAL(new_string(
			                vm, err_buf, (int)strlen(err_buf))));
			vm_pop(vm);
			return OBJ_VAL(res);
		}

		const char *body_start = NULL;
		if (!parse_response_headers(
		            raw_resp, raw_len, &resp, &body_start)) {
			response_free(&resp);
			free(raw_resp);
			for (size_t i = 0; i < nheaders; i++) {
				free(headers[i].name);
				free(headers[i].value);
			}
			free(headers);
			vm_runtime_error(
			        vm, "http error: malformed response headers.");
			return NIL_VAL;
		}

		size_t parsed_body_len =
		        (size_t)((raw_resp + raw_len) - body_start);
		char *final_body = NULL;
		size_t final_body_len = 0;

		bool is_chunked = false;
		for (size_t i = 0; i < resp.nheaders; i++) {
			if (ascii_ci(resp.headers[i].name,
			            "transfer-encoding") == 0 &&
			        strstr(resp.headers[i].value, "chunked") !=
			                NULL) {
				is_chunked = true;
				break;
			}
		}

		if (is_chunked) {
			if (!dechunk(body_start,
			            parsed_body_len,
			            &final_body,
			            &final_body_len)) {
				final_body =
				        strndup(body_start, parsed_body_len);
				final_body_len = parsed_body_len;
			}
		} else {
			final_body = strndup(body_start, parsed_body_len);
			final_body_len = parsed_body_len;
		}
		resp.body = final_body;
		resp.body_len = final_body_len;

		bool is_redirect = (resp.status == 301 || resp.status == 302 ||
		                    resp.status == 303 || resp.status == 307 ||
		                    resp.status == 308);
		if (follow && is_redirect && redirects < 5) {
			const char *loc = NULL;
			for (size_t i = 0; i < resp.nheaders; i++) {
				if (ascii_ci(resp.headers[i].name,
				            "location") == 0) {
					loc = resp.headers[i].value;
					break;
				}
			}
			if (loc != NULL) {
				redirects++;
				response_free(&resp);
				free(raw_resp);
				if (loc[0] == '/' && loc[1] == '/') {
					Url base_u;
					url_parse(current_url,
					        &base_u,
					        err_buf,
					        sizeof(err_buf));
					snprintf(current_url,
					        sizeof(current_url),
					        "%s:%s",
					        base_u.scheme,
					        loc);
					url_free(&base_u);
				} else if (loc[0] == '/') {
					Url base_u;
					url_parse(current_url,
					        &base_u,
					        err_buf,
					        sizeof(err_buf));
					snprintf(current_url,
					        sizeof(current_url),
					        "%s://%s:%s%s",
					        base_u.scheme,
					        base_u.host,
					        base_u.port,
					        loc);
					url_free(&base_u);
				} else if (strncmp(loc, "http://", 7) == 0 ||
				           strncmp(loc, "https://", 8) == 0) {
					snprintf(current_url,
					        sizeof(current_url),
					        "%s",
					        loc);
				} else {
					Url base_u;
					url_parse(current_url,
					        &base_u,
					        err_buf,
					        sizeof(err_buf));
					char *last_slash =
					        strrchr(base_u.path, '/');
					if (last_slash != NULL)
						*(last_slash + 1) = '\0';
					else
						base_u.path = strdup("/");
					snprintf(current_url,
					        sizeof(current_url),
					        "%s://%s:%s%s%s",
					        base_u.scheme,
					        base_u.host,
					        base_u.port,
					        base_u.path,
					        loc);
					url_free(&base_u);
				}
				if (resp.status == 303) {
					method = "GET";
					body = NULL;
					body_len = 0;
				}
				continue;
			}
		}
		break;
	}

	for (size_t i = 0; i < nheaders; i++) {
		free(headers[i].name);
		free(headers[i].value);
	}
	free(headers);

	ObjTable *res = new_flint_table(vm);
	vm_push(vm, OBJ_VAL(res));

	bool ok = (resp.status >= 200 && resp.status < 300);
	set_table_field(vm, res, "ok", ok ? TRUE_VAL : FALSE_VAL);
	set_table_field(vm, res, "status", NUMBER_VAL(resp.status));
	set_table_field(vm,
	        res,
	        "status_text",
	        STR_VAL(new_string(vm,
	                resp.reason != NULL ? resp.reason : "",
	                (int)strlen(resp.reason != NULL ? resp.reason : ""))));

	ObjTable *ht = new_flint_table(vm);
	vm_push(vm, OBJ_VAL(ht));
	for (size_t i = 0; i < resp.nheaders; i++) {
		ObjString *k = copy_string(vm,
		        resp.headers[i].name,
		        (int)strlen(resp.headers[i].name));
		vm_push(vm, STR_VAL(k));
		ObjString *v = copy_string(vm,
		        resp.headers[i].value,
		        (int)strlen(resp.headers[i].value));
		vm_push(vm, STR_VAL(v));
		if (ht->capacity < ht->count + 1) {
			int old_cap = ht->capacity;
			ht->capacity = GROW_CAPACITY(old_cap);
			ht->keys = GROW_ARRAY(vm,
			        ObjString *,
			        ht->keys,
			        old_cap,
			        ht->capacity);
			ht->values = GROW_ARRAY(
			        vm, Value, ht->values, old_cap, ht->capacity);
		}
		ht->keys[ht->count] = k;
		ht->values[ht->count] = STR_VAL(v);
		ht->count++;
		vm_pop(vm);
		vm_pop(vm);
	}
	set_table_field(vm, res, "headers", OBJ_VAL(ht));
	vm_pop(vm);

	set_table_field(vm,
	        res,
	        "body",
	        STR_VAL(new_string(vm,
	                resp.body != NULL ? resp.body : "",
	                (int)resp.body_len)));
	set_table_field(vm,
	        res,
	        "url",
	        STR_VAL(new_string(vm, current_url, (int)strlen(current_url))));
	set_table_field(vm, res, "redirects", NUMBER_VAL(redirects));
	set_table_field(vm, res, "error", STR_VAL(new_string(vm, "", 0)));

	vm_pop(vm);

	response_free(&resp);
	free(raw_resp);

	return OBJ_VAL(res);
}

void register_http_natives(VM *vm)
{
	vm_define_native(vm, "__http_request", http_request_native, 6);
}
