// Network and radio commands: ping, LAN discovery, port scan, DNS, HTTP, BLE scan
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard.h"
#include "lwip/etharp.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "shell.h"
#include "sound.h"
#include "term.h"
#include "wifi.h"
#include "wifitools.h"

#define PING_ID 0xDEC0

static bool require_wifi(void) {
    if (wifi_is_connected()) return true;
    term_puts("\x1b[31msem wifi\x1b[0m (wifi connect <rede> <senha>)\r\n");
    return false;
}

static bool resolve_host(const char* host, struct in_addr* out) {
    if (inet_aton(host, out)) return true;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo* result = NULL;
    if (getaddrinfo(host, NULL, &hints, &result) != 0 || result == NULL) {
        term_printf("\x1b[31mnao achei o endereco de %s\x1b[0m\r\n", host);
        return false;
    }
    *out = ((struct sockaddr_in*)result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return true;
}

// ICMP echo over a raw socket (replies include the IP header)

static int icmp_open(void) {
    int sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (sock < 0) term_puts("\x1b[31mfalha ao abrir socket icmp\x1b[0m\r\n");
    return sock;
}

static void icmp_send(int sock, struct in_addr target, uint16_t seq) {
    uint8_t packet[sizeof(struct icmp_echo_hdr) + 32] = {0};
    struct icmp_echo_hdr* header = (struct icmp_echo_hdr*)packet;
    ICMPH_TYPE_SET(header, ICMP_ECHO);
    ICMPH_CODE_SET(header, 0);
    header->id = lwip_htons(PING_ID);
    header->seqno = lwip_htons(seq);
    memcpy(packet + sizeof(*header), "cyberdeck-ping-cyberdeck-ping-cy", 32);
    header->chksum = inet_chksum(packet, sizeof(packet));
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_addr = target};
    sendto(sock, packet, sizeof(packet), 0, (struct sockaddr*)&to, sizeof(to));
}

// Waits for an echo reply with our id; returns false on timeout
static bool icmp_receive(int sock, int timeout_ms, struct in_addr* from, uint16_t* seq, int* ttl) {
    struct timeval timeout = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    while (esp_timer_get_time() < deadline) {
        uint8_t buffer[96];
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        int len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&source, &source_len);
        if (len <= 0) return false;
        struct ip_hdr* ip = (struct ip_hdr*)buffer;
        int ip_len = IPH_HL_BYTES(ip);
        if (len < ip_len + (int)sizeof(struct icmp_echo_hdr)) continue;
        struct icmp_echo_hdr* echo = (struct icmp_echo_hdr*)(buffer + ip_len);
        if (ICMPH_TYPE(echo) != ICMP_ER || lwip_ntohs(echo->id) != PING_ID) continue;
        *from = source.sin_addr;
        *seq = lwip_ntohs(echo->seqno);
        *ttl = IPH_TTL(ip);
        return true;
    }
    return false;
}

static void cmd_ping(int argc, char** argv) {
    if (argc < 2) {
        shell_print_usage("ping");
        return;
    }
    if (!require_wifi()) return;
    struct in_addr target;
    if (!resolve_host(argv[1], &target)) return;
    int count = argc > 2 ? atoi(argv[2]) : 4;

    int sock = icmp_open();
    if (sock < 0) return;
    term_printf("ping %s (%s)\r\n", argv[1], inet_ntoa(target));
    int sent = 0, received = 0;
    int64_t min_us = INT64_MAX, max_us = 0, total_us = 0;
    for (uint16_t seq = 1; count == 0 || seq <= count; seq++) {
        if (shell_interrupted()) break;
        int64_t start = esp_timer_get_time();
        icmp_send(sock, target, seq);
        sent++;
        struct in_addr from;
        uint16_t reply_seq;
        int ttl;
        bool ok = false;
        while (icmp_receive(sock, 1000 - (int)((esp_timer_get_time() - start) / 1000), &from, &reply_seq, &ttl)) {
            if (reply_seq == seq && from.s_addr == target.s_addr) {
                ok = true;
                break;
            }
        }
        int64_t elapsed = esp_timer_get_time() - start;
        if (ok) {
            received++;
            total_us += elapsed;
            if (elapsed < min_us) min_us = elapsed;
            if (elapsed > max_us) max_us = elapsed;
            term_printf("seq=%u \x1b[32m%.1f ms\x1b[0m ttl=%d\r\n", seq, elapsed / 1000.0, ttl);
        } else {
            term_printf("seq=%u \x1b[31msem resposta\x1b[0m\r\n", seq);
        }
        // One ping per second
        int64_t wait_ms = 1000 - elapsed / 1000;
        if (wait_ms > 0 && (count == 0 || seq < count)) vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
    close(sock);
    term_printf("%d enviados, %d recebidos, %d%% perda", sent, received, sent ? (sent - received) * 100 / sent : 0);
    if (received) term_printf("\r\nmin/med/max %.1f/%.1f/%.1f ms", min_us / 1000.0, total_us / 1000.0 / received, max_us / 1000.0);
    term_puts("\r\n");
}

// LAN discovery: ARP requests in batches small enough for lwIP's 10-entry ARP table

typedef struct {
    struct netif* netif;
    ip4_addr_t ip;
    struct eth_addr mac;
    bool found;
} ArpQuery;

static void arp_send(void* arg) {
    ArpQuery* query = arg;
    etharp_request(query->netif, &query->ip);
}

static void arp_lookup(void* arg) {
    ArpQuery* query = arg;
    struct eth_addr* mac = NULL;
    const ip4_addr_t* ip_out = NULL;
    query->found = etharp_find_addr(query->netif, &query->ip, &mac, &ip_out) >= 0 && mac != NULL;
    if (query->found) query->mac = *mac;
}

// Runs a function on the lwIP thread and waits for it
static void run_on_tcpip(tcpip_callback_fn function, void* arg) {
    LOCK_TCPIP_CORE();
    function(arg);
    UNLOCK_TCPIP_CORE();
}

static void cmd_netscan(int argc, char** argv) {
    if (!require_wifi()) return;
    esp_netif_ip_info_t info;
    esp_netif_get_ip_info(wifi_netif(), &info);
    struct netif* netif = esp_netif_get_netif_impl(wifi_netif());

    uint32_t own = lwip_ntohl(info.ip.addr);
    uint32_t mask = lwip_ntohl(info.netmask.addr);
    uint32_t network = own & mask;
    uint32_t hosts = ~mask - 1;
    if (hosts > 254) {
        // Bigger networks would take minutes: scan our /24
        network = own & 0xFFFFFF00;
        hosts = 254;
    }
    term_printf("varrendo " IPSTR "/%d (q cancela)\r\n", IP2STR(&info.ip), hosts == 254 ? 24 : __builtin_popcount(mask));
    shell_pager_start();
    term_puts("\x1b[90mip               mac\x1b[0m\r\n");
    shell_pager_line();

    const int batch = 8;
    int found = 0;
    ArpQuery queries[8];
    for (uint32_t start = 1; start <= hosts; start += batch) {
        if (shell_interrupted()) {
            term_puts("cancelado\r\n");
            break;
        }
        int n = (hosts - start + 1) < (uint32_t)batch ? (int)(hosts - start + 1) : batch;
        for (int i = 0; i < n; i++) {
            queries[i] = (ArpQuery) {.netif = netif, .found = false};
            queries[i].ip.addr = lwip_htonl(network + start + i);
            run_on_tcpip(arp_send, &queries[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        for (int i = 0; i < n; i++) {
            run_on_tcpip(arp_lookup, &queries[i]);
            uint32_t address = network + start + i;
            if (!queries[i].found && address != own) continue;
            found++;
            const uint8_t* m = queries[i].mac.addr;
            ip4_addr_t ip = {.addr = lwip_htonl(address)};
            char ip_text[16];
            snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&ip));
            const char* label = address == own ? " \x1b[36m(deck)\x1b[0m"
                              : (ip.addr == info.gw.addr ? " \x1b[33m(roteador)\x1b[0m" : "");
            if (address == own) {
                term_printf("%-16s %-17s%s\r\n", ip_text, "-", label);
            } else {
                // Locally administered addresses are randomized by phones and laptops for privacy
                bool random_mac = m[0] & 0x02;
                term_printf("%-16s %02x:%02x:%02x:%02x:%02x:%02x%s%s\r\n", ip_text, m[0], m[1], m[2], m[3], m[4], m[5],
                            random_mac ? " \x1b[90m*\x1b[0m" : "", label);
            }
            if (!shell_pager_line()) {
                start = hosts + 1; // stop the outer loop too
                break;
            }
        }
    }
    term_printf("%d dispositivos \x1b[90m(* = mac aleatorio)\x1b[0m\r\n", found);
    sound_play(found > 1 ? SOUND_READ : SOUND_ERROR);
}

// TCP connect scan, a batch of non-blocking sockets at a time

static const uint16_t common_ports[] = {
    21, 22, 23, 25, 53, 80, 110, 139, 143, 443, 445, 554, 631, 1883, 1900, 3000, 3306, 3389,
    5000, 5432, 5900, 6379, 7000, 8000, 8008, 8009, 8080, 8443, 8888, 9000, 9100, 32400, 49152, 62078,
};

static const char* port_name(uint16_t port) {
    switch (port) {
        case 21: return "ftp";      case 22: return "ssh";      case 23: return "telnet";
        case 25: return "smtp";     case 53: return "dns";      case 80: return "http";
        case 110: return "pop3";    case 139: return "netbios"; case 143: return "imap";
        case 443: return "https";   case 445: return "smb";     case 554: return "rtsp";
        case 631: return "ipp";     case 1883: return "mqtt";   case 3306: return "mysql";
        case 3389: return "rdp";    case 5432: return "postgres"; case 5900: return "vnc";
        case 6379: return "redis";  case 8008: case 8009: return "chromecast";
        case 8080: case 8000: case 8888: return "http-alt"; case 8443: return "https-alt";
        case 9100: return "printer"; case 32400: return "plex";  case 62078: return "iphone-sync";
        default: return "";
    }
}

static void cmd_portscan(int argc, char** argv) {
    if (argc < 2) {
        shell_print_usage("portscan");
        return;
    }
    if (!require_wifi()) return;
    struct in_addr target;
    if (!resolve_host(argv[1], &target)) return;

    uint16_t first = 0, last = 0;
    bool use_list = argc < 3;
    if (!use_list) {
        char* dash = strchr(argv[2], '-');
        first = atoi(argv[2]);
        last = dash ? atoi(dash + 1) : first;
        if (first == 0 || last < first) {
            shell_print_usage("portscan");
            return;
        }
    }
    int total = use_list ? (int)(sizeof(common_ports) / sizeof(common_ports[0])) : last - first + 1;
    term_printf("portscan %s: %d portas (q cancela)\r\n", inet_ntoa(target), total);

    const int batch = 8;
    int open_count = 0;
    for (int index = 0; index < total; index += batch) {
        if (shell_interrupted()) {
            term_puts("cancelado\r\n");
            break;
        }
        int socks[8];
        uint16_t ports[8];
        int n = total - index < batch ? total - index : batch;
        for (int i = 0; i < n; i++) {
            ports[i] = use_list ? common_ports[index + i] : first + index + i;
            socks[i] = socket(AF_INET, SOCK_STREAM, 0);
            if (socks[i] < 0) continue;
            fcntl(socks[i], F_SETFL, O_NONBLOCK);
            struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(ports[i]), .sin_addr = target};
            connect(socks[i], (struct sockaddr*)&address, sizeof(address));
        }

        // Collect the connections that complete within the timeout
        int64_t deadline = esp_timer_get_time() + 400 * 1000;
        bool done[8] = {false};
        while (esp_timer_get_time() < deadline) {
            fd_set writable;
            FD_ZERO(&writable);
            int max_fd = -1;
            for (int i = 0; i < n; i++) {
                if (socks[i] >= 0 && !done[i]) {
                    FD_SET(socks[i], &writable);
                    if (socks[i] > max_fd) max_fd = socks[i];
                }
            }
            if (max_fd < 0) break;
            struct timeval wait = {.tv_sec = 0, .tv_usec = 50 * 1000};
            if (select(max_fd + 1, NULL, &writable, NULL, &wait) <= 0) continue;
            for (int i = 0; i < n; i++) {
                if (socks[i] < 0 || done[i] || !FD_ISSET(socks[i], &writable)) continue;
                done[i] = true;
                int error = 0;
                socklen_t len = sizeof(error);
                getsockopt(socks[i], SOL_SOCKET, SO_ERROR, &error, &len);
                if (error == 0) {
                    open_count++;
                    term_printf("\x1b[32m%5u aberta\x1b[0m %s\r\n", ports[i], port_name(ports[i]));
                }
            }
        }
        for (int i = 0; i < n; i++) {
            if (socks[i] >= 0) close(socks[i]);
        }
    }
    term_printf("%d portas abertas\r\n", open_count);
    sound_play(open_count ? SOUND_READ : SOUND_NAV);
}

static void cmd_dns(int argc, char** argv) {
    if (argc < 2) {
        shell_print_usage("dns");
        return;
    }
    if (!require_wifi()) return;
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo* result = NULL;
    int64_t start = esp_timer_get_time();
    if (getaddrinfo(argv[1], NULL, &hints, &result) != 0 || result == NULL) {
        term_printf("\x1b[31m%s: nao encontrado\x1b[0m\r\n", argv[1]);
        return;
    }
    for (struct addrinfo* r = result; r != NULL; r = r->ai_next) {
        char text[48];
        if (r->ai_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in*)r->ai_addr)->sin_addr, text, sizeof(text));
        } else {
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)r->ai_addr)->sin6_addr, text, sizeof(text));
        }
        term_printf("%s\r\n", text);
    }
    freeaddrinfo(result);
    term_printf("\x1b[90m%lld ms\x1b[0m\r\n", (esp_timer_get_time() - start) / 1000);
}

// HTTP GET, body printed as text. Returns the status code, or -1.
static int http_get(const char* url, bool show_status, int max_bytes) {
    char full_url[256];
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        strlcpy(full_url, url, sizeof(full_url));
    } else {
        snprintf(full_url, sizeof(full_url), "http://%s", url);
    }
    esp_http_client_config_t config = {
        .url = full_url,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "curl/8.0 (cyberdeck)",
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL || esp_http_client_open(client, 0) != ESP_OK) {
        term_printf("\x1b[31mfalha ao conectar em %s\x1b[0m\r\n", full_url);
        if (client) esp_http_client_cleanup(client);
        return -1;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (show_status) {
        const char* color = status < 300 ? "\x1b[32m" : (status < 400 ? "\x1b[33m" : "\x1b[31m");
        term_printf("%sHTTP %d\x1b[0m  %lld bytes\r\n", color, status, esp_http_client_get_content_length(client));
    }
    char buffer[256];
    int total = 0, len;
    char last = '\n';
    bool quit = false;
    int column = 0;
    shell_pager_start();
    while (!quit && total < max_bytes && (len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < len && !quit; i++) {
            if (buffer[i] == '\n') {
                term_puts("\r\n");
                column = 0;
                quit = !shell_pager_line();
            } else if (buffer[i] != '\r') {
                term_write(&buffer[i], 1);
                // Wrapped lines count for the pager too (UTF-8 continuation bytes take no column)
                if (((unsigned char)buffer[i] & 0xC0) != 0x80 && ++column >= term_cols()) {
                    column = 0;
                    quit = !shell_pager_line();
                }
            }
        }
        last = buffer[len - 1];
        total += len;
    }
    if (last != '\n') term_puts("\r\n");
    if (total >= max_bytes) term_printf("\x1b[90m... (cortado em %d bytes)\x1b[0m\r\n", max_bytes);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

static void cmd_http(int argc, char** argv) {
    if (argc < 2) {
        shell_print_usage("http");
        return;
    }
    if (!require_wifi()) return;
    http_get(argv[1], true, 4096);
}

static void cmd_myip(int argc, char** argv) {
    if (!require_wifi()) return;
    // ip-api.com line format: one field per line, always in its own order
    term_puts("\x1b[90mpais, estado, cidade, provedor, ip publico\x1b[0m\r\n");
    http_get("http://ip-api.com/line/?fields=query,city,regionName,country,isp", false, 512);
}

static void cmd_weather(int argc, char** argv) {
    if (!require_wifi()) return;
    char url[160];
    char city[64] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) strlcat(city, "+", sizeof(city));
        strlcat(city, argv[i], sizeof(city));
    }
    // Plain text format: no emoji, which the font cannot show
    snprintf(url, sizeof(url), "http://wttr.in/%s?format=%%l:+%%t+(sens.+%%f)%%0A%%C%%0A"
             "umidade+%%h,+vento+%%w,+chuva+%%p&lang=pt&m", city);
    http_get(url, false, 512);
}

// BLE scan: every advertising device nearby

typedef struct {
    uint8_t bda[6];
    int rssi;
    char name[24];
    uint16_t appearance;
} BleDevice;

static BleDevice* ble_devices;
static int ble_count;
#define BLE_MAX 48

static void on_adv(const uint8_t* bda, int rssi, const char* name, uint16_t appearance) {
    for (int i = 0; i < ble_count; i++) {
        if (memcmp(ble_devices[i].bda, bda, 6) == 0) {
            if (name && !ble_devices[i].name[0]) strlcpy(ble_devices[i].name, name, sizeof(ble_devices[i].name));
            if (rssi > ble_devices[i].rssi) ble_devices[i].rssi = rssi;
            return;
        }
    }
    if (ble_count >= BLE_MAX) return;
    BleDevice* device = &ble_devices[ble_count++];
    memcpy(device->bda, bda, 6);
    device->rssi = rssi;
    device->appearance = appearance;
    strlcpy(device->name, name ? name : "", sizeof(device->name));
}

static const char* appearance_name(uint16_t appearance) {
    switch (appearance >> 6) {
        case 1: return "telefone";
        case 2: return "computador";
        case 3: return "relogio";
        case 5: return "tela";
        case 10: return "midia";
        case 13: return "sensor";
        case 15: return "hid";
        case 16: return "glicose";
        case 49: return "fone";
        default: return "";
    }
}

static int compare_rssi(const void* a, const void* b) {
    return ((const BleDevice*)b)->rssi - ((const BleDevice*)a)->rssi;
}

static void cmd_blescan(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 5;
    if (seconds < 1 || seconds > 30) seconds = 5;
    ble_devices = calloc(BLE_MAX, sizeof(BleDevice));
    if (ble_devices == NULL) return;
    ble_count = 0;
    term_printf("procurando dispositivos ble por %ds...\r\n", seconds);
    keyboard_ble_scan(seconds, on_adv);
    qsort(ble_devices, ble_count, sizeof(BleDevice), compare_rssi);

    shell_pager_start();
    term_puts("\x1b[90m dBm mac               nome\x1b[0m\r\n");
    shell_pager_line();
    for (int i = 0; i < ble_count; i++) {
        BleDevice* d = &ble_devices[i];
        const char* color = d->rssi > -60 ? "\x1b[32m" : (d->rssi > -80 ? "\x1b[33m" : "\x1b[31m");
        const char* kind = appearance_name(d->appearance);
        // Unnamed devices show their kind (from the appearance field) in gray
        const char* label = d->name[0] ? d->name : (kind[0] ? kind : "-");
        term_printf("%s%4d\x1b[0m %02x:%02x:%02x:%02x:%02x:%02x %s%s\x1b[0m\r\n", color, d->rssi,
                    d->bda[0], d->bda[1], d->bda[2], d->bda[3], d->bda[4], d->bda[5],
                    d->name[0] ? "" : "\x1b[90m", label);
        if (!shell_pager_line()) break;
    }
    term_printf("%d dispositivos\r\n", ble_count);
    sound_play(ble_count ? SOUND_READ : SOUND_ERROR);
    free(ble_devices);
    ble_devices = NULL;
}

static void cmd_deauth(int argc, char** argv) {
    wifitools_deauth_detector();
}

static void cmd_wifi_connect_ui(int argc, char** argv) {
    wifitools_connect_screen();
}

static const ShellCommand commands[] = {
    {"conectar", "rede", NULL,                        "conecta numa rede wifi (toque)",     cmd_wifi_connect_ui},
    {"ping",     "rede",  "<host> [vezes, 0=sem fim]", "testa se um host responde",          cmd_ping},
    {"netscan",  "rede",  NULL,                        "lista os dispositivos da sua rede",  cmd_netscan},
    {"portscan", "rede",  "<host> [porta|ini-fim]",    "portas tcp abertas (sem porta: comuns)", cmd_portscan},
    {"dns",      "rede",  "<dominio>",                 "consulta dns",                       cmd_dns},
    {"http",     "rede",  "<url>",                     "baixa uma pagina e mostra o texto",  cmd_http},
    {"myip",     "rede",  NULL,                        "ip publico e provedor",              cmd_myip},
    {"weather",  "rede",  "[cidade]",                  "previsao do tempo (wttr.in)",        cmd_weather},
    {"deauth",   "rede",  NULL,                        "detecta ataques de deauth no ar (passivo)", cmd_deauth},
    {"blescan",  "radio", "[segundos]",                "dispositivos bluetooth por perto",   cmd_blescan},
};

void tools_net_register(void) {
    shell_register(commands, sizeof(commands) / sizeof(commands[0]));
}
