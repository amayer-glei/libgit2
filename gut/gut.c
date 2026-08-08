/*
 * gut.exe - rewrite commit metadata so the committer matches the author.
 *
 * Default action amends HEAD: the committer (name, email and timestamp)
 * is replaced with the author signature.  --author / --date override the
 * author (and, by sync, the committer) like `git commit --amend`, except
 * that --author alone keeps the original author timestamp.
 *
 * "gut rebase <upstream>" (or "gut --rebase <upstream>") applies the fix
 * to every commit in <upstream>..HEAD, like `git rebase <upstream>`.
 * --sync-name / --sync-date select which parts of the author signature
 * are synced onto the committer (default: both).
 */

#include <git2.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
# include <io.h>
# include <windows.h>
#endif

/*
 * All text handled here (commit messages, author names, paths) is UTF-8.
 * On Windows consoles, print through WriteConsoleW so non-ASCII text
 * (umlauts, emoji, ...) renders correctly regardless of the console code
 * page; when redirected to a file or pipe, write plain UTF-8 bytes.
 */
static void write_utf8(FILE *stream, const char *text, size_t len)
{
#ifdef _WIN32
	HANDLE h = (HANDLE)_get_osfhandle(_fileno(stream));
	DWORD mode, written;
	wchar_t *wtext;
	int wlen;

	if (h != INVALID_HANDLE_VALUE && h != NULL &&
	    GetConsoleMode(h, &mode)) {
		wlen = MultiByteToWideChar(CP_UTF8, 0, text, (int)len, NULL, 0);
		if (wlen > 0 && (wtext = malloc((size_t)wlen * sizeof(wchar_t)))) {
			MultiByteToWideChar(CP_UTF8, 0, text, (int)len, wtext, wlen);
			WriteConsoleW(h, wtext, (DWORD)wlen, &written, NULL);
			free(wtext);
			return;
		}
	}
#endif
	fwrite(text, 1, len, stream);
}

static void vprint(FILE *stream, const char *fmt, va_list ap)
{
	char stack[4096], *buf = stack;
	va_list ap2;
	int len;

	va_copy(ap2, ap);
	len = vsnprintf(stack, sizeof(stack), fmt, ap);
	if (len >= 0 && (size_t)len >= sizeof(stack)) {
		buf = malloc((size_t)len + 1);
		if (buf)
			len = vsnprintf(buf, (size_t)len + 1, fmt, ap2);
		else
			len = -1;
	}
	va_end(ap2);

	if (len > 0)
		write_utf8(stream, buf, (size_t)len);
	if (buf != stack)
		free(buf);
}

static void out_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprint(stdout, fmt, ap);
	va_end(ap);
}

static void err_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprint(stderr, fmt, ap);
	va_end(ap);
}

static void usage(FILE *out)
{
	static const char text[] =
		"gut - fix committer metadata to match the author\n"
		"\n"
		"usage:\n"
		"  gut [options]                          amend the last commit (HEAD)\n"
		"  gut rebase <upstream> [sync options]   fix all commits in <upstream>..HEAD\n"
		"  gut --rebase <upstream> [sync options] same as above\n"
		"\n"
		"amend options:\n"
		"  --author=\"Name <email>\"   override author name/email (the author\n"
		"                            timestamp is kept, unlike git); the committer\n"
		"                            is synced to the new author\n"
		"  --date=<date>             override author timestamp; committer is synced.\n"
		"                            accepted: now, @<epoch> <+-HHMM>, <epoch>,\n"
		"                            YYYY-MM-DD[T ]HH:MM[:SS][Z| +-HHMM]\n"
		"\n"
		"rebase sync options (default: sync both name/email and timestamp):\n"
		"  --sync-name               sync only author name/email onto the committer\n"
		"                            (committer timestamp is set to now)\n"
		"  --sync-date               sync only author timestamp onto the committer\n"
		"                            (committer name/email from user.name/user.email)\n"
		"\n"
		"other:\n"
		"  -h, --help                show this help\n"
		"\n"
		"note: on cmd.exe/PowerShell quote values containing < > or spaces.\n";

	write_utf8(out, text, strlen(text));
}

static void die(const char *fmt, ...)
{
	va_list ap;

	err_printf("gut: ");
	va_start(ap, fmt);
	vprint(stderr, fmt, ap);
	va_end(ap);
	err_printf("\n");
	exit(1);
}

static void die_git(const char *fmt, ...)
{
	const git_error *e = git_error_last();
	va_list ap;

	err_printf("gut: ");
	va_start(ap, fmt);
	vprint(stderr, fmt, ap);
	va_end(ap);
	if (e && e->message)
		err_printf(": %s", e->message);
	err_printf("\n");
	exit(1);
}

static char *xstrndup(const char *s, size_t n)
{
	char *p = malloc(n + 1);

	if (!p)
		die("out of memory");
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

/* local UTC offset in minutes at the given epoch */
static int local_offset_at(git_time_t t)
{
	time_t tt = (time_t)t;
	struct tm lcl;

	if (localtime_s(&lcl, &tt) != 0)
		return 0;
	return (int)(difftime(_mkgmtime(&lcl), tt) / 60);
}

/* parse "+HHMM" / "-HHMM" / "+HH:MM" into minutes */
static int parse_tz(const char *s, int *offset_out)
{
	int sign, hh, mm;
	size_t len;

	if (*s == '+')
		sign = 1;
	else if (*s == '-')
		sign = -1;
	else
		return -1;
	s++;
	len = strlen(s);

	if (len == 4 && sscanf(s, "%2d%2d", &hh, &mm) != 2)
		return -1;
	if (len == 6 && sscanf(s, "%2d:%2d", &hh, &mm) != 2)
		return -1;
	if (len != 4 && len != 6)
		return -1;
	if (hh > 14 || mm > 59)
		return -1;

	*offset_out = sign * (hh * 60 + mm);
	return 0;
}

/*
 * Parse a date: "now", [@]<epoch>[ <tz>], or
 * YYYY-MM-DD[T| ]HH:MM[:SS][Z| +-HHMM].  Returns 0 on success.
 */
static int parse_date(const char *str, git_time_t *time_out, int *offset_out)
{
	const char *p;
	char *end;
	long long epoch;
	struct tm tmv;
	int y, mon, d, h = 0, mi = 0, s = 0, n, consumed = 0, off;

	if (!strcmp(str, "now")) {
		epoch = (long long)time(NULL);
		*time_out = (git_time_t)epoch;
		*offset_out = local_offset_at((git_time_t)epoch);
		return 0;
	}

	/* raw epoch, with optional timezone: [@]<epoch>[ <+-HHMM>] */
	p = (*str == '@') ? str + 1 : str;
	epoch = strtoll(p, &end, 10);
	if (end != p && (*end == '\0' || *end == ' ')) {
		if (*end == ' ') {
			if (parse_tz(end + 1, &off) < 0)
				return -1;
		} else {
			off = local_offset_at((git_time_t)epoch);
		}
		*time_out = (git_time_t)epoch;
		*offset_out = off;
		return 0;
	}

	/* ISO 8601 */
	n = sscanf(str, "%d-%d-%d%n", &y, &mon, &d, &consumed);
	if (n != 3 || y < 1970 || mon < 1 || mon > 12 || d < 1 || d > 31)
		return -1;
	p = str + consumed;

	if (*p == 'T' || *p == ' ') {
		const char *q = p + 1;
		int c2 = 0, c3 = 0;

		if (sscanf(q, "%d:%d%n", &h, &mi, &c2) < 2)
			return -1;
		q += c2;
		if (*q == ':') {
			if (sscanf(q + 1, "%d%n", &s, &c3) < 1)
				return -1;
			q += 1 + c3;
		}
		if (h > 23 || mi > 59 || s > 60)
			return -1;
		p = q;
	}
	while (*p == ' ')
		p++;

	memset(&tmv, 0, sizeof(tmv));
	tmv.tm_year = y - 1900;
	tmv.tm_mon = mon - 1;
	tmv.tm_mday = d;
	tmv.tm_hour = h;
	tmv.tm_min = mi;
	tmv.tm_sec = s;

	if (*p == '\0') {
		/* no timezone: interpret as local time */
		tmv.tm_isdst = -1;
		epoch = (long long)mktime(&tmv);
		if (epoch == -1)
			return -1;
		*time_out = (git_time_t)epoch;
		*offset_out = local_offset_at((git_time_t)epoch);
		return 0;
	}

	if (*p == 'Z' && p[1] == '\0')
		off = 0;
	else if (parse_tz(p, &off) < 0)
		return -1;

	epoch = (long long)_mkgmtime(&tmv);
	if (epoch == -1)
		return -1;
	*time_out = (git_time_t)(epoch - (long long)off * 60);
	*offset_out = off;
	return 0;
}

/*
 * Parse "Name <email>" with an optional trailing date
 * ("Name <email> @<epoch> <+-HHMM>", like git ident strings).
 */
static int parse_author(const char *str, char **name_out, char **email_out,
			git_time_t *time_out, int *offset_out)
{
	const char *lt = strchr(str, '<');
	const char *gt = lt ? strchr(lt, '>') : NULL;
	const char *rest;
	size_t nlen;

	if (!lt || !gt)
		return -1;

	nlen = (size_t)(lt - str);
	while (nlen > 0 && (str[nlen - 1] == ' ' || str[nlen - 1] == '\t'))
		nlen--;
	if (nlen == 0 || gt == lt + 1)
		return -1;

	*name_out = xstrndup(str, nlen);
	*email_out = xstrndup(lt + 1, (size_t)(gt - lt - 1));

	rest = gt + 1;
	while (*rest == ' ')
		rest++;
	if (*rest != '\0' && parse_date(rest, time_out, offset_out) < 0) {
		free(*name_out);
		free(*email_out);
		return -1;
	}
	return 0;
}

static const char *head_branch_name(const git_reference *head)
{
	static const char *detached = "detached HEAD";
	const char *name = NULL;

	if (git_reference_is_branch(head) && git_branch_name(&name, head) == 0)
		return name;
	return detached;
}

/* amend HEAD: committer := author, applying --author/--date overrides */
static void do_amend(git_reference *head, git_commit *commit,
		     const char *opt_author, const char *opt_date)
{
	const git_signature *author = git_commit_author(commit);
	const git_signature *author_arg = NULL; /* NULL keeps the original */
	const git_signature *committer_arg = author;
	git_signature *custom = NULL;
	git_oid new_id;
	char short_id[8];

	if (opt_author || opt_date) {
		char *name = NULL, *email = NULL;
		git_time_t when = author->when.time;
		int off = author->when.offset;

		if (opt_author &&
		    parse_author(opt_author, &name, &email, &when, &off) < 0)
			die("malformed --author '%s' (expected \"Name <email>\")",
			    opt_author);
		if (opt_date && parse_date(opt_date, &when, &off) < 0)
			die("malformed --date '%s'", opt_date);

		if (git_signature_new(&custom,
				      name ? name : author->name,
				      email ? email : author->email,
				      when, off) < 0)
			die_git("invalid signature");
		author_arg = custom;
		committer_arg = custom;
		free(name);
		free(email);
	}

	if (git_commit_amend(&new_id, commit, "HEAD", author_arg, committer_arg,
			     NULL, NULL, NULL) < 0)
		die_git("cannot amend HEAD");

	out_printf("[%s %s] %s\n", head_branch_name(head),
		   git_oid_tostr(short_id, sizeof(short_id), &new_id),
		   git_commit_summary(commit));

	git_signature_free(custom);
}

/* rewrite every commit in <upstream>..HEAD, syncing committer to author */
static void do_rebase(git_repository *repo, git_reference *head,
		      git_commit *head_commit, const char *target,
		      int sync_name, int sync_date)
{
	git_annotated_commit *upstream = NULL;
	git_rebase *rebase = NULL;
	git_rebase_options ropts = GIT_REBASE_OPTIONS_INIT;
	git_rebase_operation *op;
	git_status_list *st = NULL;
	git_status_options sopts = GIT_STATUS_OPTIONS_INIT;
	git_signature *defsig = NULL;
	git_oid last, new_id, onto_id;
	const git_oid *orig_head = git_commit_id(head_commit);
	int err;
	/* neither flag or both flags => sync everything */
	const int full_sync = !(sync_name ^ sync_date);

	/* like git rebase: refuse to run with tracked uncommitted changes */
	sopts.flags = 0; /* tracked changes only; untracked files are fine */
	if (git_status_list_new(&st, repo, &sopts) < 0)
		die_git("cannot read working tree status");
	if (git_status_list_entrycount(st) > 0) {
		git_status_list_free(st);
		die("cannot rebase: you have unstaged or staged changes");
	}
	git_status_list_free(st);

	if (git_annotated_commit_from_revspec(&upstream, repo, target) < 0)
		die_git("invalid upstream '%s'", target);

	if (sync_date && !sync_name &&
	    git_signature_default(&defsig, repo) < 0)
		die_git("--sync-date needs a configured identity "
			"(set user.name and user.email)");

	/* work fully in memory; refs and worktree move only on success */
	ropts.inmemory = 1;
	if (git_rebase_init(&rebase, repo, NULL, upstream, NULL, &ropts) < 0)
		die_git("cannot start rebase onto '%s'", target);

	/* note: git_rebase_onto_id() is unset for in-memory rebases */
	git_oid_cpy(&onto_id, git_annotated_commit_id(upstream));
	git_oid_cpy(&last, &onto_id);

	while ((err = git_rebase_next(&op, rebase)) == 0) {
		git_index *idx = NULL;
		git_commit *orig = NULL;
		const git_signature *author, *committer;
		git_signature *tmp = NULL;
		char old_s[8], new_s[8];

		if (git_rebase_inmemory_index(&idx, rebase) < 0)
			die_git("cannot get rebase index");
		if (git_index_has_conflicts(idx)) {
			git_index_conflict_iterator *it;
			const git_index_entry *anc, *our, *their;

			if (git_index_conflict_iterator_new(&it, idx) == 0) {
				while (git_index_conflict_next(&anc, &our, &their, it) == 0) {
					const git_index_entry *e =
						our ? our : (their ? their : anc);
					err_printf("gut: conflict: %s\n", e->path);
				}
				git_index_conflict_iterator_free(it);
			}
			git_index_free(idx);
			die("rebase stopped: conflict applying %s; the repository "
			    "was not changed.\nresolve it with git instead: "
			    "git rebase %s",
			    git_oid_tostr(old_s, sizeof(old_s), &op->id), target);
		}
		git_index_free(idx);

		if (git_commit_lookup(&orig, repo, &op->id) < 0)
			die_git("cannot look up commit %s",
				git_oid_tostr(old_s, sizeof(old_s), &op->id));
		author = git_commit_author(orig);

		if (full_sync) {
			committer = author;
		} else if (sync_date) {
			if (git_signature_new(&tmp, defsig->name, defsig->email,
					      author->when.time,
					      author->when.offset) < 0)
				die_git("invalid signature");
			committer = tmp;
		} else { /* sync_name */
			git_time_t now = (git_time_t)time(NULL);

			if (git_signature_new(&tmp, author->name, author->email,
					      now, local_offset_at(now)) < 0)
				die_git("invalid signature");
			committer = tmp;
		}

		err = git_rebase_commit(&new_id, rebase, NULL, committer,
					NULL, NULL);
		git_signature_free(tmp);

		if (err == GIT_EAPPLIED) {
			out_printf("skip    %s %s (already upstream)\n",
				   git_oid_tostr(old_s, sizeof(old_s), &op->id),
				   git_commit_summary(orig));
		} else if (err < 0) {
			die_git("cannot commit %s",
				git_oid_tostr(old_s, sizeof(old_s), &op->id));
		} else {
			out_printf("rewrite %s -> %s %s\n",
				   git_oid_tostr(old_s, sizeof(old_s), &op->id),
				   git_oid_tostr(new_s, sizeof(new_s), &new_id),
				   git_commit_summary(orig));
			last = new_id;
		}
		git_commit_free(orig);
	}

	if (err != GIT_ITEROVER)
		die_git("rebase failed");

	if (git_oid_equal(&last, orig_head)) {
		out_printf("Current branch %s is up to date.\n",
			   head_branch_name(head));
	} else {
		git_commit *tip = NULL;
		char onto_s[8];

		if (git_commit_lookup(&tip, repo, &last) < 0)
			die_git("cannot look up new tip");
		/* moves the branch (or detached HEAD), index and worktree */
		if (git_reset(repo, (git_object *)tip, GIT_RESET_HARD, NULL) < 0)
			die_git("cannot update HEAD to the rebased tip");
		out_printf("Successfully rebased %s onto %s.\n",
			   head_branch_name(head),
			   git_oid_tostr(onto_s, sizeof(onto_s), &onto_id));
		git_commit_free(tip);
	}

	git_signature_free(defsig);
	git_rebase_free(rebase);
	git_annotated_commit_free(upstream);
}

static int gut_main(int argc, char **argv)
{
	const char *opt_author = NULL, *opt_date = NULL, *target = NULL;
	int rebase_mode = 0, sync_name = 0, sync_date = 0;
	git_repository *repo = NULL;
	git_reference *head = NULL;
	git_object *head_obj = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(arg, "rebase")) {
			if (rebase_mode)
				die("duplicate 'rebase'");
			rebase_mode = 1;
		} else if (!strcmp(arg, "--rebase")) {
			if (rebase_mode)
				die("duplicate rebase option");
			rebase_mode = 1;
			if (++i >= argc)
				die("--rebase requires an <upstream>");
			target = argv[i];
		} else if (!strncmp(arg, "--rebase=", 9)) {
			if (rebase_mode)
				die("duplicate rebase option");
			rebase_mode = 1;
			target = arg + 9;
			if (!*target)
				die("--rebase requires an <upstream>");
		} else if (!strcmp(arg, "--author")) {
			if (++i >= argc)
				die("--author requires a value");
			opt_author = argv[i];
		} else if (!strncmp(arg, "--author=", 9)) {
			opt_author = arg + 9;
		} else if (!strcmp(arg, "--date")) {
			if (++i >= argc)
				die("--date requires a value");
			opt_date = argv[i];
		} else if (!strncmp(arg, "--date=", 7)) {
			opt_date = arg + 7;
		} else if (!strcmp(arg, "--sync-name")) {
			sync_name = 1;
		} else if (!strcmp(arg, "--sync-date")) {
			sync_date = 1;
		} else if (arg[0] == '-') {
			die("unknown option: %s", arg);
		} else {
			if (!rebase_mode)
				die("unexpected argument: '%s' "
				    "(did you mean 'gut rebase %s'?)", arg, arg);
			if (target)
				die("unexpected extra argument: '%s'", arg);
			target = arg;
		}
	}

	if (rebase_mode) {
		if (!target)
			die("rebase requires an <upstream> "
			    "(e.g. 'gut rebase main')");
		if (opt_author || opt_date)
			die("--author/--date cannot be combined with rebase; "
			    "use --sync-name/--sync-date");
	} else if (sync_name || sync_date) {
		die("--sync-name/--sync-date are only valid in rebase mode");
	}

	if (git_libgit2_init() < 0)
		die_git("cannot initialize libgit2");
	if (git_repository_open_ext(&repo, ".", 0, NULL) < 0)
		die_git("cannot find a git repository from here");
	if (git_repository_head(&head, repo) < 0)
		die_git("cannot resolve HEAD (unborn branch?)");
	if (git_reference_peel(&head_obj, head, GIT_OBJECT_COMMIT) < 0)
		die_git("HEAD does not point to a commit");

	if (rebase_mode)
		do_rebase(repo, head, (git_commit *)head_obj, target,
			  sync_name, sync_date);
	else
		do_amend(head, (git_commit *)head_obj, opt_author, opt_date);

	git_object_free(head_obj);
	git_reference_free(head);
	git_repository_free(repo);
	git_libgit2_shutdown();
	return 0;
}

#ifdef _WIN32
/*
 * The narrow CRT argv is encoded in the legacy ANSI code page, which
 * would corrupt non-ASCII arguments (e.g. --author="Björn <b@x>").
 * Take the UTF-16 command line instead and convert it to UTF-8.
 */
static char *wide_to_utf8(const wchar_t *w)
{
	char *s;
	int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);

	if (len <= 0 || !(s = malloc((size_t)len)))
		die("out of memory");
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s, len, NULL, NULL);
	return s;
}

int wmain(int argc, wchar_t *wargv[])
{
	char **argv = calloc((size_t)argc, sizeof(char *));
	int i, code;

	if (!argv)
		die("out of memory");
	for (i = 0; i < argc; i++)
		argv[i] = wide_to_utf8(wargv[i]);

	code = gut_main(argc, argv);

	for (i = 0; i < argc; i++)
		free(argv[i]);
	free(argv);
	return code;
}
#else
int main(int argc, char **argv)
{
	return gut_main(argc, argv);
}
#endif
