#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libaio.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <errno.h>
#include <fcntl.h>

#include "./../headers/util.h"
#include "./../headers/debug.h"
#include "./../headers/sock_util.h"
#include "./../headers/w_epoll.h"
#include "./http-parser/http_parser.h"
#include "aws.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

/* Parser used for requests */
static http_parser request_parser;

/* Storage for request_path */
static char request_path[BUFSIZ];

// connection statuses
enum connection_state
{
    STATE_DATA_RECEIVED,
    STATE_FRAGMENT_RECIEVED,
    STATE_DATA_SENT,
    STATE_FRAGMENT_SENT,
    STATE_CONNECTION_CLOSED
};

/* structure acting as a connection handler */
struct connection
{
    int sockfd;
    /* buffers used for receiving messages and then echoing them back */
    char recv_buffer[BUFSIZ];
    size_t recv_len;
    char send_buffer[BUFSIZ];
    size_t send_len;
    enum connection_state state;

    int id;
    int status;
    int counter_got;
    int counter_submits;
    int counter_send;
    int efd;
    int fd;
    int size;
    off_t offset;
    int message_size;
    int total;
    char **buffer_matrix;
    int last;
    io_context_t context;

    struct iocb **arr_iocb;
    struct iocb *iocb;
};

/*
 * Callback is invoked by HTTP request parser when parsing request path.
 * Request path is stored in global request_path variable.
 */
static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
    assert(p == &request_parser);
    char path[BUFSIZ];
    sscanf(buf, "%[^.]", path);
    sprintf(request_path, "%s%s.dat", AWS_DOCUMENT_ROOT, path + 1);

    return 0;
}

/* Use mostly null settings except for on_path callback. */
static http_parser_settings settings_on_path = {
    /* on_message_begin */ 0,
    /* on_header_field */ 0,
    /* on_header_value */ 0,
    /* on_path */ on_path_cb,
    /* on_url */ 0,
    /* on_fragment */ 0,
    /* on_query_string */ 0,
    /* on_body */ 0,
    /* on_headers_complete */ 0,
    /* on_message_complete */ 0};

/*
 * Receive (HTTP) request. Don't parse it, just read data in buffer
 * and print it.
 */

static struct connection *connection_create(int sockfd)
{
    struct connection *conn = malloc(sizeof(*conn));

    DIE(conn == NULL, "malloc");

    memset(&conn->context, 0, sizeof(io_context_t));
    int ret_code = io_setup(1, &conn->context);
    DIE(ret_code < 0, strerror(errno));

    conn->sockfd = sockfd;
    conn->status = 0;
    conn->id = 0;
    conn->offset = 0;
    conn->recv_len = 0;
    conn->send_len = 0;
    conn->message_size = 0;
    conn->counter_got = 0;
    conn->counter_send = 0;
    conn->counter_submits = 0;
    conn->last = 0;
    conn->total = 0;

    conn->efd = eventfd(0, EFD_NONBLOCK);
    DIE(conn->efd < 0, "eventefd");
    memset(conn->recv_buffer, 0, BUFSIZ);
    memset(conn->send_buffer, 0, BUFSIZ);

    return conn;
}

/*
 * Remove connection handler.
 */

static void connection_remove(struct connection *conn)
{
    close(conn->sockfd);
    conn->state = STATE_CONNECTION_CLOSED;
    free(conn);
}

/*
 * Handle a new connection request on the server socket.
 */

static void handle_new_connection(void)
{
    static int sockfd;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    struct sockaddr_in addr;
    struct connection *conn;
    int rc;

    /* accept new connection */
    sockfd = accept(listenfd, (SSA *)&addr, &addrlen);
    DIE(sockfd < 0, "accept");

    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

    /* instantiate new connection handler */
    conn = connection_create(sockfd);

    /* add socket to epoll */
    rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
    DIE(rc < 0, "w_epoll_add_in");
}

/*
 * Send message on socket.
 * Store message in send_buffer in struct connection.
 */

// static enum connection_state send_message(struct connection *conn)
// {
//     ssize_t message_size;
//     int rc;
//     char abuffer[64];

//     rc = get_peer_address(conn->sockfd, abuffer, 64);
//     if (rc < 0)
//     {
//         ERR("get_peer_address");
//         goto remove_connection;
//     }

//     message_size = send(conn->sockfd, conn->send_buffer + conn->message_size,
//                         conn->send_len - conn->message_size, 0);
//     if (message_size < 0)
//     { /* error in communication */
//         goto remove_connection;
//     }

//     conn->message_size = conn->message_size + message_size;
//     if (conn->message_size < conn->send_len)
//         return STATE_FRAGMENT_SENT;

//     if (message_size == 0)
//     { /* connection closed */
//         goto remove_connection;
//     }

//     conn->state = STATE_DATA_SENT;

//     return STATE_DATA_SENT;

// remove_connection:
//     rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
//     DIE(rc < 0, "w_epoll_remove_ptr");

//     /* remove current connection */
//     close(conn->fd);
//     connection_remove(conn);

//     return STATE_CONNECTION_CLOSED;
// }
static enum connection_state send_message(struct connection *conn)
{
    ssize_t message_size;
    int rc;
    char abuffer[64];

    rc = get_peer_address(conn->sockfd, abuffer, 64);
    if (rc < 0)
    {
        ERR("get_peer_address");
        goto remove_connection;
    }

    message_size = send(conn->sockfd, conn->send_buffer + conn->message_size, conn->send_len - conn->message_size, 0);
    if (message_size < 0)
        goto remove_connection;

    conn->message_size += message_size;
    if (conn->message_size < conn->send_len)
        return STATE_FRAGMENT_SENT;

    if (message_size == 0)
        goto remove_connection;

    conn->state = STATE_DATA_SENT;

    return STATE_DATA_SENT;

remove_connection:
    rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_remove_ptr");

    /* remove current connection */
    close(conn->fd);
    connection_remove(conn);

    return STATE_CONNECTION_CLOSED;
}

/*
 * Receive message on socket.
 * Store message in recv_buffer in struct connection.
 */

static enum connection_state receive_request(struct connection *conn)
{
    ssize_t bytes_recv;
    int rc;
    char abuffer[64];

    rc = get_peer_address(conn->sockfd, abuffer, 64);
    if (rc < 0)
    {
        ERR("get_peer_address");
        goto remove_connection;
    }

    bytes_recv = recv(conn->sockfd, conn->recv_buffer, BUFSIZ, 0);
    if (bytes_recv < 0)
    { /* error in communication */
        goto remove_connection;
    }
    if (bytes_recv == 0)
    { /* connection closed */
        goto remove_connection;
    }

    printf("--\n%s--\n", conn->recv_buffer);

    conn->recv_len += bytes_recv;
    conn->state = STATE_DATA_RECEIVED;

    conn->recv_buffer[conn->recv_len] = 0;
    if (strcmp(conn->recv_buffer + conn->recv_len - 4, "\r\n\r\n") != 0)
        return STATE_FRAGMENT_RECIEVED;

    http_parser_init(&request_parser, HTTP_REQUEST);

    int bytes_parsed = http_parser_execute(&request_parser, &settings_on_path, conn->recv_buffer,
                                           conn->recv_len);
    if (bytes_parsed == 0)
        goto remove_connection;

    return STATE_DATA_RECEIVED;

remove_connection:
    rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_remove_ptr");

    close(conn->sockfd);

    rc = io_destroy(conn->context);
    DIE(rc < 0, "io_destroy");

    /* remove current connection */
    connection_remove(conn);

    return STATE_CONNECTION_CLOSED;
}

/*
 * Send HTTP reply. Send simple message, don't care about request content.
 *
 * Socket is closed after HTTP reply.
 */

static void put_header(struct connection *conn)
{
    char buffer[BUFSIZ];

    sprintf(buffer, "HTTP/1.1 200 OK\r\n"
                    "Date: Sun, 08 May 2011 09:26:16 GMT\r\n"
                    "Server: Apache/2.2.9\r\n"
                    "Last-Modified: Mon, 02 Aug 2010 17:55:28 GMT\r\n"
                    "Accept-Ranges: bytes\r\n"
                    "Content-Length: %d\r\n"
                    "Vary: Accept-Encoding\r\n"
                    "Connection: close\r\n"
                    "Content-Type: text/html\r\n"
                    "\r\n",
            conn->size);
    conn->send_len = strlen(buffer);
    memcpy(conn->send_buffer, buffer, strlen(buffer));
}

static void put_error(struct connection *conn)
{
    char buffer[BUFSIZ] = "HTTP/1.1 404 Not Found\r\n"
                          "Date: Sun, 08 May 2011 09:26:16 GMT\r\n"
                          "Server: Apache/2.2.9\r\n"
                          "Last-Modified: Mon, 02 Aug 2010 17:55:28 GMT\r\n"
                          "Accept-Ranges: bytes\r\n"
                          "Content-Length: 153\r\n"
                          "Vary: Accept-Encoding\r\n"
                          "Connection: close\r\n"
                          "Content-Type: text/html\r\n"
                          "\r\n";
    conn->send_len = strlen(buffer);
    memcpy(conn->send_buffer, buffer, strlen(buffer));
}

// static void handle_client_request(struct connection *connection)
// {
//     int rc;
//     enum connection_state ret_state;

//     ret_state = receive_request(connection);
//     if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_FRAGMENT_RECIEVED)
//         return;

//     // connection_copy_buffers(connection);
//     char _static[BUFSIZ];
//     sprintf(_static, "%sstatic/", AWS_DOCUMENT_ROOT);

//     char dynamic[BUFSIZ];
//     sprintf(dynamic, "%sdynamic/", AWS_DOCUMENT_ROOT);

//     /* add socket to epoll for out events */
//     rc = w_epoll_update_ptr_out(epollfd, connection->sockfd, connection);
//     DIE(rc < 0, "w_epoll_add_ptr_out");

//     // open input
//     connection->fd = open(request_path, O_RDONLY);
//     if (connection->fd == -1)
//     {
//         connection->id = 0;
//         put_error(connection);
//         return;
//     }

//     // find input file size
//     struct stat buffer;
//     fstat(connection->fd, &buffer);
//     connection->size = buffer.st_size;
//     connection->offset = 0;

//     // find what case we are on static or dynamic
//     if (!strncmp(request_path, dynamic, strlen(dynamic)))
//     {
//         connection->id = 2;
//         put_header(connection);
//         connection->status = 0;
//     }
//     else if (!strncmp(request_path, _static, strlen(_static)))
//     {
//         connection->id = 1;
//         connection->status = 0;
//         put_header(connection);
//     }
//     else
//     {
//         put_error(connection);
//         connection->id = 0;
//         return;
//     }
// }

static void handle_client_request(struct connection *conn)
{
    int rc;
    enum connection_state ret_state;

    // connection_copy_buffers(conn);
    ret_state = receive_request(conn);
    // if(conn->stopped) return;

    if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_FRAGMENT_RECIEVED)
        return;

    char static_prefix[BUFSIZ];
    char dynamic_prefix[BUFSIZ];

    sprintf(static_prefix, "%sstatic/", AWS_DOCUMENT_ROOT);
    sprintf(dynamic_prefix, "%sdynamic/", AWS_DOCUMENT_ROOT);

    /* add socket to epoll for out events */
    rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn); // inout
    DIE(rc < 0, "w_epoll_add_ptr_out");

    struct stat stat_buf;
    /* Open the input file. */
    conn->fd = open(request_path, O_RDONLY);
    if (conn->fd == -1)
    {
        conn->id = 0;
        put_error(conn);
        return;
    }
    /* Stat the input file to obtain its size. */
    fstat(conn->fd, &stat_buf);
    conn->size = stat_buf.st_size;
    conn->offset = 0;

    if (strncmp(request_path, static_prefix, strlen(static_prefix)) == 0)
    {
        conn->id = 1;
        conn->status = 0;
        put_header(conn);
    }
    else if (strncmp(request_path, dynamic_prefix, strlen(dynamic_prefix)) == 0)
    {
        conn->id = 2;
        put_header(conn);
        conn->status = 0;
    }
    else
    {
        put_error(conn);
        conn->id = 0;
        return;
    }
}
static void handle_event(struct connection *connection)
{
    struct io_event events[connection->counter_submits];
    u_int64_t val;

    int rc = read(connection->efd, &val, sizeof(val));
    DIE(rc < 0, "read efd");

    rc = io_getevents(connection->context, val, val, events, NULL);
    DIE(rc != val, "io_getevents");

    connection->counter_got += val;

    rc = w_epoll_add_ptr_out(epollfd, connection->sockfd, connection);
    DIE(rc < 0, "w_epoll_add_ptr_out");
}

static void destroy_connection(struct connection *connection)
{
    // remove pointer
    int ret_code = w_epoll_remove_ptr(epollfd, connection->sockfd, connection);
    DIE(ret_code < 0, "w_epoll_remove_ptr");

    // destroy connection
    ret_code = io_destroy(connection->context);
    DIE(ret_code < 0, "io_destroy");

    // close file descriptors and connection
    close(connection->fd);
    close(connection->sockfd);
    connection_remove(connection);
}

static void send_message_event(struct connection *connection)
{
    int real_size = connection->size - connection->offset;

    if (real_size > BUFSIZ)
        real_size = BUFSIZ;

    // send BUFSIZ bytes every time
    int amount = sendfile(connection->sockfd, connection->fd, &connection->offset, real_size);
    connection->message_size += amount;
    DIE(amount < 0, "sendfile");

    if (!amount)
    {
        // set status and message size
        connection->status = 0;
        connection->message_size = 0;

        destroy_connection(connection);
    }
}

static void alloc_resources(struct connection *connection, int max)
{
    connection->iocb = malloc(max * sizeof(struct iocb));
    DIE(connection->iocb == NULL, "alloc_iocb");

    connection->arr_iocb = malloc(max * sizeof(struct iocb *));
    DIE(connection->iocb == NULL, "alloc_arr_iocb");

    connection->buffer_matrix = malloc(max * sizeof(char *));
    DIE(connection->iocb == NULL, "alloc_buffer_matrix");

    for (int i = 0; i < max; i++)
    {
        connection->buffer_matrix[i] = malloc(BUFSIZ * sizeof(char));
        DIE(connection->buffer_matrix[i], "alloc_buffer_matrix[]");
    }
}

// static void send_aio_message(struct connection *connection)
// {
//     int max;
//     if (!connection->size % BUFSIZ)
//         max = connection->size / BUFSIZ;
//     else
//         max = connection->size / BUFSIZ + 1;
//     alloc_resources(connection, max);

//     for (int i = 0; i < max; i++)
//     {
//         int real_size = connection->size - connection->offset;
//         if (real_size > BUFSIZ)
//             real_size = BUFSIZ;

//         connection->arr_iocb[i] = &connection->iocb[i];
//         io_prep_pread(&connection->iocb[i], connection->fd, connection->buffer_matrix[i],
//                       real_size, connection->offset);
//         io_set_eventfd(&connection->iocb[i], connection->efd);

//         connection->offset = connection->offset + real_size;

//         if (i == max - 1)
//             connection->last = real_size;
//     }

//     // remove pointer
//     int ret_code = w_epoll_remove_ptr(epollfd, connection->sockfd, connection);
//     DIE(ret_code < 0, "w_epoll_remove_ptr");

//     // submit
//     ret_code = io_submit(connection->context, max - connection->counter_submits,
//                          connection->arr_iocb + connection->counter_submits);
//     DIE(ret_code < 0, "io_submit");
//     connection->counter_submits += ret_code;

//     w_epoll_add_ptr_inout(epollfd, connection->efd, connection);

//     connection->total = max;
//     connection->status = 2;
// }

static void send_aio_message(struct connection *conn)
{

    int n = conn->size / BUFSIZ, i;
    int nr_bytes, rc;

    if (conn->size % BUFSIZ)
        n++;

    conn->iocb = malloc(n * sizeof(struct iocb));
    conn->arr_iocb = malloc(n * sizeof(struct iocb *));
    if (!conn->iocb || !conn->arr_iocb)
    {
        perror("iocb alloc");
        return;
    }

    conn->buffer_matrix = malloc(n * sizeof(char *));

    for (i = 0; i < n; i++)
    {
        conn->buffer_matrix[i] = malloc(BUFSIZ * sizeof(char));
        conn->arr_iocb[i] = &conn->iocb[i];
        if (conn->size - conn->offset <= BUFSIZ)
            nr_bytes = conn->size - conn->offset;
        else
            nr_bytes = BUFSIZ;
        io_prep_pread(&conn->iocb[i], conn->fd, conn->buffer_matrix[i], nr_bytes, conn->offset);
        io_set_eventfd(&conn->iocb[i], conn->efd);
        conn->offset += nr_bytes;

        if (i == n - 1)
            conn->last = nr_bytes;
    }
    rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_remove_conn");

    rc = io_submit(conn->context, n - conn->counter_submits, conn->arr_iocb + conn->counter_submits);
    DIE(rc < 0, "io_submit");
    conn->counter_submits += rc;

    w_epoll_add_ptr_inout(epollfd, conn->efd, conn);

    conn->total = n;
    conn->status = 2;

    // free(iocb);
    // free(arr_iocb);
}

static int check_ret_code(struct connection *connection)
{
    enum connection_state ret_code = send_message(connection);
    if (ret_code == STATE_CONNECTION_CLOSED || ret_code == STATE_FRAGMENT_SENT)
        return 0;
    return 1;
}

static void equal_counters(struct connection *connection)
{
    int ret_code = w_epoll_remove_ptr(epollfd, connection->sockfd, connection);
    DIE(ret_code < 0, "w_epoll_remove_ptr");

    if (connection->total > connection->counter_submits)
    {
        ret_code = io_submit(connection->context, connection->total - connection->counter_submits,
                             connection->arr_iocb + connection->counter_submits);
        DIE(ret_code < 0, "io_submit");
        connection->counter_submits = connection->counter_submits + ret_code;
    }
}

static void free_resources(struct connection *connection)
{
    for (int i = 0; i < connection->counter_submits; i++)
        free(connection->buffer_matrix[i]);
    free(connection->buffer_matrix);

    destroy_connection(connection);
}

// int main(void)
// {
//     int rc;
//     enum connection_state ret_state;

//     /* init multiplexing */
//     epollfd = w_epoll_create();
//     DIE(epollfd < 0, "w_epoll_create");

//     /* create server socket */
//     listenfd = tcp_create_listener(AWS_LISTEN_PORT,
//                                    DEFAULT_LISTEN_BACKLOG);
//     DIE(listenfd < 0, "tcp_create_listener");

//     rc = w_epoll_add_fd_in(epollfd, listenfd);
//     DIE(rc < 0, "w_epoll_add_fd_in");

//     /* server main loop */
//     while (1)
//     {
//         struct epoll_event rev;

//         /* wait for events */
//         rc = w_epoll_wait_infinite(epollfd, &rev);
//         DIE(rc < 0, "w_epoll_wait_infinite");
//         struct connection *connection = rev.data.ptr;

//         /*
//          * switch event types; consider
//          *   - new connection requests (on server socket)
//          *   - socket communication (on connection sockets)
//          */

//         if (rev.data.fd == listenfd)
//         {
//             if (rev.events & EPOLLIN)
//                 handle_new_connection();
//         }
//         else if (rev.events & EPOLLIN)
//         {
//             if (connection->id == 0 || connection->id == 1)
//             {
//                 // basic handle for cases 1 and 2
//                 handle_client_request(rev.data.ptr);
//                 continue;
//             }
//             else if (connection->id == 2 && connection->status == 2)
//             {
//                 // handle_event(connection);
//                 struct io_event events[connection->counter_submits];
//                 u_int64_t efd_val;
//                 int rc = read(connection->efd, &efd_val, sizeof(efd_val));
//                 DIE(rc < 0, "read efd");
//                 rc = io_getevents(connection->context, efd_val, efd_val, events, NULL);
//                 DIE(rc != efd_val, "io_getevents");
//                 connection->counter_got += efd_val;

//                 rc = w_epoll_add_ptr_out(epollfd, connection->sockfd, connection);
//                 DIE(rc < 0, "w_epoll_add_ptr_out");
//             }
//         }
//         else if (rev.events & EPOLLOUT)
//         {
//             if (connection->id == 0)
//             {
//                 // if (!check_ret_code(connection))
//                 //     continue;
//                 // destroy_connection(connection);
//                 ret_state = send_message(connection);
//                 if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_FRAGMENT_SENT)
//                     continue;
//                 /* all done - remove out notification */
//                 int rc = w_epoll_remove_ptr(epollfd, connection->sockfd, connection);
//                 DIE(rc < 0, "w_epoll_remove_ptr");
//                 rc = io_destroy(connection->context);
//                 DIE(rc < 0, "io_destroy");
//                 close(connection->sockfd);
//                 close(connection->fd);
//                 connection_remove(connection);
//             }
//             else if (connection->id == 1)
//             {
//                 if (connection->status == 0)
//                 {
//                     if (!check_ret_code(connection))
//                         continue;
//                     connection->status = 1;
//                     connection->message_size = 0;
//                 }
//                 else if (connection->status == 1)
//                 {
//                     // send_message_event(connection);
//                     int nr_bytes;
//                     if (connection->size - connection->offset <= BUFSIZ)
//                         nr_bytes = connection->size - connection->offset;
//                     else
//                         nr_bytes = BUFSIZ;
//                     int nr = sendfile(connection->sockfd, connection->fd, &connection->offset, nr_bytes);
//                     connection->message_size += nr;
//                     DIE(nr < 0, "eroare trimitere fisier");
//                     if (nr == 0)
//                     {
//                         connection->status = 0;
//                         connection->message_size = 0;
//                         int rc = w_epoll_remove_ptr(epollfd, connection->sockfd, connection);
//                         DIE(rc < 0, "w_epoll_remove_ptr");
//                         rc = io_destroy(connection->context);
//                         DIE(rc < 0, "io_destroy");
//                         close(connection->sockfd);
//                         close(connection->fd);
//                         connection_remove(connection);
//                     }
//                 }
//             }
//             else if (connection->id == 2)
//             {
//                 if (connection->status == 0)
//                 {
//                     if (!check_ret_code(connection))
//                         continue;
//                     connection->status = 1;
//                     connection->message_size = 0;
//                 }
//                 else if (connection->status == 1)
//                 {
//                     send_aio_message(connection);
//                 }
//                 else if (connection->status == 2)
//                 {

//                     // if (connection->counter_send < connection->counter_got)
//                     // {
//                     //     memcpy(connection->send_buffer,
//                     //            connection->buffer_matrix[connection->counter_send], BUFSIZ);
//                     //     connection->message_size = BUFSIZ;
//                     //     if (connection->counter_send == connection->counter_submits - 1)
//                     //         connection->message_size = connection->last;
//                     //     if (!check_ret_code(connection))
//                     //         continue;
//                     //     connection->counter_send++;
//                     //     connection->message_size = 0;
//                     // }
//                     // if (connection->counter_send == connection->counter_got)
//                     //     equal_counters(connection);
//                     // if (connection->counter_send == connection->total)
//                     //     free_resources(connection);
//                     if (connection->counter_got > connection->counter_send)
//                     {
//                         memcpy(connection->send_buffer, connection->buffer_matrix[connection->counter_send], BUFSIZ);
//                         if (connection->counter_send == connection->counter_submits - 1)
//                             connection->send_len = connection->last;
//                         else
//                             connection->send_len = BUFSIZ;

//                         ret_state = send_message(connection);
//                         if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_FRAGMENT_SENT)
//                             continue;
//                         connection->counter_send++;
//                         connection->message_size = 0;
//                     }

//                     if (connection->counter_send == connection->counter_got)
//                     {
//                         int rc = w_epoll_remove_ptr(epollfd, connection->sockfd, connection);
//                         DIE(rc < 0, "w_epoll_remove_ptr");
//                         if (connection->counter_submits < connection->total)
//                         {
//                             int rc = io_submit(connection->context, connection->total - connection->counter_submits,
//                                                connection->arr_iocb + connection->counter_submits);
//                             DIE(rc < 0, "io_submit");
//                             connection->counter_submits += rc;
//                         }
//                     }

//                     if (connection->counter_send == connection->total)
//                     {
//                         int i;
//                         for (i = 0; i < connection->counter_submits; i++)
//                             free(connection->buffer_matrix[i]);
//                         free(connection->buffer_matrix);

//                         rc = w_epoll_remove_ptr(epollfd, connection->efd, connection);
//                         DIE(rc < 0, "w_epoll_remove_efd");
//                         rc = io_destroy(connection->context);
//                         DIE(rc < 0, "io_destroy");
//                         close(connection->sockfd);
//                         close(connection->fd);
//                         connection_remove(connection);
//                     }
//                 }
//             }
//         }
//     }

//     return 0;
// }

int main(void)
{
    int rc;
    enum connection_state ret_state;

    /* init multiplexing */
    epollfd = w_epoll_create();
    DIE(epollfd < 0, "w_epoll_create");

    /* create server socket */
    listenfd = tcp_create_listener(AWS_LISTEN_PORT,
                                   DEFAULT_LISTEN_BACKLOG);
    DIE(listenfd < 0, "tcp_create_listener");

    rc = w_epoll_add_fd_in(epollfd, listenfd);
    DIE(rc < 0, "w_epoll_add_fd_in");

    /* server main loop */
    while (1)
    {
        struct epoll_event rev;

        /* wait for events */
        rc = w_epoll_wait_infinite(epollfd, &rev);
        DIE(rc < 0, "w_epoll_wait_infinite");

        /*
         * switch event types; consider
         *   - new connection requests (on server socket)
         *   - socket communication (on connection sockets)
         */
        struct connection *conn = rev.data.ptr;
        if (rev.data.fd == listenfd)
        {
            if (rev.events & EPOLLIN)
                handle_new_connection();
        }
        else if (rev.events & EPOLLIN)
        {
            if (conn->id == 0 || conn->id == 1)
            {
                handle_client_request(rev.data.ptr);
            }
            else if (conn->id == 2 && conn->status == 2)
            {
                struct io_event events[conn->counter_submits];
                u_int64_t efd_val;
                int rc = read(conn->efd, &efd_val, sizeof(efd_val));
                DIE(rc < 0, "read efd");
                rc = io_getevents(conn->context, efd_val, efd_val, events, NULL);
                DIE(rc != efd_val, "io_getevents");
                conn->counter_got += efd_val;

                rc = w_epoll_add_ptr_out(epollfd, conn->sockfd, conn);
                DIE(rc < 0, "w_epoll_add_ptr_out");
            }
        }
        else if (rev.events & EPOLLOUT)
        {
            if (conn->id == 0)
            {
                ret_state = send_message(conn);
                if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_FRAGMENT_SENT)
                    continue;
                /* all done - remove out notification */
                int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
                DIE(rc < 0, "w_epoll_remove_ptr");
                rc = io_destroy(conn->context);
                DIE(rc < 0, "io_destroy");
                close(conn->sockfd);
                close(conn->fd);
                connection_remove(conn);
            }
            else if (conn->id == 1 && conn->status == 0)
            {
                ret_state = send_message(conn);
                if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_FRAGMENT_SENT)
                    continue;
                conn->status = 1;
                conn->message_size = 0;
            }
            else if (conn->id == 1 && conn->status == 1)
            {
                int nr_bytes;
                if (conn->size - conn->offset <= BUFSIZ)
                    nr_bytes = conn->size - conn->offset;
                else
                    nr_bytes = BUFSIZ;
                int nr = sendfile(conn->sockfd, conn->fd, &conn->offset, nr_bytes);
                conn->message_size += nr;
                DIE(nr < 0, "eroare trimitere fisier");
                if (nr == 0)
                {
                    conn->status = 0;
                    conn->message_size = 0;
                    int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
                    DIE(rc < 0, "w_epoll_remove_ptr");
                    rc = io_destroy(conn->context);
                    DIE(rc < 0, "io_destroy");
                    close(conn->sockfd);
                    close(conn->fd);
                    connection_remove(conn);
                }
            }
            else if (conn->id == 2 && conn->status == 0)
            {
                ret_state = send_message(conn);
                if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_FRAGMENT_SENT)
                    continue;
                conn->status = 1;
                conn->message_size = 0;
            }
            else if (conn->id == 2 && conn->status == 1)
            {
                send_aio_message(conn);
            }
            else if (conn->id == 2 && conn->status == 2)
            {
                if (conn->counter_got > conn->counter_send)
                {
                    memcpy(conn->send_buffer, conn->buffer_matrix[conn->counter_send], BUFSIZ);
                    if (conn->counter_send == conn->counter_submits - 1)
                        conn->send_len = conn->last;
                    else
                        conn->send_len = BUFSIZ;

                    ret_state = send_message(conn);
                    if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_FRAGMENT_SENT)
                        continue;
                    conn->counter_send++;
                    conn->message_size = 0;
                }

                if (conn->counter_send == conn->counter_got)
                {
                    int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
                    DIE(rc < 0, "w_epoll_remove_ptr");
                    if (conn->counter_submits < conn->total)
                    {
                        int rc = io_submit(conn->context, conn->total - conn->counter_submits, conn->arr_iocb + conn->counter_submits);
                        DIE(rc < 0, "io_submit");
                        conn->counter_submits += rc;
                    }
                }

                if (conn->counter_send == conn->total)
                {
                    int i;
                    for (i = 0; i < conn->counter_submits; i++)
                        free(conn->buffer_matrix[i]);
                    free(conn->buffer_matrix);

                    rc = w_epoll_remove_ptr(epollfd, conn->efd, conn);
                    DIE(rc < 0, "w_epoll_remove_efd");
                    rc = io_destroy(conn->context);
                    DIE(rc < 0, "io_destroy");
                    close(conn->sockfd);
                    close(conn->fd);
                    connection_remove(conn);
                }
            }
        }
    }

    return 0;
}
