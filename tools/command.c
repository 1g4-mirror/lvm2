/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2017 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <asm/types.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <syslog.h>
#include <sched.h>
#include <dirent.h>
#include <ctype.h>
#include <getopt.h>

/*
 * This file can be compiled by itself as a man page generator.
 */
#ifdef MAN_PAGE_GENERATOR

#define log_error(fmt, args...) \
do { \
	printf(fmt "\n", ##args); \
} while (0)

/* needed to include args.h */
#define ARG_COUNTABLE 0x00000001
#define ARG_GROUPABLE 0x00000002
struct cmd_context;
struct arg_values;

/* needed to include args.h */
static inline int yes_no_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int activation_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int cachemode_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int discards_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int mirrorlog_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int size_kb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int size_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int size_mb_arg_with_percent(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int uint32_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg_with_sign(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg_with_sign_and_percent(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int major_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int minor_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int string_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int tag_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int permission_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int metadatatype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int units_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int segtype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int alloc_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int locktype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int readahead_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int regionsize_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int vgmetadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int pvmetadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int metadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int polloperation_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int writemostly_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int syncaction_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int reportformat_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int configreport_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int configtype_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }

/* needed to include commands.h when building man page generator */
#define CACHE_VGMETADATA        0x00000001
#define PERMITTED_READ_ONLY     0x00000002
#define ALL_VGS_IS_DEFAULT      0x00000004
#define ENABLE_ALL_DEVS         0x00000008      
#define ALLOW_UUID_AS_NAME      0x00000010
#define LOCKD_VG_SH             0x00000020
#define NO_METADATA_PROCESSING  0x00000040
#define REQUIRES_FULL_LABEL_SCAN 0x00000080
#define MUST_USE_ALL_ARGS        0x00000100
#define NO_LVMETAD_AUTOSCAN      0x00000200
#define ENABLE_DUPLICATE_DEVS    0x00000400
#define DISALLOW_TAG_ARGS        0x00000800
#define GET_VGNAME_FROM_OPTIONS  0x00001000

/* create foo_CMD enums for command def ID's in command-lines.in */

enum {
#define cmd(a, b) a ,
#include "cmds.h"
#undef cmd
};

/* create foo_VAL enums for option and position values */

enum {
#define val(a, b, c, d) a ,
#include "vals.h"
#undef val
};

/* create foo_ARG enums for --option's */

enum {
#define arg(a, b, c, d, e, f, g) a ,
#include "args.h"
#undef arg
};

/* create foo_LVP enums for LV properties */

enum {
#define lvp(a, b, c) a,
#include "lv_props.h"
#undef lvp
};

/* create foo_LVT enums for LV types */

enum {
#define lvt(a, b, c) a,
#include "lv_types.h"
#undef lvt
};

#else

#include "tools.h"

#endif

#include "command.h"       /* defines struct command */
#include "command-count.h" /* defines COMMAND_COUNT */


/* see opt_names[] below, also see arg_props[] in tools.h and args.h */

struct opt_name {
	const char *name;       /* "foo_ARG" */
	int opt_enum;           /* foo_ARG */
	const char short_opt;   /* -f */
	char _padding[7];
	const char *long_opt;   /* --foo */
	int val_enum;           /* xyz_VAL when --foo takes a val like "--foo xyz" */
	uint32_t unused1;
	uint32_t unused2;
	const char *desc;
};

/* see val_names[] below, also see val_props[] in tools.h and vals.h */

struct val_name {
	const char *enum_name;  /* "foo_VAL" */
	int val_enum;           /* foo_VAL */
	int (*fn) (struct cmd_context *cmd, struct arg_values *av); /* foo_arg() */
	const char *name;       /* FooVal */
	const char *usage;
};

/* see lvp_names[] below, also see lv_props[] in tools.h and lv_props.h */

struct lvp_name {
	const char *enum_name; /* "is_foo_LVP" */
	int lvp_enum;          /* is_foo_LVP */
	const char *name;      /* "lv_is_foo" */
};

/* see lvt_names[] below, also see lv_types[] in tools.h and lv_types.h */

struct lvt_name {
	const char *enum_name; /* "foo_LVT" */
	int lvt_enum;          /* foo_LVT */
	const char *name;      /* "foo" */
};

/* see cmd_names[] below, one for each unique "ID" in command-lines.in */

struct cmd_name {
	const char *enum_name; /* "foo_CMD" */
	int cmd_enum;          /* foo_CMD */
	const char *name;      /* "foo" from string after ID: */
};

/* create table of value names, e.g. String, and corresponding enum from vals.h */

struct val_name val_names[VAL_COUNT + 1] = {
#define val(a, b, c, d) { # a, a, b, c, d },
#include "vals.h"
#undef val
};

/* create table of option names, e.g. --foo, and corresponding enum from args.h */

struct opt_name opt_names[ARG_COUNT + 1] = {
#define arg(a, b, c, d, e, f, g) { # a, a, b, "", "--" c, d, e, f, g },
#include "args.h"
#undef arg
};

/* create table of lv property names, e.g. lv_is_foo, and corresponding enum from lv_props.h */

struct lvp_name lvp_names[LVP_COUNT + 1] = {
#define lvp(a, b, c) { # a, a, b },
#include "lv_props.h"
#undef lvp
};

/* create table of lv type names, e.g. linear and corresponding enum from lv_types.h */

struct lvt_name lvt_names[LVT_COUNT + 1] = {
#define lvt(a, b, c) { # a, a, b },
#include "lv_types.h"
#undef lvt
};

/* create table of command IDs */

struct cmd_name cmd_names[CMD_COUNT + 1] = {
#define cmd(a, b) { # a, a, # b },
#include "cmds.h"
#undef cmd
};

/*
 * command_names[] and commands[] are defined in lvmcmdline.c when building lvm,
 * but need to be defined here when building the stand-alone man page generator.
 */

#ifdef MAN_PAGE_GENERATOR
struct command_name command_names[MAX_COMMAND_NAMES] = {
#define xx(a, b, c...) { # a, b, c },
#include "commands.h"
#undef xx
};
struct command commands[COMMAND_COUNT];
#else
extern struct command_name command_names[MAX_COMMAND_NAMES]; /* defined in lvmcmdline.c */
extern struct command commands[COMMAND_COUNT]; /* defined in lvmcmdline.c */
#endif

/* array of pointers into opt_names[] that is sorted alphabetically (by long opt name) */

struct opt_name *opt_names_alpha[ARG_COUNT + 1];

/* lvm_all is for recording options that are common for all lvm commands */

struct command lvm_all;

/* saves OO_FOO lines (groups of optional options) to include in multiple defs */

static int oo_line_count;
#define MAX_OO_LINES 256

struct oo_line {
	char *name;
	char *line;
};
static struct oo_line oo_lines[MAX_OO_LINES];

#define REQUIRED 1  /* required option */
#define OPTIONAL 0  /* optional option */
#define IGNORE -1   /* ignore option */

#define MAX_LINE 1024
#define MAX_LINE_ARGC 256
#define DESC_LINE 1024

/*
 * Contains _command_input[] which is command-lines.in with comments
 * removed and wrapped as a string.  The _command_input[] string is
 * used to populate commands[].
 */
#include "command-lines-input.h"

static void add_optional_opt_line(struct command *cmd, int argc, char *argv[]);

/*
 * modifies buf, replacing the sep characters with \0
 * argv pointers point to positions in buf
 */

static char *split_line(char *buf, int *argc, char **argv, char sep)
{
	char *p = buf, *rp = NULL;
	int i;

	argv[0] = p;

	for (i = 1; i < MAX_LINE_ARGC; i++) {
		p = strchr(buf, sep);
		if (!p)
			break;
		*p = '\0';

		argv[i] = p + 1;
		buf = p + 1;
	}
	*argc = i;

	/* we ended by hitting \0, return the point following that */
	if (!rp)
		rp = strchr(buf, '\0') + 1;

	return rp;
}

/* convert value string, e.g. Number, to foo_VAL enum */

static int val_str_to_num(char *str)
{
	char name[32] = { 0 };
	char *new;
	int i;

	/* compare the name before any suffix like _new or _<lvtype> */

	strncpy(name, str, 31);
	if ((new = strstr(name, "_")))
		*new = '\0';

	for (i = 0; i < VAL_COUNT; i++) {
		if (!val_names[i].name)
			break;
		if (!strncmp(name, val_names[i].name, strlen(val_names[i].name)))
			return val_names[i].val_enum;
	}

	return 0;
}

/* convert "--option" to foo_ARG enum */

static int opt_str_to_num(char *str)
{
	char long_name[32];
	char *p;
	int i;

	/*
	 * --foo_long means there are two args entries
	 * for --foo, one with a short option and one
	 * without, and we want the one without the
	 * short option.
	 */
	if (strstr(str, "_long")) {
		strcpy(long_name, str);
		p = strstr(long_name, "_long");
		*p = '\0';

		for (i = 0; i < ARG_COUNT; i++) {
			if (!opt_names[i].long_opt)
				continue;
			/* skip anything with a short opt */
			if (opt_names[i].short_opt)
				continue;
			if (!strcmp(opt_names[i].long_opt, long_name))
				return opt_names[i].opt_enum;
		}

		log_error("Parsing command defs: unknown opt str: %s %s", str, long_name);
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < ARG_COUNT; i++) {
		if (!opt_names[i].long_opt)
			continue;
		/* These are only selected using --foo_long */
		if (strstr(opt_names[i].name, "_long_ARG"))
			continue;
		if (!strcmp(opt_names[i].long_opt, str))
			return opt_names[i].opt_enum;
	}

	log_error("Parsing command defs: unknown opt str: \"%s\"", str);
	exit(EXIT_FAILURE);
}

/* "foo" string to foo_CMD int */

int command_id_to_enum(const char *str)
{
	int i;

	for (i = 1; i < CMD_COUNT; i++) {
		if (!strcmp(str, cmd_names[i].name))
			return cmd_names[i].cmd_enum;
	}
	log_error("Parsing command defs: unknown cmd name %s", str);
	exit(EXIT_FAILURE);
}

/* "lv_is_prop" to is_prop_LVP */

static int lvp_name_to_enum(char *str)
{
	int i;

	for (i = 1; i < LVP_COUNT; i++) {
		if (!strcmp(str, lvp_names[i].name))
			return lvp_names[i].lvp_enum;
	}
	log_error("Parsing command defs: unknown lv property %s", str);
	exit(EXIT_FAILURE);
}

/* "type" to type_LVT */

static int lvt_name_to_enum(char *str)
{
	int i;

	for (i = 1; i < LVT_COUNT; i++) {
		if (!strcmp(str, lvt_names[i].name))
			return lvt_names[i].lvt_enum;
	}
	log_error("Parsing command defs: unknown lv type %s", str);
	exit(EXIT_FAILURE);
}

/* LV_<type> to <type>_LVT */

static int lv_to_enum(char *name)
{
	return lvt_name_to_enum(name + 3);
}

/*
 * LV_<type1>_<type2> to lvt_bits
 *
 * type1 to lvt_enum
 * lvt_bits |= lvt_enum_to_bit(lvt_enum)
 * type2 to lvt_enum
 * lvt_bits |= lvt_enum_to_bit(lvt_enum)
 */

static uint64_t lv_to_bits(char *name)
{
	char buf[64];
	char *argv[MAX_LINE_ARGC];
	uint64_t lvt_bits = 0;
	int lvt_enum;
	int argc;
	int i;

	strcpy(buf, name);

	split_line(buf, &argc, argv, '_');

	/* 0 is "LV" */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "new"))
			continue;
		lvt_enum = lvt_name_to_enum(argv[i]);
		lvt_bits |= lvt_enum_to_bit(lvt_enum);
	}

	return lvt_bits;
}

static struct command_name *find_command_name(const char *name)
{
	int i;

	for (i = 0; i < MAX_COMMAND_NAMES; i++) {
		if (!command_names[i].name)
			break;
		if (!strcmp(command_names[i].name, name))
			return &command_names[i];
	}
	return NULL;
}

static const char *is_command_name(char *str)
{
	int i;

	for (i = 0; i < MAX_COMMAND_NAMES; i++) {
		if (!command_names[i].name)
			break;
		if (!strcmp(command_names[i].name, str))
			return command_names[i].name;
	}
	return NULL;
}

static int is_opt_name(char *str)
{
	if (!strncmp(str, "--", 2))
		return 1;

	if ((str[0] == '-') && (str[1] != '-')) {
		log_error("Parsing command defs: options must be specified in long form: %s", str);
		exit(EXIT_FAILURE);
	}

	return 0;
}

/*
 * "Select" as a pos name means that the position
 * can be empty if the --select option is used.
 */

static int is_pos_name(char *str)
{
	if (!strncmp(str, "VG", 2))
		return 1;
	if (!strncmp(str, "LV", 2))
		return 1;
	if (!strncmp(str, "PV", 2))
		return 1;
	if (!strncmp(str, "Tag", 3))
		return 1;
	if (!strncmp(str, "String", 6))
		return 1;
	if (!strncmp(str, "Select", 6))
		return 1;
	return 0;
}

static int is_oo_definition(char *str)
{
	if (!strncmp(str, "OO_", 3) && strstr(str, ":"))
		return 1;
	return 0;
}

static int is_oo_line(char *str)
{
	if (!strncmp(str, "OO:", 3))
		return 1;
	return 0;
}

static int is_io_line(char *str)
{
	if (!strncmp(str, "IO:", 3))
		return 1;
	return 0;
}

static int is_op_line(char *str)
{
	if (!strncmp(str, "OP:", 3))
		return 1;
	return 0;
}

static int is_desc_line(char *str)
{
	if (!strncmp(str, "DESC:", 5))
		return 1;
	return 0;
}

static int is_flags_line(char *str)
{
	if (!strncmp(str, "FLAGS:", 6))
		return 1;
	return 0;
}

static int is_rule_line(char *str)
{
	if (!strncmp(str, "RULE:", 5))
		return 1;
	return 0;
}

static int is_id_line(char *str)
{
	if (!strncmp(str, "ID:", 3))
		return 1;
	return 0;
}

/*
 * Save a positional arg in a struct arg_def.
 * Parse str for anything that can appear in a position,
 * like VG, VG|LV, VG|LV_linear|LV_striped, etc.
 */

static void set_pos_def(struct command *cmd, char *str, struct arg_def *def)
{
	char *argv[MAX_LINE_ARGC];
	int argc;
	char *name;
	int val_enum;
	int i;

	split_line(str, &argc, argv, '|');

	for (i = 0; i < argc; i++) {
		name = argv[i];

		val_enum = val_str_to_num(name);

		if (!val_enum) {
			log_error("Parsing command defs: unknown pos arg: %s", name);
			exit(EXIT_FAILURE);
		}

		def->val_bits |= val_enum_to_bit(val_enum);

		if ((val_enum == lv_VAL) && strstr(name, "_"))
			def->lvt_bits = lv_to_bits(name);

		if (strstr(name, "_new")) {
			if (val_enum == lv_VAL)
				def->flags |= ARG_DEF_FLAG_NEW_LV;
			else if (val_enum == vg_VAL)
				def->flags |= ARG_DEF_FLAG_NEW_VG;
		}
	}
}

/*
 * Save an option arg in a struct arg_def.
 * Parse str for anything that can follow --option.
 */

static void set_opt_def(struct command *cmd, char *str, struct arg_def *def)
{
	char *argv[MAX_LINE_ARGC];
	int argc;
	char *name;
	int val_enum;
	int i;

	split_line(str, &argc, argv, '|');

	for (i = 0; i < argc; i++) {
		name = argv[i];

		val_enum = val_str_to_num(name);

		if (!val_enum) {
			/* a literal number or string */

			if (isdigit(name[0]))
				val_enum = constnum_VAL;

			else if (isalpha(name[0]))
				val_enum = conststr_VAL;

			else {
				log_error("Parsing command defs: unknown opt arg: %s", name);
				exit(EXIT_FAILURE);
			}
		}


		def->val_bits |= val_enum_to_bit(val_enum);

		if (val_enum == constnum_VAL)
			def->num = (uint64_t)atoi(name);

		if (val_enum == conststr_VAL)
			def->str = strdup(name);

		if (val_enum == lv_VAL) {
			if (strstr(name, "_"))
				def->lvt_bits = lv_to_bits(name);
		}

		if (strstr(name, "_new")) {
			if (val_enum == lv_VAL)
				def->flags |= ARG_DEF_FLAG_NEW_LV;
			else if (val_enum == vg_VAL)
				def->flags |= ARG_DEF_FLAG_NEW_VG;
				
		}
	}
}

/*
 * Save a set of common options so they can be included in
 * multiple command defs.
 *
 * OO_FOO: --opt1 ...
 *
 * oo->name = "OO_FOO";
 * oo->line = "--opt1 ...";
 */

static void add_oo_definition_line(const char *name, const char *line)
{
	struct oo_line *oo;
	char *colon;
	char *start;

	oo = &oo_lines[oo_line_count++];
	oo->name = strdup(name);

	if ((colon = strstr(oo->name, ":")))
		*colon = '\0';
	else {
		log_error("Parsing command defs: invalid OO definition");
		exit(EXIT_FAILURE);
	}

	start = strstr(line, ":") + 2;
	oo->line = strdup(start);
}

/* Support OO_FOO: continuing on multiple lines. */

static void append_oo_definition_line(const char *new_line)
{
	struct oo_line *oo;
	char *old_line;
	char *line;
	int len;

	oo = &oo_lines[oo_line_count-1];

	old_line = oo->line;

	/* +2 = 1 space between old and new + 1 terminating \0 */
	len = strlen(old_line) + strlen(new_line) + 2;
	line = malloc(len);
	memset(line, 0, len);

	strcat(line, old_line);
	strcat(line, " ");
	strcat(line, new_line);

	free(oo->line);
	oo->line = line;
}

/* Find a saved OO_FOO definition. */

static char *get_oo_line(const char *str)
{
	char *name;
	char *end;
	char str2[64];
	int i;

	strcpy(str2, str);
	if ((end = strstr(str2, ":")))
		*end = '\0';
	if ((end = strstr(str2, ",")))
		*end = '\0';

	for (i = 0; i < oo_line_count; i++) {
		name = oo_lines[i].name;
		if (!strcmp(name, str2))
			return oo_lines[i].line;
	}
	return NULL;
}

/*
 * Add optional_opt_args entries when OO_FOO appears on OO: line,
 * i.e. include common options from an OO_FOO definition.
 */

static void include_optional_opt_args(struct command *cmd, const char *str)
{
	char *oo_line;
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;

	if (!(oo_line = get_oo_line(str))) {
		log_error("Parsing command defs: no OO line found for %s", str);
		exit(EXIT_FAILURE);
	}

	if (!(line = strdup(oo_line)))
		exit(EXIT_FAILURE);

	split_line(line, &line_argc, line_argv, ' ');
	add_optional_opt_line(cmd, line_argc, line_argv);
	free(line);
}

/*
 * When an --option is seen, add a new opt_args entry for it.
 * This function sets the opt_args.opt value for it.
 */

static void add_opt_arg(struct command *cmd, char *str, int *takes_arg, int required)
{
	char *comma;
	int opt;

	/* opt_arg.opt set here */
	/* opt_arg.def will be set in update_prev_opt_arg() if needed */

	if ((comma = strstr(str, ",")))
		*comma = '\0';

	/*
	 * Work around nasty hack where --uuid is used for both uuid_ARG
	 * and uuidstr_ARG.  The input uses --uuidstr, where an actual
	 * command uses --uuid string.
	 */
	if (!strcmp(str, "--uuidstr")) {
		opt = uuidstr_ARG;
		goto skip;
	}

	opt = opt_str_to_num(str);
skip:
	if (required > 0)
		cmd->required_opt_args[cmd->ro_count++].opt = opt;
	else if (!required)
		cmd->optional_opt_args[cmd->oo_count++].opt = opt;
	else if (required < 0)
		cmd->ignore_opt_args[cmd->io_count++].opt = opt;
	else
		exit(EXIT_FAILURE);

	*takes_arg = opt_names[opt].val_enum ? 1 : 0;
}

/*
 * After --option has been seen, this function sets opt_args.def value
 * for the value that appears after --option.
 */

static void update_prev_opt_arg(struct command *cmd, char *str, int required)
{
	struct arg_def def = { 0 };
	char *comma;

	if (str[0] == '-') {
		log_error("Parsing command defs: option %s must be followed by an arg.", str);
		exit(EXIT_FAILURE);
	}

	/* opt_arg.def set here */
	/* opt_arg.opt was previously set in add_opt_arg() when --foo was read */

	if ((comma = strstr(str, ",")))
		*comma = '\0';

	set_opt_def(cmd, str, &def);

	if (required > 0)
		cmd->required_opt_args[cmd->ro_count-1].def = def;
	else if (!required)
		cmd->optional_opt_args[cmd->oo_count-1].def = def;
	else if (required < 0)
		cmd->ignore_opt_args[cmd->io_count-1].def = def;
	else
		exit(EXIT_FAILURE);
}

/*
 * When an position arg is seen, add a new pos_args entry for it.
 * This function sets the pos_args.pos and pos_args.def.
 */

static void add_pos_arg(struct command *cmd, char *str, int required)
{
	struct arg_def def = { 0 };

	/* pos_arg.pos and pos_arg.def are set here */

	set_pos_def(cmd, str, &def);

	if (required) {
		cmd->required_pos_args[cmd->rp_count].pos = cmd->pos_count++;
		cmd->required_pos_args[cmd->rp_count].def = def;
		cmd->rp_count++;
	} else {
		cmd->optional_pos_args[cmd->op_count].pos = cmd->pos_count++;;
		cmd->optional_pos_args[cmd->op_count].def = def;
		cmd->op_count++;
	}
}

/* Process something that follows a pos arg, which is not a new pos arg. */

static void update_prev_pos_arg(struct command *cmd, char *str, int required)
{
	struct arg_def *def;

	/* a previous pos_arg.def is modified here */

	if (required)
		def = &cmd->required_pos_args[cmd->rp_count-1].def;
	else
		def = &cmd->optional_pos_args[cmd->op_count-1].def;

	if (!strcmp(str, "..."))
		def->flags |= ARG_DEF_FLAG_MAY_REPEAT;
	else {
		log_error("Parsing command defs: unknown pos arg: %s", str);
		exit(EXIT_FAILURE);
	}
}

/* Process what follows OO:, which are the optional opt args for the cmd def. */

static void add_optional_opt_line(struct command *cmd, int argc, char *argv[])
{
	int takes_arg;
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "OO:", 3))
			continue;
		if (is_opt_name(argv[i]))
			add_opt_arg(cmd, argv[i], &takes_arg, OPTIONAL);
		else if (!strncmp(argv[i], "OO_", 3))
			include_optional_opt_args(cmd, argv[i]);
		else if (takes_arg)
			update_prev_opt_arg(cmd, argv[i], OPTIONAL);
		else {
			log_error("Parsing command defs: can't parse argc %d argv %s prev %s",
				i, argv[i], argv[i-1]);
			exit(EXIT_FAILURE);
		}
	}
}

/* Process what follows IO:, which are the ignore options for the cmd def. */

static void add_ignore_opt_line(struct command *cmd, int argc, char *argv[])
{
	int takes_arg;
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "IO:", 3))
			continue;
		if (is_opt_name(argv[i]))
			add_opt_arg(cmd, argv[i], &takes_arg, IGNORE);
		else if (takes_arg)
			update_prev_opt_arg(cmd, argv[i], IGNORE);
		else {
			log_error("Parsing command defs: can't parse argc %d argv %s prev %s",
				i, argv[i], argv[i-1]);
			exit(EXIT_FAILURE);
		}
	}
}

/* Process what follows OP:, which are optional pos args for the cmd def. */

static void add_optional_pos_line(struct command *cmd, int argc, char *argv[])
{
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "OP:", 3))
			continue;
		if (is_pos_name(argv[i]))
			add_pos_arg(cmd, argv[i], OPTIONAL);
		else
			update_prev_pos_arg(cmd, argv[i], OPTIONAL);
	}
}

static void add_required_opt_line(struct command *cmd, int argc, char *argv[])
{
	int takes_arg;
	int i;

	for (i = 0; i < argc; i++) {
		if (is_opt_name(argv[i]))
			add_opt_arg(cmd, argv[i], &takes_arg, REQUIRED);
		else if (takes_arg)
			update_prev_opt_arg(cmd, argv[i], REQUIRED);
		else {
			log_error("Parsing command defs: can't parse argc %d argv %s prev %s",
				  i, argv[i], argv[i-1]);
			exit(EXIT_FAILURE);
		}
	}
}

/*
 * Add to required_opt_args from an OO_FOO definition.
 * (This is the special case of vgchange/lvchange where one
 * optional option is required, and others are then optional.)
 * The set of options from OO_FOO are saved in required_opt_args,
 * and flag CMD_FLAG_ONE_REQUIRED_OPT is set on the cmd indicating
 * this special case.
 */
 
static void include_required_opt_args(struct command *cmd, char *str)
{
	char *oo_line;
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;

	if (!(oo_line = get_oo_line(str))) {
		log_error("Parsing command defs: no OO line found for %s", str);
		exit(EXIT_FAILURE);
	}

	if (!(line = strdup(oo_line)))
		exit(EXIT_FAILURE);

	split_line(line, &line_argc, line_argv, ' ');
	add_required_opt_line(cmd, line_argc, line_argv);
	free(line);
}

/* Process what follows command_name, which are required opt/pos args. */

static void add_required_line(struct command *cmd, int argc, char *argv[])
{
	int i;
	int takes_arg;
	int prev_was_opt = 0, prev_was_pos = 0;

	/* argv[0] is command name */

	for (i = 1; i < argc; i++) {

		if (is_opt_name(argv[i])) {
			/* add new required_opt_arg */
			add_opt_arg(cmd, argv[i], &takes_arg, REQUIRED);
			prev_was_opt = 1;
			prev_was_pos = 0;

		} else if (prev_was_opt && takes_arg) {
			/* set value for previous required_opt_arg */
			update_prev_opt_arg(cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 0;

		} else if (is_pos_name(argv[i])) {
			/* add new required_pos_arg */
			add_pos_arg(cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 1;

		} else if (!strncmp(argv[i], "OO_", 3)) {
			/* one required_opt_arg is required, special case lv/vgchange */
			cmd->cmd_flags |= CMD_FLAG_ONE_REQUIRED_OPT;
			include_required_opt_args(cmd, argv[i]);

		} else if (prev_was_pos) {
			/* set property for previous required_pos_arg */
			update_prev_pos_arg(cmd, argv[i], REQUIRED);
		} else {
			log_error("Parsing command defs: can't parse argc %d argv %s prev %s",
				  i, argv[i], argv[i-1]);
			exit(EXIT_FAILURE);
		}

	}
}

static void add_flags(struct command *cmd, char *line)
{
	if (strstr(line, "SECONDARY_SYNTAX"))
		cmd->cmd_flags |= CMD_FLAG_SECONDARY_SYNTAX;
}

#define MAX_RULE_OPTS 64

static void add_rule(struct command *cmd, char *line)
{
	struct cmd_rule *rule;
	char *line_argv[MAX_LINE_ARGC];
	char *arg;
	int line_argc;
	int i, lvt_enum, lvp_enum;
	int check = 0;

	if (cmd->rule_count == CMD_MAX_RULES) {
		log_error("Parsing command defs: too many rules for cmd");
		exit(EXIT_FAILURE);
	}

	rule = &cmd->rules[cmd->rule_count++];

	split_line(line, &line_argc, line_argv, ' ');

	for (i = 0; i < line_argc; i++) {
		arg = line_argv[i];

		if (!strcmp(arg, "not")) {
			rule->rule = RULE_INVALID;
			check = 1;
		}

		else if (!strcmp(arg, "and")) {
			rule->rule = RULE_REQUIRE;
			check = 1;
		}

		else if (!strncmp(arg, "all", 3)) {
			/* opt/lvt_bits/lvp_bits all remain 0 to mean all */
			continue;
		}

		else if (!strncmp(arg, "--", 2)) {
			if (!rule->opts) {
				if (!(rule->opts = malloc(MAX_RULE_OPTS * sizeof(int)))) {
					log_error("Parsing command defs: no mem");
					exit(EXIT_FAILURE);
				}
				memset(rule->opts, 0, MAX_RULE_OPTS * sizeof(int));
			}

			if (!rule->check_opts) {
				if (!(rule->check_opts = malloc(MAX_RULE_OPTS * sizeof(int)))) {
					log_error("Parsing command defs: no mem");
					exit(EXIT_FAILURE);
				}
				memset(rule->check_opts, 0, MAX_RULE_OPTS * sizeof(int));
			}

			if (check)
				rule->check_opts[rule->check_opts_count++] = opt_str_to_num(arg);
			else
				rule->opts[rule->opts_count++] = opt_str_to_num(arg);
		}

		else if (!strncmp(arg, "LV_", 3)) {
			lvt_enum = lv_to_enum(arg);

			if (check)
				rule->check_lvt_bits |= lvt_enum_to_bit(lvt_enum);
			else
				rule->lvt_bits |= lvt_enum_to_bit(lvt_enum);
		}

		else if (!strncmp(arg, "lv_is_", 6)) {
			lvp_enum = lvp_name_to_enum(arg);

			if (check)
				rule->check_lvp_bits |= lvp_enum_to_bit(lvp_enum);
			else
				rule->lvp_bits |= lvp_enum_to_bit(lvp_enum);
		}
	}
}

/* The given option is common to all lvm commands (set in lvm_all). */

static int is_lvm_all_opt(int opt)
{
	int oo;

	for (oo = 0; oo < lvm_all.oo_count; oo++) {
		if (lvm_all.optional_opt_args[oo].opt == opt)
			return 1;
	}
	return 0;
}

/* Find common options for all variants of each command name. */

static void factor_common_options(void)
{
	int cn, opt_enum, ci, oo, ro, found;
	struct command *cmd;

	for (cn = 0; cn < MAX_COMMAND_NAMES; cn++) {
		if (!command_names[cn].name)
			break;

		for (ci = 0; ci < COMMAND_COUNT; ci++) {
			cmd = &commands[ci];

			if (strcmp(cmd->name, command_names[cn].name))
				continue;

			command_names[cn].variants++;
		}

		for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {

			for (ci = 0; ci < COMMAND_COUNT; ci++) {
				cmd = &commands[ci];

				if (strcmp(cmd->name, command_names[cn].name))
					continue;

				if (cmd->ro_count)
					command_names[cn].variant_has_ro = 1;
				if (cmd->rp_count)
					command_names[cn].variant_has_rp = 1;
				if (cmd->oo_count)
					command_names[cn].variant_has_oo = 1;
				if (cmd->op_count)
					command_names[cn].variant_has_op = 1;

				for (ro = 0; ro < cmd->ro_count; ro++) {
					command_names[cn].all_options[cmd->required_opt_args[ro].opt] = 1;

					if ((cmd->required_opt_args[ro].opt == size_ARG) && !strncmp(cmd->name, "lv", 2))
						command_names[cn].all_options[extents_ARG] = 1;
				}
				for (oo = 0; oo < cmd->oo_count; oo++)
					command_names[cn].all_options[cmd->optional_opt_args[oo].opt] = 1;

				found = 0;

				for (oo = 0; oo < cmd->oo_count; oo++) {
					if (cmd->optional_opt_args[oo].opt == opt_enum) {
						found = 1;
						break;
					}
				}

				if (!found)
					goto next_opt;
			}

			/* all commands starting with this name use this option */
			command_names[cn].common_options[opt_enum] = 1;
 next_opt:
			;
		}
	}
}

static int long_name_compare(const void *on1, const void *on2)
{
	struct opt_name **optname1 = (void *)on1;
	struct opt_name **optname2 = (void *)on2;
	return strcmp((*optname1)->long_opt + 2, (*optname2)->long_opt + 2);
}

/* Create list of option names for printing alphabetically. */

static void create_opt_names_alpha(void)
{
	int i;

	for (i = 0; i < ARG_COUNT; i++)
		opt_names_alpha[i] = &opt_names[i];

	qsort(opt_names_alpha, ARG_COUNT, sizeof(long), long_name_compare);
}

static int copy_line(char *line, int max_line, int *position)
{
	int p = *position;
	int i = 0;

	memset(line, 0, max_line);

	while (1) {
		line[i] = _command_input[p];
		i++;
		p++;

		if (_command_input[p] == '\n') {
			p++;
			break;
		}

		if (i == (max_line - 1))
			break;
	}
	*position = p;
	return 1;
}

int define_commands(void)
{
	struct command *cmd;
	char line[MAX_LINE];
	char line_orig[MAX_LINE];
	char *line_argv[MAX_LINE_ARGC];
	const char *name;
	char *n;
	int line_argc;
	int cmd_count = 0;
	int prev_was_oo_def = 0;
	int prev_was_oo = 0;
	int prev_was_op = 0;
	int copy_pos = 0;

	create_opt_names_alpha();

	/* Process each line of command-lines-input.h (from command-lines.in) */

	while (copy_line(line, MAX_LINE, &copy_pos)) {
		if (line[0] == '\n')
			break;

		if ((n = strchr(line, '\n')))
			*n = '\0';

		memcpy(line_orig, line, sizeof(line));
		split_line(line, &line_argc, line_argv, ' ');

		if (!line_argc)
			continue;

		/* New cmd def begins: command_name <required opt/pos args> */
		if ((name = is_command_name(line_argv[0]))) {
			if (cmd_count >= COMMAND_COUNT) {
				return 0;
			}

			/*
			 * FIXME: when running one specific command name,
			 * we can optimize by not parsing command defs
			 * that don't start with that command name.
			 */

			cmd = &commands[cmd_count];
			cmd->command_index = cmd_count;
			cmd_count++;
			cmd->name = strdup(name);
			cmd->pos_count = 1;
			add_required_line(cmd, line_argc, line_argv);

			/* Every cmd gets the OO_ALL options */
			include_optional_opt_args(cmd, "OO_ALL:");
			continue;
		}

		/*
		 * All other kinds of lines are processed in the
		 * context of the existing command[].
		 */

		if (is_desc_line(line_argv[0])) {
			char *desc = strdup(line_orig);
			if (cmd->desc) {
				int newlen = strlen(cmd->desc) + strlen(desc) + 2;
				char *newdesc = malloc(newlen);
				memset(newdesc, 0, newlen);
				snprintf(newdesc, newlen, "%s %s", cmd->desc, desc);
				cmd->desc = newdesc;
				free(desc);
			} else
				cmd->desc = desc;
			continue;
		}

		if (is_flags_line(line_argv[0])) {
			add_flags(cmd, line_orig);
			continue;
		}

		if (is_rule_line(line_argv[0])) {
			add_rule(cmd, line_orig);
			continue;
		}

		if (is_id_line(line_argv[0])) {
			cmd->command_id = strdup(line_argv[1]);
			continue;
		}

		/* OO_FOO: ... */
		if (is_oo_definition(line_argv[0])) {
			add_oo_definition_line(line_argv[0], line_orig);
			prev_was_oo_def = 1;
			prev_was_oo = 0;
			prev_was_op = 0;
			continue;
		}

		/* OO: ... */
		if (is_oo_line(line_argv[0])) {
			add_optional_opt_line(cmd, line_argc, line_argv);
			prev_was_oo_def = 0;
			prev_was_oo = 1;
			prev_was_op = 0;
			continue;
		}

		/* OP: ... */
		if (is_op_line(line_argv[0])) {
			add_optional_pos_line(cmd, line_argc, line_argv);
			prev_was_oo_def = 0;
			prev_was_oo = 0;
			prev_was_op = 1;
			continue;
		}

		/* IO: ... */
		if (is_io_line(line_argv[0])) {
			add_ignore_opt_line(cmd, line_argc, line_argv);
			prev_was_oo = 0;
			prev_was_op = 0;
			continue;
		}

		/* handle OO_FOO:, OO:, OP: continuing on multiple lines */

		if (prev_was_oo_def) {
			append_oo_definition_line(line_orig);
			continue;
		}

		if (prev_was_oo) {
			add_optional_opt_line(cmd, line_argc, line_argv);
			continue;
		}

		if (prev_was_op) {
			add_optional_pos_line(cmd, line_argc, line_argv);
			continue;
		}
	}

	/*
	 * For usage.
	 * Looks at all variants of each command name and figures out
	 * which options are common to all variants (for compact output)
	 */
        factor_common_options();

	/*
	 * For usage.
	 * Predefined string of options common to all commands
	 * (for compact output)
	 */
        include_optional_opt_args(&lvm_all, "OO_USAGE_COMMON");

	return 1;
}

/* type_LVT to "type" */

static const char *lvt_enum_to_name(int lvt_enum)
{
	return lvt_names[lvt_enum].name;
}

static void _print_usage_description(struct command *cmd)
{
	const char *desc = cmd->desc;
	char buf[MAX_LINE] = {0};
	int di = 0;
	int bi = 0;

	for (di = 0; di < strlen(desc); di++) {
		if (!strncmp(&desc[di], "DESC:", 5)) {
			if (bi) {
				buf[bi] = '\0';
				printf("  %s\n", buf);
				memset(buf, 0, sizeof(buf));
				bi = 0;
			}
			/* skip DESC: */
			di += 5;
			continue;
		}

		if (!bi && desc[di] == ' ')
			continue;

		buf[bi++] = desc[di];

		if (bi == (MAX_LINE - 1))
			break;
	}

	if (bi) {
		buf[bi] = '\0';
		printf("  %s\n", buf);
	}
}

static void print_usage_def(struct arg_def *def)
{
	int val_enum;
	int lvt_enum;
	int sep = 0;

	for (val_enum = 0; val_enum < VAL_COUNT; val_enum++) {
		if (def->val_bits & val_enum_to_bit(val_enum)) {

			if (val_enum == conststr_VAL)
				printf("%s", def->str);

			else if (val_enum == constnum_VAL)
				printf("%llu", (unsigned long long)def->num);

			else {
				if (sep) printf("|");

				if (!val_names[val_enum].usage)
					printf("%s", val_names[val_enum].name);
				else
					printf("%s", val_names[val_enum].usage);

				sep = 1;
			}

			if (val_enum == lv_VAL && def->lvt_bits) {
				for (lvt_enum = 1; lvt_enum < LVT_COUNT; lvt_enum++) {
					if (lvt_bit_is_set(def->lvt_bits, lvt_enum))
						printf("_%s", lvt_enum_to_name(lvt_enum));
				}
			}

			if ((val_enum == vg_VAL) && (def->flags & ARG_DEF_FLAG_NEW_VG))
				printf("_new");
			if ((val_enum == lv_VAL) && (def->flags & ARG_DEF_FLAG_NEW_LV))
				printf("_new");
		}
	}

	if (def->flags & ARG_DEF_FLAG_MAY_REPEAT)
		printf(" ...");
}

void print_usage(struct command *cmd)
{
	struct command_name *cname = find_command_name(cmd->name);
	int onereq = (cmd->cmd_flags & CMD_FLAG_ONE_REQUIRED_OPT) ? 1 : 0;
	int ro, rp, oo, op, opt_enum, first;

	if (cmd->desc)
		_print_usage_description(cmd);

	printf("  %s", cmd->name);

	if (cmd->ro_count) {
		first = 1;

		for (ro = 0; ro < cmd->ro_count; ro++) {
			if (onereq) {
				if (first)
					printf("\n\t(");
				else
					printf(",\n\t ");
				first = 0;
			}

			printf(" %s", opt_names[cmd->required_opt_args[ro].opt].long_opt);
			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_usage_def(&cmd->required_opt_args[ro].def);
			}
		}
		if (onereq)
			printf(" )\n");
	}

	if (cmd->rp_count) {
		if (onereq)
			printf("\t");
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
				printf(" ");
				print_usage_def(&cmd->required_pos_args[rp].def);
			}
		}
	}

	if (!cmd->oo_count)
		goto op_count;

	if (cmd->oo_count) {
		first = 1;

		for (oo = 0; oo < cmd->oo_count; oo++) {
			opt_enum = cmd->optional_opt_args[oo].opt;

			/*
			 * Skip common opts in lvm_all and cname->common_options.
			 */

			if (is_lvm_all_opt(opt_enum))
				continue;

			if ((cname->variants > 1) && cname->common_options[opt_enum])
				continue;

			if (first)
				printf("\n\t[");
			else
				printf(",\n\t ");
			first = 0;

			printf(" %s", opt_names[opt_enum].long_opt);
			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_usage_def(&cmd->optional_opt_args[oo].def);
			}
		}

		if (first)
			printf("\n\t[");
		else
			printf(",\n\t ");
		printf(" COMMON_OPTIONS ]");
	}

 op_count:
	if (!cmd->op_count)
		goto done;

	printf("\n\t[");

	if (cmd->op_count) {
		for (op = 0; op < cmd->op_count; op++) {
			if (cmd->optional_pos_args[op].def.val_bits) {
				printf(" ");
				print_usage_def(&cmd->optional_pos_args[op].def);
			}
		}
	}

	printf(" ]");
 done:
	printf("\n");
	return;
}


void print_usage_common(struct command_name *cname, struct command *cmd)
{
	int oo, opt_enum, first;

	printf("  Common options:");

	/*
	 * when there's more than one variant, options that
	 * are common to all commands with a common name.
	 */

	if (cname->variants < 2)
		goto all;

	first = 1;

	for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++) {
		if (!cname->common_options[opt_enum])
			continue;

		if (is_lvm_all_opt(opt_enum))
			continue;

		if (first)
			printf("\n\t[");
		else
			printf(",\n\t ");
		first = 0;

		for (oo = 0; oo < cmd->oo_count; oo++) {
			if (cmd->optional_opt_args[oo].opt != opt_enum)
				continue;

			printf(" %s", opt_names[opt_enum].long_opt);
			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_usage_def(&cmd->optional_opt_args[oo].def);
			}
			break;
		}
	}

 all:
	/* options that are common to all lvm commands */

	for (oo = 0; oo < lvm_all.oo_count; oo++) {
		opt_enum = lvm_all.optional_opt_args[oo].opt;

		if (first)
			printf("\n\t[");
		else
			printf(",\n\t ");
		first = 0;

		printf(" %s", opt_names[opt_enum].long_opt);
		if (lvm_all.optional_opt_args[oo].def.val_bits) {
			printf(" ");
			print_usage_def(&lvm_all.optional_opt_args[oo].def);
		}
	}

	printf(" ]\n");
}

#ifdef MAN_PAGE_GENERATOR

static void print_val_man(const char *str)
{
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;
	int i;

	if (!strcmp(str, "Number") ||
	    !strcmp(str, "String") ||
	    !strncmp(str, "VG", 2) ||
	    !strncmp(str, "LV", 2) ||
	    !strncmp(str, "PV", 2) ||
	    !strcmp(str, "Tag")) {
		printf("\\fI%s\\fP", str);
		return;
	}

	if (strstr(str, "Number[") || strstr(str, "]Number")) {
		for (i = 0; i < strlen(str); i++) {
			if (str[i] == 'N')
				printf("\\fI");
			if (str[i] == 'r') {
				printf("%c", str[i]);
				printf("\\fP");
				continue;
			}
			printf("%c", str[i]);
		}
		return;
	}

	if (strstr(str, "|")) {
		int len = strlen(str);
		line = strdup(str);
		split_line(line, &line_argc, line_argv, '|');
		for (i = 0; i < line_argc; i++) {
			if (i) {
				printf("|");

				/* this is a hack to add a line break for
				   a long string of opt values */
				if ((len > 40) && (i >= (line_argc / 2) + 1)) {
					printf("\n");
					printf("       ");
					len = 0;
				}
			}
			if (strstr(line_argv[i], "Number"))
				printf("\\fI%s\\fP", line_argv[i]);
			else
				printf("\\fB%s\\fP", line_argv[i]);
		}
		return;
	}

	printf("\\fB%s\\fP", str);
}

static void print_def_man(struct arg_def *def, int usage)
{
	int val_enum;
	int lvt_enum;
	int sep = 0;
	int i;

	for (val_enum = 0; val_enum < VAL_COUNT; val_enum++) {
		if (def->val_bits & val_enum_to_bit(val_enum)) {

			if (val_enum == conststr_VAL) {
				printf("\\fB");
				printf("%s", def->str);
				printf("\\fP");
			}

			else if (val_enum == constnum_VAL) {
				printf("\\fB");
				printf("%llu", (unsigned long long)def->num);
				printf("\\fP");
			}

			else {
				if (sep) printf("|");

				if (!usage || !val_names[val_enum].usage) {
					printf("\\fI");
					printf("%s", val_names[val_enum].name);
					printf("\\fP");
				} else {
					print_val_man(val_names[val_enum].usage);
				}

				sep = 1;
			}

			if (val_enum == lv_VAL && def->lvt_bits) {
				printf("\\fI");
				for (lvt_enum = 1; lvt_enum < LVT_COUNT; lvt_enum++) {
					if (lvt_bit_is_set(def->lvt_bits, lvt_enum))
						printf("_%s", lvt_enum_to_name(lvt_enum));
				}
				printf("\\fP");
			}

			if ((val_enum == vg_VAL) && (def->flags & ARG_DEF_FLAG_NEW_VG)) {
				printf("\\fI");
				printf("_new");
				printf("\\fP");
			}
			if ((val_enum == lv_VAL) && (def->flags & ARG_DEF_FLAG_NEW_LV)) {
				printf("\\fI");
				printf("_new");
				printf("\\fP");
			}
		}
	}

	if (def->flags & ARG_DEF_FLAG_MAY_REPEAT)
		printf(" ...");
}

static char *man_long_opt_name(const char *cmdname, int opt_enum)
{
	static char long_opt_name[64];

	memset(&long_opt_name, 0, sizeof(long_opt_name));

	switch (opt_enum) {
	case syncaction_ARG:
		strncpy(long_opt_name, "--[raid]syncaction", 63);
		break;
	case writemostly_ARG:
		strncpy(long_opt_name, "--[raid]writemostly", 63);
		break;
	case minrecoveryrate_ARG:
		strncpy(long_opt_name, "--[raid]minrecoveryrate", 63);
		break;
	case maxrecoveryrate_ARG:
		strncpy(long_opt_name, "--[raid]maxrecoveryrate", 63);
		break;
	case writebehind_ARG:
		strncpy(long_opt_name, "--[raid]writebehind", 63);
		break;
	case vgmetadatacopies_ARG:
		if (!strncmp(cmdname, "vg", 2))
			strncpy(long_opt_name, "--[vg]metadatacopies", 63);
		else
			strncpy(long_opt_name, "--vgmetadatacopies", 63);
		break;
	case pvmetadatacopies_ARG:
		if (!strncmp(cmdname, "pv", 2))
			strncpy(long_opt_name, "--[pv]metadatacopies", 63);
		else
			strncpy(long_opt_name, "--pvmetadatacopies", 63);
		break;
	default:
		strncpy(long_opt_name, opt_names[opt_enum].long_opt, 63);
		break;
	}

	return long_opt_name;
}

void print_man_usage(char *lvmname, struct command *cmd)
{
	struct command_name *cname;
	int onereq = (cmd->cmd_flags & CMD_FLAG_ONE_REQUIRED_OPT) ? 1 : 0;
	int i, sep, ro, rp, oo, op, opt_enum;
	int need_ro_indent_end = 0;

	if (!(cname = find_command_name(cmd->name)))
		return;

	printf("\\fB%s\\fP", lvmname);

	if (!onereq)
		goto ro_normal;

	/*
	 * one required option in a set, print as:
	 * ( -a|--a,
	 *   -b|--b,
	 *      --c,
	 *      --d )
	 *
	 * First loop through ro prints those with short opts,
	 * and the second loop prints those without short opts.
	 */

	if (cmd->ro_count) {
		printf("\n");
		printf(".RS 4\n");
		printf("(");

		sep = 0;

		/* print required options with a short opt */
		for (ro = 0; ro < cmd->ro_count; ro++) {
			opt_enum = cmd->required_opt_args[ro].opt;

			if (!opt_names[opt_enum].short_opt)
				continue;

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			if (opt_names[opt_enum].short_opt) {
				printf(" \\fB-%c\\fP|\\fB%s\\fP",
				       opt_names[opt_enum].short_opt,
				       man_long_opt_name(cmd->name, opt_enum));
			} else {
				printf("   ");
				printf(" \\fB%s\\fP", man_long_opt_name(cmd->name, opt_enum));
			}

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_opt_args[ro].def, 1);
			}

			sep++;
		}

		/* print required options without a short opt */
		for (ro = 0; ro < cmd->ro_count; ro++) {
			opt_enum = cmd->required_opt_args[ro].opt;

			if (opt_names[opt_enum].short_opt)
				continue;

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			printf("   ");
			printf(" \\fB%s\\fP", man_long_opt_name(cmd->name, opt_enum));

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_opt_args[ro].def, 1);
			}

			sep++;
		}

		printf(" )\n");
		printf(".RE\n");
	}

	/* print required position args on a new line after the onereq set */
	if (cmd->rp_count) {
		printf(".RS 4\n");
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_pos_args[rp].def, 1);
			}
		}

		printf("\n");
		printf(".RE\n");
	} else {
		/* printf("\n"); */
	}

	printf(".br\n");
	goto oo_count;

 ro_normal:

	/*
	 * all are required options, print as:
	 * -a|--aaa <val> -b|--bbb <val>
	 */

	if (cmd->ro_count) {
		sep = 0;

		for (ro = 0; ro < cmd->ro_count; ro++) {

			/* avoid long line wrapping */
			if ((cmd->ro_count > 2) && (sep == 2)) {
				printf("\n.RS 5\n");
				need_ro_indent_end = 1;
			}

			opt_enum = cmd->required_opt_args[ro].opt;

			if (opt_names[opt_enum].short_opt) {
				printf(" \\fB-%c\\fP|\\fB%s\\fP",
				       opt_names[opt_enum].short_opt,
				       man_long_opt_name(cmd->name, opt_enum));
			} else {
				printf(" \\fB%s\\fP", opt_names[cmd->required_opt_args[ro].opt].long_opt);
			}

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_opt_args[ro].def, 1);
			}

			sep++;
		}
	}

	/* print required position args on the same line as the required options */
	if (cmd->rp_count) {
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_pos_args[rp].def, 1);
			}
		}

		printf("\n");
	} else {
		printf("\n");
	}

	if (need_ro_indent_end)
		printf(".RE\n");

	printf(".br\n");

 oo_count:
	if (!cmd->oo_count)
		goto op_count;

	sep = 0;

	if (cmd->oo_count) {
		printf(".RS 4\n");
		printf("[");

		/* print optional options with short opts */

		for (oo = 0; oo < cmd->oo_count; oo++) {
			opt_enum = cmd->optional_opt_args[oo].opt;

			if (!opt_names[opt_enum].short_opt)
				continue;

			if (is_lvm_all_opt(opt_enum))
				continue;

			if ((cname->variants > 1) && cname->common_options[opt_enum])
				continue;

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			printf(" \\fB-%c\\fP|\\fB%s\\fP",
				opt_names[opt_enum].short_opt,
				man_long_opt_name(cmd->name, opt_enum));

			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
		}

		/* print optional options without short opts */

		for (oo = 0; oo < cmd->oo_count; oo++) {
			opt_enum = cmd->optional_opt_args[oo].opt;

			if (opt_names[opt_enum].short_opt)
				continue;

			if (is_lvm_all_opt(opt_enum))
				continue;

			if ((cname->variants > 1) && cname->common_options[opt_enum])
				continue;

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			/* space alignment without short opt */
			printf("   ");

			printf(" \\fB%s\\fP", man_long_opt_name(cmd->name, opt_enum));

			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
		}

		if (sep) {
			printf(",");
			printf("\n.br\n");
			printf(" ");
			/* space alignment without short opt */
			printf("   ");
		}
		printf(" COMMON_OPTIONS");
		printf(" ]\n");
		printf(".RE\n");
		printf(".br\n");
	}

 op_count:
	if (!cmd->op_count)
		goto done;

	printf(".RS 4\n");
	printf("[");

	if (cmd->op_count) {
		for (op = 0; op < cmd->op_count; op++) {
			if (cmd->optional_pos_args[op].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_pos_args[op].def, 1);
			}
		}
	}

	printf(" ]\n");
	printf(".RE\n");

 done:
	printf("\n");
}

/*
 * common options listed in the usage section.
 *
 * For commands with only one variant, this is only
 * the options which are common to all lvm commands
 * (in lvm_all, see is_lvm_all_opt).
 *
 * For commands with more than one variant, this
 * is the set of options common to all variants
 * (in cname->common_options), (which obviously
 * includes the options common to all lvm commands.)
 *
 * List ordering:
 * options with short+long names, alphabetically,
 * then options with only long names, alphabetically
 */

void print_man_usage_common(struct command *cmd)
{
	struct command_name *cname;
	int i, sep, ro, rp, oo, op, opt_enum;

	if (!(cname = find_command_name(cmd->name)))
		return;

	sep = 0;

	printf(".RS 4\n");
	printf("[");

	/* print those with short opts */
	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;

		if (!cname->common_options[opt_enum])
			continue;

		if (!opt_names[opt_enum].short_opt)
			continue;

		if ((cname->variants < 2) && !is_lvm_all_opt(opt_enum))
			continue;

		if (sep) {
			printf(",");
			printf("\n.br\n");
			printf(" ");
		}

		for (oo = 0; oo < cmd->oo_count; oo++) {
			if (cmd->optional_opt_args[oo].opt != opt_enum)
				continue;

			printf(" \\fB-%c\\fP|\\fB%s\\fP",
				opt_names[opt_enum].short_opt,
				man_long_opt_name(cmd->name, opt_enum));

			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
			break;
		}

	}

	/* print those without short opts */
	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;

		if (!cname->common_options[opt_enum])
			continue;

		if (opt_names[opt_enum].short_opt)
			continue;

		if ((cname->variants < 2) && !is_lvm_all_opt(opt_enum))
			continue;

		if (sep) {
			printf(",");
			printf("\n.br\n");
			printf(" ");
		}

		for (oo = 0; oo < cmd->oo_count; oo++) {
			if (cmd->optional_opt_args[oo].opt != opt_enum)
				continue;

			/* space alignment without short opt */
			printf("   ");

			printf(" \\fB%s\\fP", man_long_opt_name(cmd->name, opt_enum));

			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
			break;
		}
	}

	printf(" ]\n");
	return;
}

/*
 * Format of description, when different command names have
 * different descriptions:
 *
 * "#cmdname1"
 * "text foo goes here"
 * "a second line of text."
 * "#cmdname2"
 * "text bar goes here"
 * "another line of text."
 *
 * When called for cmdname2, this function should just print:
 *
 * "text bar goes here"
 * "another line of text."
 */

static void print_man_option_desc(struct command_name *cname, int opt_enum)
{
	const char *desc = opt_names[opt_enum].desc;
	char buf[DESC_LINE];
	int started_cname = 0;
	int line_count = 0;
	int di, bi = 0;

	if (desc[0] != '#') {
		printf("%s", desc);
		return;
	}

	for (di = 0; di < strlen(desc); di++) {
		buf[bi++] = desc[di];

		if (bi == DESC_LINE) {
			log_error("Parsing command defs: print_man_option_desc line too long");
			exit(EXIT_FAILURE);
		}

		if (buf[bi-1] != '\n')
			continue;

		if (buf[0] != '#') {
			if (started_cname) {
				printf("%s", buf);
				line_count++;
			}

			memset(buf, 0, sizeof(buf));
			bi = 0;
			continue;
		}

		/* Line starting with #cmdname */

		/*
		 * Must be starting a new command name.
		 * If no lines have been printed, multiple command names
		 * are using the same text. If lines have been printed,
		 * then the start of a new command name means the end
		 * of text for the current command name.
		 */
		if (line_count && started_cname)
			return;

		if (!strncmp(buf + 1, cname->name, strlen(cname->name))) {
			/* The start of our command name. */
			started_cname = 1;
			memset(buf, 0, sizeof(buf));
			bi = 0;
		} else {
			/* The start of another command name. */
			memset(buf, 0, sizeof(buf));
			bi = 0;
		}
	}

	if (bi && started_cname)
		printf("%s", buf);
}

/*
 * Print a list of all options names for a given
 * command name, listed by:
 * options with short+long names, alphabetically,
 * then options with only long names, alphabetically
 */

void print_man_all_options_list(struct command_name *cname)
{
	int opt_enum, val_enum;
	int sep = 0;
	int i;

	/* print those with both short and long opts */
	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;


		if (!cname->all_options[opt_enum])
			continue;

		if (!opt_names[opt_enum].short_opt)
			continue;

		if (sep)
			printf("\n.br\n");

		printf(" \\fB-%c\\fP|\\fB%s\\fP",
			opt_names[opt_enum].short_opt,
			man_long_opt_name(cname->name, opt_enum));

		val_enum = opt_names[opt_enum].val_enum;

		if (!val_names[val_enum].fn) {
			/* takes no arg */
		} else if (!val_names[val_enum].usage) {
			printf(" ");
			printf("\\fI");
			printf("%s", val_names[val_enum].name);
			printf("\\fP");
		} else {
			printf(" ");
			print_val_man(val_names[val_enum].usage);
		}

		sep = 1;
	}

	/* print those without short opts */
	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;

		if (!cname->all_options[opt_enum])
			continue;

		if (opt_names[opt_enum].short_opt)
			continue;

		if (sep)
			printf("\n.br\n");

		/* space alignment without short opt */
		printf("   ");

		printf(" \\fB%s\\fP", man_long_opt_name(cname->name, opt_enum));

		val_enum = opt_names[opt_enum].val_enum;

		if (!val_names[val_enum].fn) {
			/* takes no arg */
		} else if (!val_names[val_enum].usage) {
			printf(" ");
			printf("\\fI");
			printf("%s", val_names[val_enum].name);
			printf("\\fP");
		} else {
			printf(" ");
			print_val_man(val_names[val_enum].usage);
		}

		sep = 1;
	}
}

/*
 * All options used for a given command name, along with descriptions.
 * listed in order of:
 * 1. options that are not common to all lvm commands, alphabetically
 * 2. options common to all lvm commands, alphabetically
 */

void print_man_all_options_desc(struct command_name *cname)
{
	int opt_enum, val_enum;
	int print_common = 0;
	int sep = 0;
	int i;

 again:
	/*
	 * Loop 1: print options that are not common to all lvm commands.
	 * Loop 2: print options common to all lvm commands (lvm_all)
	 */

	for (i = 0; i < ARG_COUNT; i++) {
		opt_enum = opt_names_alpha[i]->opt_enum;

		if (!cname->all_options[opt_enum])
			continue;

		if (!print_common && is_lvm_all_opt(opt_enum))
			continue;

		if (print_common && !is_lvm_all_opt(opt_enum))
			continue;

		printf("\n.TP\n");

		if (opt_names[opt_enum].short_opt) {
			printf("\\fB-%c\\fP|\\fB%s\\fP",
			       opt_names[opt_enum].short_opt,
			       man_long_opt_name(cname->name, opt_enum));
		} else {
			printf("\\fB%s\\fP", man_long_opt_name(cname->name, opt_enum));
		}

		val_enum = opt_names[opt_enum].val_enum;

		if (!val_names[val_enum].fn) {
			/* takes no arg */
		} else if (!val_names[val_enum].usage) {
			printf(" ");
			printf("\\fI");
			printf("%s", val_names[val_enum].name);
			printf("\\fP");
		} else {
			printf(" ");
			print_val_man(val_names[val_enum].usage);
		}

		if (opt_names[opt_enum].desc) {
			printf("\n");
			printf(".br\n");
			print_man_option_desc(cname, opt_enum);
		}

		sep = 1;
	}

	if (!print_common) {
		print_common = 1;
		goto again;
	}
}

void print_desc_man(const char *desc)
{
	char buf[DESC_LINE] = {0};
	int di = 0;
	int bi = 0;

	for (di = 0; di < strlen(desc); di++) {
		if (desc[di] == '\0')
			break;
		if (desc[di] == '\n')
			continue;

		if (!strncmp(&desc[di], "DESC:", 5)) {
			if (bi) {
				printf("%s\n", buf);
				printf(".br\n");
				memset(buf, 0, sizeof(buf));
				bi = 0;
			}
			di += 5;
			continue;
		}

		if (!bi && desc[di] == ' ')
			continue;

		buf[bi++] = desc[di];

		if (bi == (DESC_LINE - 1))
			break;
	}

	if (bi) {
		printf("%s\n", buf);
		printf(".br\n");
	}
}

static char *upper_command_name(char *str)
{
	static char str_upper[32];
	int i = 0;

	while (*str) {
		str_upper[i++] = toupper(*str);
		str++;
	}
	str_upper[i] = '\0';
	return str_upper;
}

static void include_description_file(char *name, char *des_file)
{
	char buf[1024 * 1024];
	int fd;

	memset(buf, 0, sizeof(buf));
	
	fd = open(des_file, O_RDONLY);

	if (fd < 0)
		return;

	read(fd, buf, sizeof(buf) - 1);

	printf(".SH DESCRIPTION\n");
	printf("%s\n", buf);

	close(fd);
}

void print_man(char *name, char *des_file, int include_primary, int include_secondary)
{
	struct command_name *cname;
	struct command *cmd, *prev_cmd = NULL;
	char *lvmname = name;
	const char *desc;
	int i, j, ro, rp, oo, op;

	if (!strncmp(name, "lvm-", 4))
		name += 4;

	cname = find_command_name(name);

	printf(".TH %s 8 \"LVM TOOLS #VERSION#\" \"Sistina Software UK\"\n",
		upper_command_name(lvmname));

	for (i = 0; i < COMMAND_COUNT; i++) {

		cmd = &commands[i];

		if (prev_cmd && strcmp(prev_cmd->name, cmd->name)) {
			printf("Common options:\n");
			printf(".\n");
			print_man_usage_common(prev_cmd);

			printf("\n");
			printf(".SH OPTIONS\n");
			printf(".br\n");
			print_man_all_options_desc(cname);

			prev_cmd = NULL;
		}

		if ((cmd->cmd_flags & CMD_FLAG_SECONDARY_SYNTAX) && !include_secondary)
			continue;

		if (!(cmd->cmd_flags & CMD_FLAG_SECONDARY_SYNTAX) && !include_primary)
			continue;

		if (name && strcmp(name, cmd->name))
			continue;

		if (!prev_cmd || strcmp(prev_cmd->name, cmd->name)) {
			printf(".SH NAME\n");
			printf(".\n");
			if (cname->desc)
				printf("%s \\- %s\n", lvmname, cname->desc);
			else
				printf("%s\n", lvmname);
			printf(".P\n");

			printf(".\n");
			printf(".SH SYNOPSIS\n");
			printf(".br\n");
			printf(".P\n");
			printf(".\n");
			prev_cmd = cmd;

			if (!(cname = find_command_name(cmd->name)))
				return;

			if (cname->variant_has_ro && cname->variant_has_rp)
				printf("\\fB%s\\fP \\fIrequired_option_args\\fP \\fIrequired_position_args\\fP\n", lvmname);
			else if (cname->variant_has_ro && !cname->variant_has_rp)
				printf("\\fB%s\\fP \\fIrequired_option_args\\fP\n", lvmname);
			else if (!cname->variant_has_ro && cname->variant_has_rp)
				printf("\\fB%s\\fP \\fIrequired_position_args\\fP\n", lvmname);
			else if (!cname->variant_has_ro && !cname->variant_has_rp)
				printf("\\fB%s\\fP\n", lvmname);

			printf(".br\n");

			if (cname->variant_has_oo) {
				printf("    [ \\fIoptional_option_args\\fP ]\n");
				printf(".br\n");
			}

			if (cname->variant_has_op) {
				printf("    [ \\fIoptional_position_args\\fP ]\n");
				printf(".br\n");
			}

			printf(".P\n");
			printf("\n");

			/* listing them all when there's only 1 or 2 is just repetative */
			if (cname->variants > 2) {
				printf(".P\n");
				print_man_all_options_list(cname);
				printf("\n");
				printf(".P\n");
				printf("\n");
			}

			if (des_file) {
				include_description_file(lvmname, des_file);
				printf(".P\n");
			}

			printf(".SH USAGE\n");
			printf(".br\n");
			printf(".P\n");
			printf(".\n");
		}

		if (cmd->desc) {
			print_desc_man(cmd->desc);
			printf(".P\n");
		}

		print_man_usage(lvmname, cmd);

		if (i == (COMMAND_COUNT - 1)) {
			printf("Common options:\n");
			printf(".\n");
			print_man_usage_common(cmd);

			printf("\n");
			printf(".SH OPTIONS\n");
			printf(".br\n");
			print_man_all_options_desc(cname);

		}

		printf("\n");
		continue;
	}
}

int main(int argc, char *argv[])
{
	memset(&commands, 0, sizeof(commands));

	if (argc < 2) {
		log_error("Usage: %s <command> [/path/to/description-file]", argv[0]);
		exit(EXIT_FAILURE);
	}

	define_commands();

	print_man(argv[1], (argc > 2) ? argv[2] : NULL, 1, 1);

	return 0;
}

#endif
