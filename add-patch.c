#include "cache.h"
#include "add-interactive.h"
#include "strbuf.h"
#include "run-command.h"
#include "argv-array.h"
#include "pathspec.h"

struct hunk {
	size_t start, end;
	enum { UNDECIDED_HUNK = 0, SKIP_HUNK, USE_HUNK } use;
};

struct add_p_state {
	struct repository *r;
	struct strbuf answer, buf;

	/* parsed diff */
	struct strbuf plain;
	struct hunk head;
	struct hunk *hunk;
	size_t hunk_nr, hunk_alloc;
};

static void setup_child_process(struct child_process *cp,
				struct add_p_state *state, ...)
{
	va_list ap;
	const char *arg;

	va_start(ap, state);
	while((arg = va_arg(ap, const char *)))
		argv_array_push(&cp->args, arg);
	va_end(ap);

	cp->git_cmd = 1;
	argv_array_pushf(&cp->env_array,
			 INDEX_ENVIRONMENT "=%s", state->r->index_file);
}

static int parse_diff(struct add_p_state *state, const struct pathspec *ps)
{
	struct strbuf *plain = &state->plain;
	struct child_process cp = CHILD_PROCESS_INIT;
	char *p, *pend;
	size_t i;
	struct hunk *hunk = NULL;
	int res;

	/* Use `--no-color` explicitly, just in case `diff.color = always`. */
	setup_child_process(&cp, state,
			 "diff-files", "-p", "--no-color", "--", NULL);
	for (i = 0; i < ps->nr; i++)
		argv_array_push(&cp.args, ps->items[i].original);

	res = capture_command(&cp, plain, 0);
	if (res)
		return error(_("could not parse diff"));
	if (!plain->len)
		return 0;
	strbuf_complete_line(plain);

	/* parse hunks */
	p = plain->buf;
	pend = p + plain->len;
	while (p != pend) {
		char *eol = memchr(p, '\n', pend - p);
		if (!eol)
			eol = pend;

		if (starts_with(p, "diff ")) {
			if (p != plain->buf)
				BUG("multi-file diff not yet handled");
			hunk = &state->head;
		} else if (p == plain->buf)
			BUG("diff starts with unexpected line:\n"
			    "%.*s\n", (int)(eol - p), p);
		else if (starts_with(p, "@@ ")) {
			state->hunk_nr++;
			ALLOC_GROW(state->hunk, state->hunk_nr,
				   state->hunk_alloc);
			hunk = state->hunk + state->hunk_nr - 1;
			memset(hunk, 0, sizeof(*hunk));

			hunk->start = p - plain->buf;
		}

		p = eol == pend ? pend : eol + 1;
		hunk->end = p - plain->buf;
	}

	return 0;
}

static void render_hunk(struct add_p_state *state, struct hunk *hunk,
			struct strbuf *out)
{
	strbuf_add(out, state->plain.buf + hunk->start,
		   hunk->end - hunk->start);
}

static void reassemble_patch(struct add_p_state *state, struct strbuf *out)
{
	struct hunk *hunk;
	size_t i;

	render_hunk(state, &state->head, out);

	for (i = 0; i < state->hunk_nr; i++) {
		hunk = state->hunk + i;
		if (hunk->use == USE_HUNK)
			render_hunk(state, hunk, out);
	}
}

static const char help_patch_text[] =
N_("y - stage this hunk\n"
   "n - do not stage this hunk\n"
   "a - stage this and all the remaining hunks\n"
   "d - do not stage this hunk nor any of the remaining hunks\n"
   "j - leave this hunk undecided, see next undecided hunk\n"
   "J - leave this hunk undecided, see next hunk\n"
   "k - leave this hunk undecided, see previous undecided hunk\n"
   "K - leave this hunk undecided, see previous hunk\n"
   "? - print help\n");

static int patch_update_file(struct add_p_state *state)
{
	size_t hunk_index = 0;
	ssize_t i, undecided_previous, undecided_next;
	struct hunk *hunk;
	char ch;
	struct child_process cp = CHILD_PROCESS_INIT;

	if (!state->hunk_nr)
		return 0;

	strbuf_reset(&state->buf);
	render_hunk(state, &state->head, &state->buf);
	fputs(state->buf.buf, stdout);
	for (;;) {
		if (hunk_index >= state->hunk_nr)
			hunk_index = 0;
		hunk = state->hunk + hunk_index;

		undecided_previous = -1;
		for (i = hunk_index - 1; i >= 0; i--)
			if (state->hunk[i].use == UNDECIDED_HUNK) {
				undecided_previous = i;
				break;
			}

		undecided_next = -1;
		for (i = hunk_index + 1; i < state->hunk_nr; i++)
			if (state->hunk[i].use == UNDECIDED_HUNK) {
				undecided_next = i;
				break;
			}

		/* Everything decided? */
		if (undecided_previous < 0 && undecided_next < 0 &&
		    hunk->use != UNDECIDED_HUNK)
			break;

		strbuf_reset(&state->buf);
		render_hunk(state, hunk, &state->buf);
		fputs(state->buf.buf, stdout);

		strbuf_reset(&state->buf);
		if (undecided_previous >= 0)
			strbuf_addstr(&state->buf, ",k");
		if (hunk_index)
			strbuf_addstr(&state->buf, ",K");
		if (undecided_next >= 0)
			strbuf_addstr(&state->buf, ",j");
		if (hunk_index + 1 < state->hunk_nr)
			strbuf_addstr(&state->buf, ",J");
		printf(_("Stage this hunk [y,n,a,d%s,?]? "), state->buf.buf);
		fflush(stdout);
		if (strbuf_getline(&state->answer, stdin) == EOF)
			break;
		strbuf_trim_trailing_newline(&state->answer);

		if (!state->answer.len)
			continue;
		ch = tolower(state->answer.buf[0]);
		if (ch == 'y') {
			hunk->use = USE_HUNK;
soft_increment:
			while (++hunk_index < state->hunk_nr &&
			       state->hunk[hunk_index].use
			       != UNDECIDED_HUNK)
				; /* continue looking */
		} else if (ch == 'n') {
			hunk->use = SKIP_HUNK;
			goto soft_increment;
		} else if (ch == 'a') {
			for (; hunk_index < state->hunk_nr; hunk_index++) {
				hunk = state->hunk + hunk_index;
				if (hunk->use == UNDECIDED_HUNK)
					hunk->use = USE_HUNK;
			}
		} else if (ch == 'd') {
			for (; hunk_index < state->hunk_nr; hunk_index++) {
				hunk = state->hunk + hunk_index;
				if (hunk->use == UNDECIDED_HUNK)
					hunk->use = SKIP_HUNK;
			}
		} else if (hunk_index && state->answer.buf[0] == 'K')
			hunk_index--;
		else if (hunk_index + 1 < state->hunk_nr &&
			 state->answer.buf[0] == 'J')
			hunk_index++;
		else if (undecided_previous >= 0 &&
			 state->answer.buf[0] == 'k')
			hunk_index = undecided_previous;
		else if (undecided_next >= 0 && state->answer.buf[0] == 'j')
			hunk_index = undecided_next;
		else
			puts(_(help_patch_text));
	}

	/* Any hunk to be used? */
	for (i = 0; i < state->hunk_nr; i++)
		if (state->hunk[i].use == USE_HUNK)
			break;

	if (i < state->hunk_nr) {
		/* At least one hunk selected: apply */
		strbuf_reset(&state->buf);
		reassemble_patch(state, &state->buf);

		setup_child_process(&cp, state, "apply", "--cached", NULL);
		if (pipe_command(&cp, state->buf.buf, state->buf.len,
				 NULL, 0, NULL, 0))
			error(_("'git apply --cached' failed"));
		repo_refresh_and_write_index(state->r, REFRESH_QUIET, 0);
	}

	putchar('\n');
	return 0;
}

int run_add_p(struct repository *r, const struct pathspec *ps)
{
	struct add_p_state state = { r, STRBUF_INIT, STRBUF_INIT, STRBUF_INIT };

	if (repo_refresh_and_write_index(r, REFRESH_QUIET, 0) < 0 ||
	    parse_diff(&state, ps) < 0) {
		strbuf_release(&state.plain);
		return -1;
	}

	if (state.hunk_nr)
		patch_update_file(&state);

	strbuf_release(&state.answer);
	strbuf_release(&state.buf);
	strbuf_release(&state.plain);
	return 0;
}
