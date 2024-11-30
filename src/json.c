#include "json.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

typedef struct {
	const char *p;
	const char *end;
	int depth;
} Parser;

static Json *parse_value(Parser *ps);

static void skip_ws(Parser *ps)
{
	while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

static int match(Parser *ps, const char *lit)
{
	size_t n = strlen(lit);
	if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0)
		return 0;
	ps->p += n;
	return 1;
}

static Json *new_value(JsonType t)
{
	Json *v = xcalloc(1, sizeof(*v));
	v->type = t;
	return v;
}

static int hex4(const char *p, unsigned *out)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned)(c - 'A' + 10);
		else
			return 0;
	}
	*out = v;
	return 1;
}

static void put_utf8(Buf *b, unsigned cp)
{
	char tmp[4];
	if (cp < 0x80) {
		buf_putc(b, (char)cp);
	} else if (cp < 0x800) {
		tmp[0] = (char)(0xc0 | (cp >> 6));
		tmp[1] = (char)(0x80 | (cp & 0x3f));
		buf_append(b, tmp, 2);
	} else if (cp < 0x10000) {
		tmp[0] = (char)(0xe0 | (cp >> 12));
		tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		tmp[2] = (char)(0x80 | (cp & 0x3f));
		buf_append(b, tmp, 3);
	} else {
		tmp[0] = (char)(0xf0 | (cp >> 18));
		tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
		tmp[3] = (char)(0x80 | (cp & 0x3f));
		buf_append(b, tmp, 4);
	}
}

static char *parse_string_raw(Parser *ps)
{
	if (ps->p >= ps->end || *ps->p != '"')
		return NULL;
	ps->p++;
	Buf b;
	buf_init(&b);
	while (ps->p < ps->end) {
		char c = *ps->p++;
		if (c == '"')
			return buf_steal(&b);
		if (c != '\\') {
			buf_putc(&b, c);
			continue;
		}
		if (ps->p >= ps->end)
			break;
		c = *ps->p++;
		switch (c) {
		case '"': buf_putc(&b, '"'); break;
		case '\\': buf_putc(&b, '\\'); break;
		case '/': buf_putc(&b, '/'); break;
		case 'b': buf_putc(&b, '\b'); break;
		case 'f': buf_putc(&b, '\f'); break;
		case 'n': buf_putc(&b, '\n'); break;
		case 'r': buf_putc(&b, '\r'); break;
		case 't': buf_putc(&b, '\t'); break;
		case 'u': {
			unsigned cp;
			if (ps->end - ps->p < 4 || !hex4(ps->p, &cp))
				goto fail;
			ps->p += 4;
			put_utf8(&b, cp);
			break;
		}
		default:
			goto fail;
		}
	}
fail:
	buf_free(&b);
	return NULL;
}

static Json *parse_number(Parser *ps)
{
	const char *start = ps->p;
	while (ps->p < ps->end && strchr("+-0123456789.eE", *ps->p))
		ps->p++;
	if (ps->p == start)
		return NULL;
	char tmp[64];
	size_t n = (size_t)(ps->p - start);
	if (n >= sizeof(tmp))
		return NULL;
	memcpy(tmp, start, n);
	tmp[n] = '\0';
	char *endp;
	double d = strtod(tmp, &endp);
	if (*endp)
		return NULL;
	Json *v = new_value(JSON_NUMBER);
	v->num = d;
	return v;
}

static void push_item(Json *v, char *key, Json *item)
{
	v->items = xrealloc(v->items, (v->len + 1) * sizeof(*v->items));
	v->items[v->len] = item;
	if (v->type == JSON_OBJECT) {
		v->keys = xrealloc(v->keys, (v->len + 1) * sizeof(*v->keys));
		v->keys[v->len] = key;
	}
	v->len++;
}

static Json *parse_container(Parser *ps, int is_obj)
{
	ps->p++;
	Json *v = new_value(is_obj ? JSON_OBJECT : JSON_ARRAY);
	char close = is_obj ? '}' : ']';
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == close) {
		ps->p++;
		ps->depth--;
		return v;
	}
	for (;;) {
		char *key = NULL;
		skip_ws(ps);
		if (is_obj) {
			key = parse_string_raw(ps);
			if (!key)
				goto fail;
			skip_ws(ps);
			if (ps->p >= ps->end || *ps->p != ':') {
				free(key);
				goto fail;
			}
			ps->p++;
		}
		Json *item = parse_value(ps);
		if (!item) {
			free(key);
			goto fail;
		}
		push_item(v, key, item);
		skip_ws(ps);
		if (ps->p >= ps->end)
			goto fail;
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == close) {
			ps->p++;
			ps->depth--;
			return v;
		}
		goto fail;
	}
fail:
	json_free(v);
	return NULL;
}

static Json *parse_value(Parser *ps)
{
	skip_ws(ps);
	if (ps->p >= ps->end)
		return NULL;
	switch (*ps->p) {
	case '{':
		return parse_container(ps, 1);
	case '[':
		return parse_container(ps, 0);
	case '"': {
		char *s = parse_string_raw(ps);
		if (!s)
			return NULL;
		Json *v = new_value(JSON_STRING);
		v->str = s;
		return v;
	}
	case 't':
		if (match(ps, "true")) {
			Json *v = new_value(JSON_BOOL);
			v->boolean = 1;
			return v;
		}
		return NULL;
	case 'f':
		if (match(ps, "false"))
			return new_value(JSON_BOOL);
		return NULL;
	case 'n':
		if (match(ps, "null"))
			return new_value(JSON_NULL);
		return NULL;
	default:
		return parse_number(ps);
	}
}

Json *json_parse(const char *s, size_t n)
{
	Parser ps = { s, s + n, 0 };
	Json *v = parse_value(&ps);
	if (!v)
		return NULL;
	skip_ws(&ps);
	if (ps.p != ps.end) {
		json_free(v);
		return NULL;
	}
	return v;
}

void json_free(Json *v)
{
	if (!v)
		return;
	for (size_t i = 0; i < v->len; i++) {
		json_free(v->items[i]);
		if (v->keys)
			free(v->keys[i]);
	}
	free(v->items);
	free(v->keys);
	free(v->str);
	free(v);
}

Json *json_get(const Json *obj, const char *key)
{
	if (!obj || obj->type != JSON_OBJECT)
		return NULL;
	for (size_t i = 0; i < obj->len; i++)
		if (strcmp(obj->keys[i], key) == 0)
			return obj->items[i];
	return NULL;
}

Json *json_path(const Json *v, const char *path)
{
	char key[128];
	while (v && *path) {
		const char *dot = strchr(path, '.');
		size_t n = dot ? (size_t)(dot - path) : strlen(path);
		if (n >= sizeof(key))
			return NULL;
		memcpy(key, path, n);
		key[n] = '\0';
		v = json_get(v, key);
		path += n + (dot ? 1 : 0);
	}
	return (Json *)v;
}

Json *json_at(const Json *arr, size_t i)
{
	if (!arr || (arr->type != JSON_ARRAY && arr->type != JSON_OBJECT) || i >= arr->len)
		return NULL;
	return arr->items[i];
}

size_t json_len(const Json *v)
{
	return v && (v->type == JSON_ARRAY || v->type == JSON_OBJECT) ? v->len : 0;
}

const char *json_str(const Json *v)
{
	return v && v->type == JSON_STRING ? v->str : NULL;
}

const char *json_get_str(const Json *obj, const char *key)
{
	return json_str(json_get(obj, key));
}

double json_num(const Json *v, double def)
{
	return v && v->type == JSON_NUMBER ? v->num : def;
}

int json_bool(const Json *v)
{
	return v && v->type == JSON_BOOL && v->boolean;
}
