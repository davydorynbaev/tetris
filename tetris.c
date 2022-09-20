#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define WELL_HEIGHT	(20)
#define WELL_WIDTH	(10)

#define TILE_EMPTY	(0)
#define TILE_GHOST	(8)

#define ANSI_COLORS_FG	(90)
#define ANSI_COLORS_BG	(ANSI_COLORS_FG + 10)

#define SCREEN_BORDER	"+--------------------+"

#define DELAY_FACTOR		(0.9125f)
#define LEVEL_UP_THRESHOLD	(10)

#define ARROW_UP	('A')
#define ARROW_DOWN	('B')
#define ARROW_RIGHT	('C')
#define ARROW_LEFT	('D')

#define SCORE_SOFT_DROP_BONUS	(1)
#define SCORE_HARD_DROP_BONUS	(2)

enum ret_status {
	RET_SUCCESS,
	RET_FAILURE
};

struct tt_shape {
	const char *s;
	const size_t n;
};

struct tt_state {
	int y;
	int x;
	int t;
	int r;
};

struct tt_game {
	struct tt_state ghost;
	struct tt_state state;
	struct timespec delay;
	int level;
	int rc;
	int score;
	char screen[WELL_HEIGHT * WELL_WIDTH];
	char well[WELL_HEIGHT * WELL_WIDTH];
};

static const struct tt_shape shapes[] = {

	[1] = { "\x0\x0\x0\x0\x1\x1\x1\x1\x0\x0\x0\x0\x0\x0\x0\x0", 4 },
	[2] = { "\x2\x0\x0\x2\x2\x2\x0\x0\x0", 3 },
	[3] = { "\x0\x0\x3\x3\x3\x3\x0\x0\x0", 3 },
	[4] = { "\x4\x4\x4\x4", 2 },
	[5] = { "\x0\x5\x5\x5\x5\x0\x0\x0\x0", 3 },
	[6] = { "\x0\x6\x0\x6\x6\x6\x0\x0\x0", 3 },
	[7] = { "\x7\x7\x0\x0\x7\x7\x0\x0\x0", 3 },

	{ "\x0\x0\x0\x0\x8\x8\x8\x8\x0\x0\x0\x0\x0\x0\x0\x0", 4 },
	{ "\x8\x0\x0\x8\x8\x8\x0\x0\x0", 3 },
	{ "\x0\x0\x8\x8\x8\x8\x0\x0\x0", 3 },
	{ "\x8\x8\x8\x8", 2 },
	{ "\x0\x8\x8\x8\x8\x0\x0\x0\x0", 3 },
	{ "\x0\x8\x0\x8\x8\x8\x0\x0\x0", 3 },
	{ "\x8\x8\x0\x0\x8\x8\x0\x0\x0", 3 }

};
static bool game_over = false;
static pthread_mutex_t mutex_well;
static pthread_mutex_t mutex_draw;

#include "config.h"

static int
find_filled_row(char *well, int y_min, int y_max)
{
	int count;
	int i;
	int j;

	for (i = y_min; (i < y_max) & (i < WELL_HEIGHT); ++i) {
		for (count = j = 0; j < WELL_WIDTH; ++j)
			count += !!well[i * WELL_WIDTH + j];
		if (count >= WELL_WIDTH)
			return i;
	}
	return -1;
}

static int
rotate_index(int sy, int sx, int r, size_t n)
{
	int si;

	si = 0;
	switch (r & 3) {
	case 0:
		si = sy * n + sx;
		break;
	case 1:
		si = n * n - (sx + 1) * n + sy;
		break;
	case 2:
		si = n * n - sy * n - sx - 1;
		break;
	case 3:
		si = (sx + 1) * n - sy - 1;
		break;
	}
	return si;
}

static bool
shape_fits_internal(char *well, int y, int x, int t, int r)
{
	const struct tt_shape tmp = shapes[t];
	int sy;
	int sx;

	for (sy = 0; sy < tmp.n; ++sy)
		for (sx = 0; sx < tmp.n; ++sx)
			if ((tmp.s)[rotate_index(sy, sx, r, tmp.n)]	&&
					(y + sy < 0			||
					 y + sy >= WELL_HEIGHT		||
					 x + sx < 0			||
					 x + sx >= WELL_WIDTH		||
					 well[(y + sy) * WELL_WIDTH + x + sx]))
				return false;
	return true;
}

static bool
shape_fits(char *well, struct tt_state *state)
{
	return shape_fits_internal(well, state->y, state->x, state->t, state->r);
}

static enum ret_status
move(char *well, struct tt_state *state, int dy, int dx)
{
	if (shape_fits_internal(well, state->y + dy, state->x + dx, state->t, state->r)) {
		state->y += dy;
		state->x += dx;
		return RET_SUCCESS;
	}
	return RET_FAILURE;
}

static enum ret_status
rotate(char *well, struct tt_state *state)
{
	if (state->t == 4) /* Rotating the square shape would be wasteful. */
		return RET_FAILURE;
	if (shape_fits_internal(well, state->y, state->x, state->t, state->r + 1)) {
		++(state->r);
		return RET_SUCCESS;
	}
	return RET_FAILURE;
}

static int
clear_rows(char *well, struct tt_state *state)
{
	const struct tt_shape tmp = shapes[state->t];
	int i;
	int n;

	for (n = 0; (i = find_filled_row(well, state->y + n, state->y + tmp.n)) >= 0; ++n)
		for (i = i * WELL_WIDTH - 1; i >= 0; --i)
			well[i + WELL_WIDTH] = well[i];
	return n;
}

static void
insert_shape(char *well, struct tt_state *state)
{
	const struct tt_shape tmp = shapes[state->t];
	int sy;
	int sx;
	int si;
	int fi;

	for (sy = 0; sy < tmp.n; ++sy) {
		for (sx = 0; sx < tmp.n; ++sx) {
			si = rotate_index(sy, sx, state->r, tmp.n);
			fi = (state->y + sy) * WELL_WIDTH + (state->x + sx);
			well[fi] |= (tmp.s)[si];
		}
	}
}

static inline void
clear_display(void)
{
	fputs("\x1b[0;0H\x1b[2J", stdout);
}

static void
print_tile(int t)
{
#ifdef DTETRIS_LITE
	static const char *tile_lookup[] =  {
		"\x1b[7m  \x1b[0m",
		" ."
	};

	fputs(tile_lookup[!t], stdout);
#else
	switch (t) {
	case TILE_EMPTY: 
		fputs(" .", stdout);
		break;
	case TILE_GHOST:
		fputs("[]", stdout);
		break;
	default:
		printf("\x1b[0;%dm  \x1b[0m", ANSI_COLORS_BG + t);
		break;
	}
#endif
}

#ifndef DTETRIS_LITE
static inline void
reset_ghost(struct tt_state *ghost, struct tt_state *state)
{
	memcpy(ghost, state, sizeof(struct tt_state));
	ghost->t += 7;
}
#endif

static inline void
reset_state(struct tt_state *state)
{
	state->y = 0;
	state->x = (WELL_WIDTH >> 1) - 2;
	state->t = (rand() % 7) + 1;
	state->r = 0;
}

static inline void
set_state(struct tt_state *state, int y, int x, int t, int r)
{
	state->y = y;
	state->x = x;
	state->t = t;
	state->r = r;
}

static void
draw(struct tt_game *game)
{
	int i;
	int j;

	pthread_mutex_lock(&mutex_draw);
	memcpy(game->screen, game->well, WELL_HEIGHT * WELL_WIDTH);
	insert_shape(game->screen, &game->state);
#ifndef DTETRIS_LITE
	reset_ghost(&game->ghost, &game->state);
	for (i = 0; i < WELL_HEIGHT * WELL_WIDTH; ++i)
		(game->screen)[i] &= ~TILE_GHOST;
	while (move(game->well, &game->ghost, 1, 0) != RET_FAILURE);
	insert_shape(game->screen, &game->ghost);
#endif
	clear_display();
	puts(SCREEN_BORDER);
	for (i = 0; i < WELL_HEIGHT; ++i) {
		fputs("|", stdout);
		for (j = 0; j < WELL_WIDTH; ++j)
			print_tile((game->screen)[i * WELL_WIDTH + j]);
		fputs("|\n", stdout);
	}
	puts(SCREEN_BORDER);
	pthread_mutex_unlock(&mutex_draw);
}

static enum ret_status
end_turn(struct tt_game *game)
{
	static const int rc_scores_lookup[] = { 0, 40, 100, 300, 1200 }; 
	int rc;

	pthread_mutex_lock(&mutex_well);
	insert_shape(game->well, &game->state);
	if ((rc = clear_rows(game->well, &game->state)) > 0) {
		game->rc += rc;
		game->score += (rc_scores_lookup[rc] * game->level);
		if (game->rc >= LEVEL_UP_THRESHOLD) {
			game->rc %= LEVEL_UP_THRESHOLD;
			game->delay.tv_nsec *= DELAY_FACTOR;
			++game->level;
		}
	}
	reset_state(&game->state);
	pthread_mutex_unlock(&mutex_well);
	if (!shape_fits(game->well, &game->state))
		return RET_FAILURE;
	return RET_SUCCESS;
}

static int
read_key(void)
{
	char c;

	return (read(0, &c, 1) == 1) ? (unsigned char) c : EOF;
}

static void *
fall_loop(void *arg)
{
	struct tt_game *game = arg;

loop_head:
	if (game_over)
		return NULL;
	nanosleep(&game->delay, NULL);
	if (move(game->well, &game->state, 1, 0) == RET_FAILURE &&
			end_turn(game) == RET_FAILURE)
		return NULL;
	draw(game);
	goto loop_head;
}

static void
game_loop(struct tt_game *game)
{
loop_head:
	if (game_over)
		return;
	switch (read_key()) {
	case ARROW_DOWN:
	case KEY_MOVE_DOWN:
		if (move(game->well, &game->state, 1, 0) == RET_SUCCESS)
			game->score += SCORE_SOFT_DROP_BONUS;
		else if (end_turn(game) == RET_FAILURE)
			return;
		draw(game);
		break;
	case ARROW_LEFT:
	case KEY_MOVE_LEFT:
		if (move(game->well, &game->state, 0, -1) == RET_SUCCESS)
			draw(game);
		break;
	case ARROW_RIGHT:
	case KEY_MOVE_RIGHT:
		if (move(game->well, &game->state, 0, 1) == RET_SUCCESS)
			draw(game);
		break;
	case ARROW_UP:
	case KEY_ROTATE:
		if (rotate(game->well, &game->state) == RET_SUCCESS)
			draw(game);
		break;
	case ' ':
		while (move(game->well, &game->state, 1, 0) != RET_FAILURE)
			game->score += SCORE_HARD_DROP_BONUS;
		if (end_turn(game) == RET_FAILURE)
			return;
		draw(game);
		break;
	case 'q':
		return;
	}
	goto loop_head;
}

static inline void
hide_cursor(void)
{
	fputs("\x1b[?25l", stdout);
}

static inline void
show_cursor(void)
{
	fputs("\x1b[?25h", stdout);
}

static inline void
init_term(struct termios *term_old, struct termios *term_new)
{
	tcgetattr(STDIN_FILENO, term_old);
	*term_new = *term_old;
	term_new->c_lflag &= ~(ICANON | ECHO);
	tcsetattr(STDIN_FILENO, TCSANOW, term_new);
	hide_cursor();
}

static inline void
exit_term(struct termios *term_old, struct termios *term_new)
{
	show_cursor();
	tcsetattr(STDIN_FILENO, TCSANOW, term_old);
}

int
main(int argc, char *argv[])
{
	struct termios term_old;
	struct termios term_new;
	struct tt_game game = {
		.level = 1,
		.score = 0,
		.rc = 0,
		.delay = { 0, INITIAL_DELAY_SECONDS * 1000000000 }
	};
	pthread_t loop_thread;

	init_term(&term_old, &term_new);
	srand(time(0));
	reset_state(&game.state);
#ifndef DTETRIS_LITE
	reset_ghost(&game.ghost, &game.state);
#endif
	pthread_mutex_init(&mutex_well, NULL);
	pthread_mutex_init(&mutex_draw, NULL);
	draw(&game);
	if (pthread_create(&loop_thread, NULL, fall_loop, &game)) {
		fprintf(stderr, "Cannot create loop_thread.\n");
		exit(EXIT_FAILURE);
	}
	game_loop(&game);
	game_over = true;
	if (pthread_join(loop_thread, NULL)) {
		fprintf(stderr, "Cannot join loop_thread.\n");
		exit(EXIT_FAILURE);
	}
	clear_display();
	exit_term(&term_old, &term_new);
	printf("Final score: %d\n", game.score);
	return EXIT_SUCCESS;
}
