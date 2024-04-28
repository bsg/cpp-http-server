#include <cstdio>
#include <iostream>

#include "http_server.hpp"

static int counter = 0;

void connect_handler(Client *client) {
    printf("[%d]: Connected\n", client->fd);
}

void disconnect_handler(Client *client) {
    printf("[%d]: Disconnected\n", client->fd);
}

void request_handler(Client *client, HttpRequest *request) {
    printf("[%d]: %s %s\n", client->fd, request->method_str.c_str(), request->path.c_str());
    // if (request->content_length > 0) {
    //     write(STDOUT_FILENO, request->content, request->content_length);
    //     printf("\n\n");
    // }

    HttpResponseBuilder builder;
    if (request->method == HttpMethod::GET && request->path.compare("/") == 0) {
        std::string body = std::to_string(counter++);

        auto response = builder.status(200)->body(body)->build();
        client->send(&response);
    } else {
        auto response = builder.status(404)->build();
        client->send(&response);
    }
}

int main(int argc, char const *argv[]) {
    auto const host = "127.0.0.1";
    auto const port = "8080";
    printf("listening on %s:%s\n", host, port);

    HttpServer httpServer(host, port, request_handler);
    httpServer.set_connect_handler(connect_handler);
    httpServer.set_disconnect_handler(disconnect_handler);
    int status = httpServer.run();
    if (status < 0) {
        perror("failed to start the server");
    }

    return 0;
}
