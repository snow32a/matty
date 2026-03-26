#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <ft2build.h>
#include <pty.h>
#include FT_FREETYPE_H
#include "types.h"
#include "fonts.h"
FT_Library ftlib;
FT_Face curFace;
int master_fd;
void InitFreetype() { FT_Init_FreeType(&ftlib); }
void LoadFontFromPath(char *path) { FT_New_Face(ftlib, path, 0, &curFace); }
void LoadFontFromBytes(const char *bytes, size_t arrlen) {
	FT_New_Memory_Face(ftlib, (const FT_Byte *)bytes, arrlen, 0, &curFace);
}
void SetFontSize(unsigned int size) { FT_Set_Pixel_Sizes(curFace, 0, size); }
int lineHeight=16;
int charWidth = 8;
int curX=0;
int curY=0;
rgba *framebufraw;
int framebufheight = 600;
rgb curColor={255,255,255};
char* totalText;
size_t totalTextBufSize = 4096;
int renderY=0; //fast scroll down
XImage *framebuf;
void TypeCharacter(char character) {
	int max_width = 0;
	int cur_width = 0;
	int num_lines = 1;
	int max_ascent = 0;
	int max_descent = 0;

	if (character == '\n') {
		if (cur_width > max_width)
			max_width = cur_width;
		cur_width = 0;
        curX=0;
        curY+=lineHeight;

		if(curY > framebufheight){
			framebufheight+=lineHeight;
			framebufraw=realloc(framebufraw,framebufheight*800*sizeof(rgba));
			framebuf->data=(char*)framebufraw;
			framebuf->height+=600;
			renderY+=lineHeight;
		}
		return;
	}
	if(character == '\b'){
		curX-=charWidth;

		for (int row = 0; row < (int)lineHeight; row++) {
			for (int col = 0; col < (int)charWidth; col++) {
				int dst_x = curX + col;
				int dst_y = curY + row;

				if (dst_x < 0 || dst_x >= 800 || dst_y < 0 || dst_y >= framebufheight)
					continue;
				int dst_index = dst_y * 800 + dst_x;

				framebufraw[dst_index].r = 0;
				framebufraw[dst_index].g = 0;
				framebufraw[dst_index].b = 0;
			}
		}
		return;
	}
	if (character == '\r') {
		curX = 0;
		return;
	}

	if (character == '\t') {
		curX = ((curX / charWidth) + 8) * charWidth;
		return;
	}
	if(character == '\a'){
		return;
	}
	FT_Load_Char(curFace, character, FT_LOAD_RENDER);
    FT_GlyphSlot g = curFace->glyph;
    lineHeight = curFace->size->metrics.height >> 6;

	int ascent = g->bitmap_top;
	int descent = g->bitmap.rows - g->bitmap_top;
	if (ascent > max_ascent)
		max_ascent = ascent;
	if (descent > max_descent)
		max_descent = descent;

	cur_width += g->advance.x >> 6;
	if (cur_width > max_width)
		max_width = cur_width;

    charWidth=max_width;

	FT_Load_Char(curFace, character, FT_LOAD_RENDER);
	g = curFace->glyph;

	int x = 0 + g->bitmap_left;
    int baseline = curFace->size->metrics.ascender >> 6;
    int y = baseline - g->bitmap_top;
	printf("%i\n",(int)g->bitmap.rows);
	for (int row = 0; row < (int)lineHeight; row++) {
		for (int col = 0; col < (int)g->bitmap.width; col++) {
			int dst_x = curX + x + col;
			int dst_y = curY + y + row;

			if (dst_x < 0 || dst_x >= 800 || dst_y < 0 || dst_y >= 600)
				continue;
			int dst_index = dst_y * 800 + dst_x;

			framebufraw[dst_index].r = 0;
			framebufraw[dst_index].g = 0;
			framebufraw[dst_index].b = 0;
		}
	}
	if(g->bitmap.rows+curY > framebufheight){
		framebufheight+=lineHeight;
		framebufraw=realloc(framebufraw,framebufheight*800*sizeof(rgba));
		framebuf->data=(char*)framebufraw;
		framebuf->height+=lineHeight;
		renderY+=lineHeight;
	}
	for (int row = 0; row < (int)g->bitmap.rows; row++) {
		for (int col = 0; col < (int)g->bitmap.width; col++) {
			int dst_x = curX + x + col;
			int dst_y = curY + y + row;

            if (dst_x < 0 || dst_x >= 800 || dst_y < 0 || dst_y >= framebufheight)
                continue;
            int dst_index = dst_y * 800 + dst_x;
			int src_index = row * g->bitmap.pitch + col;
			unsigned char a = g->bitmap.buffer[src_index];

			framebufraw[dst_index].r = 0;
			framebufraw[dst_index].g = 0;
			framebufraw[dst_index].b = 0;
			framebufraw[dst_index].r = a * curColor.r / 255;
			framebufraw[dst_index].g = a * curColor.g / 255;
			framebufraw[dst_index].b = a * curColor.b / 255;
			framebufraw[dst_index].a = 255;
		}
	}
	curX+=max_width;
}
void RenderFullScreen(){
	char* coolpointer = totalText;
	while(coolpointer){
		TypeCharacter(*coolpointer);
		coolpointer++;
	}
}
char *frametext;
Window MainWindow;
GC gc;
Display *dpy;
typedef enum { STATE_NORMAL, STATE_ESC, STATE_CSI } ParseState;

ParseState state = STATE_NORMAL;
char csi_buf[64];
int csi_len = 0;
void ParseCSIArgs(char *buf, int *args, int *arg_count) {
    char *token = strtok(buf, ";");
    *arg_count = 0;

    while (token != NULL && *arg_count < 16) {
        if (strlen(token) > 0) {
            args[*arg_count] = atoi(token);
        } else {
            args[*arg_count] = 0;
        }
        (*arg_count)++;
        token = strtok(NULL, ";");
    }
}
int savedX;
int savedY;
void HandleCSI(char *buf, long c) {
    int args[16];
    int arg_count = 0;

    ParseCSIArgs(buf, args, &arg_count);

    switch (c){
    case 'H': // Cursor Position
    case 'f': // Horizontal/Vertical Position
        curX = args[1] - 1 * charWidth;
        curY = args[0] - 1 * lineHeight;
        break;
	case 'A': // Cursor Up
		if (arg_count == 0) args[0] = 1;
		curY -= args[0] * lineHeight;
		if (curY < 0) curY = 0;
		break;
	case 'B': // Cursor Down
		if (arg_count == 0) args[0] = 1;
		curY += args[0] * lineHeight;
		if (curY >= 600) curY = 600 - lineHeight;
		break;
	case 'C': // Cursor Forward
		if (arg_count == 0) args[0] = 1;
		curX += args[0] * charWidth;
		if (curX >= 800) curX = 800 - charWidth;
		break;
	case 'D': // Cursor Back
		if (arg_count == 0) args[0] = 1;
		curX -= args[0] * charWidth;
		if (curX < 0) curX = 0;
		break;

    case 's': // Save cursor position
        savedX = curX/charWidth;
        savedY = curY/lineHeight;
        break;

    case 'u': // Restore cursor position
        curX = savedX * charWidth;
        curY = savedY * lineHeight;
        break;
    }
}
void CharHandler(char c){
        switch (state) {
            case STATE_NORMAL:
                if (c == '\033') {
                    state = STATE_ESC;
                } else {
                    int w;
                    int h;
                    TypeCharacter(c);
                }
                break;
            case STATE_ESC:
                if (c == '[') {
                    state = STATE_CSI;
                    csi_len = 0;
                } else {
                    state = STATE_NORMAL;
                }
                break;
            case STATE_CSI:
                if (c >= 0x40 && c <= 0x7E) { // final byte
                    csi_buf[csi_len] = '\0';
                    HandleCSI(csi_buf, c); // dispatch on final char
                    state = STATE_NORMAL;
                } else {
                    if (csi_len < 63)
                        csi_buf[csi_len++] = c;
                }
                break;
        }
}
void RenderTTY() {
	XPutImage(dpy, MainWindow, gc, framebuf, 0, renderY, 0, 0, 800, 600);
}
int main() {
	InitFreetype();
	LoadFontFromBytes(NimbusMono_Regular, sizeof(NimbusMono_Regular));
	dpy = XOpenDisplay(0);
	int screen = DefaultScreen(dpy);
	Window RootWindow = XDefaultRootWindow(dpy);

	int WindowX = 0;
	int WindowY = 0;
	int WindowWidth = 800;
	int WindowHeight = 600;
	int BorderWidth = 0;
	int WindowDepth = CopyFromParent;
	int WindowClass = CopyFromParent;
	Visual *WindowVisual = CopyFromParent;

	int AttributeValueMask = CWBackPixel | CWEventMask;
	XSetWindowAttributes WindowAttributes = {};
	WindowAttributes.background_pixel = 0x000000;
	WindowAttributes.event_mask =
	    StructureNotifyMask | KeyPressMask | KeyReleaseMask | ExposureMask;
	Bool supported;
	XkbSetDetectableAutoRepeat(dpy, True, &supported);
	MainWindow =
	    XCreateWindow(dpy, RootWindow, WindowX, WindowY, WindowWidth,
	                  WindowHeight, BorderWidth, WindowDepth, WindowClass,
	                  WindowVisual, AttributeValueMask, &WindowAttributes);
	XStoreName(dpy, MainWindow, "Matty");
	XMapWindow(dpy, MainWindow);
	framebufraw = calloc(800 * framebufheight, sizeof(rgba));
    SetFontSize(16);
    TypeCharacter('M');
    free(framebufraw);
    framebufraw = calloc(800 * framebufheight, sizeof(rgba));
	framebuf =
	    XCreateImage(dpy, DefaultVisual(dpy, screen), DefaultDepth(dpy, screen),
	                 ZPixmap, 0, (char *)framebufraw, 800, framebufheight, 32, 800 * 4);
	gc = XCreateGC(dpy, MainWindow, 0, NULL);

    printf("charWidth=%d, lineHeight=%d\n", charWidth, lineHeight);  // Debug
    struct winsize ws;
    ws.ws_row = 800/charWidth;
    ws.ws_col = 600/lineHeight;
    ws.ws_xpixel = 800;
    ws.ws_ypixel = 600;
	pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
    ioctl(master_fd, TIOCSWINSZ, &ws);

	if (pid == 0) {

		execvp("sh", (char *[]){"sh", NULL});
		exit(1);
	}
	int IsWindowOpen = 1;
	while (IsWindowOpen) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(master_fd, &fds);
		int xfd = ConnectionNumber(dpy);
		FD_SET(xfd, &fds);
		fcntl(master_fd, F_SETFL, O_NONBLOCK);
		select(xfd > master_fd ? xfd + 1 : master_fd + 1, &fds, NULL, NULL,
		       NULL);

		if (FD_ISSET(master_fd, &fds)) {

            frametext = malloc(4096);
            frametext[0] = '\0';
            int frametextsize = 4096;

			char buf[4097];
			int keepReading = 1;

			while (keepReading) {
				int n = read(master_fd, buf, sizeof(buf) - 1);
				if (n <= 0) {
					keepReading=0;
				}
				if (n == 4096 || strlen(frametext) + n + 1 > frametextsize) {
					void *tmp = realloc(frametext, frametextsize + 4096);
					if (tmp) {
						frametext = tmp;
					}
					frametextsize += 4096;
				}
				if (n > 0) {
                    buf[n+1] = '\0';
                    for(int i = 0; i<n;i++){
                        CharHandler(buf[i]);
                    }
					RenderTTY();
				}
				if (n == 4096) {
					keepReading = 1;
				} else {
					keepReading = 0;
				}
			}
		}

		while (XPending(dpy)) {
			XEvent event;
			XNextEvent(dpy, &event);

			switch (event.type) {
			case Expose: {
				// render text into a diz temp buf
				RenderTTY();
			} break;
			case KeyPress: {
				XKeyPressedEvent *Event = (XKeyEvent *)&event;
				char tmparr[32];
				KeySym ks;
				int len = XLookupString(&event.xkey, tmparr, 2, &ks, NULL);
				if(len==0){
					if(ks==XK_BackSpace){
						unsigned char bkspc = 0x7F;
						write(master_fd, &bkspc, 1);
					}
				}else{
					write(master_fd, tmparr, len);
				}
				RenderTTY();
			} break;
			}
		}
	}
}
