/*
 * wled-serial-bridge: SLIP tunnel between /dev/ttyUSBx and a TUN interface.
 *
 * Replaces pppd for the M5StickC FTDI serial link.
 * No negotiation protocol  -- just SLIP framing (RFC 1055) over UART.
 *
 * Usage: sudo ./wled-serial-bridge /dev/ttyUSB0 1500000
 *
 * Creates tun0 with 169.254.7.2, peer 169.254.7.1.
 * The ESP32 side uses matching SLIP code in wled_slip.cpp.
 *
 * Build: gcc -O2 -o wled-serial-bridge wled-serial-bridge.c
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <arpa/inet.h>

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

#define MTU          1500
#define SERIAL_BUF   4096

static volatile int running = 1;

static void sighandler(int sig) { (void)sig; running = 0; }

static int tun_open(const char *dev_name) {
    struct ifreq ifr = {0};
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) { perror("open /dev/net/tun"); return -1; }
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, dev_name, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) { perror("TUNSETIFF"); close(fd); return -1; }
    return fd;
}

static int serial_open(const char *path, int baud) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { perror(path); return -1; }

    /* clear O_NONBLOCK after open (prevents DTR assertion on some FTDI) */
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    /* don't toggle DTR/RTS */
    int status;
    ioctl(fd, TIOCMGET, &status);
    status &= ~(TIOCM_DTR | TIOCM_RTS);
    ioctl(fd, TIOCMSET, &status);

    struct termios tty = {0};
    tcgetattr(fd, &tty);
    cfmakeraw(&tty);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~(CRTSCTS | HUPCL);
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    speed_t speed;
    switch (baud) {
        case 115200:  speed = B115200;  break;
        case 230400:  speed = B230400;  break;
        case 460800:  speed = B460800;  break;
        case 500000:  speed = B500000;  break;
        case 576000:  speed = B576000;  break;
        case 921600:  speed = B921600;  break;
        case 1000000: speed = B1000000; break;
        case 1500000: speed = B1500000; break;
        case 2000000: speed = B2000000; break;
        default:      speed = B1500000; break;
    }
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    tcsetattr(fd, TCSANOW, &tty);
    tcflush(fd, TCIOFLUSH);

    return fd;
}

static int slip_encode(const unsigned char *in, int inlen, unsigned char *out) {
    int o = 0;
    out[o++] = SLIP_END;
    for (int i = 0; i < inlen; i++) {
        switch (in[i]) {
            case SLIP_END: out[o++] = SLIP_ESC; out[o++] = SLIP_ESC_END; break;
            case SLIP_ESC: out[o++] = SLIP_ESC; out[o++] = SLIP_ESC_ESC; break;
            default:       out[o++] = in[i]; break;
        }
    }
    out[o++] = SLIP_END;
    return o;
}

static int slip_decode(const unsigned char *in, int inlen,
                       unsigned char *pkt, int *pktlen,
                       unsigned char *remain, int *remainlen) {
    int p = *pktlen;
    int got_packet = 0;
    int i;
    for (i = 0; i < inlen; i++) {
        if (in[i] == SLIP_END) {
            if (p > 0) { *pktlen = p; got_packet = 1; i++; break; }
            continue;
        }
        if (in[i] == SLIP_ESC) {
            i++;
            if (i >= inlen) break;
            if (in[i] == SLIP_ESC_END) pkt[p++] = SLIP_END;
            else if (in[i] == SLIP_ESC_ESC) pkt[p++] = SLIP_ESC;
            else pkt[p++] = in[i];
        } else {
            if (p < MTU) pkt[p++] = in[i];
        }
    }
    if (!got_packet) { *pktlen = p; }
    *remainlen = inlen - i;
    if (*remainlen > 0) memmove(remain, in + i, *remainlen);
    return got_packet;
}

static void configure_tun(const char *dev, const char *local, const char *peer) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip addr add %s peer %s dev %s", local, peer, dev);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "ip link set %s up mtu %d", dev, MTU);
    system(cmd);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <serial-port> <baud> [tun-name]\n", argv[0]);
        fprintf(stderr, "  e.g. %s /dev/ttyUSB0 1500000\n", argv[0]);
        return 1;
    }

    const char *serial_path = argv[1];
    int baud = atoi(argv[2]);
    const char *tun_name = argc > 3 ? argv[3] : "wled0";

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    int tun_fd = tun_open(tun_name);
    if (tun_fd < 0) return 1;

    int ser_fd = serial_open(serial_path, baud);
    if (ser_fd < 0) { close(tun_fd); return 1; }

    configure_tun(tun_name, "169.254.7.2", "169.254.7.1");
    fprintf(stderr, "wled-serial-bridge: %s @ %d baud <-> %s (169.254.7.2 <-> 169.254.7.1)\n",
            serial_path, baud, tun_name);

    unsigned char tun_buf[MTU + 4];
    unsigned char ser_buf[SERIAL_BUF];
    unsigned char slip_out[MTU * 2 + 2];
    unsigned char pkt_buf[MTU];
    unsigned char remain_buf[SERIAL_BUF];
    int pkt_len = 0;
    int remain_len = 0;

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(tun_fd, &fds);
        FD_SET(ser_fd, &fds);
        int maxfd = (tun_fd > ser_fd ? tun_fd : ser_fd) + 1;

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(maxfd, &fds, NULL, NULL, &tv);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        if (FD_ISSET(tun_fd, &fds)) {
            int n = read(tun_fd, tun_buf, sizeof(tun_buf));
            if (n > 0) {
                int enc_len = slip_encode(tun_buf, n, slip_out);
                write(ser_fd, slip_out, enc_len);
            }
        }

        if (FD_ISSET(ser_fd, &fds)) {
            int n = read(ser_fd, ser_buf, sizeof(ser_buf));
            if (n > 0) {
                unsigned char *input = ser_buf;
                int input_len = n;
                if (remain_len > 0) {
                    memmove(remain_buf + remain_len, ser_buf, n);
                    input = remain_buf;
                    input_len = remain_len + n;
                    remain_len = 0;
                }
                while (input_len > 0) {
                    int rl = 0;
                    if (slip_decode(input, input_len, pkt_buf, &pkt_len, remain_buf, &rl)) {
                        if (pkt_len > 0) write(tun_fd, pkt_buf, pkt_len);
                        pkt_len = 0;
                        input = remain_buf;
                        input_len = rl;
                        remain_len = 0;
                    } else {
                        remain_len = rl;
                        break;
                    }
                }
            }
        }
    }

    fprintf(stderr, "\nwled-serial-bridge: shutting down\n");
    close(ser_fd);
    close(tun_fd);
    return 0;
}
