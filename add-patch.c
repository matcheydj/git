#include "cache.h"
#include "add-interactive.h"
#include "strbuf.h"
#include "run-command.h"
#include "argv-array.h"
#include "pathspec.h"
#include "color.h"
#include "diff.h"

struct hunk_header {
	unsigned long old_offset, old_count, new_offset, new_count;
	/*
	 * Start/end offsets to the extra text after the second `@@` in the
	 * hunk header, e.g. the function signature. This is expected to
	 * include the newline.
	 */
	size_t extra_start, extra_end, colored_extra_start, colored_extra_end;
};

struct hunk {
	size_t start, end, colored_start, colored_end;
	enum { UNDECIDED_HUNK = 0, SKIP_HUNK, USE_HUNK } use;
	struct hunk_header header;
};

struct add_p_state {
	struct add_i_state state;
	struct strbuf answer, buf;

	/* parsed diff */
	struct strbuf plain, colored;
	struct file_diff {
		struct hunk head;
		struct hunk *hunk;
		size_t hunk_nr, hunk_alloc;
	} *file_diff;
	size_t file_diff_nr;
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
			 INDEX_ENVIRONMENT "=%s", state->state.r->index_file);
}

static int parse_range(const char **p,
		       unsigned long *offset, unsigned long *count)
{
	char *pend;

	*offset = strtoul(*p, &pend, 10);
	if (pend == *p)
		return -1;
	if (*pend != ',') {
		*count = 1;
		*p = pend;
		return 0;
	}
	*count = strtoul(pend + 1, (char **)p, 10);
	return *p == pend + 1 ? -1 : 0;
}

static int parse_hunk_header(struct add_p_state *state, struct hunk *hunk)
{
	struct hunk_header *header = &hunk->header;
	const char *line = state->plain.buf + hunk->start, *p = line;
	char *eol = memchr(p, '\n', state->plain.len - hunk->start);

	if (!eol)
		eol = state->plain.buf + state->plain.len;

	if (!skip_prefix(p, "@@ -", &p) ||
	    parse_range(&p, &header->old_offset, &header->old_count) < 0 ||
	    !skip_prefix(p, " +", &p) ||
	    parse_range(&p, &header->new_offset, &header->new_count) < 0 ||
	    !skip_prefix(p, " @@", &p))
		return error(_("could not parse hunk header '%.*s'"),
			     (int)(eol - line), line);

	hunk->start = eol - state->plain.buf + (*eol == '\n');
	header->extra_start = p - state->plain.buf;
	header->extra_end = hunk->start;

	if (!state->colored.len) {
		header->colored_extra_start = header->colored_extra_end = 0;
		return 0;
	}

	/* Now find the extra text in the colored diff */
	line = state->colored.buf + hunk->colored_start;
	eol = memchr(line, '\n', state->colored.len - hunk->colored_start);
	if (!eol)
		eol = state->colored.buf + state->colored.len;
	p = memmem(line, eol - line, "@@ -", 4);
	if (!p)
		return error(_("could not parse colored hunk header '%.*s'"),
			     (int)(eol - line), line);
	p = memmem(p + 4, eol - p - 4, " @@", 3);
	if (!p)
		return error(_("could not parse colored hunk header '%.*s'"),
			     (int)(eol - line), line);
	hunk->colored_start = eol - state->colored.buf + (*eol == '\n');
	header->colored_extra_start = p + 3 - state->colored.buf;
	header->colored_extra_end = hunk->colored_start;

	return 0;
}

static int parse_diff(struct add_p_state *state, const struct pathspec *ps)
{
	struct argv_array args = ARGV_ARRAY_INIT;
	struct strbuf *plain = &state->plain, *colored = NULL;
	struct child_process cp = CHILD_PROCESS_INIT;
	char *p, *pend, *colored_p = NULL, *colored_pend = NULL;
	size_t file_diff_alloc = 0, i, color_arg_index;
	struct file_diff *file_diff = NULL;
	struct hunk *hunk = NULL;
	int res;

	/* Use `--no-color` explicitly, just in case `diff.color = always`. */
	argv_array_pushl(&args, "diff-files", "-p", "--no-color", "--", NULL);
	color_arg_index = args.argc - 2;
	for (i = 0; i < ps->nr; i++)
		argv_array_push(&args, ps->items[i].original);

	setup_child_process(&cp, state, NULL);
	cp.argv = args.argv;
	res = capture_command(&cp, plain, 0);
	if (res) {
		argv_array_clear(&args);
		return error(_("could not parse diff"));
	}
	if (!plain->len) {
		argv_array_clear(&args);
		return 0;
	}
	strbuf_complete_line(plain);

	if (want_color_fd(1, -1)) {
		struct child_process colored_cp = CHILD_PROCESS_INIT;

		setup_child_process(&colored_cp, state, NULL);
		xsnprintf((char *)args.argv[color_arg_index], 8, "--color");
		colored_cp.argv = args.argv;
		colored = &state->colored;
		res = capture_command(&colored_cp, colored, 0);
		argv_array_clear(&args);
		if (res)
			return error(_("could not parse colored diff"));
		strbuf_complete_line(colored);
		colored_p = colored->buf;
		colored_pend = colored_p + colored->len;
	}
	argv_array_clear(&args);

	/* parse files and hunks */
	p = plain->buf;
	pend = p + plain->len;
	while (p != pend) {
		char *eol = memchr(p, '\n', pend - p);
		if (!eol)
			eol = pend;

		if (starts_with(p, "diff ")) {
			state->file_diff_nr++;
			ALLOC_GROW(state->file_diff, state->file_diff_nr,
				   file_diff_alloc);
			file_diff = state->file_diff + state->file_diff_nr - 1;
			memset(file_diff, 0, sizeof(*file_diff));
			hunk = &file_diff->head;
			hunk->start = p - plain->buf;
			if (colored_p)
				hunk->colored_start = colored_p - colored->buf;
		} else if (p == plain->buf)
			BUG("diff starts with unexpected line:\n"
			    "%.*s\n", (int)(eol - p), p);
		else if (starts_with(p, "@@ ")) {
			file_diff->hunk_nr++;
			ALLOC_GROW(file_diff->hunk, file_diff->hunk_nr,
				   file_diff->hunk_alloc);
			hunk = file_diff->hunk + file_diff->hunk_nr - 1;
			memset(hunk, 0, sizeof(*hunk));

			hunk->start = p - plain->buf;
			if (colored)
				hunk->colored_start = colored_p - colored->buf;

			if (parse_hunk_header(state, hunk) < 0)
				return -1;
		}

		p = eol == pend ? pend : eol + 1;
		hunk->end = p - plain->buf;

		if (colored) {
			char *colored_eol = memchr(colored_p, '\n',
						   colored_pend - colored_p);
			if (colored_eol)
				colored_p = colored_eol + 1;
			else
				colored_p = colored_pend;

			hunk->colored_end = colored_p - colored->buf;
		}
	}

	return 0;
}

static void render_hunk(struct add_p_state *state, struct hunk *hunk,
			ssize_t delta, int colored, struct strbuf *out)
{
	struct hunk_header *header = &hunk->header;

	if (hunk->header.old_offset != 0 || hunk->header.new_offset != 0) {
		/*
		 * Generate the hunk header dynamically, except for special
		 * hunks (such as the diff header).
		 */
		const char *p;
		size_t len;

		if (!colored) {
			p = state->plain.buf + header->extra_start;
			len = header->extra_end - header->extra_start;
		} else {
			strbuf_addstr(out, state->state.fraginfo_color);
			p = state->colored.buf + header->colored_extra_start;
			len = header->colored_extra_end
				- header->colored_extra_start;
		}

		strbuf_addf(out, "@@ -%lu,%lu +%lu,%lu @@",
			    header->old_offset, header->old_count,
			    (unsigned long)(header->new_offset + delta),
			    header->new_count);
		if (len)
			strbuf_add(out, p, len);
		else if (colored)
			strbuf_addf(out, "%s\n", GIT_COLOR_RESET);
		else
			strbuf_addch(out, '\n');
	}

	if (colored)
		strbuf_add(out, state->colored.buf + hunk->colored_start,
			   hunk->colored_end - hunk->colored_start);
	else
		strbuf_add(out, state->plain.buf + hunk->start,
			   hunk->end - hunk->start);
}

static void reassemble_patch(struct add_p_state *state,
			     struct file_diff *file_diff, struct strbuf *out)
{
	struct hunk *hunk;
	size_t i;
	ssize_t delta = 0;

	render_hunk(state, &file_diff->head, 0, 0, out);

	for (i = 0; i < file_diff->hunk_nr; i++) {
		hunk = file_diff->hunk + i;
		if (hunk->use != USE_HUNK)
			delta += hunk->header.old_count
				- hunk->header.new_count;
		else
			render_hunk(state, hunk, delta, 0, out);
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

static int patch_update_file(struct add_p_state *state,
			     struct file_diff *file_diff)
{
	size_t hunk_index = 0;
	ssize_t i, undecided_previous, undecided_next;
	struct hunk *hunk;
	char ch;
	struct child_process cp = CHILD_PROCESS_INIT;
	int colored = !!state->colored.len;

	if (!file_diff->hunk_nr)
		return 0;

	strbuf_reset(&state->buf);
	render_hunk(state, &file_diff->head, 0, colored, &state->buf);
	fputs(state->buf.buf, stdout);
	for (;;) {
		if (hunk_index >= file_diff->hunk_nr)
			hunk_index = 0;
		hunk = file_diff->hunk + hunk_index;

		undecided_previous = -1;
		for (i = hunk_index - 1; i >= 0; i--)
			if (file_diff->hunk[i].use == UNDECIDED_HUNK) {
				undecided_previous = i;
				break;
			}

		undecided_next = -1;
		for (i = hunk_index + 1; i < file_diff->hunk_nr; i++)
			if (file_diff->hunk[i].use == UNDECIDED_HUNK) {
				undecided_next = i;
				break;
			}

		/* Everything decided? */
		if (undecided_previous < 0 && undecided_next < 0 &&
		    hunk->use != UNDECIDED_HUNK)
			break;

		strbuf_reset(&state->buf);
		render_hunk(state, hunk, 0, colored, &state->buf);
		fputs(state->buf.buf, stdout);

		strbuf_reset(&state->buf);
		if (undecided_previous >= 0)
			strbuf_addstr(&state->buf, ",k");
		if (hunk_index)
			strbuf_addstr(&state->buf, ",K");
		if (undecided_next >= 0)
			strbuf_addstr(&state->buf, ",j");
		if (hunk_index + 1 < file_diff->hunk_nr)
			strbuf_addstr(&state->buf, ",J");
		color_fprintf(stdout, state->state.prompt_color,
			      _("Stage this hunk [y,n,a,d%s,?]? "),
			      state->buf.buf);
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
			while (++hunk_index < file_diff->hunk_nr &&
			       file_diff->hunk[hunk_index].use
			       != UNDECIDED_HUNK)
				; /* continue looking */
		} else if (ch == 'n') {
			hunk->use = SKIP_HUNK;
			goto soft_increment;
		} else if (ch == 'a') {
			for (; hunk_index < file_diff->hunk_nr; hunk_index++) {
				hunk = file_diff->hunk + hunk_index;
				if (hunk->use == UNDECIDED_HUNK)
					hunk->use = USE_HUNK;
			}
		} else if (ch == 'd') {
			for (; hunk_index < file_diff->hunk_nr; hunk_index++) {
				hunk = file_diff->hunk + hunk_index;
				if (hunk->use == UNDECIDED_HUNK)
					hunk->use = SKIP_HUNK;
			}
		} else if (state->answer.buf[0] == 'K') {
			if (hunk_index)
				hunk_index--;
			else
				color_fprintf_ln(stderr,
						 state->state.error_color,
						 _("No previous hunk"));
		} else if (state->answer.buf[0] == 'J') {
			if (hunk_index + 1 < file_diff->hunk_nr)
				hunk_index++;
			else
				color_fprintf_ln(stderr,
						 state->state.error_color,
						 _("No next hunk"));
		} else if (state->answer.buf[0] == 'k') {
			if (undecided_previous >= 0)
				hunk_index = undecided_previous;
			else
				color_fprintf_ln(stderr,
						 state->state.error_color,
						 _("No previous hunk"));
		} else if (state->answer.buf[0] == 'j') {
			if (undecided_next >= 0)
				hunk_index = undecided_next;
			else
				color_fprintf_ln(stderr,
						 state->state.error_color,
						 _("No next hunk"));
		} else
			color_fprintf(stdout, state->state.error_color,
				      _(help_patch_text));
	}

	/* Any hunk to be used? */
	for (i = 0; i < file_diff->hunk_nr; i++)
		if (file_diff->hunk[i].use == USE_HUNK)
			break;

	if (i < file_diff->hunk_nr) {
		/* At least one hunk selected: apply */
		strbuf_reset(&state->buf);
		reassemble_patch(state, file_diff, &state->buf);

		setup_child_process(&cp, state, "apply", "--cached", NULL);
		if (pipe_command(&cp, state->buf.buf, state->buf.len,
				 NULL, 0, NULL, 0))
			error(_("'git apply --cached' failed"));
		repo_refresh_and_write_index(state->state.r, REFRESH_QUIET, 0);
	}

	putchar('\n');
	return 0;
}

int run_add_p(struct repository *r, const struct pathspec *ps)
{
	struct add_p_state state = {
		{ r }, STRBUF_INIT, STRBUF_INIT, STRBUF_INIT, STRBUF_INIT
	};
	size_t i;

	if (init_add_i_state(r, &state.state))
		return error("Could not read `add -i` config");

	if (repo_refresh_and_write_index(r, REFRESH_QUIET, 0) < 0 ||
	    parse_diff(&state, ps) < 0) {
		strbuf_release(&state.plain);
		strbuf_release(&state.colored);
		return -1;
	}

	for (i = 0; i < state.file_diff_nr; i++)
		if (patch_update_file(&state, state.file_diff + i))
			break;

	strbuf_release(&state.answer);
	strbuf_release(&state.buf);
	strbuf_release(&state.plain);
	strbuf_release(&state.colored);
	return 0;
}
