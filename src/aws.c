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

enum connection_state
{
    FULLY_RECEIVED = 0,
    FRAGMENT_RECEIVED = 1,
    FULLY_SENT = 2,
    FRAGMENT_SENT = 3,
    CLOSED = 4
};

// Server socket file descriptor
static int listenfd;

// Epoll file descriptor
static int epollfd;

struct connection
{

    // buffers used for receiving messages and then echoing them back */
    char recv_buffer[BUFSIZ];
    char send_buffer[BUFSIZ];

    size_t recv_len;
    size_t send_len;

    struct iocb **request_ptr;
    struct iocb *request;

    int sockfd;
    int fd;
    int efd;

    size_t message_size;
    size_t total_size;

    int step_case;
    int step;

    size_t receive_counter;
    size_t send_counter;
    size_t submitted_counter;
    size_t total_counter;
    size_t final_counter;

    off_t offset;

    void **buffers_array;

    enum connection_state state;
    io_context_t context;
};

static struct connection *connection_create(int sockfd)
{
    struct connection *conn = malloc(sizeof(*conn));

    if (conn == NULL)
    {
        fprintf(stderr, "Error allocating memory for connection struct: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    memset(&conn->context, 0, sizeof(io_context_t));
    int rs = io_setup(1, &conn->context);
    if (rs < 0)
    {
        fprintf(stderr, "Error setting up IO context: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if ((conn->efd = eventfd(0, EFD_NONBLOCK)) < 0)
    {
        perror("error efd");
        exit(EXIT_FAILURE);
    }
    memset(conn->recv_buffer, 0, BUFSIZ);
    memset(conn->send_buffer, 0, BUFSIZ);
    int empty = 0;

    conn->step = empty;
    conn->offset = empty;
    conn->sockfd = sockfd;
    conn->recv_len = empty;
    conn->send_len = empty;
    conn->step_case = empty;
    conn->message_size = empty;
    conn->send_counter = empty;
    conn->final_counter = empty;
    conn->total_counter = empty;
    conn->receive_counter = empty;
    conn->submitted_counter = empty;

    return conn;
}

static void connection_remove(struct connection *conn)
{
    close(conn->sockfd);
    conn->state = CLOSED;
    free(conn);
}

static char request_path[BUFSIZ]; // storage for request_path
static http_parser request_parser;

static void handle_new_connection(void)
{
    socklen_t addrlen = sizeof(struct sockaddr_in);
    struct sockaddr_in addr;
    struct connection *conn;

    // Accept a new connection
    int sockfd = accept(listenfd, (SSA *)&addr, &addrlen);
    if (sockfd < 0)
    {
        perror("accept");
        return;
    }

    // Set the socket to be non-blocking
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0)
    {
        perror("fcntl F_GETFL");
        close(sockfd);
        return;
    }

    flags |= O_NONBLOCK;
    int rc = fcntl(sockfd, F_SETFL, flags);
    if (rc < 0)
    {
        perror("fcntl F_SETFL");
        close(sockfd);
        return;
    }

    // Create a new connection structure for the new client
    conn = connection_create(sockfd);

    // Add the socket to epoll for incoming events
    rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
    if (rc < 0)
    {
        perror("w_epoll_add_in");
        connection_remove(conn);
        return;
    }
}

static enum connection_state send_message(struct connection *conn)
{

    // Get the peer address of the connection
    char abuffer[64];
    int rc = get_peer_address(conn->sockfd, abuffer, 64);
    if (rc < 0)
    {
        ERR("get_peer_address");
        goto remove_connection;
    }

    // Send the data in the send buffer to the client
    ssize_t message_size = send(conn->sockfd, conn->send_buffer + conn->message_size,
                                conn->send_len - conn->message_size, 0);
    if (message_size < 0)
        goto remove_connection;

    // If not all the data was sent, update the number of bytes sent and return
    // FRAGMENT_SENT
    if (conn->message_size + message_size < conn->send_len)
    {
        conn->message_size += message_size;
        return FRAGMENT_SENT;
    }

    // If the connection was closed by the client, remove the connection and return
    // CLOSED
    if (message_size == 0)
        goto remove_connection;

    // Otherwise, all the data was sent, so update the state and return FULLY_SENT
    conn->state = FULLY_SENT;

    return FULLY_SENT;

remove_connection:
    // remove the socket file descriptor from the epoll instance
    rc = epoll_ctl(epollfd, EPOLL_CTL_DEL, conn->sockfd, NULL);
    if (rc < 0)
    {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }

    // close the socket file descriptor
    close(conn->sockfd);

    // remove the connection from the linked list
    connection_remove(conn);

    // remove current connection
    close(conn->fd);
    connection_remove(conn);

    return CLOSED;
}

static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
    // Extract the path from the request and store it in the request_path array
    char path[BUFSIZ];
    if (sscanf(buf, "%[^.]", path) != 1)
    {
        fprintf(stderr, "Error parsing path\n");
        return 1;
    }
    if (snprintf(request_path, BUFSIZ, "%s%s.dat", AWS_DOCUMENT_ROOT, path + 1) < 0)
    {
        fprintf(stderr, "Error creating request path\n");
        return 1;
    }

    return 0;
}
static int on_message_begin_cb()
{
    return 0;
}
int on_header_field_cb(http_parser *p, const char *buf, size_t len)
{
    return 0;
}

int on_header_value_cb(http_parser *p, const char *buf, size_t len)
{
    return 0;
}

// Use mostly null settings except for on_path callback.
http_parser_settings settings = {
    /* on_message_begin */ on_message_begin_cb,
    /* on_header_field */ on_header_field_cb,
    /* on_header_value */ on_header_value_cb,
    /* on_path */ on_path_cb,
    /* on_url */ 0,
    /* on_fragment */ 0,
    /* on_query_string */ 0,
    /* on_body */ 0,
    /* on_headers_complete */ 0,
    /* on_message_complete */ 0};

static enum connection_state receive_request(struct connection *conn)
{
    char abuffer[64];
    int rc;
    rc = get_peer_address(conn->sockfd, abuffer, 64);
    if (rc < 0)
        goto remove_connection;

    // Receive data from the socket
    ssize_t num_bytes = recv(conn->sockfd, conn->recv_buffer + conn->recv_len,
                             BUFSIZ - conn->recv_len, 0);
    if (num_bytes <= 0)
        goto remove_connection;

    conn->recv_len += num_bytes;
    conn->state = FULLY_RECEIVED;

    // Check if the request is complete
    char *end_of_request = "\r\n\r\n";
    if (strstr(conn->recv_buffer, end_of_request) == NULL)
        return FRAGMENT_RECEIVED;

    // Parse the request
    http_parser_init(&request_parser, HTTP_REQUEST);
    size_t bytes_parsed = http_parser_execute(&request_parser, &settings,
                                              conn->recv_buffer, conn->recv_len);

    if (!bytes_parsed)
        goto remove_connection;

    return FULLY_RECEIVED;

remove_connection:
    // Close local socket
    rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
    if (rc < 0)
    {
        fprintf(stderr, "Error removing socket from epoll: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    close(conn->sockfd);

    rc = io_destroy(conn->context);
    if (rc < 0)
    {
        fprintf(stderr, "Error destroying IO context: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Remove current connection
    connection_remove(conn);
    return CLOSED;
}

static void put_header(struct connection *conn)
{
    // Put the header message into the connection's send buffer
    char header[BUFSIZ];
    int header_len = snprintf(header, BUFSIZ, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n",
                              conn->total_size);
    if (header_len < 0 || header_len >= BUFSIZ)
    {
        fprintf(stderr, "Error creating header message\n");
        return;
    }
    conn->send_len = header_len;
    strncpy(conn->send_buffer, header, conn->send_len);
}

static void put_error(struct connection *conn)
{
    // Put the error message into the connection's send buffer
    const char *error_msg = "HTTP/1.1 404 Not Found\r\n\r\n";
    conn->send_len = strlen(error_msg);
    strncpy(conn->send_buffer, error_msg, conn->send_len);
}

static void handle_client_request(struct connection *conn)
{
    // Receive the request
    enum connection_state state = receive_request(conn);
    if (state == CLOSED || state == FRAGMENT_RECEIVED)
        return;

    // Add the socket to epoll for out events
    if (w_epoll_update_ptr_out(epollfd, conn->sockfd, conn) < 0)
    {
        perror("w_epoll_add_ptr_out");
        return;
    }

    /* Open the input file. */
    conn->fd = open(request_path, O_RDONLY);
    if (conn->fd == -1)
    {
        conn->step = 0;
        put_error(conn);
        return;
    }

    /* Stat the input file to obtain its total_size. */
    struct stat stat_buf;
    fstat(conn->fd, &stat_buf);
    conn->total_size = stat_buf.st_size;
    conn->offset = 0;

    // Set the dynamic prefix
    const char *dynamic_dir = "dynamic/";
    size_t prefix_len = strlen(AWS_DOCUMENT_ROOT) + strlen(dynamic_dir) + 1;
    char *dynamic_prefix = malloc(prefix_len);
    if (dynamic_prefix == NULL)
    {
        perror("malloc");
        return;
    }
    snprintf(dynamic_prefix, prefix_len, "%s%s", AWS_DOCUMENT_ROOT, dynamic_dir);

    if (strncmp(request_path, dynamic_prefix, strlen(dynamic_prefix)) == 0)
    {
        conn->step = 2;
        put_header(conn);
        conn->step_case = 0;
        return;
    }

    // Set the static prefix
    const char *static_dir = "static/";
    prefix_len = strlen(AWS_DOCUMENT_ROOT) + strlen(static_dir) + 1;
    char *static_prefix = malloc(prefix_len);
    if (static_prefix == NULL)
    {
        perror("malloc");
        return;
    }
    snprintf(static_prefix, prefix_len, "%s%s", AWS_DOCUMENT_ROOT, static_dir);

    if (strncmp(request_path, static_prefix, strlen(static_prefix)) == 0)
    {
        conn->step = 1;
        conn->step_case = 0;
        put_header(conn);
        return;
    }

    put_error(conn);
    conn->step = 0;
}

static void prepare_async_read(struct connection *conn)
{
    size_t remaining_bytes = conn->total_size;
    size_t current_offset = 0;
    size_t nr_bytes = 0;
    int i = 0;

    // Loop until all bytes in the file have been read
    while (remaining_bytes > 0)
    {
        // Allocate memory for the send buffer
        conn->buffers_array[i] = malloc(BUFSIZ * sizeof(char));

        // Set the number of bytes to read in the current iteration
        nr_bytes = remaining_bytes;
        if (nr_bytes > BUFSIZ)
            nr_bytes = BUFSIZ;

        // Set the request_ptr for the current iocb
        conn->request_ptr[i] = &conn->request[i];

        // Prepare the iocb for an async read of the file
        io_prep_pread(&conn->request[i], conn->fd, conn->buffers_array[i], nr_bytes, current_offset);

        // Set the event file descriptor for the iocb
        io_set_eventfd(&conn->request[i], conn->efd);

        // Update the current offset and remaining bytes
        current_offset += nr_bytes;
        remaining_bytes -= nr_bytes;
        i++;
    }

    // Save the number of bytes read in the last iteration
    conn->final_counter = nr_bytes;
}

void send_file_aio(struct connection *conn)
{
    // Calculate the number of buffers needed to send the file
    int num_buffers = (conn->total_size + BUFSIZ - 1) / BUFSIZ;

    // Allocate memory for the iocb, request_ptr, and buffers_array arrays
    conn->request = malloc(num_buffers * sizeof(struct iocb));
    if (!conn->request)
    {
        perror("iocb");
        return;
    }

    conn->request_ptr = malloc(num_buffers * sizeof(struct iocb *));
    if (!conn->request_ptr)
    {
        perror("request_ptr");
        return;
    }

    conn->buffers_array = malloc(num_buffers * sizeof(char *));
    if (!conn->buffers_array)
    {
        perror("buffers_array");
        return;
    }

    // Set up the async read
    prepare_async_read(conn);

    // Remove the connection from the epoll file descriptor
    int rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
    if (rc < 0)
    {
        perror("w_epoll_remove_conn");
        return;
    }

    // Submit the iocbs for the async read
    rc = io_submit(conn->context, num_buffers - conn->submitted_counter, conn->request_ptr + conn->submitted_counter);
    if (rc < 0)
    {
        perror("io_submit");
        return;
    }
    conn->submitted_counter += rc;

    // Add the connection to the epoll file descriptor for inout events
    w_epoll_add_ptr_inout(epollfd, conn->efd, conn);

    // Update the connection's state and number of total buffers
    conn->total_counter = num_buffers;
    conn->step_case = 2;
}

static void process_events(struct connection *conn)
{
    u_int64_t efd_val;
    struct io_event events[conn->submitted_counter];

    // Read the event file descriptor to get the number of completed I/O operations
    int result = read(conn->efd, &efd_val, sizeof(efd_val));
    if (result == -1)
    {
        perror("read efd");
        return;
    }

    // Get the completed I/O events
    result = io_getevents(conn->context, efd_val, efd_val, events, NULL);
    if (result != efd_val)
    {
        perror("io_getevents");
        return;
    }

    // Update the number of completed I/O operations
    conn->receive_counter += efd_val;

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
    if (ret_code != CLOSED && ret_code != FRAGMENT_SENT)
        return 1;
    return 0;
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
    int ret_code = io_destroy(conn->context);
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
    for (int i = 0; i < (*conn)->submitted_counter; i++)
    {
        // Free the memory for each send buffer
        free((*conn)->buffers_array[i]);
    }

    // Free the memory for the array of send buffers
    free((*conn)->buffers_array);
}

static void send_data(struct connection *conn)
{
    // Determine the number of bytes to send
    int nr_bytes;

    nr_bytes = conn->total_size - conn->offset;
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
    conn->message_size += result;

    // Check if the send was successful or not
    if (result == 0)
    {
        // The connection was closed by the other end
        conn->state = FULLY_RECEIVED;
        conn->message_size = 0;
        destroy_connection(conn, 0);
    }
    else if (conn->message_size == conn->total_size)
    {
        // The entire file was sent successfully
        conn->state = FULLY_RECEIVED;
        conn->message_size = 0;
    }
}

static void copy_buffers(struct connection *conn)
{
    // Copy the send buffer for the current I/O operation
    memmove(conn->send_buffer, conn->buffers_array[conn->send_counter], BUFSIZ);

    // Determine the number of bytes to send
    conn->send_len = (conn->send_counter == conn->submitted_counter - 1) ? conn->final_counter : BUFSIZ;
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
    int num_submitted = io_submit(conn->context, conn->total_counter - conn->submitted_counter, conn->request_ptr + conn->submitted_counter);
    if (num_submitted == -1)
    {
        perror("io_submit");
        return;
    }

    // Update the number of submitted I/O operations
    conn->submitted_counter += num_submitted;
}

static void epollin_switch(struct connection *connection)
{
    switch (connection->step)
    {
    case 0:
        handle_client_request(connection);
        break;
    case 1:
        handle_client_request(connection);
        break;
    case 2:
        if (connection->step_case == connection->step)
            process_events(connection);
        break;

    default:
        break;
    }
}

static int decide_inout(struct connection *connection)
{
    // still have to manipulate recieved buffers
    if (connection->receive_counter > connection->send_counter)
    {
        // Copy the remaining buffers
        copy_buffers(connection);

        // Check the return code
        if (!check_ret_code(connection))
            return 0;

        // Increment the number of sent buffers and reset the bytes sent counter
        connection->send_counter++;
        connection->message_size = 0;
    }

    if (connection->send_counter == connection->receive_counter)
    {
        // all submitted segments were sent, do one more custom submit
        custom_submit(connection);
    }

    if (connection->send_counter == connection->total_counter)
    {
        // total number of buffers were sent, clean up resources and close the connection
        free_resources(&connection);
        destroy_connection(connection, 1);
    }

    return 1;
}

static int send_switch(struct connection *connection, int type)
{
    switch (connection->step_case)
    {
    case 0:
        if (!check_ret_code(connection))
            return 0;
        connection->step_case = 1;
        connection->message_size = 0;
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
    switch (connection->step)
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

    // Init multiplexing
    epollfd = w_epoll_create();
    if (epollfd < 0)
    {
        fprintf(stderr, "Error creating epoll descriptor: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Create server socket
    listenfd = tcp_create_listener(AWS_LISTEN_PORT,
                                   DEFAULT_LISTEN_BACKLOG);
    if (listenfd < 0)
    {
        fprintf(stderr, "Error creating listen descriptor: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    rc = w_epoll_add_fd_in(epollfd, listenfd);
    if (rc < 0)
    {
        fprintf(stderr, "Error adding fd in: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Main loop
    while (1)
    {
        struct epoll_event rev;

        // Wait for events
        rc = w_epoll_wait_infinite(epollfd, &rev);
        if (rc < 0)
        {
            fprintf(stderr, "Error waiting infinite: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

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