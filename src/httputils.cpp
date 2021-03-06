#include "vk-common.h"

#include "httputils.h"
#include "miscutils.h"

PurpleHttpConnection* http_get(PurpleConnection* gc, const string& url, const HttpCallback& callback)
{
    PurpleHttpRequest* request = purple_http_request_new(url.data());
    PurpleHttpConnection* hc = http_request(gc, request, callback);
    if (!hc)
        return nullptr;
    purple_http_request_unref(request);
    return hc;
}

namespace
{

struct HttpUserData
{
    HttpCallback callback;
    int retries;
};

const int MAX_HTTP_RETRIES = 3;

// Callback helper for http_request.
void http_cb(PurpleHttpConnection* http_conn, PurpleHttpResponse* response, void* user_data)
{
    HttpUserData* data = (HttpUserData*)user_data;
    PurpleConnection* gc = purple_http_conn_get_purple_connection(http_conn);
    int response_code = purple_http_response_get_code(response);
    if ((response_code == 0 || response_code >= 500) && data->retries < MAX_HTTP_RETRIES
            && !get_data(gc).is_closing()) {
        vkcom_debug_error("HTTP error %d, retrying %d time\n",
                           purple_http_response_get_code(response), data->retries + 1);

        // We've got a network error or Vk.com server error and have not given up retrying.
        PurpleHttpRequest* request = purple_http_conn_get_request(http_conn);
        // Reference the request, so that it does not die with http_conn
        purple_http_request_ref(request);
        timeout_add(gc, 1000, [=] {
            data->retries++;
            purple_http_request(gc, request, http_cb, data);
            purple_http_request_unref(request);
            return false;
        });
    } else {
        data->callback(http_conn, response);
        delete data;
    }
}

} // End anonymous namespace

PurpleHttpConnection* http_request(PurpleConnection* gc, PurpleHttpRequest* request,
                                   const HttpCallback& callback)
{
    VkData& gc_data = get_data(gc);
    if (gc_data.is_closing()) {
        vkcom_debug_error("Attempting to connect while closing the connection\n");
        return nullptr;
    }

    purple_http_request_set_keepalive_pool(request, gc_data.get_keepalive_pool());
    HttpUserData* data = new HttpUserData();
    data->callback = callback;
    data->retries = 0;
    PurpleHttpConnection* hc = purple_http_request(gc, request, http_cb, data);
    return hc;
}

namespace
{

// A helper callback for purple_http_request_update_on_redirect. TODO: check for infinite loops.
void http_request_redirect_cb(PurpleHttpConnection* http_conn, PurpleHttpResponse* response,
                              const HttpCallback& callback)
{
    if (purple_http_response_get_code(response) == 302) {
        PurpleConnection* gc = purple_http_conn_get_purple_connection(http_conn);
        PurpleHttpRequest* request = purple_http_conn_get_request(http_conn);
        const char* new_url = purple_http_response_get_header(response, "Location");
        purple_http_request_set_url(request, new_url);
        http_request(gc, request, [=](PurpleHttpConnection* new_http_conn, PurpleHttpResponse* new_response) {
            http_request_redirect_cb(new_http_conn, new_response, callback);
        });
    } else {
        callback(http_conn, response);
    }
}

} // End anonymous namespace

PurpleHttpConnection* http_request_update_on_redirect(PurpleConnection* gc, PurpleHttpRequest* request,
                                                      const HttpCallback& callback)
{
    purple_http_request_set_max_redirects(request, 0);

    return http_request(gc, request, [=](PurpleHttpConnection* http_conn, PurpleHttpResponse* response) {
        http_request_redirect_cb(http_conn, response, callback);
    });
}


void http_request_copy_cookie_jar(PurpleHttpRequest* target, PurpleHttpConnection* source_conn)
{
    PurpleHttpRequest* source_request = purple_http_conn_get_request(source_conn);
    purple_http_request_set_cookie_jar(target, purple_http_request_get_cookie_jar(source_request));
}
