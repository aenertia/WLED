/*
 * ble_ppp_bridge.c  -- BLE L2CAP CoC  -> PPP bridge for testing WLED BLE transport
 *
 * Usage:
 *   # Echo test (Wave 1):
 *   ./ble_ppp_bridge --echo XX:XX:XX:XX:XX:XX
 *
 *   # PPP bridge (Wave 2+):
 *   sudo ./ble_ppp_bridge XX:XX:XX:XX:XX:XX
 *   # This creates a ppp0 interface with 169.254.7.2, ESP32 at 169.254.7.1
 *
 * Build:
 *   gcc -O2 -o ble_ppp_bridge ble_ppp_bridge.c -lbluetooth
 *
 * Requires: bluez-libs-devel (RHEL/Fedora) or libbluetooth-dev (Debian/Ubuntu)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <pty.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

#define PSM     0x0080
#define BUF_SZ  1024

static volatile int running = 1;

static void sig_handler(int sig) { running = 0; }

static int ble_connect(const char *addr_str, int addr_type)
{
    int sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    /* Set BLE L2CAP CoC options */
    struct bt_security sec = { .level = BT_SECURITY_LOW };
    setsockopt(sock, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec));

    /* Set IMTU for L2CAP CoC */
    struct l2cap_options opts;
    memset(&opts, 0, sizeof(opts));
    socklen_t optlen = sizeof(opts);
    getsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, &optlen);
    opts.imtu = 512;
    opts.omtu = 512;
    setsockopt(sock, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts));

    struct sockaddr_l2 sa;
    memset(&sa, 0, sizeof(sa));
    sa.l2_family = AF_BLUETOOTH;
    sa.l2_psm = htobs(PSM);
    str2ba(addr_str, &sa.l2_bdaddr);
    sa.l2_bdaddr_type = addr_type;
    sa.l2_cid = 0;

    fprintf(stderr, "Connecting to %s PSM=0x%04x (addr_type=%d)...\n",
            addr_str, PSM, addr_type);

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }

    fprintf(stderr, "Connected!\n");
    return sock;
}

static int run_echo_test(int sock)
{
    fprintf(stderr, "\n=== BLE L2CAP CoC Echo Test ===\n\n");
    int pass = 0, fail = 0;

    for (int i = 0; i < 10; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "WLED echo test #%d", i);
        int len = strlen(msg);

        if (write(sock, msg, len) != len) {
            fprintf(stderr, "  [FAIL] write: %s\n", strerror(errno));
            fail++;
            continue;
        }

        char buf[BUF_SZ];
        int n = read(sock, buf, sizeof(buf));
        if (n < 0) {
            fprintf(stderr, "  [FAIL] read: %s\n", strerror(errno));
            fail++;
            continue;
        }

        buf[n] = 0;
        if (n == len && memcmp(buf, msg, len) == 0) {
            fprintf(stderr, "  [PASS] sent=\"%s\" recv=\"%s\"\n", msg, buf);
            pass++;
        } else {
            fprintf(stderr, "  [FAIL] sent=\"%s\" recv=\"%.*s\" (len %d vs %d)\n",
                    msg, n, buf, len, n);
            fail++;
        }
        usleep(100000); /* 100ms between tests */
    }

    fprintf(stderr, "\nResults: %d pass, %d fail\n", pass, fail);
    return fail > 0 ? 1 : 0;
}

static void bridge_loop(int ble_fd, int pty_fd)
{
    char buf[BUF_SZ];
    fprintf(stderr, "Bridging BLE <-> PTY (Ctrl+C to stop)\n");

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ble_fd, &fds);
        FD_SET(pty_fd, &fds);
        int maxfd = (ble_fd > pty_fd ? ble_fd : pty_fd) + 1;

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(maxfd, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (ret == 0) continue;

        /* BLE  -> PTY (pppd) */
        if (FD_ISSET(ble_fd, &fds)) {
            int n = read(ble_fd, buf, sizeof(buf));
            if (n <= 0) {
                fprintf(stderr, "BLE disconnected\n");
                break;
            }
            write(pty_fd, buf, n);
        }

        /* PTY (pppd)  -> BLE */
        if (FD_ISSET(pty_fd, &fds)) {
            int n = read(pty_fd, buf, sizeof(buf));
            if (n <= 0) {
                fprintf(stderr, "PTY closed\n");
                break;
            }
            write(ble_fd, buf, n);
        }
    }
}

static int run_ppp_bridge(int ble_fd)
{
    int master, slave;
    char slave_name[256];

    if (openpty(&master, &slave, slave_name, NULL, NULL) < 0) {
        perror("openpty");
        return 1;
    }

    fprintf(stderr, "PTY: %s\n", slave_name);
    fprintf(stderr, "Starting pppd on %s...\n", slave_name);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* Child: run pppd */
        close(master);
        close(ble_fd);
        execlp("pppd", "pppd", slave_name, "noauth", "nodetach",
               "local", "nocrtscts", "debug",
               "169.254.7.2:169.254.7.1",
               "mru", "512", "mtu", "512",
               "lcp-echo-interval", "10", "lcp-echo-failure", "3",
               NULL);
        perror("execlp pppd");
        _exit(1);
    }

    /* Parent: bridge BLE <-> master PTY */
    close(slave);
    bridge_loop(ble_fd, master);

    /* Cleanup */
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    close(master);
    return 0;
}

int main(int argc, char **argv)
{
    int echo_mode = 0;
    int addr_type = BDADDR_LE_PUBLIC; /* 1 */
    const char *addr = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--echo") == 0)
            echo_mode = 1;
        else if (strcmp(argv[i], "--random") == 0)
            addr_type = BDADDR_LE_RANDOM; /* 2 */
        else
            addr = argv[i];
    }

    if (!addr) {
        fprintf(stderr, "Usage: %s [--echo] [--random] XX:XX:XX:XX:XX:XX\n\n"
                "  --echo    Echo test only (Wave 1 validation)\n"
                "  --random  Use LE Random address type (try if Public fails)\n"
                "  default   PPP bridge mode (Wave 2+, needs sudo for pppd)\n",
                argv[0]);
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int sock = ble_connect(addr, addr_type);
    if (sock < 0) {
        if (addr_type == BDADDR_LE_PUBLIC) {
            fprintf(stderr, "Retrying with LE Random address type...\n");
            sock = ble_connect(addr, BDADDR_LE_RANDOM);
        }
        if (sock < 0) return 1;
    }

    int ret;
    if (echo_mode) {
        ret = run_echo_test(sock);
    } else {
        ret = run_ppp_bridge(sock);
    }

    close(sock);
    return ret;
}
