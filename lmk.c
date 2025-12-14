#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <time.h>

#define PORT 8888
#define DEFAULT_DURATION 5000
#define MIN_WIDTH 300
#define MAX_WIDTH 500
#define PADDING 12
#define LINE_SPACING 6
#define BORDER_WIDTH 1

#define NOTIF_X_OFFSET 10      /* Distance from right edge of screen */
#define NOTIF_Y_OFFSET 30      /* Distance from top of screen */
#define NOTIF_ALIGN_RIGHT 1    /* 1 = right side, 0 = left side */
#define NOTIF_ALIGN_TOP 1      /* 1 = top, 0 = bottom */

typedef struct Notification {
    char title[1024];
    char body[2048];
    char icon[256];
    char urgency[16];
    int duration;
    time_t timestamp;
    int dismissed;
    int showing_toast;
    int width;
    int height;
    struct Notification *next;
} Notification;

typedef struct {
    Display *dpy;
    Window win;
    GC gc;
    XftDraw *xft_draw;
    XftFont *title_font;
    XftFont *body_font;
    XftColor fg_color;
    XftColor bg_color;
    XftColor border_color;
    XftColor urgent_color;
    int screen;
    int screen_width;
    int screen_height;
} NotifDisplay;

Notification *notif_head = NULL;
pthread_mutex_t notif_mutex = PTHREAD_MUTEX_INITIALIZER;
NotifDisplay ndisp;
int center_visible = 0;

void init_display() {
    ndisp.dpy = XOpenDisplay(NULL);
    if (!ndisp.dpy) {
        fprintf(stderr, "Cannot open display\n");
        exit(1);
    }
    
    ndisp.screen = DefaultScreen(ndisp.dpy);
    ndisp.screen_width = DisplayWidth(ndisp.dpy, ndisp.screen);
    ndisp.screen_height = DisplayHeight(ndisp.dpy, ndisp.screen);
    
    Visual *vis = DefaultVisual(ndisp.dpy, ndisp.screen);
    Colormap cmap = DefaultColormap(ndisp.dpy, ndisp.screen);
    
    XSetWindowAttributes wa = {
        .override_redirect = True,
        .background_pixel = 0x222222,
        .border_pixel = 0x444444,
        .event_mask = ExposureMask | ButtonPressMask
    };
    
    ndisp.win = XCreateWindow(ndisp.dpy, DefaultRootWindow(ndisp.dpy),
        0, 0, MIN_WIDTH, 100, BORDER_WIDTH,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &wa);
    
    ndisp.gc = XCreateGC(ndisp.dpy, ndisp.win, 0, NULL);
    
    ndisp.xft_draw = XftDrawCreate(ndisp.dpy, ndisp.win, vis, cmap);
    ndisp.title_font = XftFontOpenName(ndisp.dpy, ndisp.screen, "monospace:size=10:bold");
    ndisp.body_font = XftFontOpenName(ndisp.dpy, ndisp.screen, "monospace:size=9");
    
    XftColorAllocName(ndisp.dpy, vis, cmap, "#bbbbbb", &ndisp.fg_color);
    XftColorAllocName(ndisp.dpy, vis, cmap, "#222222", &ndisp.bg_color);
    XftColorAllocName(ndisp.dpy, vis, cmap, "#444444", &ndisp.border_color);
    XftColorAllocName(ndisp.dpy, vis, cmap, "#ff0000", &ndisp.urgent_color);
}

int wrap_text(const char *text, XftFont *font, int max_width, char lines[][256], int max_lines) {
    int line_count = 0;
    int text_len = strlen(text);
    int start = 0;
    
    while (start < text_len && line_count < max_lines) {
        int end = start;
        int last_space = -1;
        int current_width = 0;
        
        while (end < text_len) {
            if (text[end] == '\n') {
                strncpy(lines[line_count], text + start, end - start);
                lines[line_count][end - start] = '\0';
                line_count++;
                start = end + 1;
                break;
            }
            
            XGlyphInfo extents;
            char temp[2] = {text[end], '\0'};
            XftTextExtentsUtf8(ndisp.dpy, font, (XftChar8*)temp, 1, &extents);
            current_width += extents.xOff;
            
            if (text[end] == ' ') last_space = end;
            
            if (current_width > max_width - 2 * PADDING) {
                int break_point = (last_space > start) ? last_space : end;
                strncpy(lines[line_count], text + start, break_point - start);
                lines[line_count][break_point - start] = '\0';
                line_count++;
                start = (last_space > start) ? last_space + 1 : end;
                break;
            }
            
            end++;
        }
        
        if (end >= text_len && start < text_len) {
            strncpy(lines[line_count], text + start, text_len - start);
            lines[line_count][text_len - start] = '\0';
            line_count++;
            break;
        }
    }
    
    return line_count;
}

void calculate_notification_size(Notification *notif) {
    char title_lines[20][256];
    char body_lines[50][256];
    
    int title_line_count = wrap_text(notif->title, ndisp.title_font, MAX_WIDTH, title_lines, 20);
    int body_line_count = wrap_text(notif->body, ndisp.body_font, MAX_WIDTH, body_lines, 50);
    
    int max_text_width = 0;
    for (int i = 0; i < title_line_count; i++) {
        XGlyphInfo extents;
        XftTextExtentsUtf8(ndisp.dpy, ndisp.title_font, (XftChar8*)title_lines[i], 
                          strlen(title_lines[i]), &extents);
        if (extents.width > max_text_width) max_text_width = extents.width;
    }
    for (int i = 0; i < body_line_count; i++) {
        XGlyphInfo extents;
        XftTextExtentsUtf8(ndisp.dpy, ndisp.body_font, (XftChar8*)body_lines[i], 
                          strlen(body_lines[i]), &extents);
        if (extents.width > max_text_width) max_text_width = extents.width;
    }
    
    notif->width = max_text_width + 2 * PADDING;
    if (notif->width < MIN_WIDTH) notif->width = MIN_WIDTH;
    if (notif->width > MAX_WIDTH) notif->width = MAX_WIDTH;
    
    int title_height = title_line_count * (ndisp.title_font->height + LINE_SPACING);
    int body_height = body_line_count * (ndisp.body_font->height + LINE_SPACING);
    notif->height = title_height + body_height + 2 * PADDING + LINE_SPACING;
}

void add_notification(const char *title, const char *body, const char *icon, 
                     const char *urgency, int duration) {
    Notification *notif = malloc(sizeof(Notification));
    strncpy(notif->title, title, sizeof(notif->title) - 1);
    strncpy(notif->body, body, sizeof(notif->body) - 1);
    strncpy(notif->icon, icon ? icon : "", sizeof(notif->icon) - 1);
    strncpy(notif->urgency, urgency ? urgency : "normal", sizeof(notif->urgency) - 1);
    notif->duration = duration > 0 ? duration : DEFAULT_DURATION;
    notif->timestamp = time(NULL);
    notif->dismissed = 0;
    notif->showing_toast = 1;
    notif->next = NULL;
    
    calculate_notification_size(notif);
    
    pthread_mutex_lock(&notif_mutex);
    if (!notif_head) {
        notif_head = notif;
    } else {
        Notification *curr = notif_head;
        while (curr->next) curr = curr->next;
        curr->next = notif;
    }
    pthread_mutex_unlock(&notif_mutex);
}

void draw_notification(Notification *notif, int y_offset) {
    XSetForeground(ndisp.dpy, ndisp.gc, 0x222222);
    XFillRectangle(ndisp.dpy, ndisp.win, ndisp.gc, 
                   0, y_offset, notif->width, notif->height);
    
    XftColor *title_color = strcmp(notif->urgency, "critical") == 0 ? 
                            &ndisp.urgent_color : &ndisp.fg_color;
    
    char title_lines[20][256];
    char body_lines[50][256];
    
    int title_line_count = wrap_text(notif->title, ndisp.title_font, notif->width, title_lines, 20);
    int body_line_count = wrap_text(notif->body, ndisp.body_font, notif->width, body_lines, 50);
    
    int y = y_offset + PADDING + ndisp.title_font->ascent;
    for (int i = 0; i < title_line_count; i++) {
        XftDrawStringUtf8(ndisp.xft_draw, title_color, ndisp.title_font,
                          PADDING, y, 
                          (XftChar8*)title_lines[i], strlen(title_lines[i]));
        y += ndisp.title_font->height + LINE_SPACING;
    }
    
    y += LINE_SPACING;
    
    for (int i = 0; i < body_line_count; i++) {
        XftDrawStringUtf8(ndisp.xft_draw, &ndisp.fg_color, ndisp.body_font,
                          PADDING, y,
                          (XftChar8*)body_lines[i], strlen(body_lines[i]));
        y += ndisp.body_font->height + LINE_SPACING;
    }
}

void show_toast(Notification *notif) {
    int x, y;
    
    if (NOTIF_ALIGN_RIGHT) {
        x = ndisp.screen_width - notif->width - NOTIF_X_OFFSET;
    } else {
        x = NOTIF_X_OFFSET;
    }
    
    if (NOTIF_ALIGN_TOP) {
        y = NOTIF_Y_OFFSET;
    } else {
        y = ndisp.screen_height - notif->height - NOTIF_Y_OFFSET;
    }
    
    XMoveResizeWindow(ndisp.dpy, ndisp.win, x, y, 
                      notif->width, notif->height);
    XMapRaised(ndisp.dpy, ndisp.win);
    
    XClearWindow(ndisp.dpy, ndisp.win);
    draw_notification(notif, 0);
    XFlush(ndisp.dpy);
    
    usleep(notif->duration * 1000);
    notif->showing_toast = 0;
    
    if (!center_visible) {
        XUnmapWindow(ndisp.dpy, ndisp.win);
        XFlush(ndisp.dpy);
    }
}

void show_notification_center() {
    pthread_mutex_lock(&notif_mutex);
    
    int count = 0;
    int total_height = 0;
    int max_width = MIN_WIDTH;
    
    Notification *curr = notif_head;
    while (curr) {
        if (!curr->dismissed) {
            count++;
            total_height += curr->height + LINE_SPACING;
            if (curr->width > max_width) max_width = curr->width;
        }
        curr = curr->next;
    }
    
    if (count == 0) {
        pthread_mutex_unlock(&notif_mutex);
        XUnmapWindow(ndisp.dpy, ndisp.win);
        XFlush(ndisp.dpy);
        return;
    }
    
    total_height += LINE_SPACING;
    
    int x, y;
    if (NOTIF_ALIGN_RIGHT) {
        x = ndisp.screen_width - max_width - NOTIF_X_OFFSET;
    } else {
        x = NOTIF_X_OFFSET;
    }
    
    if (NOTIF_ALIGN_TOP) {
        y = NOTIF_Y_OFFSET;
    } else {
        y = ndisp.screen_height - total_height - NOTIF_Y_OFFSET;
    }
    
    XMoveResizeWindow(ndisp.dpy, ndisp.win, x, y, max_width, total_height);
    XMapRaised(ndisp.dpy, ndisp.win);
    XClearWindow(ndisp.dpy, ndisp.win);
    
    int y_offset = LINE_SPACING;
    curr = notif_head;
    while (curr) {
        if (!curr->dismissed) {
            draw_notification(curr, y_offset);
            y_offset += curr->height + LINE_SPACING;
        }
        curr = curr->next;
    }
    
    XFlush(ndisp.dpy);
    pthread_mutex_unlock(&notif_mutex);
}

void *toast_thread(void *arg) {
    Notification *notif = (Notification *)arg;
    show_toast(notif);
    return NULL;
}

void handle_request(int client_sock) {
    char buffer[8192];
    int bytes = read(client_sock, buffer, sizeof(buffer) - 1);
    if (bytes <= 0) return;
    buffer[bytes] = '\0';
    
    if (strstr(buffer, "POST /notify") == NULL) {
        const char *response = "HTTP/1.1 404 Not Found\r\n\r\n";
        write(client_sock, response, strlen(response));
        return;
    }
    
    char *body = strstr(buffer, "\r\n\r\n");
    if (!body) {
        const char *response = "HTTP/1.1 400 Bad Request\r\n\r\n";
        write(client_sock, response, strlen(response));
        return;
    }
    body += 4;
    
    char title[1024] = "Notification";
    char msg[2048] = "";
    char icon[256] = "";
    char urgency[16] = "normal";
    int duration = DEFAULT_DURATION;
    
    char *title_start = strstr(body, "\"title\"");
    if (title_start) {
        title_start = strchr(title_start, ':');
        if (title_start) {
            title_start = strchr(title_start, '"');
            if (title_start) {
                title_start++;
                char *title_end = strchr(title_start, '"');
                if (title_end) {
                    int len = title_end - title_start;
                    if (len > 1023) len = 1023;
                    strncpy(title, title_start, len);
                    title[len] = '\0';
                }
            }
        }
    }
    
    char *body_start = strstr(body, "\"body\"");
    if (body_start) {
        body_start = strchr(body_start, ':');
        if (body_start) {
            body_start = strchr(body_start, '"');
            if (body_start) {
                body_start++;
                char *body_end = strchr(body_start, '"');
                if (body_end) {
                    int len = body_end - body_start;
                    if (len > 2047) len = 2047;
                    strncpy(msg, body_start, len);
                    msg[len] = '\0';
                }
            }
        }
    }
    
    char *dur_start = strstr(body, "\"duration\"");
    if (dur_start) {
        dur_start = strchr(dur_start, ':');
        if (dur_start) {
            duration = atoi(dur_start + 1);
            if (duration <= 0) duration = DEFAULT_DURATION;
        }
    }
    
    char *urg_start = strstr(body, "\"urgency\"");
    if (urg_start) {
        urg_start = strchr(urg_start, ':');
        if (urg_start) {
            urg_start = strchr(urg_start, '"');
            if (urg_start) {
                urg_start++;
                char *urg_end = strchr(urg_start, '"');
                if (urg_end) {
                    int len = urg_end - urg_start;
                    if (len > 15) len = 15;
                    strncpy(urgency, urg_start, len);
                    urgency[len] = '\0';
                }
            }
        }
    }
    
    add_notification(title, msg, icon, urgency, duration);
    
    Notification *last = notif_head;
    while (last && last->next) last = last->next;
    
    pthread_t tid;
    pthread_create(&tid, NULL, toast_thread, last);
    pthread_detach(tid);
    
    const char *response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"status\":\"ok\"}\n";
    write(client_sock, response, strlen(response));
}

void *http_server(void *arg) {
    (void)arg;
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT)
    };
    
    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        exit(1);
    }
    
    listen(server_sock, 5);
    printf("Notification server listening on port %d\n", PORT);
    
    while (1) {
        int client_sock = accept(server_sock, NULL, NULL);
        if (client_sock < 0) continue;
        
        handle_request(client_sock);
        close(client_sock);
    }
    
    return NULL;
}

void toggle_notification_center() {
    center_visible = !center_visible;
    if (center_visible) {
        show_notification_center();
    } else {
        XUnmapWindow(ndisp.dpy, ndisp.win);
        XFlush(ndisp.dpy);
    }
}

void dismiss_notification_at(int y) {
    pthread_mutex_lock(&notif_mutex);
    int y_offset = LINE_SPACING;
    Notification *curr = notif_head;
    
    while (curr) {
        if (!curr->dismissed) {
            if (y >= y_offset && y < y_offset + curr->height) {
                curr->dismissed = 1;
                break;
            }
            y_offset += curr->height + LINE_SPACING;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&notif_mutex);
    
    if (center_visible) {
        show_notification_center();
    }
}

void cleanup_old_notifications() {
    pthread_mutex_lock(&notif_mutex);
    
    Notification *prev = NULL;
    Notification *curr = notif_head;
    
    while (curr) {
        if (curr->dismissed) {
            if (prev) {
                prev->next = curr->next;
                Notification *to_free = curr;
                curr = curr->next;
                free(to_free);
            } else {
                notif_head = curr->next;
                Notification *to_free = curr;
                curr = curr->next;
                free(to_free);
            }
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
    
    pthread_mutex_unlock(&notif_mutex);
}

int main() {
    init_display();
    
    pthread_t server_thread;
    pthread_create(&server_thread, NULL, http_server, NULL);
    
    printf("LMK Manager started\n");
    
    signal(SIGUSR1, (void (*)(int))toggle_notification_center);
    
    XEvent ev;
    time_t last_cleanup = time(NULL);
    
    while (1) {
        while (XPending(ndisp.dpy)) {
            XNextEvent(ndisp.dpy, &ev);
            
            if (ev.type == ButtonPress && center_visible) {
                dismiss_notification_at(ev.xbutton.y);
            } else if (ev.type == Expose && center_visible) {
                show_notification_center();
            }
        }
        
        time_t now = time(NULL);
        if (now - last_cleanup > 60) {
            cleanup_old_notifications();
            last_cleanup = now;
        }
        
        usleep(50000);
    }
    
    XCloseDisplay(ndisp.dpy);
    return 0;
}
