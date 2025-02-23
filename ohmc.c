/** includes **/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/** defines **/

#define OHMC_VERSION "0.0.1"
#define OHMC_TABSTOP 8
#define OHMC_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum key_mappings {
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/** data **/

typedef struct editor_row {
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

struct state {
	int rows, cols; /* Screen rows and columns */
	int cx, cy; /* Cursor x and y position */
	int rx;
	int rowoff; /* Row scrolling offset */
	int coloff; /* Column offset */
	int numrows;
	erow *row;
	int dirty; /* Whether the file has been modified */
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

struct state S;

/** prototypes **/

void set_status_message(const char *fmt, ...);
void refresh_screen();
char *editor_prompt(char *prompt);

/** terminal **/

void
die(const char *s)
{
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void
disable_raw_mode()
{
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &S.orig_termios) == -1) die("tcsetattr");
}

void
enable_raw_mode()
{
	if (tcgetattr(STDIN_FILENO, &S.orig_termios) == -1) die("tcgetattr");
	atexit(disable_raw_mode);

	struct termios raw = S.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int
read_key()
{
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) die("read"); 
	}

	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
					switch (seq[1]) {
					case '1': return HOME_KEY;
					case '3': return DEL_KEY;
					case '4': return END_KEY;
					case '5': return PAGE_UP;
					case '6': return PAGE_DOWN;
					case '7': return HOME_KEY;
					case '8': return END_KEY;
					}
				}
			} else {
				switch (seq[1]) {
				case 'A': return ARROW_UP;
				case 'B': return ARROW_DOWN;
				case 'C': return ARROW_RIGHT;
				case 'D': return ARROW_LEFT;
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
			case 'H': return HOME_KEY;
			case 'F': return END_KEY;
			}
		}

		return '\x1b';
	}
	return c;
}

int
get_cursor_position(int *rows, int *cols)
{
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int
get_window_size(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return get_cursor_position(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/** row operations **/

int
row_cx_to_rx(erow *row, int cx)
{
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t')
			rx += (OHMC_TABSTOP - 1) - (rx % OHMC_TABSTOP);
		rx++;
	}
	return rx;
}

void
update_row(erow *row)
{
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++)
		if (row->chars[j] == '\t') tabs++;
	free(row->render);
	row->render = malloc(row->size + tabs*(OHMC_TABSTOP - 1) + 1);
	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % OHMC_TABSTOP != 0) row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void
insert_row(int at, char *s, size_t len)
{
	if (at < 0 || at > S.numrows) return;

	S.row = realloc(S.row, sizeof(erow) * (S.numrows + 1));
	memmove(&S.row[at+1], &S.row[at], sizeof(erow) * (S.numrows - at));

	S.row[at].size = len;
	S.row[at].chars = malloc(len + 1);
	memcpy(S.row[at].chars, s, len);
	S.row[at].chars[len] = '\0';

	S.row[at].rsize = 0;
	S.row[at].render = NULL;
	update_row(&S.row[at]);

	S.numrows++;
	S.dirty++;
}

void
free_row(erow *row)
{
	free(row->render);
	free(row->chars);
}

void
del_row(int at)
{
	if (at < 0 || at >= S.numrows) return;
	free_row(&S.row[at]);
	memmove(&S.row[at], &S.row[at+1], sizeof(erow) * (S.numrows - at - 1));
	S.numrows--;
	S.dirty++;
}

void
row_insert_char(erow *row, int at, int c)
{
	if (at < 0 || at > row->size) at = row->size;
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at+1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	update_row(row);
	S.dirty++;
}

void
row_append_string(erow *row, char *s, size_t len)
{
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	update_row(row);
	S.dirty++;
}

void
row_del_char(erow *row, int at)
{
	if (at < 0 || at > row->size) return;
	memmove(&row->chars[at], &row->chars[at+1], row->size - at);
	row->size--;
	update_row(row);
	S.dirty++;
}

/** editor operations **/

void
insert_char(int c)
{
	if (S.cy == S.numrows) {
		insert_row(S.numrows, "", 0);
	}
	row_insert_char(&S.row[S.cy], S.cx, c);
	S.cx++;
}

void
insert_newline()
{
	if (S.cx == 0) {
		insert_row(S.cy, "", 0);
	} else {
		erow *row = &S.row[S.cy];
		insert_row(S.cy+1, &row->chars[S.cx], row->size - S.cx);
		row = &S.row[S.cy];
		row->size = S.cx;
		row->chars[row->size] = '\0';
		update_row(row);
	}
	S.cy++;
	S.cx = 0;
}

void
del_char()
{
	if (S.cy == S.numrows) return;
	if (S.cx == 0 && S.cy == 0) return;

	erow *row = &S.row[S.cy];
	if (S.cx > 0) {
		row_del_char(row, S.cx - 1);
		S.cx--;
	} else {
		S.cx = S.row[S.cy - 1].size;
		row_append_string(&S.row[S.cy - 1], row->chars, row->size);
		del_row(S.cy);
		S.cy--;
	}
}

/** file io **/

char
*rows_to_string(int *buflen)
{
	int totlen = 0;
	for (int j = 0; j < S.numrows; j++) {
		totlen += S.row[j].size + 1;
	}
	*buflen = totlen;

	char *buf = malloc(totlen);
	char *p = buf;
	for (int j = 0; j < S.numrows; j++) {
		memcpy(p, S.row[j].chars, S.row[j].size);
		p += S.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

void
editor_open(char *filename)
{
	free(S.filename);
	S.filename = strdup(filename);

	FILE *fp = fopen(filename, "r");
	if (!fp) die("fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;

		insert_row(S.numrows, line, linelen);
	}
	free(line);
	fclose(fp);

	S.dirty = 0;
}

void
save()
{
	if (S.filename == NULL) {
		S.filename = editor_prompt("Save as: %s (ESC to cancel)");
		if (S.filename == NULL) {
			set_status_message("Aborted save");
			return;
		}
	}

	int len;
	char *buf = rows_to_string(&len);

	int fd = open(S.filename, O_RDWR | O_CREAT, 0644);
	if (fd != 1) {
		if (ftruncate(fd, len) != -1) {
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				S.dirty = 0;
				set_status_message("%d bytes written to disk", len);

				return;
			}
		}
		close(fd);
	}

	free(buf);
	set_status_message("Failed to save: %s", strerror(errno));
}

/** append buffer **/

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void
ab_append(struct abuf *ab, const char *s, int len)
{
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;

	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void
ab_free(struct abuf *ab)
{
	free(ab->b);
}

/** input **/
char
*editor_prompt(char *prompt)
{
	size_t bufsize = 128;
	char *buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while (1) {
		set_status_message(prompt, buf);
		refresh_screen();

		int c = read_key();
		if (c == DEL_KEY || c == BACKSPACE || c == CTRL_KEY('h')) {
			if (buflen != 0) buf[--buflen] = '\0';
		} else if (c == '\x1b') {
			set_status_message("");
			free(buf);
			return NULL;
		} else if (c == '\r') {
			if (buflen != 0) {
				set_status_message("");
				return buf;
			}
		} else if (!iscntrl(c) && c < 128) {
			if (buflen == bufsize - 1) {
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}
	}
}

void
move_cursor(int key)
{
	erow *row = (S.cy >= S.numrows) ? NULL : &S.row[S.cy];

	switch (key) {
	case ARROW_LEFT:
		if (S.cx != 0) {
			S.cx--;
		} else if (S.cy > 0) {
			S.cy--;
			S.cx = S.row[S.cy].size;
		}
		break;
	case ARROW_RIGHT:
		if (row && row->size > S.cx) {
			S.cx++;
		} else if (row && S.cx == row->size) {
			S.cy++;
			S.cx = 0;
		}
		break;
	case ARROW_UP:
		if (S.cy != 0) {
			S.cy--;
		}
		break;
	case ARROW_DOWN:
		if (S.cy < S.numrows) {
			S.cy++;
		}
		break;
	}

	row = (S.cy >= S.numrows) ? NULL : &S.row[S.cy];
	int rowlen = row ? row->size : 0;
	if (S.cx > rowlen) S.cx = rowlen;
}

void
process_keypress()
{
	static int quit_times = OHMC_QUIT_TIMES;
	int c = read_key();

	switch (c) {
	case '\r':
		insert_newline();
		break;
	case CTRL_KEY('q'):
		if (S.dirty && quit_times > 0) {
			set_status_message("WARNING!!! File has unsaved changes. "
			                   "Press Ctrl-Q %d more times to quit.", quit_times);
			quit_times--;
		}
		write(STDOUT_FILENO, "\x1b[2J", 4);
		write(STDOUT_FILENO, "\x1b[H", 3);

		exit(0);
		break;
	case CTRL_KEY('s'):
		save();
		break;
	case PAGE_UP: /* FALLTHROUGH */
	case PAGE_DOWN:
		{
			if (c == PAGE_UP) {
				S.cy = S.rowoff;
			} else if (c == PAGE_DOWN) {
				S.cy = S.rowoff + S.rows - 1;
				if (S.cy > S.numrows) S.cy = S.numrows;
			}

			int times = S.rows;
			while (times--) move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
		}

		break;
	case HOME_KEY:
		S.cx = 0;
		break;
	case END_KEY:
		if (S.cy < S.numrows) S.cx = S.row[S.cy].size;
		break;
	case BACKSPACE: /* FALLTHROUGH */
	case CTRL_KEY('h'):
	case DEL_KEY:
		if (c == DEL_KEY) move_cursor(ARROW_RIGHT);
		del_char();
		break;
	case ARROW_UP: /* FALLTHROUGH */
	case ARROW_DOWN:
	case ARROW_LEFT:
	case ARROW_RIGHT:
		move_cursor(c);
		break;
	case CTRL_KEY('l'): /* FALLTHROUGH */
	case '\x1b':
		break;
	default:
		insert_char(c);
		break;
	}

	quit_times = OHMC_QUIT_TIMES;
}

/** output **/

void
scroll()
{
	S.rx = 0;
	if (S.cy < S.numrows) {
		S.rx = row_cx_to_rx(&S.row[S.cy], S.cx);
	}

	if (S.cy < S.rowoff) {
		S.rowoff = S.cy;
	}
	if (S.cy >= S.rowoff + S.rows) {
		S.rowoff = S.cy - S.rows + 1;
	}

	if (S.rx < S.coloff) {
		S.coloff = S.rx;
	}
	if (S.rx >= S.coloff + S.cols) {
		S.coloff = S.rx - S.cols + 1;
	}
}

void
draw_rows(struct abuf *ab)
{
	for (int y = 0; y < S.rows; y++) {
		int filerow = y + S.rowoff;
		if (filerow >= S.numrows) {
			if (S.numrows == 0 && y == S.rows / 3) {
				char msg[80];
				int msglen = snprintf(msg, sizeof(msg), "ohmc v%s", OHMC_VERSION);
				if (msglen > S.cols) msglen = S.cols;

				int padding = (S.cols - msglen) / 2;
				if (padding) {
					ab_append(ab, "~", 1);
					padding--;
				}
				while (padding--) ab_append(ab, " ", 1);

				ab_append(ab, msg, msglen);
			} else {
				ab_append(ab, "~", 1);
			}
		} else {
			int len = S.row[filerow].rsize - S.coloff;
			if (len < 0) len = 0;
			if (len > S.cols) len = S.cols;
			ab_append(ab, &S.row[filerow].render[S.coloff], len);
		}

		ab_append(ab, "\x1b[K", 3);

		ab_append(ab, "\r\n", 2);
	}
}

void
draw_status_bar(struct abuf *ab)
{
	ab_append(ab, "\x1b[7m", 4);

	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
	                   S.filename ? S.filename : "[No File]", S.numrows,
	                   S.dirty ? "(modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
	                    S.cy + 1, S.numrows);
	if (len > S.cols) len = S.cols;
	ab_append(ab, status, len);

	while (len < S.cols) {
		if (S.cols - len == rlen) {
			ab_append(ab, rstatus, rlen);
			break;
		} else {
			ab_append(ab, " ", 1);
			len++;
		}
	}

	ab_append(ab, "\x1b[m\r\n", 5);
}

void
draw_message_bar(struct abuf *ab)
{
	ab_append(ab, "\x1b[K", 3);
	int msglen = strlen(S.statusmsg);
	if (msglen > S.cols) msglen = S.cols;
	if (msglen && time(NULL) - S.statusmsg_time < 5) ab_append(ab, S.statusmsg, msglen);
}

void
refresh_screen()
{
	scroll();

	struct abuf ab = ABUF_INIT;

	ab_append(&ab, "\x1b[?25l", 6);
	ab_append(&ab, "\x1b[H", 3);

	draw_rows(&ab);
	draw_status_bar(&ab);
	draw_message_bar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (S.cy - S.rowoff) + 1,
	         				  (S.rx - S.coloff) + 1);
	ab_append(&ab, buf, strlen(buf));

	ab_append(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	ab_free(&ab);
}

void
set_status_message(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(S.statusmsg, sizeof(S.statusmsg), fmt, ap);
	va_end(ap);
	S.statusmsg_time = time(NULL);
}

/** init **/

void
init()
{
	enable_raw_mode();

	S.cx = 0;
	S.cy = 0;
	S.rx = 0;
	S.rowoff = 0;
	S.coloff = 0;
	S.numrows = 0;
	S.row = NULL;
	S.dirty = 0;
	S.filename = NULL;
	S.statusmsg[0] = '\0';
	S.statusmsg_time = 0;

	if (get_window_size(&S.rows, &S.cols) == -1) die("get_window_size");
	S.rows -= 2;
}

int
main(int argc, char *argv[])
{
	init();

	if (argc >= 2) {
		editor_open(argv[1]);
	}

	set_status_message("HELP: ctrl+s = save | ctrl+q = quit");

	while (1) {
		refresh_screen();
		process_keypress();
	}

	return 0;
}
