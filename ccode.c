/**
 * Includes:
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/**
 * Defines:
 */
#define CTRL_KEY(k) ((k) & 0x1f)

/**
 * Data:
 */

struct editorConfig
{
	struct termios orig_termios;
};
struct editorConfig E;

/**
 * Terminal
 */
void die(const char *s)
{
	write(STDOUT_FILENO, "\x1b[2j]", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disableRawMode()
{
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		die("tcsetattr");
}

void enableRawMode()
{
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
		die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_iflag &= ~(ICRNL | IXON);
	raw.c_iflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("tcsetattr");
}

char editorReadKey()
{
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
	{
		if (nread == -1 && errno != EAGAIN)
			die("read");
	}
	return c;
}

/**
 * Output
 */

void editorRefreshScreen()
{
	write(STDOUT_FILENO, "\x1b[2j]", 4);
	// \x1b - escape character or 27 in decimal
	// [ - escape sequence,
	// 2J - clear entire screen

	write(STDOUT_FILENO, "\x1b[H", 3);
	// H - cursor position

	editorDrawRows();
	write(STDOUT_FILENO, "\x1b[H", 3);
}

void editorDrawRows()
{
	int i;
	for (i = 0; i < 24; i++)
	{
		write(STDOUT_FILENO, "~\r\n", 3);
	}
}

/**
 * Input
 */

void editorProcessKeypress()
{
	char c = editorReadKey();

	switch (c)
	{
	case CTRL_KEY('q'):
		write(STDOUT_FILENO, "\x1b[2j]", 4);
		write(STDOUT_FILENO, "\x1b[H", 3);
		exit(0);
		break;
	}
}

/**
 * Init
 */
int main()
{
	enableRawMode();

	while (1)
	{
		editorProcessKeypress();
	}
	return 0;
}