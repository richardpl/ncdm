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

WINDOW *openwin   = NULL;
WINDOW *helpwin   = NULL;
WINDOW *statuswin = NULL;
WINDOW *downloads = NULL;

#define MAX(a, b) (a > b ? a : b)
#define MIN(a, b) (a < b ? a : b)

static DownloadItem* delete_ditem(DownloadItem *ditem)
{
    if (ditem->handle) {
        if (!ditem->inactive && ditem->handle)
            curl_multi_remove_handle(mhandle, ditem->handle);
        if (ditem->handle)
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

static void write_status(const char *statusstr, int color)
{
    werase(statuswin);
    wattrset(statuswin, color);
    waddstr(statuswin, statusstr);
    wnoutrefresh(statuswin);
}

static void write_help()
{
    wattrset(helpwin, COLOR_PAIR(5));
    mvwaddstr(helpwin,  2, 2, " Key a - add new download URL, downloading from last position ");
    mvwaddstr(helpwin,  3, 2, " Key A - add new download URL, downloading from beginning ");
    mvwaddstr(helpwin,  4, 2, " Key Q - quit ");
    mvwaddstr(helpwin,  5, 2, " Key S - start downloads ");
    mvwaddstr(helpwin,  6, 2, " Key H - halt selected download ");
    mvwaddstr(helpwin,  7, 2, " Key h - unhalt selected download ");
    mvwaddstr(helpwin,  8, 2, " Key p - pause/unpause selected download ");
    mvwaddstr(helpwin,  9, 2, " Key D - delete download from the list ");
    mvwaddstr(helpwin, 10, 2, " Key R - set referer for the selected download ");
    wnoutrefresh(helpwin);
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

static void finish(int sig)
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
    wrefresh(helpwin);
    delwin(helpwin);
    wrefresh(statuswin);
    delwin(statuswin);
    prefresh(downloads, 0, 0, 0, 0, 0, 0);
    delwin(downloads);
    refresh();
    clear();
    endwin();
    curl_multi_cleanup(mhandle);
    curl_global_cleanup();
    free(items);
    items = NULL;
    free(url);
    url = NULL;

    exit(sig);
}

static int create_handle(int *open_active, int overwrite)
{
    DownloadItem *item;
    FILE *outputfile = NULL;
    char *ofilename, *ofilenamecopy;
    CURL *handle;
    int urllen, i;

    open_urlpos = 0;
    urllen = strlen(url);
    if (urllen <= 1) {
        werase(openwin);
        *open_active = 0;
        return 1;
    }

    for (i = urllen - 1; i >= 0; i--) {
        if (url[i] == '/')
            break;
    }

    if (items == NULL) {
        items = calloc(1, sizeof(DownloadItem));
        if (!items) {
            write_status("Failed to allocate DownloadItem", A_REVERSE | COLOR_PAIR(1));
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
            write_status("Failed to allocate DownloadItem", A_REVERSE | COLOR_PAIR(1));
            return 1;
        }

        prev = item;
        item = item->next;
        item->prev = prev;
    }

    ofilename = &url[i+1];
    if (!ofilename) {
        finish(-1);
    }
    if (!overwrite)
        item->outputfile = outputfile = fopen(ofilename, "rb+");
    if (!outputfile)
        item->outputfile = outputfile = fopen(ofilename, "wb");
    if (!outputfile) {
        write_status("Failed to open file", A_REVERSE | COLOR_PAIR(1));
        delete_ditem(item);
        return 1;
    }
    item->handle = handle = curl_easy_init();
    if (!handle) {
        write_status("Failed to create curl easy handle", A_REVERSE | COLOR_PAIR(1));
        delete_ditem(item);
        return 1;
    }
    item->outputfilename = ofilenamecopy = calloc(strlen(ofilename) + 1, 1);
    if (!ofilenamecopy)
        finish(-1);
    memcpy(ofilenamecopy, ofilename, strlen(ofilename));

    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, item);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progressf);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, outputfile);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);

    if (!overwrite) {
        curl_off_t from;
        fseek(outputfile, 0, SEEK_END);
        from = item->downloaded = ftell(outputfile);
        curl_easy_setopt(handle, CURLOPT_RESUME_FROM_LARGE, from);
    }
    if (start_all && curl_multi_add_handle(mhandle, handle) != CURLM_OK) {
        write_status("Failed to add curl easy handle", A_REVERSE | COLOR_PAIR(1));
        delete_ditem(item);
        return 1;
    }
    item->paused = 1;
    nb_ditems++;

    return 0;
}

int main(int argc, char *argv[])
{
    DownloadItem *sitem = NULL;
    int downloading = 0, help_active = 0, overwrite = 0;
    int open_active = 0, referer_active = 0, need_refresh = 0;

    signal(SIGINT, finish);

    url = calloc(MAX_URL_LEN, sizeof(*url));
    if (!url) {
        fprintf(stderr, "Failed to allocate url storage.\n");
        finish(-1);
    }

    mhandle = curl_multi_init();
    if (!mhandle) {
        fprintf(stderr, "Failed to create pad window.\n");
        finish(-1);
    }

    initscr();
    nonl();
    cbreak();
    noecho();
    curs_set(0);

    downloads = newpad(4096, COLS);
    if (!downloads) {
        fprintf(stderr, "Failed to create pad window.\n");
        finish(-1);
    }
    keypad(downloads, TRUE);

    statuswin = newwin(1, COLS, LINES-1, 0);
    if (!statuswin) {
        fprintf(stderr, "Failed to create status window.\n");
        finish(-1);
    }

    helpwin = newwin(LINES-1, COLS, 0, 0);
    if (!statuswin) {
        fprintf(stderr, "Failed to create help window.\n");
        finish(-1);
    }

    openwin = newwin(1, COLS, LINES/2, 0);
    if (!openwin) {
        fprintf(stderr, "Failed to create open window.\n");
        finish(-1);
    }

    wtimeout(downloads, -1);
    wtimeout(openwin, -1);

    leaveok(downloads, 1);
    leaveok(openwin, 0);

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

    for (;;) {
        int c;

        if (open_active || referer_active) {
            if (open_active)
                mvwaddstr(openwin, 0, 0, "Enter URL: ");
            else
                mvwaddstr(openwin, 0, 0, "Enter referer: ");

            c = wgetch(openwin);
            if (c == KEY_ENTER || c == '\n' || c == '\r') {
                if (open_active && create_handle(&open_active, overwrite)) {
                    open_active = 0;
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
            mvwaddstr(openwin, 0, 11 + 4 * referer_active, url);
            mvwchgat(openwin, 0, 11 + 4 * referer_active + strlen(url), 1, A_BLINK | A_REVERSE, 2, NULL);
        } else if (!open_active) {
            c = wgetch(downloads);

            if (c == KEY_F(1)) {
                help_active = !help_active;
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
                if (downloading) {
                    start_time = time(NULL);
                    wtimeout(downloads, 0);
                    wtimeout(openwin, 0);
                } else {
                    wtimeout(downloads, -1);
                    wtimeout(openwin, -1);
                }
            } else if (c == 'h') {
                if (sitem && sitem->inactive) {
                    sitem->inactive = 0;
                    fseek(sitem->outputfile, sitem->downloaded, SEEK_SET);
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
                    curl_multi_remove_handle(mhandle, sitem->handle);
                    need_refresh = 1;
                }
            } else if (c == 'p') {
                if (sitem && !sitem->inactive) {
                    sitem->paused = !sitem->paused;
                    curl_multi_remove_handle(mhandle, sitem->handle);
                    if (!sitem->paused) {
                        curl_off_t from;

                        fseek(sitem->outputfile, 0, SEEK_END);
                        from = sitem->downloaded = ftell(sitem->outputfile);
                        curl_easy_setopt(sitem->handle, CURLOPT_RESUME_FROM_LARGE, from);
                        curl_multi_add_handle(mhandle, sitem->handle);
                        downloading = 1;
                        sitem->start_time = time(NULL);
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
                delwin(helpwin);
                delwin(statuswin);
                delwin(downloads);
                clear();
                refresh();
                endwin();

                downloads = newpad(4096, COLS);
                if (!downloads) {
                    fprintf(stderr, "Failed to create pad window.\n");
                    finish(-1);
                }

                keypad(downloads, TRUE);
                leaveok(downloads, 1);

                helpwin = newwin(LINES-1, COLS, 0, 0);
                if (!helpwin) {
                    fprintf(stderr, "Failed to create help window.\n");
                    finish(-1);
                }

                statuswin = newwin(1, COLS, LINES-1, 0);
                if (!statuswin) {
                    fprintf(stderr, "Failed to create status window.\n");
                    finish(-1);
                }

                openwin = newwin(1, COLS, LINES/2, 0);
                if (!openwin) {
                    fprintf(stderr, "Failed to create open window.\n");
                    finish(-1);
                }
                if (downloading) {
                    wtimeout(downloads, 0);
                    wtimeout(openwin, 0);
                } else {
                    wtimeout(downloads, -1);
                    wtimeout(openwin, -1);
                }

                leaveok(openwin, 0);

                help_active = 0;
                open_active = 0;
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

            if (start_time != INT_MIN && downloading) {
                char statusstr[256] = { 0 };
                snprintf((char *)&statusstr, sizeof(statusstr), "seconds elapsed: %ld", time(NULL) - start_time);
                write_status((const char *)&statusstr, COLOR_PAIR(7));
            }

            if (help_active) {
                write_help();
            }
            doupdate();
            need_refresh = 0;
        }
    }

    finish(0);
}
