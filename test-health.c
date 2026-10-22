/* Test the actual nonblocking Docker health probe against a local fake API. */
#define main appliance_pid1_main
#include "manager.c"
#undef main
#include <assert.h>

static int accept_request(int server)
{
    int peer = accept4(server, NULL, NULL, SOCK_CLOEXEC);
    assert(peer >= 0);
    char request[128] = {0};
    assert(recv(peer, request, sizeof(request)-1, 0) > 0);
    assert(strstr(request, "GET /_ping HTTP/1.0"));
    return peer;
}

int main(void)
{
    char directory[] = "/tmp/fractal-health-test-XXXXXX";
    assert(mkdtemp(directory));
    struct sockaddr_un address = {.sun_family=AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s/socket", directory);
    docker_socket = address.sun_path;
    struct service engine = {.pid=123}; container_engine = &engine;
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(server >= 0 && !bind(server, (struct sockaddr *)&address, sizeof(address)) && !listen(server, 4));
    long long start = monotonic_ms();
    assert(docker_ping() == -1 && monotonic_ms() - start < 100);
    int peer = accept_request(server);
    usleep(300000); /* Longer than the former blocking 200 ms timeout. */
    assert(send(peer, "HTTP/1.1 ", 9, MSG_NOSIGNAL) == 9);
    assert(docker_ping() == -1);
    assert(send(peer, "200 OK\r\n", 8, MSG_NOSIGNAL) == 8);
    assert(docker_ping() == 1); close(peer);
    puts("PASS delayed and fragmented Docker replies are healthy without blocking PID 1");

    assert(docker_ping() == -1); peer = accept_request(server);
    docker_health.deadline = monotonic_ms()-1;
    start = monotonic_ms();
    assert(docker_ping() == 0 && monotonic_ms()-start < 100);
    close(peer);
    puts("PASS an unresponsive Docker API fails at its bounded deadline");

    assert(docker_ping() == -1); peer = accept_request(server);
    assert(send(peer, "HTTP/1.1 200 OK\r\n", 17, MSG_NOSIGNAL) == 17);
    engine.pid++;
    assert(docker_ping() == -1); /* Must discard the previous daemon's reply. */
    close(peer); peer = accept_request(server);
    assert(send(peer, "HTTP/1.1 500 Error\r\n", 20, MSG_NOSIGNAL) == 20);
    assert(docker_ping() == 0); close(peer);
    puts("PASS daemon restart discards stale readiness and HTTP errors fail the probe");
    close(server); unlink(address.sun_path); rmdir(directory);
    return 0;
}
