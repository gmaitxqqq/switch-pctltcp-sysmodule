/**
 * http_server.h - Lightweight HTTP server for pctltcp-web
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/* Custom module ID for MAKERESULT */
#ifndef Module_Custom
#define Module_Custom 362  /* Arbitrary module ID */
#endif


#include <switch.h>

#define HTTP_PORT  8081

/* Custom module ID for MAKERESULT */
#ifndef Module_Custom
#define Module_Custom 362  /* Arbitrary module ID */
#endif

Result http_server_start(void);
void http_server_stop(void);
bool http_server_is_running(void);

#endif /* HTTP_SERVER_H */