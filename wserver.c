#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>    /* Internet domain header */

#include "wrapsock.h"
#include "ws_helpers.h"

#define MAXCLIENTS 10
// Close the server when the 11th connection is made.
int handleClient(struct clientstate *cs, char *line);
int acceptClient(int sockfd, struct clientstate *cs);
int handleCGIOutput(struct clientstate *cs);
void closeClient(struct clientstate *cs, fd_set *all_fds);

// You may want to use this function for initial testing
void write_page(int fd){
    char *str =	 "Content-type: text/html\r\n\r\n"
        "<html><head><title>Hello World</title></head>\n"
        "<body>\n<h2>Hello, world!</h2>\n"
        "</body></html>\n";
    write(fd, str, strlen(str));
    return;
};

int
main(int argc, char **argv) {

    if(argc != 2) {
        fprintf(stderr, "Usage: wserver <port>\n");
        exit(1);
    }
    unsigned short port = (unsigned short)atoi(argv[1]);
    int listenfd;
    struct clientstate client[MAXCLIENTS];
    int num_read;
    int count = 0;


    // Set up the socket to which the clients will connect
    listenfd = setupServerSocket(port);

    initClients(client, MAXCLIENTS);

    // TODO: complete this function

    // 1. Setup set to listen and call select on
    fd_set all_fds;
    int max_fd = listenfd;
    FD_ZERO(&all_fds);
    FD_SET(listenfd, &all_fds);

    struct timeval five_minutes;
    five_minutes.tv_sec = 300;

    // 2. Loop
    while(1){
        fd_set selected = all_fds;
        Select(max_fd + 1, &selected, NULL, NULL, &five_minutes);

    // 3. Select all ready clients

        // a. If the listening socket is ready, we know there is a new connection being attempted. Accept this connection
        if (FD_ISSET(listenfd, &selected)){
            int client_fd = acceptClient(listenfd, client);
            if (client_fd > max_fd) {
                max_fd = client_fd;
            }
            if (client_fd == -1 || count == MAXCLIENTS){
                printf("Max connections reached\n");
                return 0;
            }

            FD_SET(client_fd, &all_fds);
            count += 1;
            printf("Accepted connection from client: %d\n", client_fd);
        }

        // b. For each clientstate, process its request
        for (int client_index = 0; client_index < MAXCLIENTS; client_index++){
            // Check if this client has something to read in
            if (client[client_index].sock > -1 && FD_ISSET(client[client_index].sock, &selected)){
                char buf[MAXLINE + 1];
                // Check if the value from this client is valid
                if ((num_read = read(client[client_index].sock, buf, MAXLINE)) <= 0){
                    perror("read");
                    printServerError(client[client_index].sock);
                    closeClient(&(client[client_index]), &all_fds);
                }
                else {
                    buf[num_read] = '\0';
                    // Prepare the client struct
                    int client_status = handleClient(&(client[client_index]), buf);
                    // If client is ready, begin processing
                    if (client_status == 1){
                        int process_result = processRequest(&client[client_index]);
                        if (process_result == -1){
                            closeClient(&(client[client_index]), &all_fds);
                        }
                        // If client request has been processed completely, indicate it is ready for CGI output
                        else {
                            client[client_index].fd[0] = process_result;
                            FD_SET(client[client_index].fd[0], &all_fds);
                            if (client[client_index].fd[0] > max_fd){
                                max_fd = client[client_index].fd[0];
                            }
                        }
                    }
                    if (client_status == -1){
                        closeClient(&(client[client_index]), &all_fds);
                    }
                }
            }
            // If client is recieving CGI output, process it
            if (client[client_index].fd[0] > -1 && FD_ISSET(client[client_index].fd[0], &selected)){
                // Read from the pipe
                // Output if ready
                int read_status = handleCGIOutput(&(client[client_index]));
                if (read_status != 1){
                    closeClient(&(client[client_index]), &all_fds);
                //     client[client_index].sock = -2;
                //     client[client_index].fd[0] = -2;
                }
            }
        }
    }
    return 0;
}

/* Update the client state cs with the request input in line.
 * Intializes cs->request if this is the first read call from the socket.
 * Note that line must be null-terminated string.
 *
 * Return 0 if the get request message is not complete and we need to wait for
 *     more data
 * Return -1 if there is an error and the socket should be closed
 *     - Request is not a GET request
 *     - The first line of the GET request is poorly formatted (getPath, getQuery)
 * 
 * Return 1 if the get request message is complete and ready for processing
 *     cs->request will hold the complete request
 *     cs->path will hold the executable path for the CGI program
 *     cs->query will hold the query string
 *     cs->output will be allocated to hold the output of the CGI program
 *     cs->optr will point to the beginning of cs->output
 */
int handleClient(struct clientstate *cs, char *line) {
    if (cs->request == NULL){
        cs->request = malloc(MAXLINE + 1);
        strncpy(cs->request, line, strlen(line));
        // cs->request[strlen(line)] = '\0';
    }
    else{
        strncat(cs->request, line, MAXLINE - strlen(cs->request));
    }
    
    if(strstr(cs->request, "\r\n\r\n") == NULL){
        printf("Incomplete message from client %d. Waiting for more\n", cs->sock);
        return 0;
    }

    char *path;
    char *query;
    if ((path = getPath(cs->request)) == NULL){
        printServerError(cs->sock);
        return -1;
    }
    if ((query = getQuery(cs->request)) == NULL){
        printServerError(cs->sock);
        return -1;
    }
    cs->path = path;
    cs->query_string = query;
    
    
    // If the resource is favicon.ico we will ignore the request
    if(strcmp("favicon.ico", cs->path) == 0){
        // A suggestion for debugging output
        fprintf(stderr, "Client: sock = %d\n", cs->sock);
        fprintf(stderr, "        path = %s (ignoring)\n", cs->path);
		printNotFound(cs->sock);
        return -1;
    }
    cs->output = malloc(MAXPAGE);
    cs->optr = cs->output;

    // A suggestion for printing some information about each client. 
    // You are welcome to modify or remove these print statements
    // fprintf(stderr, "Client: sock = %d\n", cs->sock);
    // fprintf(stderr, "        path = %s\n", cs->path);
    // fprintf(stderr, "        query_string = %s\n", cs->query_string);

    return 1;
}

/* Accept this client on the socket sockfd
* Return:
        socket fd if successfull
*       -1 if max number of clients reached
*/
int acceptClient(int sockfd, struct clientstate *cs){
    int user_index = 0;
    while (user_index < MAXCLIENTS && cs[user_index].sock != -1) {
        user_index++;
    }

    if (user_index == MAXCLIENTS) {
        fprintf(stderr, "server: max concurrent connections\n");
        return -1;
    }

    int new_client = Accept(sockfd, NULL, NULL);

    cs[user_index].sock = new_client;

    return new_client;
    }


/*
* Prepare the output of the CGI program for writing to client
* Return:
*       -1 If client has been closed
*       0 If message was successfully accumulated
*       1 if no errors but incomplete message
*/
int handleCGIOutput(struct clientstate *cs){
    int n = read(cs->fd[0], cs->optr, MAXPAGE);
    if (n <= 0){
        int status;
        wait(&status);
        if (WEXITSTATUS(status) == 100){
            printf("%d", WEXITSTATUS(status));
            printNotFound(cs->sock);
        }

        if (WIFSIGNALED(status)){
            printServerError(cs->sock);
        }
        return -1;
    }
    else{
        cs->optr += n;
    }
    // String has been fully accumulated.
    if (strstr(cs->output, "</body></html>\n") != NULL){
        printOK(cs->sock, cs->output, MAXPAGE);
        return 0;
    }
    return 1;
}

void closeClient(struct clientstate *cs, fd_set *all_fds){
    fprintf(stderr, "Client %d is done processing. Closing client\n", cs->sock);
    if (cs->sock != -1){
        Close(cs->sock);
    }
    if (cs->fd[0] != -1){
        FD_CLR(cs->sock, all_fds);
    }
    FD_CLR(cs->sock, all_fds);
    FD_CLR(cs->fd[0], all_fds);
    resetClient(cs);
    return;
}