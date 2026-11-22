#define _GNU_SOURCE
#include "kobodash.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/wireless.h>
#endif

static bool read_file_str(const char *path, char *buf, size_t sz) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    size_t n = fread(buf, 1, sz - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    return true;
}

static bool read_file_int(const char *path, int *out) {
    char buf[64];
    if (!read_file_str(path, buf, sizeof(buf)) || buf[0] == '\0') return false;
    *out = atoi(buf);
    return true;
}

static void read_battery(Telemetry *t) {
    t->bat_pct = -1;
    t->bat_mv = 0;
    t->bat_ma = 0;
    snprintf(t->bat_status, sizeof(t->bat_status), "Unknown");
    t->charger_online = 0;

    const char *bat_dir = "/sys/class/power_supply/mc13892_bat";
    struct stat st;
    if (stat(bat_dir, &st) != 0) {
        bat_dir = "/sys/class/power_supply/battery";
    }

    char path[160];
    int val = 0;
    snprintf(path, sizeof(path), "%s/capacity", bat_dir);
    if (read_file_int(path, &val)) {
        t->bat_pct = val;
    }
    snprintf(path, sizeof(path), "%s/voltage_now", bat_dir);
    if (read_file_int(path, &val)) {
        t->bat_mv = (val > 100000) ? (val / 1000) : val;
    }
    snprintf(path, sizeof(path), "%s/current_now", bat_dir);
    if (read_file_int(path, &val)) {
        t->bat_ma = (abs(val) > 10000) ? (val / 1000) : val;
    }
    snprintf(path, sizeof(path), "%s/status", bat_dir);
    char sbuf[32];
    if (read_file_str(path, sbuf, sizeof(sbuf)) && sbuf[0]) {
        snprintf(t->bat_status, sizeof(t->bat_status), "%s", sbuf);
    }

    if (read_file_int("/sys/class/power_supply/mc13892_charger/online", &val)) {
        t->charger_online = val ? 1 : 0;
    } else if (strcmp(t->bat_status, "Charging") == 0 || strcmp(t->bat_status, "Full") == 0) {
        t->charger_online = 1;
    }
}

static void read_memory_and_cpu(Telemetry *t) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[128];
        unsigned long mem_free = 0, buffers = 0, cached = 0, mem_avail = 0;
        while (fgets(line, sizeof(line), fp)) {
            unsigned long v = 0;
            if (sscanf(line, "MemTotal: %lu kB", &v) == 1) t->mem_total_kb = v;
            else if (sscanf(line, "MemAvailable: %lu kB", &v) == 1) mem_avail = v;
            else if (sscanf(line, "MemFree: %lu kB", &v) == 1) mem_free = v;
            else if (sscanf(line, "Buffers: %lu kB", &v) == 1) buffers = v;
            else if (sscanf(line, "Cached: %lu kB", &v) == 1) cached = v;
        }
        fclose(fp);
        /* Linux 2.6.35 does not expose MemAvailable; Free + Buffers + Cached is accurate */
        t->mem_avail_kb = mem_avail ? mem_avail : (mem_free + buffers + cached);
    }

    fp = fopen("/proc/self/status", "r");
    if (fp) {
        char line[128];
        while (fgets(line, sizeof(line), fp)) {
            unsigned long v = 0;
            if (sscanf(line, "VmRSS: %lu kB", &v) == 1) {
                t->self_rss_kb = v;
                break;
            }
        }
        fclose(fp);
    }

    fp = fopen("/proc/loadavg", "r");
    if (fp) {
        if (fscanf(fp, "%lf %lf %lf", &t->load1, &t->load5, &t->load15) != 3) {
            t->load1 = t->load5 = t->load15 = 0.0;
        }
        fclose(fp);
    }

    fp = fopen("/proc/uptime", "r");
    if (fp) {
        double up = 0.0;
        if (fscanf(fp, "%lf", &up) == 1) {
            t->uptime_sec = (long)up;
        }
        fclose(fp);
    }

    int khz = 0;
    if (read_file_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", &khz) ||
        read_file_int("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq", &khz)) {
        t->cpu_khz = khz;
    }
    if (!read_file_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                       t->cpu_gov, sizeof(t->cpu_gov))) {
        snprintf(t->cpu_gov, sizeof(t->cpu_gov), "default");
    }
}

static void read_network(Telemetry *t) {
    t->wifi_up = 0;
    snprintf(t->wifi_ip, sizeof(t->wifi_ip), "Offline");
    t->wifi_ssid[0] = '\0';
    t->wifi_rssi = 0;

    char iface[16] = "wlan0";
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == 0) {
        for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            char ipbuf[32];
            if (inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf))) {
                if (strncmp(ifa->ifa_name, "wlan", 4) == 0 ||
                    strncmp(ifa->ifa_name, "eth", 3) == 0 ||
                    strncmp(ifa->ifa_name, "en", 2) == 0) {
                    snprintf(iface, sizeof(iface), "%s", ifa->ifa_name);
                    snprintf(t->wifi_ip, sizeof(t->wifi_ip), "%s", ipbuf);
                    t->wifi_up = 1;
                    if (strncmp(ifa->ifa_name, "wlan", 4) == 0) break;
                }
            }
        }
        freeifaddrs(ifaddr);
    }

#ifdef __linux__
    /* Direct WEXT ioctl for SSID and RSSI on ar6000 (zero fork overhead) */
    if (t->wifi_up) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct iwreq wrq;
            memset(&wrq, 0, sizeof(wrq));
            snprintf(wrq.ifr_name, sizeof(wrq.ifr_name), "%s", iface);
            char essid[IW_ESSID_MAX_SIZE + 2];
            memset(essid, 0, sizeof(essid));
            wrq.u.essid.pointer = essid;
            wrq.u.essid.length = IW_ESSID_MAX_SIZE + 1;
            if (ioctl(sock, SIOCGIWESSID, &wrq) == 0 && essid[0] != '\0') {
                snprintf(t->wifi_ssid, sizeof(t->wifi_ssid), "%s", essid);
            }

            struct iw_statistics iwstat;
            memset(&iwstat, 0, sizeof(iwstat));
            memset(&wrq, 0, sizeof(wrq));
            snprintf(wrq.ifr_name, sizeof(wrq.ifr_name), "%s", iface);
            wrq.u.data.pointer = &iwstat;
            wrq.u.data.length = sizeof(iwstat);
            wrq.u.data.flags = 1;
            if (ioctl(sock, SIOCGIWSTATS, &wrq) == 0) {
                if (iwstat.qual.updated & IW_QUAL_DBM) {
                    int dbm = (int)iwstat.qual.level - 256;
                    if (dbm < 0 && dbm > -120) t->wifi_rssi = dbm;
                } else if (iwstat.qual.level > 0 && iwstat.qual.level < 120) {
                    t->wifi_rssi = -((int)iwstat.qual.level);
                }
            }
            close(sock);
        }

        /* Fallback to /proc/net/wireless if ioctl didn't yield dBm */
        if (t->wifi_rssi == 0) {
            FILE *wfp = fopen("/proc/net/wireless", "r");
            if (wfp) {
                char line[256];
                while (fgets(line, sizeof(line), wfp)) {
                    if (strstr(line, iface)) {
                        char *colon = strchr(line, ':');
                        if (colon) {
                            int status = 0;
                            float link = 0, level = 0;
                            if (sscanf(colon + 1, "%d %f %f", &status, &link, &level) == 3) {
                                int lv = (int)level;
                                if (lv > 128) lv -= 256;
                                if (lv < 0 && lv > -120) t->wifi_rssi = lv;
                            }
                        }
                        break;
                    }
                }
                fclose(wfp);
            }
        }

        /* Fallback to iwconfig wlan0 only if still unknown */
        if (t->wifi_rssi == 0) {
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "iwconfig %s 2>/dev/null", iface);
            FILE *pp = popen(cmd, "r");
            if (pp) {
                char out[512];
                size_t n = fread(out, 1, sizeof(out) - 1, pp);
                pclose(pp);
                out[n] = '\0';
                char *sig = strstr(out, "Signal level");
                if (sig) {
                    while (*sig && *sig != '=' && *sig != ':') sig++;
                    if (*sig) {
                        int dbm = atoi(sig + 1);
                        if (dbm < 0 && dbm > -120) t->wifi_rssi = dbm;
                    }
                }
            }
        }
    }

    /* RX / TX bytes from /proc/net/dev */
    FILE *dfp = fopen("/proc/net/dev", "r");
    if (dfp) {
        char line[256];
        while (fgets(line, sizeof(line), dfp)) {
            char *p = strstr(line, iface);
            if (!p) continue;
            char *colon = strchr(p, ':');
            if (!colon) continue;
            unsigned long rx_b = 0, tx_b = 0, d1, d2, d3, d4, d5, d6, d7;
            if (sscanf(colon + 1, "%lu %lu %lu %lu %lu %lu %lu %lu %lu",
                       &rx_b, &d1, &d2, &d3, &d4, &d5, &d6, &d7, &tx_b) == 9) {
                t->wifi_rx_kb = rx_b / 1024UL;
                t->wifi_tx_kb = tx_b / 1024UL;
            }
            break;
        }
        fclose(dfp);
    }
#endif

    /* Check dashboard Dropbear SSH PID */
    int pid = 0;
    if (read_file_int("/var/run/kobo-dashboard-ssh.pid", &pid) && pid > 1) {
        t->ssh_running = (kill((pid_t)pid, 0) == 0) ? 1 : 0;
    }
}

static bool is_mountpoint(const char *path) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) return false;
    char line[384];
    bool found = false;
    size_t plen = strlen(path);
    while (fgets(line, sizeof(line), fp)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        sp++;
        if (strncmp(sp, path, plen) == 0 && (sp[plen] == ' ' || sp[plen] == '\0')) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

static void read_storage(Telemetry *t) {
    struct statvfs sv;
    const char *onboard = "/mnt/onboard";
    if (statvfs(onboard, &sv) == 0 && sv.f_blocks > 0) {
        unsigned long long bsz = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        t->onboard_total_mb = (unsigned long)((sv.f_blocks * bsz) / (1024ULL * 1024ULL));
        t->onboard_free_mb  = (unsigned long)((sv.f_bavail * bsz) / (1024ULL * 1024ULL));
    }

    t->sd_mounted = 0;
    if (is_mountpoint("/mnt/sd") && statvfs("/mnt/sd", &sv) == 0 && sv.f_blocks > 0) {
        unsigned long long bsz = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        t->sd_mounted = 1;
        t->sd_total_mb = (unsigned long)((sv.f_blocks * bsz) / (1024ULL * 1024ULL));
        t->sd_free_mb  = (unsigned long)((sv.f_bavail * bsz) / (1024ULL * 1024ULL));
    }
}

void sysinfo_collect(Telemetry *t, const Config *cfg) {
    (void)cfg;
    memset(t, 0, sizeof(*t));
    read_battery(t);
    read_memory_and_cpu(t);
    read_network(t);
    read_storage(t);
}

int sysinfo_toggle_ssh(const Config *cfg, bool enable) {
    char cmd[384];
    snprintf(cmd, sizeof(cmd), "/bin/sh \"%s/ssh-toggle.sh\" %s >/dev/null 2>&1",
             cfg->base_dir, enable ? "start" : "stop");
    return system(cmd);
}

#ifdef __linux__
static bool write_sysfs_str(const char *path, const char *val) {
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    bool ok = (fputs(val, fp) >= 0);
    if (fflush(fp) != 0) ok = false;
    if (fclose(fp) != 0) ok = false;
    return ok;
}
#endif

int sysinfo_enter_deep_sleep(const Config *cfg, time_t wake_utc) {
#ifdef __linux__
    /* 1. Power down Wi-Fi radio cleanly so ar6000 never blocks suspend or wedges on resume */
    if (cfg && cfg->base_dir[0]) {
        char cmd[384];
        snprintf(cmd, sizeof(cmd), "/bin/sh \"%s/wifi-ctl.sh\" off >/dev/null 2>&1", cfg->base_dir);
        (void)system(cmd);
    }

    /* 2. Arm hardware RTC wakealarm if a scheduled wake epoch was requested */
    (void)write_sysfs_str("/sys/class/rtc/rtc0/wakealarm", "0");
    if (wake_utc > time(NULL) + 10) {
        char wbuf[32];
        snprintf(wbuf, sizeof(wbuf), "%lld", (long long)wake_utc);
        (void)write_sysfs_str("/sys/class/rtc/rtc0/wakealarm", wbuf);
    }

    /* 3. Standard safe Kobo suspend sequence (identical to KOReader/Plato; never touches /dev/ntx_io) */
    sync();
    (void)write_sysfs_str("/sys/power/state-extended", "1");
    usleep(200000);

    bool ok = write_sysfs_str("/sys/power/state", "mem");

    /* 4. Resume hardware cleanly */
    (void)write_sysfs_str("/sys/power/state-extended", "0");
    (void)write_sysfs_str("/sys/class/rtc/rtc0/wakealarm", "0");
    usleep(150000);
    return ok ? 0 : -1;
#else
    (void)cfg;
    (void)wake_utc;
    return -1;
#endif
}
