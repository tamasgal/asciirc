#include "files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static char *expand(const char *s)
{
	Buf b;
	buf_init(&b);
	if (strncmp(s, "file://", 7) == 0) {
		s += 7;
		if (strncmp(s, "localhost/", 10) == 0)
			s += 9;
		for (; *s; s++) {
			int h, l;
			if (*s == '%' && (h = hexval(s[1])) >= 0 && (l = hexval(s[2])) >= 0) {
				buf_putc(&b, (char)(h * 16 + l));
				s += 2;
			} else {
				buf_putc(&b, *s);
			}
		}
	} else {
		const char *home = getenv("HOME");
		if (s[0] == '~' && (s[1] == '/' || s[1] == '\0') && home) {
			buf_puts(&b, home);
			s++;
		}
		buf_puts(&b, s);
	}
	if (!b.data)
		buf_puts(&b, "");
	return buf_steal(&b);
}

static const char *token(const char *s, char **out, int exp)
{
	*out = NULL;
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	if (!*s)
		return NULL;
	Buf b;
	buf_init(&b);
	char quote = 0;
	int quoted_tilde = *s == '\'' || *s == '"' || *s == '\\';
	for (; *s; s++) {
		if (quote) {
			if (*s == quote)
				quote = 0;
			else if (quote == '"' && *s == '\\' && s[1] && strchr("\"\\$`", s[1]))
				buf_putc(&b, *++s);
			else
				buf_putc(&b, *s);
		} else if (*s == '\'' || *s == '"') {
			quote = *s;
		} else if (*s == '\\' && s[1]) {
			buf_putc(&b, *++s);
		} else if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
			break;
		} else {
			buf_putc(&b, *s);
		}
	}
	if (!b.data)
		buf_puts(&b, "");
	if (quoted_tilde || !exp) {
		*out = buf_steal(&b);
	} else {
		*out = expand(b.data);
		buf_free(&b);
	}
	return s;
}

/* one shell-like word: backslash escapes, '...' and "..." quotes, ~ and file:// expanded */
const char *path_token(const char *s, char **out)
{
	return token(s, out, 1);
}

static int has_ctrl(const char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s < 0x20 || *s == 0x7f)
			return 1;
	return 0;
}

static int is_file(const char *path)
{
	struct stat st;
	if (has_ctrl(path))
		return 0;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* all words of s, only if every one of them is an existing regular file */
size_t path_files(const char *s, char ***paths)
{
	char **list = NULL;
	size_t n = 0;
	char *tok;
	while ((s = path_token(s, &tok)) != NULL) {
		if (!is_file(tok)) {
			free(tok);
			path_list_free(list, n);
			*paths = NULL;
			return 0;
		}
		list = xrealloc(list, (n + 1) * sizeof(*list));
		list[n++] = tok;
	}
	*paths = list;
	return n;
}

void path_escape(Buf *b, const char *path)
{
	for (const char *p = path; *p; p++) {
		if (strchr(" \t\\'\"$`!*?[]{}()<>|&;#~", *p))
			buf_putc(b, '\\');
		buf_putc(b, *p);
	}
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* completions of a partial path as typed, escaped, directories end in '/' */
size_t path_complete(const char *typed, char ***out)
{
	*out = NULL;
	char *word;
	if (!token(typed, &word, 0))
		word = xstrdup("");
	const char *slash = strrchr(word, '/');
	size_t dlen = slash ? (size_t)(slash - word) + 1 : 0;
	const char *base = word + dlen;
	size_t blen = strlen(base);
	char *dir_typed = xstrndup(word, dlen);
	char *dir = expand(dlen ? dir_typed : ".");
	DIR *d = opendir(dir);
	if (!d) {
		free(dir);
		free(dir_typed);
		free(word);
		return 0;
	}
	char **list = NULL;
	size_t n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		const char *name = e->d_name;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
			continue;
		if ((name[0] == '.' && base[0] != '.') || has_ctrl(name))
			continue;
		if (strncmp(name, base, blen) != 0)
			continue;
		Buf full;
		buf_init(&full);
		buf_printf(&full, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", name);
		struct stat st;
		int isdir = stat(full.data, &st) == 0 && S_ISDIR(st.st_mode);
		buf_free(&full);
		Buf b;
		buf_init(&b);
		if (dir_typed[0] == '~')
			buf_putc(&b, '~');
		path_escape(&b, dir_typed[0] == '~' ? dir_typed + 1 : dir_typed);
		path_escape(&b, name);
		if (isdir)
			buf_putc(&b, '/');
		list = xrealloc(list, (n + 1) * sizeof(*list));
		list[n++] = buf_steal(&b);
	}
	closedir(d);
	free(dir);
	free(dir_typed);
	free(word);
	if (n > 1)
		qsort(list, n, sizeof(*list), cmp_str);
	*out = list;
	return n;
}

const char *path_base(const char *path)
{
	const char *s = strrchr(path, '/');
	return s && s[1] ? s + 1 : path;
}

const char *mime_type(const char *name)
{
	static const char *types[][2] = {
		{"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"},
		{"webp", "image/webp"}, {"heic", "image/heic"}, {"svg", "image/svg+xml"}, {"pdf", "application/pdf"},
		{"txt", "text/plain"}, {"log", "text/plain"}, {"md", "text/markdown"}, {"csv", "text/csv"},
		{"json", "application/json"}, {"zip", "application/zip"}, {"gz", "application/gzip"},
		{"mp4", "video/mp4"}, {"mov", "video/quicktime"}, {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"},
		{"wav", "audio/wav"},
	};
	const char *dot = strrchr(path_base(name), '.');
	if (dot)
		for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
			if (strncasecmp_ascii(dot + 1, types[i][0], strlen(types[i][0]) + 1) == 0)
				return types[i][1];
	return "application/octet-stream";
}

void path_list_free(char **list, size_t n)
{
	for (size_t i = 0; i < n; i++)
		free(list[i]);
	free(list);
}

/* writes data to dir/name, or "name (1).ext" and so on if that exists */
char *save_unique(const char *dir, const char *name, const void *data, size_t len, char *err, size_t errlen)
{
	Buf base;
	buf_init(&base);
	for (const char *p = path_base(name); *p; p++)
		buf_putc(&base, (unsigned char)*p < 0x20 || *p == '/' ? '_' : *p);
	if (!base.data || base.data[0] == '.') {
		Buf b;
		buf_init(&b);
		buf_printf(&b, "file%s", base.data ? base.data : "");
		buf_free(&base);
		base = b;
	}
	const char *dot = strrchr(base.data, '.');
	size_t stem = dot && dot != base.data ? (size_t)(dot - base.data) : base.len;
	for (int i = 0; i < 1000; i++) {
		Buf path;
		buf_init(&path);
		if (i == 0)
			buf_printf(&path, "%s/%s", dir, base.data);
		else
			buf_printf(&path, "%s/%.*s (%d)%s", dir, (int)stem, base.data, i, base.data + stem);
		int fd = open(path.data, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd < 0 && errno == EEXIST) {
			buf_free(&path);
			continue;
		}
		if (fd < 0) {
			snprintf(err, errlen, "cannot create %s: %s", path.data, strerror(errno));
			buf_free(&path);
			buf_free(&base);
			return NULL;
		}
		const char *p = data;
		size_t left = len;
		while (left > 0) {
			ssize_t w = write(fd, p, left);
			if (w < 0 && errno == EINTR)
				continue;
			if (w <= 0)
				break;
			p += w;
			left -= (size_t)w;
		}
		if (close(fd) < 0 || left > 0) {
			snprintf(err, errlen, "cannot write %s", path.data);
			unlink(path.data);
			buf_free(&path);
			buf_free(&base);
			return NULL;
		}
		buf_free(&base);
		return buf_steal(&path);
	}
	snprintf(err, errlen, "too many files named %s in %s", base.data, dir);
	buf_free(&base);
	return NULL;
}

void open_external(const char *path)
{
#ifdef __APPLE__
	const char *cmd = "open";
#else
	const char *cmd = "xdg-open";
#endif
	pid_t pid = fork();
	if (pid == 0) {
		if (fork() == 0) {
			int fd = open("/dev/null", O_RDWR);
			if (fd >= 0) {
				dup2(fd, 0);
				dup2(fd, 1);
				dup2(fd, 2);
			}
			execlp(cmd, cmd, path, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (pid > 0)
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
}

char *downloads_dir(const char *setting)
{
	if (setting && *setting)
		return expand(setting);
	const char *home = getenv("HOME");
	if (!home)
		return xstrdup(".");
	Buf b;
	buf_init(&b);
	buf_printf(&b, "%s/Downloads", home);
	struct stat st;
	if (stat(b.data, &st) == 0 && S_ISDIR(st.st_mode))
		return buf_steal(&b);
	buf_free(&b);
	return xstrdup(home);
}

/* converts an image to PNG with sips, which macOS always has */
int convert_png(const Buf *in, Buf *out)
{
#ifdef __APPLE__
	const char *tmp = getenv("TMPDIR");
	char src[1024], dst[1100];
	snprintf(src, sizeof(src), "%s/asciirc-img-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	int fd = mkstemp(src);
	if (fd < 0)
		return -1;
	int ok = write(fd, in->data, in->len) == (ssize_t)in->len;
	close(fd);
	snprintf(dst, sizeof(dst), "%s.png", src);
	int ret = -1;
	pid_t pid = ok ? fork() : -1;
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			dup2(nul, 2);
		}
		execl("/usr/bin/sips", "sips", "-s", "format", "png", src, "--out", dst, (char *)NULL);
		_exit(127);
	}
	int status = 0;
	if (pid > 0) {
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
		FILE *f = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? fopen(dst, "rb") : NULL;
		if (f) {
			buf_init(out);
			char chunk[65536];
			size_t n;
			while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
				buf_append(out, chunk, n);
			fclose(f);
			ret = out->len > 0 ? 0 : -1;
			if (ret < 0)
				buf_free(out);
		}
	}
	unlink(src);
	unlink(dst);
	return ret;
#else
	(void)in;
	(void)out;
	return -1;
#endif
}
