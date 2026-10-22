/* Exercise PID 1's real control receiver without running the PID 1 bootstrap. */
#define main appliance_pid1_main
#include "manager.c"
#undef main
#include <assert.h>

static int connect_client(const struct sockaddr_un *address)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(fd >= 0 && !connect(fd, (const struct sockaddr *)address, sizeof(*address)));
    struct timeval timeout = {.tv_sec=2};
    assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    return fd;
}

int main(void)
{
    assert(geteuid() == 0); /* Same root peer credentials as the appliance CLI. */
    char directory[] = "/tmp/fractal-control-test-XXXXXX";
    assert(mkdtemp(directory));
    struct sockaddr_un address = {.sun_family=AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "%s/socket", directory);
    int server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    assert(server >= 0 && !bind(server, (struct sockaddr *)&address, sizeof(address)) && !listen(server, 4));
    struct control_request request = {.version=FCR_PROTOCOL, .op=CT_LIST};
    struct control_reply reply;

    int client = connect_client(&address);
    long long start = monotonic_ms();
    control(server); /* Accept before the client has sent anything. */
    assert(monotonic_ms() - start < 100);
    usleep(100000); /* Longer than the old receiver's ten-millisecond deadline. */
    assert(send(client, &request, sizeof(request), 0) == sizeof(request));
    control(server);
    assert(recv(client, &reply, sizeof(reply), 0) == sizeof(reply) && reply.result == 0);
    close(client);
    puts("PASS delayed control packet preserves the connection without blocking service ticks");

    client = connect_client(&address);
    control(server);
    usleep(1100000);
    control(server);
    assert(recv(client, &reply, sizeof(reply), 0) == sizeof(reply) && reply.result == -1);
    close(client);
    client = connect_client(&address);
    assert(send(client, &request, sizeof(request), 0) == sizeof(request));
    control(server);
    assert(recv(client, &reply, sizeof(reply), 0) == sizeof(reply) && reply.result == 0);
    close(client);
    puts("PASS idle control client expires and the next request succeeds");

    client = connect_client(&address);
    request.op = CT_RELOAD;
    assert(send(client, &request, sizeof(request)-1, 0) == sizeof(request)-1);
    control(server);
    assert(recv(client, &reply, sizeof(reply), 0) == sizeof(reply) && reply.result == -1);
    assert(!shutdown_requested);
    close(client);
    puts("PASS malformed control packet cannot trigger reload");
    close(server); unlink(address.sun_path); rmdir(directory);
    return 0;
}
