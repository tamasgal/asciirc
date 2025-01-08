#include "rc.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *or(const char *s, const char *def)
{
	return s ? s : def;
}

int main(int argc, char **argv)
{
	const char *server = NULL, *uid = getenv("ASCIIRC_USER_ID"), *token = getenv("ASCIIRC_TOKEN");
	int c;
	while ((c = getopt(argc, argv, "s:i:t:")) != -1) {
		switch (c) {
		case 's': server = optarg; break;
		case 'i': uid = optarg; break;
		case 't': token = optarg; break;
		default: break;
		}
	}
	if (!server || !uid || !token) {
		fputs("usage: asciirc -s server -i user_id -t token\n", stderr);
		return 2;
	}
	Url url;
	if (url_parse(server, &url) < 0) {
		fprintf(stderr, "bad url: %s\n", server);
		return 2;
	}
	net_init(0);
	Rc rc;
	rc_init(&rc, &url);
	rc.user_id = xstrdup(uid);
	rc.token = xstrdup(token);
	char err[512];
	if (rc_fetch_me(&rc, err, sizeof(err)) < 0) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	printf("logged in as %s\n", rc.username);
	Json *j = rc_get(&rc, "/api/v1/subscriptions.get", err, sizeof(err));
	if (!j) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	const Json *upd = json_get(j, "update");
	for (size_t i = 0; i < json_len(upd); i++) {
		const Json *s = json_at(upd, i);
		const char *t = or(json_get_str(s, "t"), "?");
		printf("%s%s\n", *t == 'd' ? "" : "#", or(json_get_str(s, "name"), "?"));
	}
	json_free(j);
	rc_free(&rc);
	return 0;
}
