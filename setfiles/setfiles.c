#include "restore.h"
#include <unistd.h>
#include <fcntl.h>
#include <stdio_ext.h>
#include <ctype.h>
#include <regex.h>
#include <sys/vfs.h>
#include <sys/utsname.h>
#define __USE_XOPEN_EXTENDED 1	/* nftw */
#include <libgen.h>
#ifdef USE_AUDIT
#include <libaudit.h>

#ifndef AUDIT_FS_RELABEL
#define AUDIT_FS_RELABEL 2309
#endif
#endif
static int mass_relabel;
static int mass_relabel_errs;


/* cmdline opts*/

static char *policyfile = NULL;
static int warn_no_match = 0;
static int null_terminated = 0;
static int errors;
static int ignore_enoent;
static struct restore_opts r_opts;

#define STAT_BLOCK_SIZE 1



#define SETFILES "setfiles"
#define RESTORECON "restorecon"
static int iamrestorecon;

/* Behavior flags determined based on setfiles vs. restorecon */
static int ctx_validate; /* Validate contexts */
static const char *altpath; /* Alternate path to file_contexts */

void usage(const char *const name)
{
	if (iamrestorecon) {
		fprintf(stderr,
			"usage:  %s [-iFnrRv0] [-e excludedir ] [-o filename ] [-f filename | pathname... ]\n",
			name);
	} else {
		fprintf(stderr,
			"usage:  %s [-dnpqvW] [-o filename] [-r alt_root_path ] spec_file pathname...\n"
			"usage:  %s -c policyfile spec_file\n"
			"usage:  %s -s [-dnqvW] [-o filename ] spec_file\n", name, name,
			name);
	}
	exit(1);
}

static int nerr = 0;

void inc_err()
{
	nerr++;
	if (nerr > 9 && !r_opts.debug) {
		fprintf(stderr, "Exiting after 10 errors.\n");
		exit(1);
	}
}



void set_rootpath(const char *arg)
{
	int len;

	r_opts.rootpath = strdup(arg);
	if (NULL == r_opts.rootpath) {
		fprintf(stderr, "%s:  insufficient memory for r_opts.rootpath\n",
			r_opts.progname);
		exit(1);
	}

	/* trim trailing /, if present */
	len = strlen(r_opts.rootpath);
	while (len && ('/' == r_opts.rootpath[len - 1]))
		r_opts.rootpath[--len] = 0;
	r_opts.rootpathlen = len;
}

int canoncon(char **contextp)
{
	char *context = *contextp, *tmpcon;
	int rc = 0;

	if (policyfile) {
		if (sepol_check_context(context) < 0) {
			fprintf(stderr, "invalid context %s\n", context);
			exit(1);
		}
	} else if (security_canonicalize_context_raw(context, &tmpcon) == 0) {
		free(context);
		*contextp = tmpcon;
	} else if (errno != ENOENT) {
		rc = -1;
		inc_err();
	}

	return rc;
}

#ifndef USE_AUDIT
static void maybe_audit_mass_relabel(void)
{
#else
static void maybe_audit_mass_relabel(void)
{
	int audit_fd = -1;
	int rc = 0;

	if (!mass_relabel)		/* only audit a forced full relabel */
		return;

	audit_fd = audit_open();

	if (audit_fd < 0) {
		fprintf(stderr, "Error connecting to audit system.\n");
		exit(-1);
	}

	rc = audit_log_user_message(audit_fd, AUDIT_FS_RELABEL,
				    "op=mass relabel", NULL, NULL, NULL, !mass_relabel_errs);
	if (rc <= 0) {
		fprintf(stderr, "Error sending audit message: %s.\n",
			strerror(errno));
		/* exit(-1); -- don't exit atm. as fix for eff_cap isn't in most kernels */
	}
	audit_close(audit_fd);
#endif
}

/*
   Search /proc/mounts for all file systems that do not support extended
   attributes and add them to the exclude directory table.  File systems
   that support security labels have the seclabel option.
*/
static void exclude_non_seclabel_mounts()
{
	struct utsname uts;
	FILE *fp;
	size_t len;
	ssize_t num;
	int index = 0, found = 0;
	char *mount_info[4];
	char *buf = NULL, *item;

	/* Check to see if the kernel supports seclabel */
	if (uname(&uts) == 0 && strverscmp(uts.release, "2.6.30") < 0)
		return;
	if (is_selinux_enabled() <= 0)
		return;

	fp = fopen("/proc/mounts", "r");
	if (!fp)
		return;

	while ((num = getline(&buf, &len, fp)) != -1) {
		found = 0;
		index = 0;
		item = strtok(buf, " ");
		while (item != NULL) {
			mount_info[index] = item;
			if (index == 3)
				break;
			index++;
			item = strtok(NULL, " ");
		}
		if (index < 3) {
			fprintf(stderr,
				"/proc/mounts record \"%s\" has incorrect format.\n",
				buf);
			continue;
		}

		/* remove pre-existing entry */
		remove_exclude(mount_info[1]);

		item = strtok(mount_info[3], ",");
		while (item != NULL) {
			if (strcmp(item, "seclabel") == 0) {
				found = 1;
				break;
			}
			item = strtok(NULL, ",");
		}

		/* exclude mount points without the seclabel option */
		if (!found)
			add_exclude(mount_info[1]);
	}

	free(buf);
}

int main(int argc, char **argv)
{
	struct stat sb;
	int opt, i = 0;
	char *input_filename = NULL;
	int use_input_file = 0;
	char *buf = NULL;
	size_t buf_len;
	int recurse; /* Recursive descent. */
	char *base;
	
	memset(&r_opts, 0, sizeof(r_opts));

	/* Initialize variables */
	r_opts.progress = 0;
	r_opts.count = 0;
	r_opts.debug = 0;
	r_opts.change = 1;
	r_opts.verbose = 0;
	r_opts.logging = 0;
	r_opts.rootpath = NULL;
	r_opts.rootpathlen = 0;
	r_opts.outfile = NULL;
	r_opts.force = 0;
	r_opts.hard_links = 1;

	altpath = NULL;

	r_opts.progname = strdup(argv[0]);
	if (!r_opts.progname) {
		fprintf(stderr, "%s:  Out of memory!\n", argv[0]);
		exit(1);
	}
	base = basename(r_opts.progname);
	
	if (!strcmp(base, SETFILES)) {
		/* 
		 * setfiles:  
		 * Recursive descent,
		 * Does not expand paths via realpath, 
		 * Aborts on errors during the file tree walk, 
		 * Try to track inode associations for conflict detection,
		 * Does not follow mounts,
		 * Validates all file contexts at init time. 
		 */
		iamrestorecon = 0;
		recurse = 1;
		r_opts.expand_realpath = 0;
		r_opts.abort_on_error = 1;
		r_opts.add_assoc = 1;
		r_opts.fts_flags = FTS_PHYSICAL | FTS_XDEV;
		ctx_validate = 1;
	} else {
		/*
		 * restorecon:  
		 * No recursive descent unless -r/-R,
		 * Expands paths via realpath, 
		 * Do not abort on errors during the file tree walk,
		 * Do not try to track inode associations for conflict detection,
		 * Follows mounts,
		 * Does lazy validation of contexts upon use. 
		 */
		if (strcmp(base, RESTORECON) && !r_opts.quiet) 
			printf("Executed with an unrecognized name (%s), defaulting to %s behavior.\n", base, RESTORECON);
		iamrestorecon = 1;
		recurse = 0;
		r_opts.expand_realpath = 1;
		r_opts.abort_on_error = 0;
		r_opts.add_assoc = 0;
		r_opts.fts_flags = FTS_PHYSICAL;
		ctx_validate = 0;

		/* restorecon only:  silent exit if no SELinux.
		   Allows unconditional execution by scripts. */
		if (is_selinux_enabled() <= 0)
			exit(0);
	}

	/* This must happen before getopt. */
	exclude_non_seclabel_mounts();

	/* Process any options. */
	while ((opt = getopt(argc, argv, "c:de:f:ilnpqrsvo:FRW0")) > 0) {
		switch (opt) {
		case 'c':
			{
				FILE *policystream;

				if (iamrestorecon)
					usage(argv[0]);

				policyfile = optarg;

				policystream = fopen(policyfile, "r");
				if (!policystream) {
					fprintf(stderr,
						"Error opening %s: %s\n",
						policyfile, strerror(errno));
					exit(1);
				}
				__fsetlocking(policystream,
					      FSETLOCKING_BYCALLER);

				if (sepol_set_policydb_from_file(policystream) <
				    0) {
					fprintf(stderr,
						"Error reading policy %s: %s\n",
						policyfile, strerror(errno));
					exit(1);
				}
				fclose(policystream);

				ctx_validate = 1;

				break;
			}
		case 'e':
			remove_exclude(optarg);
			if (lstat(optarg, &sb) < 0 && errno != EACCES) {
				fprintf(stderr, "Can't stat exclude path \"%s\", %s - ignoring.\n",
					optarg, strerror(errno));
				break;
			}
			if (add_exclude(optarg))
				exit(1);
			break;
		case 'f':
			use_input_file = 1;
			input_filename = optarg;
			break;			
		case 'd':
			r_opts.debug = 1;
			break;
		case 'i':
			ignore_enoent = 1;
			break;
		case 'l':
			r_opts.logging = 1;
			break;
		case 'F':
			r_opts.force = 1;
			break;
		case 'n':
			r_opts.change = 0;
			break;
		case 'o':
			if (strcmp(optarg, "-") == 0) {
				r_opts.outfile = stdout;
				break;
			}

			r_opts.outfile = fopen(optarg, "w");
			if (!r_opts.outfile) {
				fprintf(stderr, "Error opening %s: %s\n",
					optarg, strerror(errno));

				usage(argv[0]);
			}
			__fsetlocking(r_opts.outfile, FSETLOCKING_BYCALLER);
			break;
		case 'q':
			r_opts.quiet = 1;
			break;
		case 'R':
		case 'r':
			if (iamrestorecon) {
				recurse = 1;
				break;
			}
			if (optind + 1 >= argc) {
				fprintf(stderr, "usage:  %s -r r_opts.rootpath\n",
					argv[0]);
				exit(1);
			}
			if (NULL != r_opts.rootpath) {
				fprintf(stderr,
					"%s: only one -r can be specified\n",
					argv[0]);
				exit(1);
			}
			set_rootpath(argv[optind++]);
			break;
		case 's':
			use_input_file = 1;
			input_filename = "-";
			r_opts.add_assoc = 0;
			break;
		case 'v':
			if (r_opts.progress) {
				fprintf(stderr,
					"Progress and Verbose mutually exclusive\n");
				exit(1);
			}
			r_opts.verbose++;
			break;
		case 'p':
			if (r_opts.verbose) {
				fprintf(stderr,
					"Progress and Verbose mutually exclusive\n");
				usage(argv[0]);
			}
			r_opts.progress = 1;
			break;
		case 'W':
			warn_no_match = 1;
			break;
		case '0':
			null_terminated = 1;
			break;
		case '?':
			usage(argv[0]);
		}
	}

	if (!iamrestorecon) {
		if (policyfile) {
			if (optind != (argc - 1))
				usage(argv[0]);
		} else if (use_input_file) {
			if (optind != (argc - 1)) {
				/* Cannot mix with pathname arguments. */
				usage(argv[0]);
			}
		} else {
			if (optind > (argc - 2))
				usage(argv[0]);
		}

		/* Use our own invalid context checking function so that
		   we can support either checking against the active policy or
		   checking against a binary policy file. */
		selinux_set_callback(SELINUX_CB_VALIDATE,
				     (union selinux_callback)&canoncon);

		if (stat(argv[optind], &sb) < 0) {
			perror(argv[optind]);
			exit(1);
		}
		if (!S_ISREG(sb.st_mode)) {
			fprintf(stderr, "%s:  spec file %s is not a regular file.\n",
				argv[0], argv[optind]);
			exit(1);
		}

		altpath = argv[optind];
		optind++;
	}

	/* Load the file contexts configuration and check it. */
	r_opts.selabel_opt_validate = (ctx_validate ? (char *)1 : NULL);
	r_opts.selabel_opt_path = altpath;

	if (nerr)
		exit(1);

	restore_init(&r_opts);
	if (use_input_file) {
		FILE *f = stdin;
		ssize_t len;
		int delim;
		if (strcmp(input_filename, "-") != 0)
			f = fopen(input_filename, "r");
		if (f == NULL) {
			fprintf(stderr, "Unable to open %s: %s\n", input_filename,
				strerror(errno));
			usage(argv[0]);
		}
		__fsetlocking(f, FSETLOCKING_BYCALLER);

		delim = (null_terminated != 0) ? '\0' : '\n';
		while ((len = getdelim(&buf, &buf_len, delim, f)) > 0) {
			buf[len - 1] = 0;
			if (!strcmp(buf, "/"))
				mass_relabel = 1;
			errors |= process_one_realpath(buf, recurse) < 0;
		}
		if (strcmp(input_filename, "-") != 0)
			fclose(f);
	} else {
		for (i = optind; i < argc; i++) {
			if (!strcmp(argv[i], "/"))
				mass_relabel = 1;
			errors |= process_one_realpath(argv[i], recurse) < 0;
		}
	}
	
	if (mass_relabel)
		mass_relabel_errs = errors;
	maybe_audit_mass_relabel();

	if (warn_no_match)
		selabel_stats(r_opts.hnd);

	selabel_close(r_opts.hnd);
	restore_finish();

	if (r_opts.outfile)
		fclose(r_opts.outfile);

       if (r_opts.progress && r_opts.count >= STAR_COUNT)
               printf("\n");
	exit(errors);
}
