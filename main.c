#include <curl/curl.h>
#include <curses.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct DownloadItem {
    int paused;
    int inactive;
    int selected;
    int finished;
    int ufinished;
    char *url;
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
    long int downloaded;
    long int start_time;
    CURL *handle;
    struct DownloadItem *next;
    struct DownloadItem *prev;
} DownloadItem;

#define MAX_URL_LEN 16384

char *url = NULL;
int start_all = 0;
CURLM *mhandle = NULL;
DownloadItem *items = NULL;
long int start_time = INT_MIN;
int nb_ditems = 0;
int open_urlpos = 0;

WINDOW *infowin   = NULL;
WINDOW *openwin   = NULL;
WINDOW *helpwin   = NULL;
WINDOW *statuswin = NULL;
WINDOW *downloads = NULL;

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static DownloadItem* delete_ditem(DownloadItem *ditem)
{
    if (ditem->handle) {
        if (!ditem->inactive)
            curl_multi_remove_handle(mhandle, ditem->handle);
        curl_easy_cleanup(ditem->handle);
    }

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
        ditem = ditem->prev;
        ditem->next = NULL;
        ditem->selected = 1;
        free(old);
    } else {
        free(ditem);
        items = ditem = NULL;
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
    wattrset(helpwin, COLOR_PAIR(5));
    mvwaddstr(helpwin,  2, 2, " Key a - add new download URL, downloading from last position ");
    mvwaddstr(helpwin,  3, 2, " Key A - add new download URL, downloading from beginning ");
    mvwaddstr(helpwin,  4, 2, " Key Q - quit ");
    mvwaddstr(helpwin,  5, 2, " Key S - start/stop all downloads ");
    mvwaddstr(helpwin,  6, 2, " Key H - halt selected download ");
    mvwaddstr(helpwin,  7, 2, " Key h - unhalt selected download ");
    mvwaddstr(helpwin,  8, 2, " Key p - pause/unpause selected download ");
    mvwaddstr(helpwin,  9, 2, " Key D - delete download from the list ");
    mvwaddstr(helpwin, 10, 2, " Key R - set referer for the selected download ");
    mvwaddstr(helpwin, 11, 2, " Key i - show extra info for selected download ");
    wnoutrefresh(helpwin);
}

static void write_infowin(DownloadItem *sitem)
{
    if (!sitem)
        return;

    wattrset(infowin, COLOR_PAIR(7));
    mvwprintw(infowin,  0, 0, " Filename: %s ", sitem->outputfilename);
    mvwprintw(infowin,  1, 0, " Effective URL: %s ", sitem->url);
    mvwprintw(infowin,  2, 0, " Response code: %ld ", sitem->rcode);
    mvwprintw(infowin,  3, 0, " Content-length: %f ", sitem->contentlength);
    mvwprintw(infowin,  4, 0, " Download size: %f ", sitem->download_size);
    mvwprintw(infowin,  5, 0, " Primary IP: %s ", sitem->primary_ip);
    mvwprintw(infowin,  6, 0, " Primary port: %ld ", sitem->primary_port);
    mvwprintw(infowin,  7, 0, " Used Protocol: ");
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
    long int tdiff = time(NULL) - stime;

    if (item->finished) {
        item->progress = 100;
        return 0;
    }

    if (dltotal)
        item->progress = 100. * (dlnow + item->downloaded)/(dltotal + item->downloaded);
    else
        item->progress = 0;

    item->finished = (dltotal != 0 && dlnow == dltotal);

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
    DownloadItem *item = items;

    for (;;) {
        if (item == NULL)
            break;
        curl_easy_cleanup(item->handle);
        fclose(item->outputfile);
        free(item->outputfilename);
        item->outputfilename = NULL;
        item = item->next;
    }

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
    curl_multi_cleanup(mhandle);
    mhandle = NULL;
    curl_global_cleanup();
    free(items);
    items = NULL;
    free(url);
    url = NULL;
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

static int create_handle(int overwrite, const char *newurl, const char *referer)
{
    DownloadItem *item;
    FILE *outputfile = NULL;
    char *ofilename, *ofilenamecopy;
    CURL *handle;
    int urllen, i;

    open_urlpos = 0;
    urllen = strlen(newurl);
    if (urllen <= 1) {
        werase(openwin);
        return 1;
    }

    for (i = urllen - 1; i >= 0; i--) {
        if (newurl[i] == '/')
            break;
    }

    if (items == NULL) {
        items = calloc(1, sizeof(DownloadItem));
        if (!items) {
            write_status(A_REVERSE | COLOR_PAIR(1), "Failed to allocate DownloadItem");
            return 1;
        }
        item = items;
    } else {
        DownloadItem *prev;

        item = items;
        for (;;) {
            if (item->next)
                item = item->next;
            else
                break;
        }

        item->next = calloc(1, sizeof(DownloadItem));
        if (!item->next) {
            write_status(A_REVERSE | COLOR_PAIR(1), "Failed to allocate DownloadItem");
            return 1;
        }

        prev = item;
        item = item->next;
        item->prev = prev;
    }

    ofilename = (char *)&newurl[i+1];
    if (!ofilename) {
        finish(-1);
    }
    if (!overwrite)
        item->outputfile = outputfile = fopen(ofilename, "rb+");
    if (!outputfile)
        item->outputfile = outputfile = fopen(ofilename, "wb");
    if (!outputfile) {
        write_status(A_REVERSE | COLOR_PAIR(1), "Failed to open file: %s", ofilename);
        delete_ditem(item);
        return 1;
    }
    item->handle = handle = curl_easy_init();
    if (!handle) {
        write_status(A_REVERSE | COLOR_PAIR(1), "Failed to create curl easy handle");
        delete_ditem(item);
        return 1;
    }
    item->outputfilename = ofilenamecopy = calloc(strlen(ofilename) + 1, 1);
    if (!ofilenamecopy)
        finish(-1);
    memcpy(ofilenamecopy, ofilename, strlen(ofilename));

    curl_easy_setopt(handle, CURLOPT_URL, newurl);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, item);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progressf);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, outputfile);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
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
    leaveok(downloads, TRUE);
    leaveok(infowin,   TRUE);
    leaveok(openwin,   TRUE);
}

int main(int argc, char *argv[])
{
    DownloadItem *sitem = NULL;
    int downloading = 0, help_active = 0, overwrite = 0;
    int open_active = 0, referer_active = 0, info_active = 0;
    int need_refresh = 0;
    long max_total_connections = 0;
    int i;

    signal(SIGINT, finish);

    url = calloc(MAX_URL_LEN, sizeof(*url));
    if (!url) {
        error(-1, "Failed to allocate url storage.\n");
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

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-R") && argc >= i+3) {
            create_handle(0, argv[i+2], argv[i+1]);
            i+=2;
        } else if (!strcmp(argv[i], "-M") && argc >= i+2) {
            max_total_connections = atol(argv[i+1]);
            i+=1;
        } else {
            create_handle(0, argv[i], NULL);
        }
    }

    curl_multi_setopt(mhandle, CURLMOPT_MAX_TOTAL_CONNECTIONS, max_total_connections);

    if (i > 1)
        write_downloads();

    write_statuswin(downloading);
    doupdate();

    for (;;) {
        int c;

        if (open_active || referer_active) {
            int skip_y, skip_x;

            if (open_active)
                mvwaddstr(openwin, 0, 0, "Enter URL: ");
            else
                mvwaddstr(openwin, 0, 0, "Enter referer: ");
            getyx(openwin, skip_y, skip_x);

            c = wgetch(openwin);
            if (c == KEY_ENTER || c == '\n' || c == '\r') {
                if (open_active && create_handle(overwrite, url, NULL)) {
                    open_active = 0;
                    werase(openwin);
                    wnoutrefresh(openwin);
                    doupdate();
                    continue;
                } else if (referer_active) {
                    curl_easy_setopt(sitem->handle, CURLOPT_REFERER, url);
                    referer_active = 0;
                }
                need_refresh = 1;
                open_urlpos = 0;
                open_active = 0;
                url[0] = '\0';
                werase(openwin);
            } else if (c == KEY_BACKSPACE || c == KEY_DC || c == '\b' || c == 127 || c == 8) {
                werase(openwin);
                if (open_urlpos > 0) {
                    url[open_urlpos - 1] = 0;
                    open_urlpos--;
                }
                need_refresh = 1;
            } else if (!(c & KEY_CODE_YES)) {
                if (open_urlpos < MAX_URL_LEN - 1) {
                    url[open_urlpos++] = c;
                    url[open_urlpos] = 0;
                }
                need_refresh = 1;
            }
            mvwaddstr(openwin, skip_y, skip_x, url + MAX((signed)strlen(url) + skip_x - COLS, 0));
            mvwchgat(openwin, skip_y, MIN(skip_x + (signed)strlen(url), COLS-1), 1, A_BLINK | A_REVERSE, 2, NULL);
        } else if (!open_active && !referer_active) {
            c = wgetch(downloads);

            if (c == KEY_F(1)) {
                help_active = !help_active;
                need_refresh = 1;
            } else if (c == 'i') {
                info_active = !info_active;
                if (info_active && sitem) {
                    curl_easy_getinfo(sitem->handle, CURLINFO_EFFECTIVE_URL, &sitem->url);
                    curl_easy_getinfo(sitem->handle, CURLINFO_RESPONSE_CODE, &sitem->rcode);
                    curl_easy_getinfo(sitem->handle, CURLINFO_PROTOCOL, &sitem->protocol);
                    curl_easy_getinfo(sitem->handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &sitem->contentlength);
                    curl_easy_getinfo(sitem->handle, CURLINFO_SIZE_DOWNLOAD, &sitem->download_size);
                    curl_easy_getinfo(sitem->handle, CURLINFO_PRIMARY_IP, &sitem->primary_ip);
                    curl_easy_getinfo(sitem->handle, CURLINFO_PRIMARY_PORT, &sitem->primary_port);
                }
                need_refresh = 1;
            } else if (c == 'A' || c == 'a') {
                if (c == 'A')
                    overwrite = 1;
                else
                    overwrite = 0;

                if (!open_active) {
                    open_active = 1;
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
                    if (!referer_active) {
                        referer_active = 1;
                        continue;
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
                open_active = 0;
                referer_active = 0;
                need_refresh = 1;
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
            } else if (ret == CURLM_OK) {
                curl_multi_wait(mhandle, NULL, 0, 1000, &numdfs);
                need_refresh = 1;
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
