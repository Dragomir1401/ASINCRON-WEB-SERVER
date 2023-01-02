#include "aws.h"

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
#include <fcntl.h>
#include <sys/sendfile.h>
#include <libaio.h>
#include <sys/eventfd.h>
#include "./../headers/util.h"
#include "./../headers/debug.h"
#include "./../headers/sock_util.h"
#include "./../headers/w_epoll.h"
#include "./http-parser/http_parser.h"
#include "aws.h"

#include <errno.h>

#define NR_EVENTS 1
/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

enum connection_state
{
    STATE_DATA_RECEIVED,
    STATE_DATA_PARTIAL_RECEIVED,
    STATE_DATA_SENT,
    STATE_DATA_PARTIAL_SENT,
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
    size_t bytes_sent;
    int fd;
    size_t size;
    int stare;
    int caz;
    off_t offset;

    size_t nr_got;
    size_t nr_trimise;
    size_t nr_sub;

    size_t nr_total;

    size_t last_bytes;
    void **send_buffers;
    enum connection_state state;

    io_context_t ctx;
    int efd;

    struct iocb **piocb;
    struct iocb *iocb;
};

/*
 * Initialize connection structure on given socket.
 */

static struct connection *connection_create(int sockfd)
{
    struct connection *conn = malloc(sizeof(*conn));

    DIE(conn == NULL, "malloc");

    memset(&conn->ctx, 0, sizeof(io_context_t));
    int rs = io_setup(NR_EVENTS, &conn->ctx);
    DIE(rs < 0, strerror(errno));

    conn->sockfd = sockfd;
    conn->stare = 0;
    conn->caz = 0;
    conn->offset = 0;
    conn->recv_len = 0;
    conn->send_len = 0;
    conn->bytes_sent = 0;
    conn->nr_got = 0;
    conn->nr_sub = 0;
    conn->nr_total = 0;
    conn->nr_trimise = 0;
    conn->last_bytes = 0;

    conn->efd = eventfd(0, EFD_NONBLOCK);
    DIE(conn->efd < 0, "error efd");
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

static enum connection_state send_message(struct connection *conn)
{
    ssize_t bytes_sent;
    int rc;
    char abuffer[64];

    rc = get_peer_address(conn->sockfd, abuffer, 64);
    if (rc < 0)
    {
        ERR("get_peer_address");
        goto remove_connection;
    }

    bytes_sent = send(conn->sockfd, conn->send_buffer + conn->bytes_sent, conn->send_len - conn->bytes_sent, 0);
    if (bytes_sent < 0)
        goto remove_connection;

    conn->bytes_sent += bytes_sent;
    if (conn->bytes_sent < conn->send_len)
        return STATE_DATA_PARTIAL_SENT;

    if (bytes_sent == 0)
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

static http_parser request_parser;
static char request_path[BUFSIZ]; /* storage for request_path */
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

static enum connection_state receive_request(struct connection *conn)
{
    ssize_t bytes_recv;
    char abuffer[64];
    int rc;

    rc = get_peer_address(conn->sockfd, abuffer, 64);
    if (rc < 0)
    {
        ERR("get_peer_address");
        goto remove_connection;
    }

    bytes_recv = recv(conn->sockfd, conn->recv_buffer + conn->recv_len, BUFSIZ - conn->recv_len, 0);
    if (bytes_recv < 0)
    { /* error in communication */
        goto remove_connection;
    }
    if (bytes_recv == 0)
    { /* connection closed */
        goto remove_connection;
    }

    conn->recv_len += bytes_recv;
    conn->state = STATE_DATA_RECEIVED;

    conn->recv_buffer[conn->recv_len] = 0;
    if (strcmp(conn->recv_buffer + conn->recv_len - 4, "\r\n\r\n") != 0)
        return STATE_DATA_PARTIAL_RECEIVED;

    size_t bytes_parsed;

    /* init HTTP_REQUEST parser */
    http_parser_init(&request_parser, HTTP_REQUEST);

    bytes_parsed = http_parser_execute(&request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);
    if (bytes_parsed == 0)
        goto remove_connection;
    return STATE_DATA_RECEIVED;

remove_connection:
    /* close local socket */
    rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_remove_ptr");
    close(conn->sockfd);
    rc = io_destroy(conn->ctx);
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
                    "Content-Length: %ld\r\n"
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

/*
 * Handle a client request on a client connection.
 */

static void handle_client_request(struct connection *conn)
{
    int rc;
    enum connection_state ret_state;

    // connection_copy_buffers(conn);
    ret_state = receive_request(conn);
    // if(conn->stopped) return;

    if (ret_state == STATE_CONNECTION_CLOSED || ret_state == STATE_DATA_PARTIAL_RECEIVED)
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
        conn->caz = 0;
        put_error(conn);
        return;
    }
    /* Stat the input file to obtain its size. */
    fstat(conn->fd, &stat_buf);
    conn->size = stat_buf.st_size;
    conn->offset = 0;

    if (strncmp(request_path, static_prefix, strlen(static_prefix)) == 0)
    {
        conn->caz = 1;
        conn->stare = 0;
        put_header(conn);
    }
    else if (strncmp(request_path, dynamic_prefix, strlen(dynamic_prefix)) == 0)
    {
        conn->caz = 2;
        put_header(conn);
        conn->stare = 0;
    }
    else
    {
        put_error(conn);
        conn->caz = 0;
        return;
    }
}

static void send_file_aio(struct connection *conn)
{

    int n = conn->size / BUFSIZ, i;
    int nr_bytes, rc;

    if (conn->size % BUFSIZ)
        n++;

    conn->iocb = malloc(n * sizeof(struct iocb));
    conn->piocb = malloc(n * sizeof(struct iocb *));
    if (!conn->iocb || !conn->piocb)
    {
        perror("iocb alloc");
        return;
    }

    conn->send_buffers = malloc(n * sizeof(char *));

    for (i = 0; i < n; i++)
    {
        conn->send_buffers[i] = malloc(BUFSIZ * sizeof(char));
        conn->piocb[i] = &conn->iocb[i];
        if (conn->size - conn->offset <= BUFSIZ)
            nr_bytes = conn->size - conn->offset;
        else
            nr_bytes = BUFSIZ;
        io_prep_pread(&conn->iocb[i], conn->fd, conn->send_buffers[i], nr_bytes, conn->offset);
        io_set_eventfd(&conn->iocb[i], conn->efd);
        conn->offset += nr_bytes;

        if (i == n - 1)
            conn->last_bytes = nr_bytes;
    }
    rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
    DIE(rc < 0, "w_epoll_remove_conn");

    rc = io_submit(conn->ctx, n - conn->nr_sub, conn->piocb + conn->nr_sub);
    DIE(rc < 0, "io_submit");
    conn->nr_sub += rc;

    w_epoll_add_ptr_inout(epollfd, conn->efd, conn);

    conn->nr_total = n;
    conn->stare = 2;
}

static void process_events(struct connection *conn)
{
    u_int64_t efd_val;
    struct io_event events[conn->nr_sub];

    // Read the event file descriptor to get the number of completed I/O operations
    int result = read(conn->efd, &efd_val, sizeof(efd_val));
    if (result == -1)
    {
        perror("read efd");
        return;
    }

    // Get the completed I/O events
    result = io_getevents(conn->ctx, efd_val, efd_val, events, NULL);
    if (result != efd_val)
    {
        perror("io_getevents");
        return;
    }

    // Update the number of completed I/O operations
    conn->nr_got += efd_val;

    // Add the connection's socket to the epoll event loop for writing
    result = epoll_ctl(epollfd, EPOLL_CTL_ADD, conn->sockfd, &(struct epoll_event){
                                                                 .events = EPOLLOUT,
                                                                 .data = {.ptr = conn},
                                                             });
    if (result == -1)
    {
        perror("epoll_ctl");
        return;
    }
}

static int check_ret_code(struct connection *connection)
{
    enum connection_state ret_code = send_message(connection);
    if (ret_code == STATE_CONNECTION_CLOSED || ret_code == STATE_DATA_PARTIAL_SENT)
        return 0;
    return 1;
}

static void destroy_connection(struct connection *conn, int type)
{
    // Remove the connection from the epoll event loop
    if (type == 0)
    {
        int result = epoll_ctl(epollfd, EPOLL_CTL_DEL, conn->sockfd, NULL);
        if (result == -1)
        {
            perror("epoll_ctl");
            return;
        }
    }
    else if (type == 1)
    {
        int result = epoll_ctl(epollfd, EPOLL_CTL_DEL, conn->efd, NULL);
        if (result == -1)
        {
            perror("epoll_ctl");
            return;
        }
    }

    // Destroy the connection's I/O context
    int ret_code = io_destroy(conn->ctx);
    if (ret_code == -1)
    {
        perror("io_destroy");
        return;
    }

    // Close the connection's socket and file descriptor
    close(conn->sockfd);
    close(conn->fd);

    // Remove the connection from the list of connections
    connection_remove(conn);
}

static void free_resources(struct connection **conn)
{
    for (int i = 0; i < (*conn)->nr_sub; i++)
    {
        // Free the memory for each send buffer
        free((*conn)->send_buffers[i]);
    }

    // Free the memory for the array of send buffers
    free((*conn)->send_buffers);
}

static void send_data(struct connection *conn)
{
    // Determine the number of bytes to send
    int nr_bytes;

    nr_bytes = conn->size - conn->offset;
    if (nr_bytes > BUFSIZ)
        nr_bytes = BUFSIZ;

    // Send the data
    int result = sendfile(conn->sockfd, conn->fd, &conn->offset, nr_bytes);
    if (result == -1)
    {
        perror("sendfile");
        return;
    }

    // Update the number of bytes sent
    conn->bytes_sent += result;

    // Check if the send was successful or not
    if (result == 0)
    {
        // The connection was closed by the other end
        conn->state = STATE_DATA_RECEIVED;
        conn->bytes_sent = 0;
        destroy_connection(conn, 0);
    }
    else if (conn->bytes_sent == conn->size)
    {
        // The entire file was sent successfully
        conn->state = STATE_DATA_RECEIVED;
        conn->bytes_sent = 0;
    }
}

static void copy_buffers(struct connection *conn)
{
    // Copy the send buffer for the current I/O operation
    memmove(conn->send_buffer, conn->send_buffers[conn->nr_trimise], BUFSIZ);

    // Determine the number of bytes to send
    conn->send_len = (conn->nr_trimise == conn->nr_sub - 1) ? conn->last_bytes : BUFSIZ;
}

static void custom_submit(struct connection *conn)
{
    // Remove the connection from the epoll event loop
    int result = epoll_ctl(epollfd, EPOLL_CTL_DEL, conn->sockfd, NULL);
    if (result == -1)
    {
        perror("epoll_ctl");
        return;
    }

    // Submit any remaining I/O operations for the connection
    int num_submitted = io_submit(conn->ctx, conn->nr_total - conn->nr_sub, conn->piocb + conn->nr_sub);
    if (num_submitted == -1)
    {
        perror("io_submit");
        return;
    }

    // Update the number of submitted I/O operations
    conn->nr_sub += num_submitted;
}

static void epollin_switch(struct connection *connection)
{
    switch (connection->caz)
    {
    case 0:
        handle_client_request(connection);
        break;
    case 1:
        handle_client_request(connection);
        break;
    case 2:
        if (connection->stare == connection->caz)
            process_events(connection);
        break;

    default:
        break;
    }
}

static int decide_inout(struct connection *connection)
{
    // still have to manipulate recieved buffers
    if (connection->nr_got > connection->nr_trimise)
    {
        // Copy the remaining buffers
        copy_buffers(connection);

        // Check the return code
        if (!check_ret_code(connection))
            return 0;

        // Increment the number of sent buffers and reset the bytes sent counter
        connection->nr_trimise++;
        connection->bytes_sent = 0;
    }

    if (connection->nr_trimise == connection->nr_got)
    {
        // all submitted segments were sent, do one more custom submit
        custom_submit(connection);
    }

    if (connection->nr_trimise == connection->nr_total)
    {
        // total number of buffers were sent, clean up resources and close the connection
        free_resources(&connection);
        destroy_connection(connection, 1);
    }

    return 1;
}

static int send_switch(struct connection *connection, int type)
{
    switch (connection->stare)
    {
    case 0:
        if (!check_ret_code(connection))
            return 0;
        connection->stare = 1;
        connection->bytes_sent = 0;
        break;
    case 1:
        if (type == 0)
            send_data(connection);
        else if (type == 1)
            send_file_aio(connection);
        break;
    case 2:
        if (type == 1)
            if (!decide_inout(connection))
                return 0;
        break;
    default:
        break;
    }
    return 1;
}

static int epollout_switch(struct connection *connection)
{
    switch (connection->caz)
    {
    case 0:
        if (!check_ret_code(connection))
            return 0;
        destroy_connection(connection, 0);

        break;
    case 1:
        if (!send_switch(connection, 0))
            return 0;
        break;
    case 2:
        if (!send_switch(connection, 1))
            return 0;
        break;

    default:
        break;
    }
    return 1;
}

int main(void)
{
    int rc;

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
            epollin_switch(conn);
        }
        else if (rev.events & EPOLLOUT)
        {
            if (!epollout_switch(conn))
                continue;
        }
    }

    return 0;
}
