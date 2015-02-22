#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>     // close
#include <arpa/inet.h>  // inet_ntop
#include <sys/ioctl.h>  // ioctl, SIOCGIFCONF
#include <net/if.h>     // struct ifconf, struct ifreq
#include "lssdp.h"

/* This is defined on Mac OS X */
#ifndef _SIZEOF_ADDR_IFREQ
#define _SIZEOF_ADDR_IFREQ sizeof
#endif

#define LSSDP_IP_LEN            16
#define LSSDP_MESSAGE_MAX_LEN   2048
#define LSSDP_MULTICAST_ADDR    "239.255.255.250"

#define lssdp_debug(fmt, agrs...) lssdp_log("DEBUG", __LINE__, __func__, fmt, ##agrs)
#define lssdp_warn(fmt, agrs...)  lssdp_log("WARN",  __LINE__, __func__, fmt, ##agrs)
#define lssdp_error(fmt, agrs...) lssdp_log("ERROR", __LINE__, __func__, fmt, ##agrs)

static int send_multicast_data(const char * data, const struct lssdp_interface interface, int ssdp_port);
static int lssdp_log(const char * level, int line, const char * func, const char * format, ...);


/** Global Variable **/
static int (* lssdp_log_callback)(const char * file, const char * tag, const char * level, int line, const char * func, const char * message) = NULL;


// 01. lssdp_set_log_callback
int lssdp_set_log_callback(int (* callback)(const char * file, const char * tag, const char * level, int line, const char * func, const char * message)) {
    lssdp_log_callback = callback;
    return 0;
}

// 02. lssdp_get_network_interface
int lssdp_get_network_interface(lssdp_ctx * lssdp) {
    if (lssdp == NULL) {
        lssdp_error("lssdp should not be NULL\n");
        return -1;
    }

    // reset lssdp->interface
    memset(lssdp->interface, 0, sizeof(struct lssdp_interface) * LSSDP_INTERFACE_LIST_SIZE);

    int result = -1;

    /* Reference to this article:
     * http://stackoverflow.com/a/8007079
     */

    // create socket
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        lssdp_error("create socket failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    char buffer[4096] = {0};
    struct ifconf ifc = {
        .ifc_len = sizeof(buffer),
        .ifc_buf = (caddr_t) buffer
    };

    if (ioctl(fd, SIOCGIFCONF, &ifc) < 0) {
        lssdp_error("ioctl failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    size_t index, num = 0;
    struct ifreq * ifr;
    for (index = 0; index < ifc.ifc_len; index += _SIZEOF_ADDR_IFREQ(*ifr)) {
        ifr = (struct ifreq *)(buffer + index);
        if (ifr->ifr_addr.sa_family != AF_INET) {
            // only support IPv4
            continue;
        }

        // get interface ip string
        char ip[LSSDP_IP_LEN] = {0};  // ip = "xxx.xxx.xxx.xxx"
        struct sockaddr_in * addr_in = (struct sockaddr_in *) &ifr->ifr_addr;
        if (inet_ntop(AF_INET, &addr_in->sin_addr, ip, sizeof(ip)) == NULL) {
            lssdp_error("inet_ntop failed, errno = %s (%d)\n", strerror(errno), errno);
            goto end;
        }

        // too many network interface
        if (num >= LSSDP_INTERFACE_LIST_SIZE) {
            lssdp_warn("the number of network interface is over than max size %d\n", LSSDP_INTERFACE_LIST_SIZE);
            lssdp_debug("%2d. %s : %s\n", num, ifr->ifr_name, ip);
        } else {
            // 1. set interface.name
            snprintf(lssdp->interface[num].name, LSSDP_INTERFACE_NAME_LEN - 1, "%s", ifr->ifr_name);

            // 2. set interface.ip = [ xxx, xxx, xxx, xxx ]
            char * token_ptr;
            size_t i;
            for (i = 0; i < 4; i++) {
                lssdp->interface[num].ip[i] = atoi(strtok_r(i == 0 ? ip : NULL, ".", &token_ptr));
            }
        }

        // increase interface number
        num++;
    }

    result = 0;
end:
    if (fd > 0) close(fd);
    return result;
}

// 03. lssdp_create_socket
int lssdp_create_socket(lssdp_ctx * lssdp) {
    if (lssdp == NULL) {
        lssdp_error("lssdp should not be NULL\n");
        return -1;
    }

    if (lssdp->sock >= 0) {
        lssdp_debug("close socket %d\n", lssdp->sock);
        close(lssdp->sock);
        lssdp->sock = -1;
    }

    // create UDP socket
    lssdp->sock = socket(PF_INET, SOCK_DGRAM, 0);
    if (lssdp->sock < 0) {
        lssdp_error("create socket failed, errno = %s (%d)\n", strerror(errno), errno);
        return -1;
    }

    int result = -1;

    // set non-blocking
    int opt = 1;
    if (ioctl(lssdp->sock, FIONBIO, &opt) != 0) {
        lssdp_error("ioctl failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // set reuse address
    if (setsockopt(lssdp->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        lssdp_error("setsockopt SO_REUSEADDR failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // bind socket
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(lssdp->port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(lssdp->sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        lssdp_error("bind failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // set IP_ADD_MEMBERSHIP
    struct ip_mreq imr = {
        .imr_multiaddr.s_addr = inet_addr(LSSDP_MULTICAST_ADDR),
        .imr_interface.s_addr = htonl(INADDR_ANY)
    };
    if (setsockopt(lssdp->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imr, sizeof(struct ip_mreq)) != 0) {
        lssdp_error("setsockopt IP_ADD_MEMBERSHIP failed: %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    result = 0;
end:
    if (result == -1) {
        close(lssdp->sock);
        lssdp->sock = -1;
    }
    return result;
}

// 04. lssdp_read_socket
int lssdp_read_socket(lssdp_ctx * lssdp) {
    char buffer[2048] = {};
    struct sockaddr_in address = {};
    socklen_t address_len = sizeof(struct sockaddr_in);

    ssize_t recv_len = recvfrom(lssdp->sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&address, &address_len);
    if (recv_len == -1) {
        lssdp_error("recvfrom failed, errno = %s (%d)\n", strerror(errno), errno);
        return -1;
    }

    // TODO: parse SSDP packet to struct

    if (lssdp->data_callback == NULL) {
        lssdp_warn("data_callback has not been setup\n");
        return 0;
    }

    // invoke data received callback
    lssdp->data_callback(lssdp, buffer, recv_len);
    return 0;
}

// 05. lssdp_send_msearch
int lssdp_send_msearch(lssdp_ctx * lssdp) {
    // 1. update network interface
    lssdp_get_network_interface(lssdp);

    // 2. set M-SEARCH packet
    char msearch[1024] = {};
    snprintf(msearch, sizeof(msearch),
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST:%s:%d\r\n"
        "MAN:\"ssdp:discover\"\r\n"
        "ST:%s\r\n"
        "MX:1\r\n"
        "\r\n",
        LSSDP_MULTICAST_ADDR, lssdp->port,  // HOST
        lssdp->header.st                    // ST (Search Target)
    );

    // 3. send M-SEARCH to each interface
    size_t i;
    for (i = 0; i < LSSDP_INTERFACE_LIST_SIZE; i++) {
        struct lssdp_interface * interface = &lssdp->interface[i];
        if (strlen(interface->name) == 0) {
            break;
        }

        send_multicast_data(msearch, *interface, lssdp->port);
    }
    return 0;
}


/** Internal Function **/

static int send_multicast_data(const char * data, const struct lssdp_interface interface, int ssdp_port) {
    if (data == NULL) {
        lssdp_error("data should not be NULL\n");
        return -1;
    }

    size_t data_len = strlen(data);
    if (data_len == 0) {
        lssdp_error("data length should not be empty\n");
        return -1;
    }

    if (strlen(interface.name) == 0) {
        lssdp_error("interface.name should not be empty\n");
        return -1;
    }

    if (ssdp_port < 0 || ssdp_port > 0xFFFF) {
        lssdp_error("ssdp_port (%d) is invalid\n");
        return -1;
    }

    int result = -1;

    // 1. create UDP socket
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        lssdp_error("create socket failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // 2. get IP address
    char ip[LSSDP_IP_LEN] = {};
    sprintf(ip, "%d.%d.%d.%d", interface.ip[0], interface.ip[1], interface.ip[2], interface.ip[3]);

    // 3. bind socket
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = inet_addr(ip)
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        lssdp_error("bind failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // 4. disable IP_MULTICAST_LOOP
    char opt = 0;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &opt, sizeof(opt)) < 0) {
        lssdp_error("setsockopt IP_MULTICAST_LOOP failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // 5. set destination address
    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ssdp_port)
    };
    if (inet_aton(LSSDP_MULTICAST_ADDR, &dest_addr.sin_addr) == 0) {
        lssdp_error("inet_aton failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // 6. send data
    if (sendto(fd, data, data_len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) == -1) {
        lssdp_error("sendto %s (%s) failed, errno = %s (%d)\n", interface.name, ip, strerror(errno), errno);
        goto end;
    }

    result = 0;
end:
    if (fd > 0) close(fd);
    return result;
}

static int lssdp_log(const char * level, int line, const char * func, const char * format, ...) {
    if (lssdp_log_callback == NULL) {
        return -1;
    }

    char message[LSSDP_MESSAGE_MAX_LEN] = {0};

    // create message by va_list
    va_list args;
    va_start(args, format);
    vsnprintf(message, LSSDP_MESSAGE_MAX_LEN, format, args);
    va_end(args);

    // invoke log callback function
    lssdp_log_callback(__FILE__, "SSDP", level, line, func, message);
    return 0;
}
