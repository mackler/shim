
#include <sys/queue.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <event2/util.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include "httpconn.h"
#include "headers.h"
#include "util.h"
#include "log.h"

#define METHOD0(conn, slot) \
	(conn)->cbs->slot((conn), (conn)->cbarg)
#define METHOD1(conn, slot, a) \
	(conn)->cbs->slot((conn), (a), (conn)->cbarg)
#define METHOD2(conn, slot, a, b) \
	(conn)->cbs->slot((conn), (a), (b), (conn)->cbarg)
#define METHOD3(conn, slot, a, b, c) \
	(conn)->cbs->slot((conn), (a), (b), (c), (conn)->cbarg)
#define METHOD4(conn, slot, a, b, c, d) \
	(conn)->cbs->slot((conn), (a), (b), (c), (d), (conn)->cbarg)

/* max amount of data we can have backlogged on outbuf before choaking */
static size_t max_write_backlog = 50 * 1024;

/* the number of seconds to keep an idle connections hanging around */
static int idle_client_timeout = 120;
static int idle_server_timeout = 120;

struct http_conn {
	enum http_state state;
	enum http_version vers;
	enum http_te te;
	enum http_type type;
	int is_choaked;
	int has_body;
	int read_paused;
	int msg_complete_on_eof;
	int persistent;
	const struct http_cbs *cbs;
	void *cbarg;
	ev_int64_t data_remaining;
	char *firstline;
	struct header_list *headers;
	struct bufferevent *bev;
	struct evbuffer *inbuf_processed;
};

static int
method_from_string(enum http_method *m, const char *method)
{
	if (!evutil_ascii_strcasecmp(method, "GET"))
		*m = METH_GET;
	else if (!evutil_ascii_strcasecmp(method, "HEAD"))
		*m = METH_HEAD;
	else if (!evutil_ascii_strcasecmp(method, "POST"))
		*m = METH_POST;
	else if (!evutil_ascii_strcasecmp(method, "PUT"))
		*m = METH_PUT;
	else if (!evutil_ascii_strcasecmp(method, "CONNECT"))
		*m = METH_CONNECT;
	else {
		log_warn("method_from_string: unknown method, '%s'", method);
		return -1;
	}

	return 0;
}

static const char *
method_to_string(enum http_method m)
{
	switch (m) {
	case METH_GET:
		return "GET";
	case METH_HEAD:
		return "HEAD";
	case METH_POST:
		return "POST";
	case METH_PUT:
		return "PUT";
	case METH_CONNECT:
		return "CONNECT";
	}

	log_fatal("method_to_string: unknown method %d", m);
	return "???";
}

static int
version_from_string(enum http_version *v, const char *vers)
{
	if (evutil_ascii_strncasecmp(vers, "HTTP/", 5)) {
		log_warn("version_from_string: bad http-version, '%s'", vers);
		return -1;
	}

	vers += 5;

	/* XXX this only understands 1.0 and 1.1 */

	if (!strcmp(vers, "1.0"))
		*v = HTTP_10;
	else if (!strcmp(vers, "1.1"))
		*v = HTTP_11;
	else {
		log_warn("version_from_string: unknown http-version, '%s'",
		         vers);
		return -1;
	}
	
	return 0;
}

static const char *
version_to_string(enum http_version v)
{
	switch (v) {
	case HTTP_UNKNOWN:
		return "HTTP/??";
	case HTTP_10:
		return "HTTP/1.0";
	case HTTP_11:
		return "HTTP/1.1";
	}
	
	log_fatal("version_to_string: unknown version %d", v);
	return "???";
}

static void
begin_message(struct http_conn *conn)
{
	// XXX read timeout?
	assert(conn->headers == NULL && conn->firstline == NULL);
	assert(!conn->read_paused);
	conn->headers = mem_calloc(1, sizeof(*conn->headers));
	TAILQ_INIT(conn->headers);
	conn->state = HTTP_STATE_IDLE;
	bufferevent_enable(conn->bev, EV_WRITE | EV_READ);
}

static void
end_message(struct http_conn *conn, enum http_conn_error err)
{
	if (conn->firstline)
		mem_free(conn->firstline);
	if (conn->headers)
		headers_clear(conn->headers);

	if (err != ERROR_NONE || !conn->persistent) {
		conn->state = HTTP_STATE_MANGLED;
		bufferevent_disable(conn->bev, EV_WRITE | EV_READ);
	} else
		begin_message(conn);

	if (err != ERROR_NONE)
		METHOD1(conn, on_error, err);
	else
		METHOD0(conn, on_msg_complete);
}

static struct http_request *
build_request(struct http_conn *conn)
{
	struct http_request *req;
	struct token_list tokens;
	struct token *method, *url, *vers;
	enum http_method m;
	enum http_version v;
	struct url *u = NULL;
	size_t ntokens;

	assert(conn->type == HTTP_CLIENT);

	TAILQ_INIT(&tokens);
	req = NULL;

	ntokens = tokenize(conn->firstline, " ", 4, &tokens);
	if (ntokens != 3)
		goto out;

	method = TAILQ_FIRST(&tokens);
	url = TAILQ_NEXT(method, next);	
	vers = TAILQ_NEXT(url, next);	
	u = url_tokenize(url->token);

	if (!u || method_from_string(&m, method->token) < 0 ||
            version_from_string(&v, vers->token) < 0)
		goto out;

	req = mem_calloc(1, sizeof(*req));
	req->meth = m;
	req->vers = v;
	req->url = u;
	u = NULL;
	req->headers = conn->headers;

out:
	url_free(u);
	token_list_clear(&tokens);
	
	return req;
}

static struct http_response *
build_response(struct http_conn *conn)
{
	struct http_response *resp;
	struct token_list tokens;
	struct token *vers, *code, *reason;
	enum http_version v;
	int c;
	size_t ntokens;

	assert(conn->type == HTTP_SERVER);

	TAILQ_INIT(&tokens);
	resp = NULL;

	ntokens = tokenize(conn->firstline, " ", 2, &tokens);
	if (ntokens != 3)
		goto out;

	vers = TAILQ_FIRST(&tokens);
	code = TAILQ_NEXT(vers, next);
	reason = TAILQ_NEXT(code, next);
	c = atoi(code->token);

        if (version_from_string(&v, vers->token) < 0 || c < 100 || c > 999)
		goto out;

	resp = mem_calloc(1, sizeof(*resp));
	resp->vers = v;
	resp->code = c;
	resp->reason = reason->token;
	reason->token = NULL; /* so token_list_clear will skip this */
	resp->headers = conn->headers;

out:
	token_list_clear(&tokens);
	
	return resp;
}

/* return -1 failure, 0 incomplete, 1 ok */
static int
parse_chunk_len(struct http_conn *conn)
{
	struct evbuffer *inbuf = bufferevent_get_input(conn->bev);
	char *line;
	ev_int64_t len;

	while ((line = evbuffer_readln(inbuf, NULL, EVBUFFER_EOL_CRLF))) {
		if (*line == '\0') {
			mem_free(line);
			continue;
		}

		len = get_int(line, 16);
		if (len < 0) {
			mem_free(line);
			log_warn("parse_chunk_len: invalid chunk len");
			return -1;
		}

		conn->data_remaining = len;
		return 1;
	}

	return 0;
}

static void
read_chunk(struct http_conn *conn)
{
	struct evbuffer *inbuf = bufferevent_get_input(conn->bev);
	size_t len = evbuffer_get_length(inbuf);
	char *line;

	if (conn->data_remaining < 0) {
		if (parse_chunk_len(conn) < 0)
			end_message(conn, ERROR_CHUNK_PARSE_FAILED);
	} else if (conn->data_remaining == 0) {
		line = evbuffer_readln(inbuf, NULL, EVBUFFER_EOL_CRLF);
		if (line) {
			/* XXX doesn't handle trailers */
			if (*line != '\0')
				log_warn("http_conn: garbage after last chunk");
			mem_free(line);		
			end_message(conn, ERROR_NONE);
			return;
		}
	} else {
		/* XXX should mind potential overflow */
		if (len >= (size_t)conn->data_remaining)
			len = (size_t)conn->data_remaining;

		evbuffer_remove_buffer(inbuf, conn->inbuf_processed, len);
		METHOD1(conn, on_read_body, conn->inbuf_processed);
		conn->data_remaining -= len;

		if (conn->data_remaining == 0)
			conn->data_remaining = -1;
	}
}

static void
read_body(struct http_conn *conn)
{
	struct evbuffer *inbuf = bufferevent_get_input(conn->bev);
	size_t len;

	assert(conn->has_body);

	if (conn->te == TE_CHUNKED) {
		read_chunk(conn);
		return;
	}

	len = evbuffer_get_length(inbuf);
	if (len) {
		/* XXX should mind potential overflow */
		if (conn->data_remaining >= 0 &&
		    len > (size_t)conn->data_remaining) {
			len = (size_t)conn->data_remaining;
			evbuffer_remove_buffer(inbuf, conn->inbuf_processed, len);
			METHOD1(conn, on_read_body, conn->inbuf_processed);
		} else {
			evbuffer_add_buffer(conn->inbuf_processed, inbuf);
			METHOD1(conn, on_read_body, conn->inbuf_processed);
		}

		conn->data_remaining -= len;
		if (conn->data_remaining == 0)
			end_message(conn, ERROR_NONE);
	}
}

static void
check_headers(struct http_conn *conn, struct http_request *req,
	   struct http_response *resp)
{
	enum http_version vers;
	int persistent;
	char *val;

	conn->te = TE_IDENTITY;
	conn->has_body = 1;
	conn->msg_complete_on_eof = 0;
	conn->data_remaining = -1;

	if (conn->type == HTTP_CLIENT) {
		vers = req->vers;
		conn->has_body = 0;
		if (req->meth == METH_POST ||
		    req->meth == METH_PUT)
			conn->has_body = 1;
	} else { /* server */
		vers = resp->vers;
		if ((resp->code >= 100 && resp->code < 200) ||
		    resp->code == 204 || resp->code == 205 ||
		    resp->code == 304)
			conn->has_body = 0;
	}

	/* check headers */
	if (conn->has_body) {
		val = headers_find(conn->headers, "transfer-encoding");
		if (val) {
			if (!evutil_ascii_strcasecmp(val, "chunked"))
				conn->te = TE_CHUNKED;
			mem_free(val);
		}

		if (conn->te != TE_CHUNKED) {
			val = headers_find(conn->headers, "content-length");
			if (val) {
				ev_int64_t iv;
				iv = get_int(val, 10);
				if (iv < 0)
					log_warn("http_conn: mangled Content-Length");
				else
					conn->data_remaining = iv;
				mem_free(val);
				if (conn->data_remaining == 0)
					conn->has_body = 0;
			} else {
				conn->msg_complete_on_eof = 1;
			}
		}

		if (conn->type == HTTP_CLIENT && conn->data_remaining < 0
		    && conn->te != TE_CHUNKED) {
			METHOD1(conn, on_error,
				ERROR_CLIENT_POST_WITHOUT_LENGTH);
			return;
		}
	}

	assert(vers != HTTP_UNKNOWN);

	persistent = 0;
	if (!conn->msg_complete_on_eof && vers == HTTP_11)
		persistent = 1;

	if (conn->vers != HTTP_UNKNOWN && conn->vers != vers) {
		log_warn("http_conn: http version changed!");
		persistent = 0;
	}
	conn->vers = vers;

	if (persistent) {
		val = headers_find(conn->headers, "connection");
		if (val) {
			if (evutil_ascii_strcasecmp(val, "close"))
				persistent = 0;
			mem_free(val);
		}
	}
	conn->persistent = persistent;
}

static void
read_headers(struct http_conn *conn)
{
	int failed = 0;
	struct evbuffer *inbuf = bufferevent_get_input(conn->bev);
	struct http_request *req = NULL;
	struct http_response *resp = NULL;

	assert(conn->state == HTTP_STATE_READ_HEADERS);

	switch (headers_load(conn->headers, inbuf)) {
	case -1:
		end_message(conn, ERROR_HEADER_PARSE_FAILED);
		return;
	case 0:
		return;
	/* case 1: finished, fall thru */
	}

	assert(conn->firstline);

	if (conn->type == HTTP_CLIENT) {
		req = build_request(conn);
		if (!req)
			failed = 1;
	} else {
		resp = build_response(conn);
		if (!resp)
			failed = 1;
	}
	
	mem_free(conn->firstline);
	conn->firstline = NULL;

	if (failed) {
		assert(!req && !resp);
		end_message(conn, ERROR_HEADER_PARSE_FAILED);
		return;
	}

	check_headers(conn, req, resp);
	conn->headers = NULL;

	/* ownership of req or resp is now passed on */
	if (req)
		METHOD1(conn, on_client_request, req);
	if (resp)
		METHOD1(conn, on_server_response, resp);

	if (!conn->has_body)
		end_message(conn, ERROR_NONE);
	else
		conn->state = HTTP_STATE_READ_BODY;
}

static void
http_errorcb(struct bufferevent *bev, short what, void *_conn)
{
	enum http_state state;
	struct http_conn *conn = _conn;

	if (conn->state == HTTP_STATE_CONNECTING) {
		if (what & BEV_EVENT_CONNECTED) {
			begin_message(conn);
			METHOD0(conn, on_connect);
		} else {	
			conn->state = HTTP_STATE_MANGLED;
			METHOD1(conn, on_error, ERROR_CONNECT_FAILED);
		}
		return;
	}

	assert(!(what & BEV_EVENT_CONNECTED));

	state = conn->state;
	conn->state = HTTP_STATE_MANGLED;

	if (what & BEV_EVENT_WRITING) {
		end_message(conn, ERROR_WRITE_FAILED);
		return;
	}
	
	switch (state) {
	case HTTP_STATE_IDLE:
		end_message(conn, ERROR_IDLE_CONN_TIMEDOUT);
		break;
	case HTTP_STATE_READ_FIRSTLINE:
	case HTTP_STATE_READ_HEADERS:
		end_message(conn, ERROR_INCOMPLETE_HEADERS);
		break;
	case HTTP_STATE_READ_BODY:
		if ((what & BEV_EVENT_EOF) && conn->msg_complete_on_eof)
			end_message(conn, ERROR_NONE);
		else
			end_message(conn, ERROR_INCOMPLETE_BODY);
		break;
	default:
		log_fatal("http_conn: errorcb called in invalid state");
	}
}

static void
process_one_message(struct http_conn *conn)
{
	struct evbuffer *inbuf = bufferevent_get_input(conn->bev);

	switch (conn->state) {
	case HTTP_STATE_IDLE:
		conn->state = HTTP_STATE_READ_FIRSTLINE;
		// XXX should remove idle timeout at this point?
		/* fallthru... */
	case HTTP_STATE_READ_FIRSTLINE:
		assert(conn->firstline == NULL);
		conn->firstline = evbuffer_readln(inbuf, NULL, EVBUFFER_EOL_CRLF);
		if (conn->firstline)
			conn->state = HTTP_STATE_READ_HEADERS;
		break;	
	case HTTP_STATE_READ_HEADERS:
		read_headers(conn);
		break;
	case HTTP_STATE_READ_BODY:
		read_body(conn);
		break;
	default:
		log_fatal("http_conn: read cb called in invalid state");	
	}
}

static void
process_inbuf(struct http_conn *conn)
{
	struct evbuffer *inbuf = bufferevent_get_input(conn->bev);

	do {
		process_one_message(conn);
	} while (!conn->read_paused &&
		 evbuffer_get_length(inbuf) > 0);
}

static void
http_readcb(struct bufferevent *bev, void *_conn)
{
	process_inbuf(_conn);
}

static void
http_writecb(struct bufferevent *bev, void *_conn)
{
	struct http_conn *conn = _conn;
	struct evbuffer *outbuf = bufferevent_get_output(bev);

	if (conn->is_choaked) {
		bufferevent_setwatermark(bev, EV_WRITE, 0, 0);
		conn->is_choaked = 0;
		METHOD0(conn, on_write_more);
	} else if (evbuffer_get_length(outbuf) == 0)
		METHOD0(conn, on_flush);
}

struct http_conn *
http_conn_new(struct event_base *base, evutil_socket_t sock,
	      enum http_type type, const struct http_cbs *cbs, void *cbarg)
{
	struct http_conn *conn;

	conn = mem_calloc(1, sizeof(*conn));
	conn->type = type;
	conn->cbs = cbs;
	conn->cbarg = cbarg;
	conn->bev = bufferevent_socket_new(base, sock,
			BEV_OPT_CLOSE_ON_FREE);
	if (!conn->bev)
		log_fatal("http_conn: failed to create bufferevent");

	conn->inbuf_processed = evbuffer_new();
	if (!conn->inbuf_processed)
		log_fatal("http_conn: failed to create evbuffer");

	bufferevent_setcb(conn->bev, http_readcb, http_writecb,
		          http_errorcb, conn);
	
	if (sock >= 0)
		begin_message(conn);

	return conn;
}

int
http_conn_connect(struct http_conn *conn, struct evdns_base *dns,
		      int family, const char *host, int port)
{
	// XXX need SOCKS
	conn->state = HTTP_STATE_CONNECTING;
	return bufferevent_socket_connect_hostname(conn->bev, dns, family,
					    	   host, port);	
}

void
http_conn_free(struct http_conn *conn)
{
	bufferevent_free(conn->bev);
	evbuffer_free(conn->inbuf_processed);
	mem_free(conn);
}

void
http_conn_write_request(struct http_conn *conn, struct http_request *req)
{
	struct evbuffer *outbuf;

	assert(conn->type == HTTP_SERVER);

	// XXX note the TE of resp

	outbuf = bufferevent_get_output(conn->bev);

	evbuffer_add_printf(outbuf, "%s %s %s\r\n",
		method_to_string(req->meth),
		req->url->query,
		version_to_string(req->vers));
		
	headers_dump(req->headers, outbuf);	
}

void
http_conn_write_response(struct http_conn *conn, struct http_response *resp)
{
	struct evbuffer *outbuf;

	assert(conn->type == HTTP_CLIENT);

	// XXX note the TE of resp

	outbuf = bufferevent_get_output(conn->bev);

	evbuffer_add_printf(outbuf, "%s %d %s\r\n",
		        version_to_string(conn->vers),
			resp->code,
			resp->reason);
		
	headers_dump(resp->headers, outbuf);	
}

int
http_conn_write_buf(struct http_conn *conn, struct evbuffer *buf)
{
	struct evbuffer *outbuf;
	
	// XXX translate input to chunked format if needed

	outbuf = bufferevent_get_output(conn->bev);

	evbuffer_add_buffer(outbuf, buf);

	/* have we choaked? */	
	if (evbuffer_get_length(outbuf) > max_write_backlog) {
		bufferevent_setwatermark(conn->bev, EV_WRITE,
					 max_write_backlog / 2, 0);
		conn->is_choaked = 1;
		return 0;
	}

	return 1;
}

int
http_conn_current_message_has_body(struct http_conn *conn)
{
	return conn->has_body;
}

void
http_conn_set_current_message_bodyless(struct http_conn *conn)
{
	assert(conn->type == HTTP_SERVER);
	conn->has_body = 0;
}

int
http_conn_is_persistent(struct http_conn *conn)
{
	return conn->persistent;
}

void
http_conn_stop_reading(struct http_conn *conn)
{
	bufferevent_disable(conn->bev, EV_READ);
	conn->read_paused = 1;
}

void
http_conn_start_reading(struct http_conn *conn)
{
	struct evbuffer *inbuf = bufferevent_get_input(conn->bev);

	bufferevent_enable(conn->bev, EV_READ);
	conn->read_paused = 0;
	// XXX this might cause recursion
	if (evbuffer_get_length(inbuf) > 0)
		process_inbuf(conn);
}

void
http_conn_flush(struct http_conn *conn)
{
	struct evbuffer *outbuf = bufferevent_get_outbuf(conn->bev);

	// XXX this might cause recursion
	if (evbuffer_get_length(outbuf) == 0)
		METHOD0(conn, on_flush);
}

void
http_conn_send_error(struct http_conn *conn, int code)
{
	struct http_response resp;
	struct header_list headers;

	resp.headers = &headers;
	
	TAILQ_INIT(headers);

	headers_add_key(&headers, "Connection");
	if (conn->state == HTTP_STATE_READ_BODY ||
	    !conn->persistent) {
		headers_add_value(&headers, "close");
	} else {
		headers_add_value(&headers, "keep-alive");
	}
	// XXX
	headers_clear(&headers);
}

void
http_request_free(struct http_request *req)
{
	url_free(req->url);
	headers_clear(req->headers);
	mem_free(req);
}

void
http_response_free(struct http_response *resp)
{
	headers_clear(resp->headers);
	mem_free(resp->headers);
	mem_free(resp->reason);
	mem_free(resp);
}

#ifdef TEST_HTTP
#include <netinet/in.h>
#include <stdio.h>
#include <event2/dns.h>

static void
proxy_connected(struct http_conn *conn, void *arg)
{
	struct http_request req;
	struct header_list headers;
	struct evbuffer *buf;

	TAILQ_INIT(&headers);
	req.meth = METH_GET;
	req.url = arg;
	req.vers = HTTP_11;
	req.headers = &headers;	

	buf = evbuffer_new();
	evbuffer_add_printf(buf, "Host: %s\r\n\r\n", req.url->host);
	headers_load(&headers, buf);
	evbuffer_free(buf);

	http_conn_write_request(conn, &req);
}

static void
proxy_error(struct http_conn *conn, enum http_conn_error err, void *arg)
{
	fprintf(stderr, "error %d\n", err);
	http_conn_free(conn);
}

static void
proxy_response(struct http_conn *conn, struct http_response *resp, void *arg)
{
	struct evbuffer *buf;

	fprintf(stderr, "response: %s, %d, %s\n",
		version_to_string(resp->vers),
		resp->code,
		resp->reason);

	buf = evbuffer_new();
	headers_dump(resp->headers, buf);
	fwrite(evbuffer_pullup(buf, evbuffer_get_length(buf)), evbuffer_get_length(buf), 1, stderr);
	evbuffer_free(buf);
}

static void
proxy_read_body(struct http_conn *conn, struct evbuffer *buf, void *arg)
{
	size_t len = evbuffer_get_length(buf);
	fwrite(evbuffer_pullup(buf, len), len, 1, stderr);
	evbuffer_drain(buf, len);
}

static void
proxy_msg_complete(struct http_conn *conn, void *arg)
{
	fprintf(stderr, "\n...MSG COMPLETE...\n");
}

static void
proxy_write_more(struct http_conn *conn, void *arg)
{
}

static struct http_cbs test_proxy_cbs = {
	proxy_connected,
	proxy_error,
	0,
	proxy_response,
	proxy_read_body,
	proxy_msg_complete,
	proxy_write_more
};

int
main(int argc, char **argv)
{
	struct event_base *base;
	struct evdns_base *dns;
	struct http_conn *http;
	struct url *url;

	url = url_tokenize(argv[1]);
	if (!url)
		return 0;

	if (url->port < 0)
		url->port = 80;

	base = event_base_new();
	dns = evdns_base_new(base, 1);

	http = http_conn_new(base, -1, HTTP_SERVER, &test_proxy_cbs, url);
	http_conn_connect(http, dns, AF_UNSPEC, url->host, url->port);

	event_base_dispatch(base);

	return 0;
}

#endif
