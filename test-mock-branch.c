#include "cache.h"
#include "blob.h"
#include "commit.h"
#include "cache-tree.h"
#include "parse-options.h"

/*
 * This helper generates artificial repositories. To do so, it uses a
 * deterministic pseudo random number generator to generate artificial content
 * and content changes.
 *
 * Please note that true randomness (or even cryptigraphically strong) is not
 * required.
 *
 * The following deterministic pseudo-random number generator was adapted from
 * the public domain code on http://burtleburtle.net/bob/rand/smallprng.html.
 */
struct random_context {
	uint32_t a, b, c, d;
};

static inline uint32_t rot(uint32_t x, int bits)
{
	return x << bits | x >> (32 - bits);
}

static uint32_t random_value(struct random_context *context)
{
	uint32_t e = context->a - rot(context->b, 27);
	context->a = context->b ^ rot(context->c, 17);
	context->b = context->c + context->d;
	context->c = context->d + e;
	context->d = e + context->a;
	return context->d;
}

/*
 * Returns a random number in the range 0, ..., range - 1.
 */
static int random_value_in_range(struct random_context *context, int range)
{
	return (int)(random_value(context) *
		     (uint64_t)range / (UINT_MAX + (uint64_t)1));
}

static void random_init(struct random_context *context, uint32_t seed)
{
	int i;

	context->a = 0xf1ea5eed;
	context->b = context->c = context->d = seed;

	for (i = 0; i < 20; i++)
		(void) random_value(context);
}

/*
 * Relatively stupid, but fun, code to generate file content that looks like
 * text in some foreign language.
 */
static void random_word(struct random_context *context, struct strbuf *buf)
{
	static char vowels[] = {
		'a', 'e', 'e', 'i', 'i', 'o', 'u', 'y'
	};
	static char consonants[] = {
		'b', 'c', 'd', 'f', 'g', 'h', 'k', 'l',
		'm', 'n', 'p', 'r', 's', 't', 'v', 'z'
	};
	int syllable_count = 1 + (random_value(context) & 0x3);

	while (syllable_count--) {
		strbuf_addch(buf, consonants[random_value(context) & 0xf]);
		strbuf_addch(buf, vowels[random_value(context) & 0x7]);
	}
}

static void random_sentence(struct random_context *context, struct strbuf *buf)
{
	int word_count = 2 + (random_value(context) & 0xf);

	if (buf->len && buf->buf[buf->len - 1] != '\n')
		strbuf_addch(buf, ' ');

	while (word_count--) {
		random_word(context, buf);
		strbuf_addch(buf, word_count ? ' ' : '.');
	}
}

static void random_paragraph(struct random_context *context, struct strbuf *buf)
{
	int sentence_count = 1 + (random_value(context) & 0x7);

	if (buf->len)
		strbuf_addstr(buf, "\n\n");

	while (sentence_count--)
		random_sentence(context, buf);
}

static void random_content(struct random_context *context, struct strbuf *buf)
{
	int paragraph_count = 1 + (random_value(context) & 0x7);

	while (paragraph_count--)
		random_paragraph(context, buf);
}

/*
 * Relatively stupid, but fun, simulation of what software developers do all
 * day long: change files, add files, occasionally remove files.
 */
static const char *random_new_path(struct random_context *context,
			       struct index_state *istate)
{
	static struct strbuf buf = STRBUF_INIT;
	int pos, count, slash, i;
	const char *name, *slash_p;

	strbuf_reset(&buf);
	if (!istate->cache_nr) {
		random_word(context, &buf);
		return buf.buf;
	}

	pos = random_value_in_range(context, istate->cache_nr);
	name = istate->cache[pos]->name;

	/* determine number of files in the same directory */
	slash_p = strrchr(name, '/');
	slash = slash_p ? slash_p - name + 1 : 0;
	strbuf_add(&buf, name, slash);
	count = 1;
	for (i = pos; i > 0; i--)
		if (strncmp(istate->cache[i - 1]->name, name, slash))
			break;
		else if (!strchr(istate->cache[i - 1]->name + slash, '/'))
			count++;
	for (i = pos + 1; i < istate->cache_nr; i++)
		if (strncmp(istate->cache[i]->name, name, slash))
			break;
		else if (!strchr(istate->cache[i]->name + slash, '/'))
			count++;

	/* Depending how many files there are already, add a new directory */
	if (random_value_in_range(context, 20) < count) {
		int len = buf.len;
		for (;;) {
			strbuf_setlen(&buf, len);
			random_word(context, &buf);
			/* Avoid clashes with existing files or directories */
			i = index_name_pos(istate, buf.buf, buf.len);
			if (i >= 0)
				continue;
			strbuf_addch(&buf, '/');
			i = -1 - i;
			if (i >= istate->cache_nr ||
			    !starts_with(istate->cache[i]->name, buf.buf))
				break;
		}
	}

	/* Make sure that the new path is, in fact, new */
	i = buf.len;
	for (;;) {
		int pos;

		random_word(context, &buf);
		pos = index_name_pos(istate, buf.buf, buf.len);
		if (pos < 0) {
			/* Make sure that we do not clash with a directory */
			for (pos = -1 - pos; pos < istate->cache_nr; pos++) {
				const char *name = istate->cache[pos]->name;
				char c;

				if (!starts_with(name, buf.buf))
					return buf.buf;
				c = name[buf.len];
				if (c > '/')
					return buf.buf;
				if (c == '/')
					break;
			}
			if (pos == istate->cache_nr)
				return buf.buf;
		}
		strbuf_setlen(&buf, i);
	}
}

static void modify_randomly(struct random_context *context, struct strbuf *buf)
{
	int count = 1 + random_value_in_range(context, 5);
	struct strbuf replace = STRBUF_INIT;

	while (count--) {
		int pos = random_value_in_range(context, buf->len);
		const char *eol = strchrnul(buf->buf + pos, '\n');
		int end_pos = pos + random_value_in_range(context,
			eol - buf->buf - pos);
		int new_count;

		while (pos && !isspace(buf->buf[pos - 1]))
			pos--;
		while (end_pos < buf->len && !isspace(buf->buf[end_pos]))
			end_pos++;

		new_count = !pos + random_value_in_range(context,
			(end_pos - pos) / 3);
		/* Do not simply delete ends of paragraphs. */
		if (!new_count && (buf->buf[end_pos] == '\n' ||
				   end_pos == buf->len))
			new_count++;
		strbuf_reset(&replace);
		while (new_count--) {
			if (replace.len)
				strbuf_addch(&replace, ' ');
			random_word(context, &replace);
		}
		if (buf->buf[end_pos] == '\n' || end_pos == buf->len)
			strbuf_addch(&replace, '.');
		strbuf_splice(buf, pos, end_pos - pos,
			      replace.buf, replace.len);
	}

	strbuf_release(&replace);
}

static void random_work(struct random_context *context,
			struct index_state *istate)
{
	int count, delete = 0, add = 0, modify;

	/* Obey a totally made-up distribution how many files to remove */
	if (istate->cache_nr > 20) {
		delete = !random_value_in_range(context, 40);
		if (!random_value_in_range(context, 100))
			delete++;
	}

	/* Obey a totally made-up distribution how many files to add */
	add = !istate->cache_nr;
	if (!random_value_in_range(context, istate->cache_nr < 5 ? 2 : 5)) {
		add++;
		if (!random_value_in_range(context, 2)) {
			add++;
			if (!random_value_in_range(context, 3))
				add++;
		}
	}

	/* Obey a totally made-up distribution how many files to modify */
	modify = !(delete + add) + random_value_in_range(context, 5);
	if (modify > istate->cache_nr)
		modify = istate->cache_nr;

	count = delete;
	while (count--) {
		int pos = random_value_in_range(context, istate->cache_nr);

		remove_index_entry_at(istate, pos);
	}

	count = modify;
	while (count--) {
		int pos = random_value_in_range(context, istate->cache_nr);
		enum object_type type;
		unsigned long sz;
		struct strbuf buf;

		buf.buf = read_sha1_file(istate->cache[pos]->sha1, &type, &sz);
		buf.alloc = buf.len = sz;
		strbuf_grow(&buf, 0);
		buf.buf[buf.len] = '\0';

		modify_randomly(context, &buf);
		write_sha1_file(buf.buf, buf.len, blob_type,
				istate->cache[pos]->sha1);
		cache_tree_invalidate_path(istate, istate->cache[pos]->name);
		strbuf_release(&buf);
	}

	count = add;
	while (count--) {
		const char *path = random_new_path(context, istate);
		struct strbuf buf = STRBUF_INIT;
		unsigned char sha1[GIT_SHA1_RAWSZ];
		struct cache_entry *ce;

		random_content(context, &buf);
		write_sha1_file(buf.buf, buf.len, blob_type, sha1);
		ce = make_cache_entry(0644, sha1, path, 0, 0);
		if (!ce || add_index_entry(istate, ce, ADD_CACHE_OK_TO_ADD))
			die("Could not add %s", path);
		strbuf_release(&buf);
	}
}

static void random_commit_message(struct random_context *context,
				  struct strbuf *buf)
{
	int count = 3 + random_value_in_range(context, 4);

	while (count--) {
		if (buf->len)
			strbuf_addch(buf, ' ');
		random_word(context, buf);
	}

	count = random_value_in_range(context, 5);
	while (count--)
		random_paragraph(context, buf);
}

static void random_branch(struct random_context *context,
	int file_count_goal, unsigned char *sha1)
{
	struct index_state istate = { NULL };
	struct commit_list *parents = NULL;
	uint64_t tick = 1234567890ul;
	struct strbuf msg = STRBUF_INIT, date = STRBUF_INIT;

	setenv("GIT_AUTHOR_NAME", "A U Thor", 1);
	setenv("GIT_AUTHOR_EMAIL", "author@email.com", 1);
	setenv("GIT_COMMITTER_NAME", "C O M Mitter", 1);
	setenv("GIT_COMMITTER_EMAIL", "committer@mitter.com", 1);

	istate.cache_tree = cache_tree();

	while (istate.cache_nr < file_count_goal) {
		random_work(context, &istate);

		if (msg.len) {
			parents = NULL;
			commit_list_insert(lookup_commit(sha1), &parents);
			strbuf_reset(&msg);
		}
		random_commit_message(context, &msg);

		strbuf_reset(&date);
		strbuf_addf(&date, "%" PRIu64 " -0400", tick);
		setenv("GIT_COMMITTER_DATE", date.buf, 1);
		setenv("GIT_AUTHOR_DATE", date.buf, 1);
		tick += 60 + random_value_in_range(context, 86400 * 3);

		if (cache_tree_update(&istate, 0) ||
		    commit_tree(msg.buf, msg.len, istate.cache_tree->sha1,
				parents, sha1, NULL, NULL))
			die("Could not commit (parent: %s)",
			    parents ? sha1_to_hex(sha1) : "(none)");
	}
}

int main(int argc, char **argv)
{
	struct random_context context;
	unsigned char sha1[GIT_SHA1_RAWSZ];
	int seed = 123, target_file_count = 50;
	const char * const usage[] = { argv[0], NULL };
	struct option options[] = {
		OPT_INTEGER(0, "seed", &seed,
			"seed number for the pseudo-random number generator"),
		OPT_INTEGER(0, "target-file-count", &target_file_count,
			"stop generating revisions at this number of files"),
		OPT_END(),
	};

	argc = parse_options(argc, (const char **)argv,
		NULL, options, usage, 0);
	if (argc)
		usage_with_options(usage, options);

	random_init(&context, seed);
	random_branch(&context, target_file_count, sha1);

	printf("%s", sha1_to_hex(sha1));

	return 0;
}
