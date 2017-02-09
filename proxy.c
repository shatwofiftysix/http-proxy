// proxy.c
//
// Compile and use:
//    clear && gcc -Wall -g proxy.c -o proxy && ./proxy
//    export http_proxy=localhost:8888
//    wget website.com --no-http-keep-alive
//
//    "--no-http-keep-alive" is required for now otherwise connection doesn't properly terminate
//
//
// Errors and Debug:
//    [server] - operations for client making the proxy request
//    [server www] - operations making request on behalf of client and fetching data

#include <stdio.h> /* printf, sprintf */
#include <stdlib.h> /* exit */
#include <unistd.h> /* read, write, close */
#include <string.h> /* memcpy, memset */
#include <sys/socket.h> /* socket, connect */
#include <netinet/in.h> /* struct sockaddr_in, struct sockaddr */
#include <netdb.h> /* struct hostent, gethostbyname */
#include <arpa/inet.h> /* inet_ntop */
#include <signal.h> /* sigaction */
#include <errno.h>
#include <sys/wait.h>

#define BACKLOG 10
#define PORT "8888"
#define HTTP_PORT "80"
#define MAXBUFLEN 16384

// get sockaddr, IPv4 or IPv6:
void *getInAddr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in*)sa)->sin_addr);
  }

  return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

void sigChildHandler() {
  // waitpid() might overwrite errno, so we save and restore it:
  int saved_errno = errno;
  while(waitpid(-1, NULL, WNOHANG) > 0);
  errno = saved_errno;
}

int main(int argc, char **argv) {
  /* setup socket and start listening */
  int sockfd, clientfd;
  struct addrinfo hints, *servinfo, *p;
  struct sockaddr_storage client;
  socklen_t sinSize;
  struct sigaction sa;
  int sockYes = 1;
  int rv;
  char addrStr[INET6_ADDRSTRLEN];
  char buf[MAXBUFLEN];

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  // server should just use sockaddr_in with INADDR_ANY instead of using getaddrinfo()
  // much shorter, easier, and more robust
  if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) < 0) {
    perror("[server] getaddrinfo");
    return -1;
  }

  for (p = servinfo; p != NULL; p = p->ai_next) {
    if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) {
      perror("[server] socket");
      continue;
    }

    // let >1 process use socket
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &sockYes, sizeof(int)) < 0) {
        perror("[server] setsockopt");
        return -1;
    }

    if (bind(sockfd, p->ai_addr, p->ai_addrlen) < 0) {
        close(sockfd);
        perror("[server] bind");
        continue;
    }
    break;
  }

  freeaddrinfo(servinfo);

  if (p == NULL) {
    fprintf (stderr, "[server] failed to bind\n");
    return -1;
  }

  if (listen(sockfd, BACKLOG) < 0) {
    perror ("server listen");
    return -1;
  }

  // reap all dead processes
  sa.sa_handler = sigChildHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if (sigaction(SIGCHLD, &sa, NULL) < 0) {
      perror("[server] sigaction");
      return -1;
  }

  printf("[server] waiting for connections...\n");

  /* wait for connections */
  while(1) {
    sinSize = sizeof(client);
    clientfd = accept(sockfd, (struct sockaddr *)&client, &sinSize);
    if (clientfd < 0) {
      perror("[server] accept");
      continue;
    }
    inet_ntop(client.ss_family, getInAddr((struct sockaddr *)&client), addrStr, sizeof addrStr);
    printf("[server] got connection from %s\n", addrStr);

    /* fork, open our own request, return it to client */
    if (!fork()) {
      int recvdBytes;
      int sentBytes;
      int currBytes;
      int totalBytes;
      char *reqTkn[10];
      char *lineTkn[10];
      char *strTkn;
      char *strTknBuf;
      int i;

      close(sockfd); // old sock no longer needed

      // get client request
      if ((recvdBytes = recv(clientfd, buf, MAXBUFLEN - 1, 0)) < 0) {
        perror("[server] recv");
        return -1;
      }

      buf[recvdBytes] = '\0';
      printf("[server] received request: \n%s\n", buf);

      // read the http request and look for server
      // is there a more efficient way of using strtok?
      strTknBuf = malloc(recvdBytes + 1);
      strcpy(strTknBuf, buf);
      reqTkn[0] = strtok(strTknBuf, "\t\n");
      strTkn = strtok(reqTkn[0], " /");
      i = 0;
      lineTkn[i] = strTkn;
      while (strTkn != NULL) {
        strTkn = strtok(NULL, " /");
        lineTkn[i] = strTkn;
        i++;
      }
      // free(strTknBuf); // can't free because other Tkn arrays point to it
      printf("[server www] %s\n", lineTkn[1]);

      // fill up struct, open new socket
      // is it bad practice to reuse hints and sockfd in child?
      memset(&hints, 0, sizeof hints); // reinitialize to get rid of AI_PASSIVE, not sure if it matters
      hints.ai_family = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      if ((rv = getaddrinfo(lineTkn[1], HTTP_PORT, &hints, &servinfo)) < 0) {
        perror("[server www] getaddrinfo");
        return -1;
      }

      for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0) {
          perror("[server www] socket");
          continue;
        }
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) < 0) {
          close(sockfd);
          perror("[server www] connect");
          continue;
        }
        /* start debug */
        char ipstr[INET6_ADDRSTRLEN];
        void *addr;
        struct sockaddr_in *serv = (struct sockaddr_in *)p -> ai_addr;
        addr = &serv -> sin_addr;
        inet_ntop(p -> ai_family, addr, ipstr, sizeof ipstr);
        printf("[server www] ip: %s\n", ipstr);
        printf("[server www] port: %d\n", ntohs(serv -> sin_port));
        /* end debug */
        break;
      }

      if (p == NULL) {
        perror("[server www] failed to connect");
        return -1;
      }

      // make request
      printf("[server] making www request\n");
      totalBytes = strlen(buf);
      sentBytes = 0;
      do {
        currBytes = write(sockfd, buf + sentBytes, strlen(buf) - sentBytes);
        if (currBytes < 0) 
          perror("[server www] write socket");
        if (currBytes == 0)
          break;
        sentBytes += currBytes;
      } while (sentBytes < totalBytes);

      // get response
      printf("[server] getting www response\n");
      memset(buf, 0, sizeof buf);
      totalBytes = sizeof(buf) - 1;
      recvdBytes = 0;
      do {
        currBytes = read(sockfd, buf + recvdBytes, totalBytes - recvdBytes);
        // printf("recvd bytes: %d\n", currBytes);
        if (currBytes < 0)
          perror("[server www] read socket");
        if (currBytes == 0)
          break;
        recvdBytes += currBytes;
      } while(recvdBytes < totalBytes);

      if (recvdBytes == totalBytes)
        printf("[server www] error getting complete responses\n");

      close(sockfd);

      printf("[server] www response: \n%s\n", buf);

      // send shit back
      if (send(clientfd, buf, sizeof buf, 0) < 0) {
        perror("[server] send");
        return -1;
      }
      close(clientfd);
      return 0;
    }
    close(clientfd); // parent has no use for clientfd
  }
  return 0;
}
