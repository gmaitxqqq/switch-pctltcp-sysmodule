#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <switch.h>

#define HTTP_PORT  8080

void http_server_start(void);
void http_server_stop(void);
bool http_server_is_running(void);

#endif /* HTTP_SERVER_H */
