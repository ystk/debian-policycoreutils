#include "restore.h"

#define SKIP -2
#define ERR -1
#define MAX_EXCLUDES 1000

/*
 * The hash table of associations, hashed by inode number.
 * Chaining is used for collisions, with elements ordered
 * by inode number in each bucket.  Each hash bucket has a dummy 
 * header.
 */
#define HASH_BITS 16
#define HASH_BUCKETS (1 << HASH_BITS)
#define HASH_MASK (HASH_BUCKETS-1)

/*
 * An association between an inode and a context.
 */
typedef struct file_spec {
	ino_t ino;		/* inode number */
	char *con;		/* matched context */
	char *file;		/* full pathname */
	struct file_spec *next;	/* next association in hash bucket chain */
} file_spec_t;

struct edir {
	char *directory;
	size_t size;
};


static file_spec_t *fl_head;
static int exclude(const char *file);
static int filespec_add(ino_t ino, const security_context_t con, const char *file);
static int only_changed_user(const char *a, const char *b);
struct restore_opts *r_opts = NULL;
static void filespec_destroy(void);
static void filespec_eval(void);
static int excludeCtr = 0;
static struct edir excludeArray[MAX_EXCLUDES];

void remove_exclude(const char *directory)
{
	int i = 0;
	for (i = 0; i < excludeCtr; i++) {
		if (strcmp(directory, excludeArray[i].directory) == 0) {
			free(excludeArray[i].directory);
			if (i != excludeCtr-1)
				excludeArray[i] = excludeArray[excludeCtr-1];
			excludeCtr--;
			return;
		}
	}
	return;

}

void restore_init(struct restore_opts *opts)
{	
	r_opts = opts;
	struct selinux_opt selinux_opts[] = {
		{ SELABEL_OPT_VALIDATE, r_opts->selabel_opt_validate },
		{ SELABEL_OPT_PATH, r_opts->selabel_opt_path }
	};
	r_opts->hnd = selabel_open(SELABEL_CTX_FILE, selinux_opts, 2);
	if (!r_opts->hnd) {
		perror(r_opts->selabel_opt_path);
		exit(1);
	}	
}

void restore_finish()
{
	int i;
	for (i = 0; i < excludeCtr; i++) {
		free(excludeArray[i].directory);
	}
}

static int match(const char *name, struct stat *sb, char **con)
{
	if (!(r_opts->hard_links) && !S_ISDIR(sb->st_mode) && (sb->st_nlink > 1)) {
		fprintf(stderr, "Warning! %s refers to a file with more than one hard link, not fixing hard links.\n",
					name);
		return -1;
	}
	
	if (NULL != r_opts->rootpath) {
		if (0 != strncmp(r_opts->rootpath, name, r_opts->rootpathlen)) {
			fprintf(stderr, "%s:  %s is not located in %s\n",
				r_opts->progname, name, r_opts->rootpath);
			return -1;
		}
		name += r_opts->rootpathlen;
	}

	if (r_opts->rootpath != NULL && name[0] == '\0')
		/* this is actually the root dir of the alt root */
		return selabel_lookup_raw(r_opts->hnd, con, "/", sb->st_mode);
	else
		return selabel_lookup_raw(r_opts->hnd, con, name, sb->st_mode);
}
static int restore(FTSENT *ftsent)
{
	char *my_file = strdupa(ftsent->fts_path);
	int ret;
	char *context, *newcon;
	int user_only_changed = 0;

	if (match(my_file, ftsent->fts_statp, &newcon) < 0)
		/* Check for no matching specification. */
		return (errno == ENOENT) ? 0 : -1;

	if (r_opts->progress) {
		r_opts->count++;
		if (r_opts->count % STAR_COUNT == 0) {
			fprintf(stdout, "*");
			fflush(stdout);
		}
	}

	/*
	 * Try to add an association between this inode and
	 * this specification.  If there is already an association
	 * for this inode and it conflicts with this specification,
	 * then use the last matching specification.
	 */
	if (r_opts->add_assoc) {
		ret = filespec_add(ftsent->fts_statp->st_ino, newcon, my_file);
		if (ret < 0)
			goto err;

		if (ret > 0)
			/* There was already an association and it took precedence. */
			goto out;
	}

	if (r_opts->debug) {
		printf("%s:  %s matched by %s\n", r_opts->progname, my_file, newcon);
	}

	/* Get the current context of the file. */
	ret = lgetfilecon_raw(ftsent->fts_accpath, &context);
	if (ret < 0) {
		if (errno == ENODATA) {
			context = NULL;
		} else {
			fprintf(stderr, "%s get context on %s failed: '%s'\n",
				r_opts->progname, my_file, strerror(errno));
			goto err;
		}
		user_only_changed = 0;
	} else
		user_only_changed = only_changed_user(context, newcon);
	/* lgetfilecon returns number of characters and ret needs to be reset
	 * to 0.
	 */
	ret = 0;

	/*
	 * Do not relabel the file if the matching specification is 
	 * <<none>> or the file is already labeled according to the 
	 * specification.
	 */
	if ((strcmp(newcon, "<<none>>") == 0) ||
	    (context && (strcmp(context, newcon) == 0))) {
		freecon(context);
		goto out;
	}

	if (!r_opts->force && context && (is_context_customizable(context) > 0)) {
		if (r_opts->verbose > 1) {
			fprintf(stderr,
				"%s: %s not reset customized by admin to %s\n",
				r_opts->progname, my_file, context);
		}
		freecon(context);
		goto out;
	}

	if (r_opts->verbose) {
		/* If we're just doing "-v", trim out any relabels where
		 * the user has r_opts->changed but the role and type are the
		 * same.  For "-vv", emit everything. */
		if (r_opts->verbose > 1 || !user_only_changed) {
			printf("%s reset %s context %s->%s\n",
			       r_opts->progname, my_file, context ?: "", newcon);
		}
	}

	if (r_opts->logging && !user_only_changed) {
		if (context)
			syslog(LOG_INFO, "relabeling %s from %s to %s\n",
			       my_file, context, newcon);
		else
			syslog(LOG_INFO, "labeling %s to %s\n",
			       my_file, newcon);
	}

	if (r_opts->outfile && !user_only_changed)
		fprintf(r_opts->outfile, "%s\n", my_file);

	if (context)
		freecon(context);

	/*
	 * Do not relabel the file if -n was used.
	 */
	if (!r_opts->change || user_only_changed)
		goto out;

	/*
	 * Relabel the file to the specified context.
	 */
	ret = lsetfilecon(ftsent->fts_accpath, newcon);
	if (ret) {
		fprintf(stderr, "%s set context %s->%s failed:'%s'\n",
			r_opts->progname, my_file, newcon, strerror(errno));
		goto skip;
	}
	ret = 1;
out:
	freecon(newcon);
	return ret;
skip:
	freecon(newcon);
	return SKIP;
err:
	freecon(newcon);
	return ERR;
}
/*
 * Apply the last matching specification to a file.
 * This function is called by fts on each file during
 * the directory traversal.
 */
static int apply_spec(FTSENT *ftsent)
{
	if (ftsent->fts_info == FTS_DNR) {
		fprintf(stderr, "%s:  unable to read directory %s\n",
			r_opts->progname, ftsent->fts_path);
		return SKIP;
	}
	
	int rc = restore(ftsent);
	if (rc == ERR) {
		if (!r_opts->abort_on_error)
			return SKIP;
	}
	return rc;
}

static int symlink_realpath(char *name, char *path)
{
	char *p = NULL, *file_sep;
	char *tmp_path = strdupa(name);
	size_t len = 0;

	if (!tmp_path) {
		fprintf(stderr, "strdupa on %s failed:  %s\n", name,
			strerror(errno));
		return -1;
	}
	file_sep = strrchr(tmp_path, '/');
	if (file_sep == tmp_path) {
		file_sep++;
		p = strcpy(path, "");
	} else if (file_sep) {
		*file_sep = 0;
		file_sep++;
		p = realpath(tmp_path, path);
	} else {
		file_sep = tmp_path;
		p = realpath("./", path);
	}
	if (p)
		len = strlen(p);
	if (!p || len + strlen(file_sep) + 2 > PATH_MAX) {
		fprintf(stderr, "symlink_realpath(%s) failed %s\n", name,
			strerror(errno));
		return -1;
	}
	p += len;
	/* ensure trailing slash of directory name */
	if (len == 0 || *(p - 1) != '/') {
		*p = '/';
		p++;
	}
	strcpy(p, file_sep);
	return 0;
}

static int process_one(char *name, int recurse_this_path)
{
	int rc = 0;
	const char *namelist[2] = {name, NULL};
	dev_t dev_num = 0;
	FTS *fts_handle;
	FTSENT *ftsent;

	fts_handle = fts_open((char **)namelist, r_opts->fts_flags, NULL);
	if (fts_handle  == NULL) {
		fprintf(stderr,
			"%s: error while labeling %s:  %s\n",
			r_opts->progname, namelist[0], strerror(errno));
		goto err;
	}


	ftsent = fts_read(fts_handle);
	if (ftsent != NULL) {
		/* Keep the inode of the first one. */
		dev_num = ftsent->fts_statp->st_dev;
	}

	do {
		rc = 0;
		/* Skip the post order nodes. */
		if (ftsent->fts_info == FTS_DP)
			continue;
		/* If the XDEV flag is set and the device is different */
		if (ftsent->fts_statp->st_dev != dev_num &&
		    FTS_XDEV == (r_opts->fts_flags & FTS_XDEV))
			continue;
		if (excludeCtr > 0) {
			if (exclude(ftsent->fts_path)) {
				fts_set(fts_handle, ftsent, FTS_SKIP);
				continue;
			}
		}
		rc = apply_spec(ftsent);
		if (rc == SKIP)
			fts_set(fts_handle, ftsent, FTS_SKIP);
		if (rc == ERR)
			goto err;
		if (!recurse_this_path)
			break;
	} while ((ftsent = fts_read(fts_handle)) != NULL);

out:
	if (r_opts->add_assoc) {
		if (!r_opts->quiet)
			filespec_eval();
		filespec_destroy();
	}
	if (fts_handle)
		fts_close(fts_handle);
	return rc;

err:
	rc = -1;
	goto out;
}

int process_one_realpath(char *name, int recurse)
{
	int rc = 0;
	char *p;
	struct stat sb;

	if (r_opts == NULL){
		fprintf(stderr,
			"Must call initialize first!");
		return -1;
	}

	if (!r_opts->expand_realpath) {
		return process_one(name, recurse);
	} else {
		rc = lstat(name, &sb);
		if (rc < 0) {
			fprintf(stderr, "%s:  lstat(%s) failed:  %s\n",
				r_opts->progname, name,	strerror(errno));
			return -1;
		}

		if (S_ISLNK(sb.st_mode)) {
			char path[PATH_MAX + 1];

			rc = symlink_realpath(name, path);
			if (rc < 0)
				return rc;
			rc = process_one(path, 0);
			if (rc < 0)
				return rc;

			p = realpath(name, NULL);
			if (p) {
				rc = process_one(p, recurse);
				free(p);
			}
			return rc;
		} else {
			p = realpath(name, NULL);
			if (!p) {
				fprintf(stderr, "realpath(%s) failed %s\n", name,
					strerror(errno));
				return -1;
			}
			rc = process_one(p, recurse);
			free(p);
			return rc;
		}
	}
}

static int exclude(const char *file)
{
	int i = 0;
	for (i = 0; i < excludeCtr; i++) {
		if (strncmp
		    (file, excludeArray[i].directory,
		     excludeArray[i].size) == 0) {
			if (file[excludeArray[i].size] == 0
			    || file[excludeArray[i].size] == '/') {
				return 1;
			}
		}
	}
	return 0;
}

int add_exclude(const char *directory)
{
	size_t len = 0;

	if (directory == NULL || directory[0] != '/') {
		fprintf(stderr, "Full path required for exclude: %s.\n",
			directory);
		return 1;
	}
	if (excludeCtr == MAX_EXCLUDES) {
		fprintf(stderr, "Maximum excludes %d exceeded.\n",
			MAX_EXCLUDES);
		return 1;
	}

	len = strlen(directory);
	while (len > 1 && directory[len - 1] == '/') {
		len--;
	}
	excludeArray[excludeCtr].directory = strndup(directory, len);

	if (excludeArray[excludeCtr].directory == NULL) {
		fprintf(stderr, "Out of memory.\n");
		return 1;
	}
	excludeArray[excludeCtr++].size = len;

	return 0;
}

/* Compare two contexts to see if their differences are "significant",
 * or whether the only difference is in the user. */
static int only_changed_user(const char *a, const char *b)
{
	char *rest_a, *rest_b;	/* Rest of the context after the user */
	if (r_opts->force)
		return 0;
	if (!a || !b)
		return 0;
	rest_a = strchr(a, ':');
	rest_b = strchr(b, ':');
	if (!rest_a || !rest_b)
		return 0;
	return (strcmp(rest_a, rest_b) == 0);
}

/*
 * Evaluate the association hash table distribution.
 */
static void filespec_eval(void)
{
	file_spec_t *fl;
	int h, used, nel, len, longest;

	if (!fl_head)
		return;

	used = 0;
	longest = 0;
	nel = 0;
	for (h = 0; h < HASH_BUCKETS; h++) {
		len = 0;
		for (fl = fl_head[h].next; fl; fl = fl->next) {
			len++;
		}
		if (len)
			used++;
		if (len > longest)
			longest = len;
		nel += len;
	}

	if (r_opts->verbose > 1)
		printf
		    ("%s:  hash table stats: %d elements, %d/%d buckets used, longest chain length %d\n",
		     __FUNCTION__, nel, used, HASH_BUCKETS, longest);
}

/*
 * Destroy the association hash table.
 */
static void filespec_destroy(void)
{
	file_spec_t *fl, *tmp;
	int h;

	if (!fl_head)
		return;

	for (h = 0; h < HASH_BUCKETS; h++) {
		fl = fl_head[h].next;
		while (fl) {
			tmp = fl;
			fl = fl->next;
			freecon(tmp->con);
			free(tmp->file);
			free(tmp);
		}
		fl_head[h].next = NULL;
	}
	free(fl_head);
	fl_head = NULL;
}
/*
 * Try to add an association between an inode and a context.
 * If there is a different context that matched the inode,
 * then use the first context that matched.
 */
static int filespec_add(ino_t ino, const security_context_t con, const char *file)
{
	file_spec_t *prevfl, *fl;
	int h, ret;
	struct stat sb;

	if (!fl_head) {
		fl_head = malloc(sizeof(file_spec_t) * HASH_BUCKETS);
		if (!fl_head)
			goto oom;
		memset(fl_head, 0, sizeof(file_spec_t) * HASH_BUCKETS);
	}

	h = (ino + (ino >> HASH_BITS)) & HASH_MASK;
	for (prevfl = &fl_head[h], fl = fl_head[h].next; fl;
	     prevfl = fl, fl = fl->next) {
		if (ino == fl->ino) {
			ret = lstat(fl->file, &sb);
			if (ret < 0 || sb.st_ino != ino) {
				freecon(fl->con);
				free(fl->file);
				fl->file = strdup(file);
				if (!fl->file)
					goto oom;
				fl->con = strdup(con);
				if (!fl->con)
					goto oom;
				return 1;
			}

			if (strcmp(fl->con, con) == 0)
				return 1;

			fprintf(stderr,
				"%s:  conflicting specifications for %s and %s, using %s.\n",
				__FUNCTION__, file, fl->file, fl->con);
			free(fl->file);
			fl->file = strdup(file);
			if (!fl->file)
				goto oom;
			return 1;
		}

		if (ino > fl->ino)
			break;
	}

	fl = malloc(sizeof(file_spec_t));
	if (!fl)
		goto oom;
	fl->ino = ino;
	fl->con = strdup(con);
	if (!fl->con)
		goto oom_freefl;
	fl->file = strdup(file);
	if (!fl->file)
		goto oom_freefl;
	fl->next = prevfl->next;
	prevfl->next = fl;
	return 0;
      oom_freefl:
	free(fl);
      oom:
	fprintf(stderr,
		"%s:  insufficient memory for file label entry for %s\n",
		__FUNCTION__, file);
	return -1;
}



