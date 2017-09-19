#include <assert.h>
#include <curl/curl.h>
#include <curses.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct DownloadItem {
    int paused;
    int inactive;
    int selected;
    int finished;
    int ufinished;
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
    double progress;
    double uprogress;
    int speed;
    curl_off_t max_speed;
    long int downloaded;
    long int start_time;
    long int end_time;
    CURL *handle;
    struct DownloadItem *next;
    struct DownloadItem *prev;
} DownloadItem;

#define ENTERING_URL     1
#define ENTERING_REFERER 2
#define ENTERING_SEARCH  3

#define PARAM_REFERER    1
#define PARAM_OUTPUT     2
#define PARAM_MAXTCONN   3
#define PARAM_OUTPUTOVER 4
#define PARAM_INPUTFILE  5
#define PARAM_MAXSPEED   6

#define MAX_STRING_LEN 16384

char *last_search = NULL;
char *string = NULL;
int start_all = 0;
CURLM *mhandle = NULL;
DownloadItem *items = NULL;
DownloadItem *items_tail = NULL;
long int start_time = INT_MIN;
int nb_ditems = 0;
int string_pos = 0;

WINDOW *infowin   = NULL;
WINDOW *openwin   = NULL;
WINDOW *helpwin   = NULL;
WINDOW *statuswin = NULL;
WINDOW *downloads = NULL;

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

static DownloadItem* delete_ditem(DownloadItem *ditem)
{
    if (ditem->handle) {
        if (!ditem->inactive)
            curl_multi_remove_handle(mhandle, ditem->handle);
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

    if (ditem->prev && ditem->next) {
        DownloadItem *old = ditem;

        ditem = old->prev;
        ditem->next = old->next;
        old->next->prev = ditem;
        ditem->next->selected = 1;
        ditem = ditem->next;
        free(old);
    } else if (ditem->next) {
        DownloadItem *old;

        old = ditem;
        items = ditem = ditem->next;
        ditem->prev = NULL;
        ditem->selected = 1;
        free(old);
    } else if (ditem->prev) {
        DownloadItem *old;

        old = ditem;
        ditem = items_tail = ditem->prev;
        ditem->next = NULL;
        ditem->selected = 1;
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
    vwprintw(statuswin, fmt, vl);
    va_end(vl);

    wnoutrefresh(statuswin);
}

static void write_helpwin()
{
    int i = 0;

    wattrset(helpwin, COLOR_PAIR(5));
    mvwaddstr(helpwin, i++, 0, " a - add new download URL, downloading from last position ");
    mvwaddstr(helpwin, i++, 0, " A - add new download URL, downloading from beginning ");
    mvwaddstr(helpwin, i++, 0, " S - start/stop all downloads ");
    mvwaddstr(helpwin, i++, 0, " H - halt selected download ");
    mvwaddstr(helpwin, i++, 0, " h - unhalt selected download ");
    mvwaddstr(helpwin, i++, 0, " p - pause/unpause selected download ");
    mvwaddstr(helpwin, i++, 0, " D - delete selectedd download from the list ");
    mvwaddstr(helpwin, i++, 0, " R - set referer for the selected download ");
    mvwaddstr(helpwin, i++, 0, " i - show extra info for selected download ");
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
    mvwprintw(infowin, i++, 0, " Content-length: %f ", sitem->contentlength);
    mvwprintw(infowin, i++, 0, " Download size: %f ", sitem->download_size);
    mvwprintw(infowin, i++, 0, " Download time: %ld ", sitem->start_time ? ((sitem->end_time ? sitem->end_time : time(NULL)) - sitem->start_time) : 0);
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

    item->ufinished = (ultotal != 0 && ulnow == ultotal);

    if (tdiff != 0)
        item->speed = dlnow / tdiff;
    else
        item->speed = 0;

    return 0;
}

static int get_fg(DownloadItem *ditem)
{
    int bold = 0;

    if (!ditem->paused)
        bold = A_BOLD;

    if (ditem->inactive) {
        return ditem->selected ? bold | A_REVERSE | COLOR_PAIR(5) : bold | A_REVERSE | COLOR_PAIR(4);
    } else {
        return ditem->selected ? bold | A_REVERSE | COLOR_PAIR(3) : bold | A_REVERSE | COLOR_PAIR(2);
    }
}

static int get_bg(DownloadItem *ditem)
{
    int bold = 0;

    if (!ditem->paused)
        bold = A_BOLD;

    if (ditem->inactive) {
        return ditem->selected ? bold | COLOR_PAIR(5) : bold | COLOR_PAIR(4);
    } else {
        return ditem->selected ? bold | COLOR_PAIR(3) : bold | COLOR_PAIR(2);
    }
}

static void uninit()
{
    DownloadItem *item = items_tail;

    wrefresh(openwin);
    delwin(openwin);
    wrefresh(infowin);
    delwin(infowin);
    wrefresh(helpwin);
    delwin(helpwin);
    wrefresh(statuswin);
    delwin(statuswin);
    prefresh(downloads, 0, 0, 0, 0, 0, 0);
    delwin(downloads);
    refresh();
    endwin();

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

static int create_handle(int overwrite, const char *newurl,
                         const char *referer, const char *outname,
                         curl_off_t speed)
{
    DownloadItem *item;
    FILE *outputfile = NULL;
    const char *ofilename;
    const char *lpath;
    char *unescape;
    CURL *handle;
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

    lpath = strrchr(unescape, '/') + 1;
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

    if (!overwrite)
        item->outputfile = outputfile = fopen(item->outputfilename, "rb+");
    if (!outputfile)
        item->outputfile = outputfile = fopen(item->outputfilename, "wb");
    if (!outputfile) {
        write_status(A_REVERSE | COLOR_PAIR(1), "Failed to open file: %s", item->outputfilename);
        delete_ditem(item);
        return 1;
    }

    curl_easy_setopt(handle, CURLOPT_URL, item->escape_url);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, item);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progressf);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, outputfile);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAX_RECV_SPEED_LARGE, item->max_speed);
    if (referer)
        curl_easy_setopt(handle, CURLOPT_REFERER, referer);

    if (!overwrite) {
        curl_off_t from;
        fseek(outputfile, 0, SEEK_END);
        from = item->downloaded = ftell(outputfile);
        curl_easy_setopt(handle, CURLOPT_RESUME_FROM_LARGE, from);
    }
    if (start_all && curl_multi_add_handle(mhandle, handle) != CURLM_OK) {
        write_status(A_REVERSE | COLOR_PAIR(1), "Failed to add curl easy handle");
        delete_ditem(item);
        return 1;
    }
    item->paused = 1;
    nb_ditems++;

    return 0;
}

static void write_downloads()
{
    DownloadItem *item = items;
    int line, cline = -1;

    item = items;
    for (line = 0; item; line++) {
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

        if (item->selected) {
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

        item = item->next;
    }

    pnoutrefresh(downloads, MAX(cline - (LINES - 2), 0), 0, 0, 0, LINES-1, COLS);
}

static void write_statuswin(int downloading)
{
    wattrset(statuswin, COLOR_PAIR(7));
    if (start_time != INT_MIN && downloading) {
        werase(statuswin);
        wprintw(statuswin, "seconds elapsed %ld", time(NULL) - start_time);
    }
    mvwprintw(statuswin, 0, COLS-12, " Help (F1) ");
    wnoutrefresh(statuswin);
}

static void add_handle(DownloadItem *ditem)
{
    curl_off_t from;

    fseek(ditem->outputfile, 0, SEEK_END);
    from = ditem->downloaded = ftell(ditem->outputfile);
    curl_easy_setopt(ditem->handle, CURLOPT_RESUME_FROM_LARGE, from);
    ditem->start_time = time(NULL);
    curl_multi_add_handle(mhandle, ditem->handle);
}

static void init_windows(int downloading)
{
    downloads = newpad(4096, COLS);
    if (!downloads) {
        error(-1, "Failed to create downloads window.\n");
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

    if (downloading) {
        wtimeout(downloads, 0);
        wtimeout(openwin, 0);
    } else {
        wtimeout(downloads, -1);
        wtimeout(openwin, -1);
    }

    keypad(downloads,  TRUE);
    keypad(openwin,    TRUE);

    leaveok(downloads, TRUE);
    leaveok(statuswin, TRUE);
    leaveok(helpwin,   TRUE);
    leaveok(infowin,   TRUE);
    leaveok(openwin,   TRUE);
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
                            long *max_total_connections)
{
    const char *referer = NULL;
    const char *output = NULL;
    long max = 0, speed = 0;
    int overwrite = 0;
    int i, param = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-R")) {
            param = PARAM_REFERER;
        } else if (!strcmp(argv[i], "-o")) {
            param = PARAM_OUTPUT;
            overwrite = 0;
        } else if (!strcmp(argv[i], "-M")) {
            param = PARAM_MAXTCONN;
        } else if (!strcmp(argv[i], "-O")) {
            param = PARAM_OUTPUTOVER;
            overwrite = 1;
        } else if (!strcmp(argv[i], "-i")) {
            param = PARAM_INPUTFILE;
        } else if (!strcmp(argv[i], "-s")) {
            param = PARAM_MAXSPEED;
        } else {
            if (param == PARAM_REFERER) {
                referer = argv[i];
            } else if (param == PARAM_OUTPUT || param == PARAM_OUTPUTOVER) {
                output = argv[i];
            } else if (param == PARAM_MAXTCONN) {
                max = atol(argv[i]);
            } else if (param == PARAM_INPUTFILE) {
                parse_file(argv[i]);
            } else if (param == PARAM_MAXSPEED) {
                speed = atol(argv[i]);
            } else {
                create_handle(overwrite, argv[i], referer, output, speed);
                referer = output = NULL;
                overwrite = 0;
                speed = 0;
            }
            param = 0;
        }
    }

    *max_total_connections = max;

    return i;
}

int main(int argc, char *argv[])
{
    DownloadItem *sitem = NULL;
    int downloading = 0, help_active = 0, overwrite = 0;
    int active_input = 0, info_active = 0;
    int need_refresh = 0;
    long max_total_connections = 0;

    signal(SIGINT, finish);

    string = calloc(MAX_STRING_LEN, sizeof(*string));
    if (!string) {
        error(-1, "Failed to allocate string storage.\n");
    }

    mhandle = curl_multi_init();
    if (!mhandle) {
        error(-1, "Failed to create curl multi handle.\n");
    }

    initscr();
    nonl();
    cbreak();
    noecho();
    curs_set(0);

    mousemask(ALL_MOUSE_EVENTS, NULL);
    mouseinterval(0);

    init_windows(downloading);

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


    if (parse_parameters(argc, argv, &max_total_connections))
        write_downloads();

    curl_multi_setopt(mhandle, CURLMOPT_MAX_TOTAL_CONNECTIONS, max_total_connections);

    write_statuswin(downloading);
    doupdate();

    for (;;) {
        int c;

        if (active_input) {
            int skip_y, skip_x;

            if (active_input == ENTERING_URL)
                mvwaddstr(openwin, 0, 0, "URL: ");
            else if (active_input == ENTERING_REFERER)
                mvwaddstr(openwin, 0, 0, "Referer: ");
            else if (active_input == ENTERING_SEARCH)
                mvwaddstr(openwin, 0, 0, "Search: ");
            getyx(openwin, skip_y, skip_x);

            c = wgetch(openwin);
            if (c == KEY_ENTER || c == '\n' || c == '\r') {
                if (active_input == ENTERING_URL && create_handle(overwrite, string, NULL, NULL, 0)) {
                    active_input = 0;
                    werase(openwin);
                    wnoutrefresh(openwin);
                    doupdate();
                    continue;
                } else if (active_input == ENTERING_REFERER) {
                    curl_easy_setopt(sitem->handle, CURLOPT_REFERER, string);
                } else if (active_input == ENTERING_SEARCH) {
                    DownloadItem *nsitem = items;

                    for (;nsitem; nsitem = nsitem->next) {
                        if (strstr(nsitem->outputfilename, string)) {
                            if (sitem)
                                sitem->selected = 0;
                            sitem = nsitem;
                            sitem->selected = 1;
                            break;
                        }
                    }
                    free(last_search);
                    last_search = clonestring(string, strlen(string));
                }
                need_refresh = 1;
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
                need_refresh = 1;
            } else if (!(c & KEY_CODE_YES)) {
                if (string_pos < MAX_STRING_LEN - 1) {
                    string[string_pos++] = c;
                    string[string_pos] = 0;
                }
                need_refresh = 1;
            }
            mvwaddstr(openwin, skip_y, skip_x, string + MAX((signed)strlen(string) + skip_x - COLS, 0));
            mvwchgat(openwin, skip_y, MIN(skip_x + (signed)strlen(string), COLS-1), 1, A_BLINK | A_REVERSE, 2, NULL);
        } else if (!active_input) {
            c = wgetch(downloads);

            if (c == KEY_F(1) || c == '?') {
                help_active = !help_active;
                need_refresh = 1;
            } else if (c == 'i') {
                info_active = !info_active;
                need_refresh = 1;
            } else if (c == 'A' || c == 'a') {
                if (c == 'A')
                    overwrite = 1;
                else
                    overwrite = 0;

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

                    for (;item;) {
                        if (item->paused && !item->inactive && !item->finished) {
                            add_handle(item);
                            item->paused = 0;
                        }
                        item = item->next;
                    }

                    start_time = time(NULL);
                    wtimeout(downloads, 0);
                    wtimeout(openwin, 0);
                } else if (items) {
                    DownloadItem *item = items;
                    for (;item;) {
                        if (!item->paused && !item->inactive && !item->finished) {
                            item->paused = 1;
                            curl_multi_remove_handle(mhandle, item->handle);
                        }
                        item = item->next;
                    }

                    wtimeout(downloads, -1);
                    wtimeout(openwin, -1);
                }
            } else if (c == 'h') {
                if (sitem && sitem->inactive) {
                    sitem->inactive = 0;
                    fseek(sitem->outputfile, sitem->downloaded, SEEK_SET);
                    curl_easy_setopt(sitem->handle, CURLOPT_RESUME_FROM_LARGE, ftell(sitem->outputfile));
                    curl_multi_add_handle(mhandle, sitem->handle);
                    need_refresh = 1;
                }
            } else if (c == 'D') {
                if (sitem) {
                    sitem = delete_ditem(sitem);
                    nb_ditems--;
                    werase(downloads);
                    need_refresh = 1;
                }
            } else if (c == 'R') {
                if (sitem) {
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
                    DownloadItem *nsitem = sitem ? sitem->next : items;

                    for (;nsitem; nsitem = nsitem->next) {
                        if (strstr(nsitem->outputfilename, last_search)) {
                            if (sitem)
                                sitem->selected = 0;
                            sitem = nsitem;
                            sitem->selected = 1;
                            need_refresh = 1;
                            break;
                        }
                    }
                }
            } else if (c == 'N') {
                if (last_search) {
                    DownloadItem *nsitem = sitem ? sitem->prev : items_tail;

                    for (;nsitem; nsitem = nsitem->prev) {
                        if (strstr(nsitem->outputfilename, last_search)) {
                            if (sitem)
                                sitem->selected = 0;
                            sitem = nsitem;
                            sitem->selected = 1;
                            need_refresh = 1;
                            break;
                        }
                    }
                }
            } else if (c == 'H') {
                if (sitem && !sitem->inactive) {
                    sitem->inactive = 1;
                    sitem->finished = 0;
                    curl_multi_remove_handle(mhandle, sitem->handle);
                    need_refresh = 1;
                }
            } else if (c == 'p') {
                if (sitem && !sitem->inactive) {
                    if (!sitem->paused)
                        curl_multi_remove_handle(mhandle, sitem->handle);
                    sitem->paused = !sitem->paused;
                    if (!sitem->paused) {
                        add_handle(sitem);
                        downloading = 1;
                        if (start_time == INT_MIN)
                            start_time = sitem->start_time;
                        wtimeout(downloads, 0);
                        wtimeout(openwin, 0);
                    }
                    need_refresh = 1;
                }
            } else if (c == KEY_DOWN) {
                if (!sitem) {
                    sitem = items;
                    if (sitem)
                        sitem->selected = 1;
                } else {
                    if (sitem->next) {
                        sitem->selected = 0;
                        sitem = sitem->next;
                        sitem->selected = 1;
                    }
                }
                need_refresh = 1;
            } else if (c == KEY_UP) {
                if (!sitem) {
                    sitem = items;
                    if (sitem)
                        sitem->selected = 1;
                } else {
                    if (sitem->prev) {
                        sitem->selected = 0;
                        sitem = sitem->prev;
                        sitem->selected = 1;
                    }
                }
                need_refresh = 1;
            } else if (c == KEY_RIGHT) {
                if (sitem) {
                    sitem->max_speed += 1024;
                    curl_easy_setopt(sitem->handle, CURLOPT_MAX_RECV_SPEED_LARGE, sitem->max_speed);
                    need_refresh = 1;
                }
            } else if (c == KEY_LEFT) {
                if (sitem) {
                    sitem->max_speed = MAX(0, sitem->max_speed - 1024);
                    curl_easy_setopt(sitem->handle, CURLOPT_MAX_RECV_SPEED_LARGE, sitem->max_speed);
                    need_refresh = 1;
                }
            } else if (c == KEY_HOME) {
                if (sitem && sitem != items) {
                    sitem->selected = 0;
                    sitem = items;
                    sitem->selected = 1;
                    need_refresh = 1;
                }
            } else if (c == KEY_END) {
                if (sitem) {
                    sitem->selected = 0;
                    for (;sitem->next;) {
                        sitem = sitem->next;
                    }
                    sitem->selected = 1;
                    need_refresh = 1;
                }
            } else if (c == KEY_RESIZE) {
                delwin(openwin);
                delwin(infowin);
                delwin(helpwin);
                delwin(statuswin);
                delwin(downloads);
                clear();
                refresh();
                endwin();

                init_windows(downloading);

                help_active = 0;
                active_input = 0;
                need_refresh = 1;
            } else if (c == KEY_MOUSE) {
                MEVENT mouse_event;
                int y;

                if (getmouse(&mouse_event) == OK) {
                    if (sitem)
                        sitem->selected = 0;
                    sitem = items;
                    for (y = 0; sitem->next; y++) {
                        if (y == mouse_event.y)
                            break;
                        sitem = sitem->next;
                    }
                    sitem->selected = 1;
                    need_refresh = 1;
                }
            }
        }

        if (downloading) {
            int ret, numdfs, still_running;

            ret = curl_multi_perform(mhandle, &still_running);
            while (ret == CURLM_CALL_MULTI_PERFORM) {
                ret = curl_multi_perform(mhandle, &still_running);
            }
            if (still_running == 0) {
                downloading = 0;
                wtimeout(downloads, -1);
                wtimeout(openwin, -1);
                need_refresh = 1;
            }

            if (ret == CURLM_OK) {
                struct CURLMsg *m;

                curl_multi_wait(mhandle, NULL, 0, 100, &numdfs);

                do {
                    int msgq = 0;

                    m = curl_multi_info_read(mhandle, &msgq);
                    if (m && (m->msg == CURLMSG_DONE)) {
                        DownloadItem *item = NULL;
                        CURL *handle = m->easy_handle;

                        curl_multi_remove_handle(mhandle, handle);
                        curl_easy_getinfo(handle, CURLINFO_PRIVATE, &item);
                        if (item) {
                            item->finished = 1;
                            item->end_time = time(NULL);
                            item->progress = 100;
                        }
                    }
                } while (m);

                if (numdfs > 0) {
                    need_refresh = 1;
                } else {
                    struct timespec req, rem;

                    req.tv_sec = 0;
                    req.tv_nsec = 10000000L;
                    nanosleep(&req, &rem);
                }
            }
        }

        if (need_refresh) {
            write_downloads();
            write_statuswin(downloading);

            if (info_active)
                write_infowin(sitem);

            if (help_active)
                write_helpwin();

            doupdate();
            need_refresh = 0;
        }
    }

    finish(0);
}
