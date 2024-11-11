#include <assert.h>
#include <curl/curl.h>
#include <curses.h>
#include <event.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct DownloadItem {
    int mode;
    char *url;
    char *escape_url;
    char *effective_url;
    char *primary_ip;
    long primary_port;
    long rcode;
    long protocol;
    double contentlength;
    double download_size;
    FILE *outputfile;
    char *outputfilename;
    char *contenttype;
    double progress;
    double uprogress;
    int speed;
    long eta;
    curl_off_t max_speed;
    long int downloaded;
    long int start_time;
    long int end_time;
    CURL *handle;
    struct DownloadItem *next;
    struct DownloadItem *prev;
} DownloadItem;

typedef struct SockInfo {
    curl_socket_t sockfd;
    CURL *easy;
    struct event *ev;
    long timeout;
    int action;
    int evset;
} SockInfo;

#define MODE_ALL         0
#define MODE_INACTIVE    1
#define MODE_PAUSED      2
#define MODE_ACTIVE      3
#define MODE_FINISHED    4
#define NB_MODES         5

int current_mode = MODE_ALL;

#define ENTERING_URL     1
#define ENTERING_REFERER 2
#define ENTERING_SEARCH  3

#define PARAM_REFERER    1
#define PARAM_OUTPUT     2
#define PARAM_MAXTCONN   3
#define PARAM_OUTPUTOVER 4
#define PARAM_INPUTFILE  5
#define PARAM_MAXSPEED   6
#define PARAM_MAXHCONN   7
#define PARAM_AUTOSTART  8
#define PARAM_AUTOEXIT   9

#define MAX_STRING_LEN 16384

char *last_search = NULL;
char *string = NULL;
CURLM *mhandle = NULL;
struct event_base *eventbase = NULL;
struct event *timerevent = NULL;

DownloadItem *items = NULL;
DownloadItem *items_tail = NULL;
DownloadItem *sitem[NB_MODES] = { NULL };

long int start_time = INT_MIN;
int nb_ditems = 0;
int nb_logs = 0;
int string_pos = 0;
int current_page = 0;
int still_running = 0;
int help_active = 0;
int info_active = 0;
int log_active  = 0;
int downloading = 0;
int inactive_downloads = 0;
int paused_downloads = 0;
int active_downloads = 0;
int finished_downloads = 0;
int auto_start = 0;
int auto_exit = 0;

pthread_t curses_thread;
pthread_t curl_thread;

WINDOW *downloads = NULL;
WINDOW *helpwin   = NULL;
WINDOW *infowin   = NULL;
WINDOW *logwin    = NULL;
WINDOW *openwin   = NULL;
WINDOW *statuswin = NULL;

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static char *clonestring(const char *string, size_t string_len)
{
    char *clone = malloc((string_len + 1) * sizeof(*string));

    if (clone) {
        memcpy(clone, string, (string_len + 1) * sizeof(*string));
        clone[string_len] = '\0';
    }

    return clone;
}

static void write_log(int color, const char *fmt, ...)
{
    int y, y0, x;
    va_list vl;

    getyx(logwin, y0, x);
    wattrset(logwin, color);

    va_start(vl, fmt);
    vw_printw(logwin, fmt, vl);
    va_end(vl);

    getyx(logwin, y, x);
    nb_logs += y - y0;
    (void)x;
}

static void check_erc(const char *where, CURLcode code)
{
    if (code == CURLE_OK)
        return;

    write_log(COLOR_PAIR(1), "%s returns %s\n", where, curl_easy_strerror(code));
}

static void check_mrc(const char *where, CURLMcode code)
{
    if (code == CURLM_OK)
        return;
    write_log(COLOR_PAIR(1), "%s returns %s\n", where, curl_multi_strerror(code));
}

static DownloadItem* delete_ditem(DownloadItem *ditem)
{
    int i;

    for (i = 0; i < NB_MODES; i++) {
        if (ditem == sitem[i]) {
            sitem[i] = NULL;
        }
    }

    if (ditem->handle) {
        if (ditem->mode == MODE_ACTIVE) {
            CURLMcode rc;

            rc = curl_multi_remove_handle(mhandle, ditem->handle);
            check_mrc("delete:", rc);
        }
        curl_easy_cleanup(ditem->handle);
    }

    if (ditem->url)
        free(ditem->url);

    if (ditem->escape_url)
        free(ditem->escape_url);

    if (ditem->outputfile)
        fclose(ditem->outputfile);

    if (ditem->outputfilename)
        free(ditem->outputfilename);

    nb_ditems--;
    if (ditem->mode == MODE_INACTIVE) {
        inactive_downloads--;
    } else if (ditem->mode == MODE_PAUSED) {
        paused_downloads--;
    } else if (ditem->mode == MODE_ACTIVE) {
        active_downloads--;
    } else if (ditem->mode == MODE_FINISHED) {
        finished_downloads--;
    }

    if (ditem->prev && ditem->next) {
        DownloadItem *old = ditem;

        ditem = old->prev;
        ditem->next = old->next;
        old->next->prev = ditem;
        ditem = ditem->next;

        if (current_mode) {
            DownloadItem *temp = ditem;

            for (; ditem; ditem = ditem->next) {
                if (ditem->mode == current_mode)
                    break;
            }

            if (!ditem) {
                ditem = temp;

                for (; ditem; ditem = ditem->prev) {
                    if (ditem->mode == current_mode)
                        break;
                }
            }
        }

        free(old);
    } else if (ditem->next) {
        DownloadItem *old;

        old = ditem;
        items = ditem = ditem->next;
        ditem->prev = NULL;

        if (current_mode) {
            for (; ditem; ditem = ditem->next) {
                if (ditem->mode == current_mode)
                    break;
            }
        }

        free(old);
    } else if (ditem->prev) {
        DownloadItem *old;

        old = ditem;
        ditem = items_tail = ditem->prev;
        ditem->next = NULL;

        if (current_mode) {
            for (; ditem; ditem = ditem->prev) {
                if (ditem->mode == current_mode)
                    break;
            }
        }

        free(old);
    } else {
        free(ditem);
        items = ditem = items_tail = NULL;
    }

    return ditem;
}

static void write_status(int color, const char *fmt, ...)
{
    va_list vl;

    werase(statuswin);
    wattrset(statuswin, color);

    va_start(vl, fmt);
    vw_printw(statuswin, fmt, vl);
    va_end(vl);

    wnoutrefresh(statuswin);
}

static void write_helpwin()
{
    int i = 0;

    wattrset(helpwin, COLOR_PAIR(5));
    mvwaddstr(helpwin, i++, 0, " 1 - show all downloads ");
    mvwaddstr(helpwin, i++, 0, " 2 - show only inactive downloads ");
    mvwaddstr(helpwin, i++, 0, " 3 - show only paused downloads ");
    mvwaddstr(helpwin, i++, 0, " 4 - show only active downloads ");
    mvwaddstr(helpwin, i++, 0, " 5 - show only finished downloads ");
    mvwaddstr(helpwin, i++, 0, " a - add new download URL, downloading from last position ");
    mvwaddstr(helpwin, i++, 0, " A - add new download URL, downloading from beginning ");
    mvwaddstr(helpwin, i++, 0, " S - start/stop all downloads ");
    mvwaddstr(helpwin, i++, 0, " H - halt selected download ");
    mvwaddstr(helpwin, i++, 0, " h - unhalt selected download ");
    mvwaddstr(helpwin, i++, 0, " p - pause/unpause selected download ");
    mvwaddstr(helpwin, i++, 0, " D - delete selected download from the list ");
    mvwaddstr(helpwin, i++, 0, " R - set referer for the selected download ");
    mvwaddstr(helpwin, i++, 0, " i - show extra info for selected download ");
    mvwaddstr(helpwin, i++, 0, " l - show log of all downloads ");
    mvwaddstr(helpwin, i++, 0, " / - search for download ");
    mvwaddstr(helpwin, i++, 0, " n - repeat last search ");
    mvwaddstr(helpwin, i++, 0, " N - repeat last search backward ");
    mvwaddstr(helpwin, i++, 0, " UP/DOWN - select download ");
    mvwaddstr(helpwin, i++, 0, " LEFT/RIGHT - decrease/increase download speed ");
    mvwaddstr(helpwin, i++, 0, " Q - quit ");
    wnoutrefresh(helpwin);
}

static void write_infowin(DownloadItem *sitem)
{
    int i = 0;

    if (!sitem)
        return;

    curl_easy_getinfo(sitem->handle, CURLINFO_EFFECTIVE_URL, &sitem->effective_url);
    curl_easy_getinfo(sitem->handle, CURLINFO_RESPONSE_CODE, &sitem->rcode);
    curl_easy_getinfo(sitem->handle, CURLINFO_PROTOCOL, &sitem->protocol);
    curl_easy_getinfo(sitem->handle, CURLINFO_CONTENT_TYPE, &sitem->contenttype);
    curl_easy_getinfo(sitem->handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &sitem->contentlength);
    curl_easy_getinfo(sitem->handle, CURLINFO_SIZE_DOWNLOAD, &sitem->download_size);
    curl_easy_getinfo(sitem->handle, CURLINFO_PRIMARY_IP, &sitem->primary_ip);
    curl_easy_getinfo(sitem->handle, CURLINFO_PRIMARY_PORT, &sitem->primary_port);

    wattrset(infowin, COLOR_PAIR(7));
    mvwprintw(infowin, i++, 0, " Filename: %.*s ", COLS, sitem->outputfilename);
    mvwprintw(infowin, i++, 0, " URL: %.*s ", COLS, sitem->url);
    mvwprintw(infowin, i++, 0, " Effective URL: %.*s ", COLS, sitem->effective_url);
    mvwprintw(infowin, i++, 0, " Current download speed: %ldB/s ", sitem->speed);
    mvwprintw(infowin, i++, 0, " Max download speed: %ldB/s ", sitem->max_speed);
    mvwprintw(infowin, i++, 0, " Response code: %ld ", sitem->rcode);
    mvwprintw(infowin, i++, 0, " Content-type: %s ", sitem->contenttype);
    mvwprintw(infowin, i++, 0, " Content-length: %.0f ", sitem->contentlength);
    mvwprintw(infowin, i++, 0, " Download size:  %.0f ", sitem->download_size);
    mvwprintw(infowin, i++, 0, " Download time: %ld ", sitem->start_time ? ((sitem->end_time ? sitem->end_time : time(NULL)) - sitem->start_time) : 0);
    mvwprintw(infowin, i++, 0, " ETA: %ld ", sitem->eta);
    mvwprintw(infowin, i++, 0, " Primary IP: %s ", sitem->primary_ip);
    mvwprintw(infowin, i++, 0, " Primary port: %ld ", sitem->primary_port);
    mvwprintw(infowin, i++, 0, " Used Protocol: ");
    switch (sitem->protocol) {
    case CURLPROTO_HTTP:   waddstr(infowin, "HTTP");   break;
    case CURLPROTO_HTTPS:  waddstr(infowin, "HTTPS");  break;
    case CURLPROTO_FTP:    waddstr(infowin, "FTP");    break;
    case CURLPROTO_FTPS:   waddstr(infowin, "FTPS");   break;
    case CURLPROTO_SCP:    waddstr(infowin, "SCP");    break;
    case CURLPROTO_SFTP:   waddstr(infowin, "SFTP");   break;
    case CURLPROTO_FILE:   waddstr(infowin, "FILE");   break;
    case CURLPROTO_TFTP:   waddstr(infowin, "TFTP");   break;
    case CURLPROTO_TELNET: waddstr(infowin, "TELNET"); break;
    default:               waddstr(infowin, "unknown");
    }

    wnoutrefresh(infowin);
}

static size_t write_data(void *ptr, size_t size, size_t nmemb, void *ourptr)
{
    FILE *outfile = ourptr;

    return fwrite(ptr, size, nmemb, outfile);
}

static int progressf(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    DownloadItem *item = clientp;
    long int stime = item->start_time ? item->start_time : start_time;
    long int curr_time = time(NULL);
    long int tdiff = curr_time - stime;

    if (dltotal)
        item->progress = 100. * (dlnow + item->downloaded)/(dltotal + item->downloaded);
    else
        item->progress = 0;

    if (ultotal)
        item->uprogress = 100. * ulnow/ultotal;
    else
        item->uprogress = 0;

    if (tdiff != 0)
        item->speed = dlnow / tdiff;
    else
        item->speed = 0;

    if (item->speed)
        item->eta = (dltotal - dlnow) / item->speed;
    else
        item->eta = -1;

    return 0;
}

static int get_fg(DownloadItem *ditem)
{
    int bold = 0;

    if (ditem->mode == MODE_ACTIVE)
        bold = A_BOLD;

    if (ditem->mode == MODE_INACTIVE) {
        return ditem == sitem[current_mode] ? bold | A_REVERSE | COLOR_PAIR(5) : bold | A_REVERSE | COLOR_PAIR(4);
    } else {
        return ditem == sitem[current_mode] ? bold | A_REVERSE | COLOR_PAIR(3) : bold | A_REVERSE | COLOR_PAIR(2);
    }
}

static int get_bg(DownloadItem *ditem)
{
    int bold = 0;

    if (ditem->mode == MODE_ACTIVE)
        bold = A_BOLD;

    if (ditem->mode == MODE_INACTIVE) {
        return ditem == sitem[current_mode] ? bold | COLOR_PAIR(5) : bold | COLOR_PAIR(4);
    } else {
        return ditem == sitem[current_mode] ? bold | COLOR_PAIR(3) : bold | COLOR_PAIR(2);
    }
}

static void uninit()
{
    DownloadItem *item = items_tail;

    pthread_cancel(curl_thread);
    pthread_cancel(curses_thread);

    clear();
    refresh();
    endwin();
    delwin(openwin);
    delwin(infowin);
    delwin(helpwin);
    delwin(statuswin);
    delwin(logwin);
    delwin(downloads);

    for (;item;)
        item = delete_ditem(item);

    curl_multi_cleanup(mhandle);
    mhandle = NULL;
    curl_global_cleanup();
    items = NULL;
    items_tail = NULL;
    free(last_search);
    last_search = NULL;
    free(string);
    string = NULL;

    event_free(timerevent);
    timerevent = NULL;
    event_base_free(eventbase);
    eventbase = NULL;
}

static void error(int sig, const char *error_msg)
{
    uninit();

    fprintf(stderr, "%s", error_msg);

    exit(sig);
}

static void finish(int sig)
{
    uninit();

    exit(sig);
}

static int create_handle(int overwritefile, const char *newurl,
                         const char *referer, const char *outname,
                         curl_off_t speed)
{
    DownloadItem *item;
    FILE *outputfile = NULL;
    const char *ofilename;
    const char *lpath;
    char *unescape;
    CURL *handle;
    CURLcode rc;
    int urllen, escape_url_size, i;

    string_pos = 0;
    urllen = strlen(newurl);
    if (urllen <= 1) {
        werase(openwin);
        return 1;
    }

    for (item = items; item; item = item->next) {
        if (item->url && !strcmp(newurl, item->url)) {
            write_status(A_REVERSE | COLOR_PAIR(1), "URL already in use");
            return 1;
        }
    }

    if (items == NULL) {
        items = calloc(1, sizeof(DownloadItem));
        if (!items) {
            write_status(A_REVERSE | COLOR_PAIR(1), "Failed to allocate DownloadItem");
            return 1;
        }
        item = items_tail = items;
    } else {
        DownloadItem *prev;

        item = items_tail;
        item->next = calloc(1, sizeof(DownloadItem));
        if (!item->next) {
            write_status(A_REVERSE | COLOR_PAIR(1), "Failed to allocate DownloadItem");
            return 1;
        }

        prev = item;
        item = items_tail = item->next;
        item->prev = prev;
    }

    item->url = clonestring(newurl, urllen);
    if (!item->url) {
        write_status(A_REVERSE | COLOR_PAIR(1), "Failed to copy URL");
        delete_ditem(item);
        return 1;
    }

    item->handle = handle = curl_easy_init();
    if (!handle) {
        write_status(A_REVERSE | COLOR_PAIR(1), "Failed to create curl easy handle");
        delete_ditem(item);
        return 1;
    }

    unescape = curl_easy_unescape(handle, newurl, urllen, NULL);
    if (!unescape) {
        write_status(A_REVERSE | COLOR_PAIR(1), "Failed to unescape URL");
        delete_ditem(item);
        return 1;
    }

    lpath = strrchr(unescape, '/');
    if (!lpath) {
        curl_free(unescape);
        write_status(A_REVERSE | COLOR_PAIR(1), "Invalid URL");
        delete_ditem(item);
        return 1;
    }

    lpath += 1;
    if (!strcmp(unescape, newurl)) {
        char *escape = curl_easy_escape(handle, lpath, 0);
        if (!escape) {
            curl_free(unescape);
            write_status(A_REVERSE | COLOR_PAIR(1), "Failed to escape URL");
            delete_ditem(item);
            return 1;
        }
        escape_url_size = strlen(escape) + strlen(newurl);
        item->escape_url = calloc(escape_url_size, sizeof(*item->escape_url));
        if (!item->escape_url) {
            curl_free(unescape);
            write_status(A_REVERSE | COLOR_PAIR(1), "Failed to allocate escape URL");
            delete_ditem(item);
            return 1;
        }

        for (i = urllen - 1; i >= 0; i--) {
            if (newurl[i] == '/')
                break;
        }
        snprintf(item->escape_url, escape_url_size, "%.*s/%s", i, newurl, escape);
        curl_free(escape);
    } else {
        item->escape_url = clonestring(newurl, urllen);
        if (!item->escape_url) {
            curl_free(unescape);
            write_status(A_REVERSE | COLOR_PAIR(1), "Failed to duplicate url");
            delete_ditem(item);
            return 1;
        }
    }

    ofilename = outname ? outname : lpath;
    item->outputfilename = clonestring(ofilename, strlen(ofilename));
    item->max_speed = speed;
    curl_free(unescape);

    if (!item->outputfilename) {
        write_status(A_REVERSE | COLOR_PAIR(1), "Failed to duplicate output filename");
        delete_ditem(item);
        return 1;
    }

    if (!overwritefile)
        item->outputfile = outputfile = fopen(item->outputfilename, "rb+");
    if (!outputfile)
        item->outputfile = outputfile = fopen(item->outputfilename, "wb");
    if (!outputfile) {
        write_status(A_REVERSE | COLOR_PAIR(1), "Failed to open file: %s", item->outputfilename);
        delete_ditem(item);
        return 1;
    }

    rc = curl_easy_setopt(handle, CURLOPT_URL, item->escape_url);
    check_erc("url:", rc);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(handle, CURLOPT_PRIVATE, item);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, item);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progressf);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, outputfile);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_AUTOREFERER, 1L);
    curl_easy_setopt(handle, CURLOPT_MAX_RECV_SPEED_LARGE, item->max_speed);
    if (referer) {
        rc = curl_easy_setopt(handle, CURLOPT_REFERER, referer);
        check_erc("referer:", rc);
    }

    if (!overwritefile) {
        curl_off_t from;
        fseek(outputfile, 0, SEEK_END);
        from = item->downloaded = ftell(outputfile);
        rc = curl_easy_setopt(handle, CURLOPT_RESUME_FROM_LARGE, from);
        check_erc("resume:", rc);
    }
    item->mode = MODE_PAUSED;
    paused_downloads++;
    nb_ditems++;

    return 0;
}

static void write_downloads()
{
    DownloadItem *item = items;
    int line, cline = -1, offset;

    wmove(downloads, 0, 0);

    item = items;
    for (line = 0; item; item = item->next) {
        double progress = item->progress;
        char speedstr[128] = { 0 };
        char progstr[8] = { 0 };
        int speed = item->speed;
        char *namestr = item->outputfilename;
        int j, pos = progress * COLS / 100;
        int fg = get_fg(item);
        int bg = get_bg(item);
        int namestrlen, k, l;
        int speedstrlen;
        int progstrlen;

        if (current_mode && item->mode != current_mode) {
            continue;
        }

        if (item == sitem[current_mode]) {
            cline = line;
        }

        snprintf((char *)&progstr, sizeof(progstr), "%3.2f%%", progress);
        snprintf((char *)&speedstr, sizeof(speedstr), "%dKB/s", speed / 1024);
        namestrlen = strlen(namestr);
        speedstrlen = strlen(speedstr);
        progstrlen = strlen(progstr);
        for (j = 0, k = 0, l = 0; j < COLS; j++) {
            if (j < pos)
                wattrset(downloads, fg);
            else
                wattrset(downloads, bg);
            if (j < namestrlen)
                mvwaddch(downloads, line, j, namestr[j]);
            else if (j >= COLS/2 && l < speedstrlen)
                mvwaddch(downloads, line, j, speedstr[l++]);
            else if (j >= COLS - progstrlen && k < progstrlen)
                mvwaddch(downloads, line, j, progstr[k++]);
            else
                mvwaddch(downloads, line, j, ' ');
        }

        line++;
    }

    if (cline >= 0) {
        offset = MAX(cline - (LINES - 2), 0);
        current_page = cline / (LINES - 1);
    } else {
        offset = current_page * (LINES - 1);
    }
    wclrtobot(downloads);

    pnoutrefresh(downloads, offset, 0, 0, 0, LINES-1, COLS);
}

static void write_logwin()
{
    pnoutrefresh(logwin, MAX(nb_logs - LINES, 0), 0, 0, 0, LINES - 1, COLS);
}

static void write_statuswin(int downloading)
{
    wattrset(statuswin, COLOR_PAIR(7));
    mvwprintw(statuswin, 0, 0,  "[");
    wattrset(statuswin, A_BOLD | COLOR_PAIR(4));
    waddstr(statuswin, "I");
    wattrset(statuswin, COLOR_PAIR(7));
    wprintw(statuswin, ":%d ", inactive_downloads);
    wattrset(statuswin, COLOR_PAIR(2));
    waddstr(statuswin, "P");
    wattrset(statuswin, COLOR_PAIR(7));
    wprintw(statuswin, ":%d ", paused_downloads);
    wattrset(statuswin, A_BOLD | COLOR_PAIR(2));
    waddstr(statuswin, "A");
    wattrset(statuswin, COLOR_PAIR(7));
    wprintw(statuswin, ":%d ", active_downloads);
    wattrset(statuswin, A_REVERSE | COLOR_PAIR(2));
    waddstr(statuswin, "F");
    wattrset(statuswin, COLOR_PAIR(7));
    wprintw(statuswin, ":%d ", finished_downloads);
    wprintw(statuswin, "N:%d", nb_ditems);
    if (start_time != INT_MIN && downloading)
        wprintw(statuswin, " T:%ld", time(NULL) - start_time);
    wprintw(statuswin, "] ");
    wclrtoeol(statuswin);
    mvwprintw(statuswin, 0, COLS-12, " Help (F1) ");
    wnoutrefresh(statuswin);
}

static void add_handle(DownloadItem *ditem)
{
    CURLMcode rc;
    curl_off_t from;
    int still_running;

    fseek(ditem->outputfile, 0, SEEK_END);
    from = ditem->downloaded = ftell(ditem->outputfile);
    curl_easy_setopt(ditem->handle, CURLOPT_RESUME_FROM_LARGE, from);
    ditem->start_time = time(NULL);
    ditem->end_time = 0;
    rc = curl_multi_add_handle(mhandle, ditem->handle);
    check_mrc("add:", rc);
    curl_multi_socket_action(mhandle, CURL_SOCKET_TIMEOUT, 0, &still_running);
}

static void remove_handle(DownloadItem *ditem)
{
    CURLMcode rc;
    rc = curl_multi_remove_handle(mhandle, ditem->handle);
    check_mrc("remove:", rc);
    ditem->end_time = time(NULL);
}

static void set_timeouts(int check)
{
    if (check) {
        wtimeout(downloads, 100);
        wtimeout(openwin, 100);
    } else {
        wtimeout(downloads, -1);
        wtimeout(openwin, -1);
    }
}

static void init_windows()
{
    downloads = newpad(4096, COLS);
    if (!downloads) {
        error(-1, "Failed to create downloads window.\n");
    }

    logwin = newpad(4096, COLS);
    if (!logwin) {
        error(-1, "Failed to create log window.\n");
    }

    statuswin = newwin(1, COLS, LINES-1, 0);
    if (!statuswin) {
        error(-1, "Failed to create status window.\n");
    }

    helpwin = newwin(LINES-1, COLS, 0, 0);
    if (!statuswin) {
        error(-1, "Failed to create help window.\n");
    }

    infowin = newwin(LINES-1, COLS, 0, 0);
    if (!infowin) {
        error(-1, "Failed to create info window.\n");
    }

    openwin = newwin(1, COLS, LINES/2, 0);
    if (!openwin) {
        error(-1, "Failed to create open window.\n");
    }

    keypad(downloads,  TRUE);
    keypad(openwin,    TRUE);

    leaveok(downloads, TRUE);
    leaveok(helpwin,   TRUE);
    leaveok(infowin,   TRUE);
    leaveok(logwin,    TRUE);
    leaveok(openwin,   TRUE);
    leaveok(statuswin, TRUE);
}

static void auto_startall()
{
    DownloadItem *item;

    if (!auto_start)
        return;

    for (item = items; item; item = item->next) {
        item->mode = MODE_ACTIVE;
        add_handle(item);
        active_downloads++;
    }
}

static int parse_file(char *filename)
{
    FILE *file = fopen(filename, "r");

    if (!file)
        return -1;

    while (fgets(string, MAX_STRING_LEN, file) != NULL) {
        size_t len = strlen(string);

        if (len > 0 && string[len-1] == '\n')
            string[len-1] = '\0';
        create_handle(0, string, NULL, NULL, 0);
    }

    return 0;
}

static int parse_parameters(int argc, char *argv[],
                            long *max_total_connections,
                            long *max_host_connections)
{
    const char *referer = NULL;
    const char *output = NULL;
    long max = 0, maxh = 0, speed = 0;
    int overwritefile = 0;
    int i, param = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-R")) {
            param = PARAM_REFERER;
        } else if (!strcmp(argv[i], "-o")) {
            param = PARAM_OUTPUT;
            overwritefile = 0;
        } else if (!strcmp(argv[i], "-M")) {
            param = PARAM_MAXTCONN;
        } else if (!strcmp(argv[i], "-O")) {
            param = PARAM_OUTPUTOVER;
            overwritefile = 1;
        } else if (!strcmp(argv[i], "-i")) {
            param = PARAM_INPUTFILE;
        } else if (!strcmp(argv[i], "-s")) {
            param = PARAM_MAXSPEED;
        } else if (!strcmp(argv[i], "-H")) {
            param = PARAM_MAXHCONN;
        } else if (!strcmp(argv[i], "-X")) {
            param = PARAM_AUTOEXIT;
        } else if (!strcmp(argv[i], "-x")) {
            param = PARAM_AUTOSTART;
        } else {
            if (param == PARAM_REFERER) {
                referer = argv[i];
            } else if (param == PARAM_OUTPUT || param == PARAM_OUTPUTOVER) {
                output = argv[i];
            } else if (param == PARAM_MAXTCONN) {
                max = atol(argv[i]);
            } else if (param == PARAM_MAXHCONN) {
                maxh = atol(argv[i]);
            } else if (param == PARAM_INPUTFILE) {
                parse_file(argv[i]);
            } else if (param == PARAM_MAXSPEED) {
                speed = atol(argv[i]);
            } else if (param == PARAM_AUTOEXIT) {
                auto_exit = !!atol(argv[i]);
            } else if (param == PARAM_AUTOSTART) {
                auto_start = !!atol(argv[i]);
            } else {
                create_handle(overwritefile, argv[i], referer, output, speed);
                referer = output = NULL;
                overwritefile = 0;
                speed = 0;
            }
            param = 0;
        }
    }

    *max_total_connections = max;
    *max_host_connections = maxh;

    return i;
}

static void check_multi_info()
{
    char *eff_url;
    CURLMsg *msg;
    int msgs_left;
    DownloadItem *ditem;
    CURL *easy;

    while ((msg = curl_multi_info_read(mhandle, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            easy = msg->easy_handle;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &ditem);
            curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff_url);
            remove_handle(ditem);
            ditem->mode = MODE_FINISHED;
            ditem->progress = 100.;
            finished_downloads++;
            active_downloads--;
            write_log(COLOR_PAIR(7), "Finished downloading %s.\n", ditem->outputfilename);
        }
    }

    if (auto_exit && (finished_downloads > 0) && (finished_downloads == nb_ditems))
        finish(0);
}

static void timer_cb(int fd, short kind, void *userp)
{
    CURLMcode rc;
    (void)fd;
    (void)kind;
    (void)userp;

    rc = curl_multi_socket_action(mhandle, CURL_SOCKET_TIMEOUT, 0, &still_running);
    check_mrc("timer_cb:", rc);
    check_multi_info();
    downloading = still_running > 0;
}

static int multi_timer_cb(CURLM *multi, long timeout_ms, void *unused)
{
    (void)multi;
    (void)unused;

    if (timeout_ms > 0) {
        struct timeval timeout;

        timeout.tv_sec = timeout_ms/1000;
        timeout.tv_usec = (timeout_ms%1000)*1000;

        evtimer_add(timerevent, &timeout);
    } else if (timeout_ms == 0) {
        timer_cb(0, 0, NULL);
    }

    return 0;
}

static void remove_sock(SockInfo *f)
{
    if (f) {
        if (f->evset)
            event_free(f->ev);
        free(f);
    }
}

static void event_cb(int fd, short kind, void *userp)
{
    CURLMcode rc;
    (void)userp;

    int action = (kind & EV_READ ? CURL_CSELECT_IN : 0) | (kind & EV_WRITE ? CURL_CSELECT_OUT : 0);

    rc = curl_multi_socket_action(mhandle, fd, action, &still_running);
    check_mrc("event_cb:", rc);

    check_multi_info();
    downloading = still_running > 0;
    if (still_running <= 0) {
        if (evtimer_pending(timerevent, NULL)) {
            evtimer_del(timerevent);
        }
    }
}

static void set_sock(SockInfo *f, curl_socket_t s, CURL *e, int act)
{
    int kind = (act&CURL_POLL_IN?EV_READ:0)|(act&CURL_POLL_OUT?EV_WRITE:0)|EV_PERSIST;

    f->sockfd = s;
    f->action = act;
    f->easy = e;
    if (f->evset)
        event_free(f->ev);
    f->ev = event_new(eventbase, f->sockfd, kind, event_cb, NULL);
    f->evset = 1;
    event_add(f->ev, NULL);
}

static void add_sock(curl_socket_t s, CURL *easy, int action)
{
    SockInfo *fdp = calloc(1, sizeof(SockInfo));

    set_sock(fdp, s, easy, action);
    curl_multi_assign(mhandle, s, fdp);
}

static int sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp)
{
    SockInfo *fdp = (SockInfo*)sockp;
    (void)cbp;

    if (what == CURL_POLL_REMOVE) {
        remove_sock(fdp);
    } else {
        if (!fdp) {
            add_sock(s, e, what);
        } else {
            set_sock(fdp, s, e, what);
        }
    }

    return 0;
}

static void *do_curl(void *unused)
{
    (void)unused;

    for (;;) {
        event_base_loop(eventbase, 0);
        sleep(1);
    }

    return NULL;
}

static void *do_ncurses(void *unused)
{
    int active_input = 0;
    int overwritefile = 0;
    (void)unused;

    for (;;) {
        int c;

        if (active_input) {
            int skip_y, skip_x;

            wclrtoeol(openwin);
            if (active_input == ENTERING_URL)
                mvwaddstr(openwin, 0, 0, "URL: ");
            else if (active_input == ENTERING_REFERER)
                mvwaddstr(openwin, 0, 0, "Referer: ");
            else if (active_input == ENTERING_SEARCH)
                mvwaddstr(openwin, 0, 0, "Search: ");
            getyx(openwin, skip_y, skip_x);

            c = wgetch(openwin);
            if (c == KEY_ENTER || c == '\n' || c == '\r') {
                if (active_input == ENTERING_URL && create_handle(overwritefile, string, NULL, NULL, 0)) {
                    active_input = 0;
                    doupdate();
                    continue;
                } else if (active_input == ENTERING_REFERER) {
                    if (sitem[current_mode])
                        curl_easy_setopt(sitem[current_mode]->handle, CURLOPT_REFERER, string);
                } else if (active_input == ENTERING_SEARCH) {
                    DownloadItem *nsitem = items;

                    for (;nsitem; nsitem = nsitem->next) {
                        if ((nsitem->mode == current_mode || !current_mode) &&
                            strstr(nsitem->outputfilename, string)) {
                            sitem[current_mode] = nsitem;
                            break;
                        }
                    }
                    free(last_search);
                    last_search = clonestring(string, strlen(string));
                }
                string_pos = 0;
                string[0] = '\0';
                active_input = 0;
                werase(openwin);
            } else if (c == KEY_BACKSPACE || c == KEY_DC || c == '\b' || c == 127 || c == 8) {
                werase(openwin);
                if (string_pos > 0) {
                    string[string_pos - 1] = 0;
                    string_pos--;
                }
            } else if (!(c & KEY_CODE_YES)) {
                if (string_pos < MAX_STRING_LEN - 1) {
                    string[string_pos++] = c;
                    string[string_pos] = 0;
                }
            }
            mvwaddstr(openwin, skip_y, skip_x, string + MAX((signed)strlen(string) + skip_x - COLS, 0));
            mvwchgat(openwin, skip_y, MIN(skip_x + (signed)strlen(string), COLS-1), 1, A_BLINK | A_REVERSE, 2, NULL);
        } else if (!active_input) {
            c = wgetch(downloads);

            if (c == '1') {
                current_mode = MODE_ALL;
            } else if (c == '2') {
                current_mode = MODE_INACTIVE;
            } else if (c == '3') {
                current_mode = MODE_PAUSED;
            } else if (c == '4') {
                current_mode = MODE_ACTIVE;
            } else if (c == '5') {
                current_mode = MODE_FINISHED;
            } else if (c == KEY_F(1) || c == '?') {
                help_active = !help_active;
            } else if (c == 'i') {
                info_active = !info_active;
            } else if (c == 'l') {
                log_active = !log_active;
            } else if (c == 'A' || c == 'a') {
                overwritefile = c == 'A';

                if (!active_input) {
                    active_input = ENTERING_URL;
                    continue;
                }
            } else if (c == 'Q') {
                finish(0);
            } else if (c == 'S') {
                downloading = !downloading;
                if (downloading && items) {
                    DownloadItem *item = items;

                    wtimeout(downloads, 100);
                    wtimeout(openwin, 100);
                    start_time = time(NULL);

                    for (;item;) {
                        if (item->mode == MODE_PAUSED) {
                            item->mode = MODE_ACTIVE;
                            add_handle(item);
                            active_downloads++;
                            paused_downloads--;
                        }
                        item = item->next;
                    }
                } else if (items) {
                    DownloadItem *item = items;
                    for (;item;) {
                        if (item->mode == MODE_ACTIVE) {
                            item->mode = MODE_PAUSED;
                            paused_downloads++;
                            active_downloads--;
                            remove_handle(item);
                        }
                        item = item->next;
                    }

                    wtimeout(downloads, -1);
                    wtimeout(openwin, -1);
                }
            } else if (c == 'h') {
                if (sitem[current_mode] && sitem[current_mode]->mode == MODE_INACTIVE) {
                    sitem[current_mode]->mode = MODE_PAUSED;
                    inactive_downloads--;
                    paused_downloads++;
                }
            } else if (c == 'D') {
                if (sitem[current_mode] && (!current_mode || (sitem[current_mode]->mode == current_mode))) {
                    sitem[current_mode] = delete_ditem(sitem[current_mode]);
                }
            } else if (c == 'R') {
                if (sitem[current_mode]) {
                    if (!active_input) {
                        active_input = ENTERING_REFERER;
                        continue;
                    }
                }
            } else if (c == '/') {
                if (!active_input) {
                    active_input = ENTERING_SEARCH;
                    continue;
                }
            } else if (c == 'n') {
                if (last_search) {
                    DownloadItem *nsitem = sitem[current_mode] ? sitem[current_mode]->next : items;

                    for (;nsitem; nsitem = nsitem->next) {
                        if ((nsitem->mode == current_mode || !current_mode) &&
                            strstr(nsitem->outputfilename, last_search)) {
                            sitem[current_mode] = nsitem;
                            break;
                        }
                    }
                }
            } else if (c == 'N') {
                if (last_search) {
                    DownloadItem *nsitem = sitem[current_mode] ? sitem[current_mode]->prev : items_tail;

                    for (;nsitem; nsitem = nsitem->prev) {
                        if ((nsitem->mode == current_mode || !current_mode) &&
                            strstr(nsitem->outputfilename, last_search)) {
                            sitem[current_mode] = nsitem;
                            break;
                        }
                    }
                }
            } else if (c == 'H') {
                if (sitem[current_mode] && sitem[current_mode]->mode != MODE_INACTIVE) {
                    if (sitem[current_mode]->mode == MODE_ACTIVE) {
                        active_downloads--;
                    } else if (sitem[current_mode]->mode == MODE_FINISHED) {
                        finished_downloads--;
                    } else if (sitem[current_mode]->mode == MODE_PAUSED) {
                        paused_downloads--;
                    }
                    sitem[current_mode]->mode = MODE_INACTIVE;
                    inactive_downloads++;
                    remove_handle(sitem[current_mode]);
                }
            } else if (c == 'p') {
                if (sitem[current_mode] && (sitem[current_mode]->mode == MODE_ACTIVE ||
                                            sitem[current_mode]->mode == MODE_PAUSED)) {
                    if (sitem[current_mode]->mode == MODE_ACTIVE) {
                        remove_handle(sitem[current_mode]);
                        paused_downloads++;
                        active_downloads--;
                        sitem[current_mode]->mode = MODE_PAUSED;
                    } else {
                        sitem[current_mode]->mode = MODE_ACTIVE;
                        wtimeout(downloads, 100);
                        wtimeout(openwin, 100);
                        downloading = 1;
                        paused_downloads--;
                        active_downloads++;
                        add_handle(sitem[current_mode]);
                        if (start_time == INT_MIN)
                            start_time = sitem[current_mode]->start_time;
                    }

                    if (current_mode) {
                        DownloadItem *temp = sitem[current_mode];

                        for (; sitem[current_mode]; sitem[current_mode] = sitem[current_mode]->next) {
                            if (sitem[current_mode]->mode == current_mode)
                                break;
                        }

                        if (!sitem[current_mode]) {
                            sitem[current_mode] = temp;

                            for (; sitem[current_mode]; sitem[current_mode] = sitem[current_mode]->prev) {
                                if (sitem[current_mode]->mode == current_mode)
                                    break;
                            }
                        }
                    }

                }
            } else if (c == KEY_DOWN) {
                if (sitem[current_mode] && sitem[current_mode]->next) {
                    DownloadItem *item = sitem[current_mode]->next;

                    if (current_mode) {
                        for (;item; item = item->next) {
                            if (item->mode == current_mode)
                                break;
                        }
                    }
                    if (item && (item->mode == current_mode || !current_mode))
                        sitem[current_mode] = item;
                }

                if (!sitem[current_mode]) {
                    DownloadItem *item = items;

                    if (current_mode) {
                        for (;item; item = item->next) {
                            if (item->mode == current_mode)
                                break;
                        }
                    }

                    sitem[current_mode] = item;
                }
            } else if (c == KEY_UP) {
                if (sitem[current_mode] && sitem[current_mode]->prev) {
                    DownloadItem *item = sitem[current_mode]->prev;

                    if (current_mode) {
                        for (;item; item = item->prev) {
                            if (item->mode == current_mode)
                                break;
                        }
                    }
                    if (item && (item->mode == current_mode || !current_mode))
                        sitem[current_mode] = item;
                }

                if (!sitem[current_mode]) {
                    DownloadItem *item = items;

                    if (current_mode) {
                        for (;item; item = item->prev) {
                            if (item->mode == current_mode)
                                break;
                        }
                    }

                    sitem[current_mode] = item;
                }
            } else if (c == KEY_NPAGE) {
                if (!sitem[current_mode]) {
                    current_page++;
                    current_page = MIN(current_page, nb_ditems / (LINES-1));
                } else {
                    if (sitem[current_mode]->next) {
                        int i;

                        sitem[current_mode] = sitem[current_mode]->next;
                        for (i = 0; i < LINES-1; i++) {
                            if (!sitem[current_mode]->next)
                                break;
                            sitem[current_mode] = sitem[current_mode]->next;
                        }
                    }
                }
            } else if (c == KEY_PPAGE) {
                if (!sitem[current_mode]) {
                    current_page--;
                    current_page = MAX(0, current_page);
                } else {
                    if (sitem[current_mode]->prev) {
                        int i;

                        sitem[current_mode] = sitem[current_mode]->prev;
                        for (i = 0; i < LINES-1; i++) {
                            if (!sitem[current_mode]->prev)
                                break;
                            sitem[current_mode] = sitem[current_mode]->prev;
                        }
                    }
                }
            } else if (c == KEY_RIGHT) {
                if (sitem[current_mode]) {
                    sitem[current_mode]->max_speed += 1024;
                    curl_easy_setopt(sitem[current_mode]->handle, CURLOPT_MAX_RECV_SPEED_LARGE,
                                     sitem[current_mode]->max_speed);
                }
            } else if (c == KEY_LEFT) {
                if (sitem[current_mode]) {
                    sitem[current_mode]->max_speed = MAX(0, sitem[current_mode]->max_speed - 1024);
                    curl_easy_setopt(sitem[current_mode]->handle, CURLOPT_MAX_RECV_SPEED_LARGE,
                                     sitem[current_mode]->max_speed);
                }
            } else if (c == KEY_HOME) {
                if (sitem[current_mode]) {
                    DownloadItem *item = items;
                    for (;item; item = item->next) {
                        if (item->mode == current_mode || !current_mode)
                            break;
                    }
                    sitem[current_mode] = item;
                }
            } else if (c == KEY_END) {
                if (sitem[current_mode]) {
                    DownloadItem *item = items_tail;
                    for (;item; item = item->prev) {
                        if (item->mode == current_mode || !current_mode)
                            break;
                    }
                    sitem[current_mode] = item;
                }
            } else if (c == KEY_RESIZE) {
                delwin(openwin);
                delwin(infowin);
                delwin(helpwin);
                delwin(statuswin);
                delwin(logwin);
                delwin(downloads);
                clear();
                refresh();
                endwin();

                init_windows();
                set_timeouts(downloading);

                help_active = 0;
                active_input = 0;
                current_page = 0;
            } else if (c == KEY_MOUSE) {
                MEVENT mouse_event;
                int y;

                if (getmouse(&mouse_event) == OK) {
                    sitem[current_mode] = items;
                    for (y = 0; sitem[current_mode]->next; y++) {
                        if (y == ((current_page * (LINES - 1)) + mouse_event.y))
                            break;
                        sitem[current_mode] = sitem[current_mode]->next;
                    }
                }
            }
        }

        write_downloads();
        write_statuswin(downloading);

        set_timeouts(downloading);

        if (info_active)
            write_infowin(sitem[current_mode]);

        if (log_active)
            write_logwin();

        if (help_active)
            write_helpwin();

        doupdate();
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    long max_total_connections = 0;
    long max_host_connections = 0;

    signal(SIGINT, finish);

    mhandle = curl_multi_init();
    if (!mhandle) {
        error(-1, "Failed to create curl multi handle.\n");
    }

    eventbase = event_base_new();
    if (!eventbase) {
        error(-1, "Failed to create new event base.\n");
    }

    timerevent = evtimer_new(eventbase, timer_cb, NULL);
    if (!timerevent) {
        error(-1, "Failed to create new event timer.\n");
    }

    string = calloc(MAX_STRING_LEN, sizeof(*string));
    if (!string) {
        error(-1, "Failed to allocate string storage.\n");
    }

    curl_multi_setopt(mhandle, CURLMOPT_SOCKETFUNCTION, sock_cb);
    curl_multi_setopt(mhandle, CURLMOPT_SOCKETDATA, NULL);
    curl_multi_setopt(mhandle, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
    curl_multi_setopt(mhandle, CURLMOPT_TIMERDATA, NULL);

    initscr();
    nonl();
    cbreak();
    noecho();
    curs_set(0);

    mousemask(ALL_MOUSE_EVENTS, NULL);
    mouseinterval(0);

    if (has_colors()) {
        start_color();

        init_pair(1, COLOR_RED,     COLOR_BLACK);
        init_pair(2, COLOR_GREEN,   COLOR_BLACK);
        init_pair(3, COLOR_YELLOW,  COLOR_BLACK);
        init_pair(4, COLOR_BLUE,    COLOR_BLACK);
        init_pair(5, COLOR_CYAN,    COLOR_BLACK);
        init_pair(6, COLOR_MAGENTA, COLOR_BLACK);
        init_pair(7, COLOR_WHITE,   COLOR_BLACK);
    }

    init_windows();

    if (parse_parameters(argc, argv, &max_total_connections, &max_host_connections))
        write_downloads();

    curl_multi_setopt(mhandle, CURLMOPT_MAX_TOTAL_CONNECTIONS, max_total_connections);
    curl_multi_setopt(mhandle, CURLMOPT_MAX_HOST_CONNECTIONS, max_host_connections);

    auto_startall();

    write_statuswin(downloading);
    doupdate();

    set_timeouts(downloading);

    pthread_create(&curses_thread, NULL, do_ncurses, NULL);
    pthread_create(&curl_thread, NULL, do_curl, NULL);

    pthread_join(curses_thread, NULL);
    pthread_join(curl_thread, NULL);

    finish(0);
}
