/* 
  Copyright (C) 2006 Mandriva Conectiva S.A.
  Copyright (C) 2006 Arnaldo Carvalho de Melo <acme@mandriva.com>

  This program is free software; you can redistribute it and/or modify it
  under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.
*/

#include <argp.h>
#include <assert.h>
#include <dwarf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dwarves.h"
#include "dutil.h"

static int show_struct_diffs;
static int show_function_diffs;
static int verbose;
static int show_terse_type_changes;

static struct strlist *structs_printed;

#define TCHANGEF__SIZE		(1 << 0)
#define TCHANGEF__NR_MEMBERS	(1 << 1)
#define TCHANGEF__TYPE		(1 << 2)
#define TCHANGEF__OFFSET	(1 << 3)
#define TCHANGEF__BIT_OFFSET	(1 << 4)
#define TCHANGEF__BIT_SIZE	(1 << 5)
#define TCHANGEF__PADDING	(1 << 6)
#define TCHANGEF__NR_HOLES	(1 << 7)
#define TCHANGEF__NR_BIT_HOLES	(1 << 8)

static uint32_t terse_type_changes;

static uint32_t total_cus_changed;
static uint32_t total_nr_functions_changed;
static uint32_t total_function_bytes_added;
static uint32_t total_function_bytes_removed;

struct diff_info {
	const struct tag *tag;
	const struct cu	 *cu;
	int32_t		 diff;
};

static struct diff_info *diff_info__new(const struct tag *twin,
					const struct cu *cu,
					int32_t diff)
{
	struct diff_info *self = malloc(sizeof(*self));

	if (self == NULL) {
		puts("out of memory!");
		exit(1);
	}
	self->tag  = twin;
	self->cu   = cu;
	self->diff = diff;
	return self;
}

static void diff_function(const struct cu *new_cu, struct function *function,
			  struct cu *cu)
{
	struct tag *new_tag;
	const char *name;

	if (function->inlined || function->abstract_origin != 0)
		return;

	name = function__name(function, cu);
	new_tag = cu__find_function_by_name(new_cu, name);
	if (new_tag != NULL) {
		struct function *new_function = tag__function(new_tag);
		int32_t diff = (function__size(new_function) -
				function__size(function));
		if (diff != 0) {
			const size_t len = strlen(name);

			function->priv = diff_info__new(&new_function->proto.tag, new_cu,
							diff);
			if (len > cu->max_len_changed_item)
				cu->max_len_changed_item = len;

			++cu->nr_functions_changed;
			if (diff > 0)
				cu->function_bytes_added += diff;
			else
				cu->function_bytes_removed += -diff;
		} else {
			char proto[1024], twin_proto[1024];

			if (strcmp(function__prototype(function, cu,
						       proto, sizeof(proto)),
				   function__prototype(new_function, new_cu,
						       twin_proto,
						       sizeof(twin_proto))) != 0) {
				++cu->nr_functions_changed;
				function->priv = diff_info__new(function__tag(new_function),
								new_cu, 0);
			}
		}
	} else {
		const size_t len = strlen(name);
		const uint32_t diff = -function__size(function);

		if (len > cu->max_len_changed_item)
			cu->max_len_changed_item = len;
		function->priv = diff_info__new(NULL, NULL, diff);
		++cu->nr_functions_changed;
		cu->function_bytes_removed += -diff;
	}
}

static int check_print_change(const struct class_member *old,
			      const struct cu *old_cu,
			      const struct class_member *new,
			      const struct cu *new_cu,
			      int print)
{
	size_t old_size, new_size;
	char old_type_name[128], new_type_name[128];
	const struct tag *old_type = cu__find_type_by_id(old_cu, old->tag.type);
	const struct tag *new_type = cu__find_type_by_id(new_cu, new->tag.type);
	int changes = 0;

	if (old_type == NULL || new_type == NULL)
		return 0;

	old_size = tag__size(old_type, old_cu);
	new_size = tag__size(new_type, new_cu);
	if (old_size != new_size)
		changes = 1;

	if (old->offset != new->offset) {
		changes = 1;
		terse_type_changes |= TCHANGEF__OFFSET;
	}

	if (old->bit_offset != new->bit_offset) {
		changes = 1;
		terse_type_changes |= TCHANGEF__BIT_OFFSET;
	}

	if (old->bit_size != new->bit_size) {
		changes = 1;
		terse_type_changes |= TCHANGEF__BIT_SIZE;
	}

	if (strcmp(tag__name(old_type, old_cu, old_type_name,
			     sizeof(old_type_name)),
		   tag__name(new_type, new_cu, new_type_name,
			     sizeof(new_type_name))) != 0) {
		changes = 1;
		terse_type_changes |= TCHANGEF__TYPE;
	}

	if (changes && print && !show_terse_type_changes)
		printf("    %s\n"
		       "     from:    %-21s /* %5u(%2u) %5zd(%2d) */\n"
		       "     to:      %-21s /* %5u(%2u) %5zd(%2u) */\n",
		       class_member__name(old),
		       old_type_name, old->offset, old->bit_offset,
		       old_size, old->bit_size,
		       new_type_name, new->offset, new->bit_offset,
		       new_size, new->bit_size);

	return changes;
}

static int check_print_members_changes(const struct class *structure,
				       const struct cu *cu,
				       const struct class *new_structure,
				       const struct cu *new_cu,
				       int print)
{
	int changes = 0;
	struct class_member *member;
	uint16_t nr_twins_found = 0;

	type__for_each_member(&structure->type, member) {
		const char *member_name = class_member__name(member);
		struct class_member *twin =
			class__find_member_by_name(new_structure, member_name);
		if (twin != NULL) {
			twin->tag.visited = 1;
			++nr_twins_found;
			if (check_print_change(member, cu, twin, new_cu, print))
				changes = 1;
		} else {
			changes = 1;
			if (print) {
				char name[128];
				struct tag *type;
				type = cu__find_type_by_id(cu, member->tag.type);
				printf("    %s\n"
				       "     removed: %-21s /* %5u(%2u) %5zd(%2d) */\n",
				       class_member__name(member),
				       tag__name(type, cu, name, sizeof(name)),
				       member->offset, member->bit_offset,
				       tag__size(type, cu), member->bit_size);
			}
		}
	}

	if (nr_twins_found == new_structure->type.nr_members)
		goto out;

	changes = 1;
	if (!print)
		goto out;

	type__for_each_member(&new_structure->type, member) {
		if (!member->tag.visited) {
			char name[128];
			struct tag *type;
			type = cu__find_type_by_id(new_cu, member->tag.type);
			printf("    %s\n"
			       "     added:   %-21s /* %5u(%2u) %5zd(%2d) */\n",
			       class_member__name(member),
			       tag__name(type, new_cu, name, sizeof(name)),
			       member->offset, member->bit_offset,
			       tag__size(type, new_cu), member->bit_size);
		}
	}
out:
	return changes;
}

static void diff_struct(const struct cu *new_cu, struct class *structure,
			struct cu *cu)
{
	struct tag *new_tag;
	struct class *new_structure = NULL;
	size_t len;
	int32_t diff;

	assert(class__is_struct(structure));

	if (class__size(structure) == 0 || class__name(structure) == NULL)
		return;

	new_tag = cu__find_struct_by_name(new_cu,
					  class__name(structure), 0, NULL);
	if (new_tag == NULL)
		return;

	new_structure = tag__class(new_tag);
	if (class__size(new_structure) == 0)
		return;

	assert(class__is_struct(new_structure));

	class__find_holes(structure, cu);
	class__find_holes(new_structure, new_cu);
	diff = class__size(structure) != class__size(new_structure) ||
	       class__nr_members(structure) != class__nr_members(new_structure) ||
	       check_print_members_changes(structure, cu,
			       		   new_structure, new_cu, 0) ||
	       structure->padding != new_structure->padding ||
	       structure->nr_holes != new_structure->nr_holes ||
	       structure->nr_bit_holes != new_structure->nr_bit_holes;

	if (diff == 0)
		return;

	++cu->nr_structures_changed;
	len = strlen(class__name(structure)) + sizeof("struct");
	if (len > cu->max_len_changed_item)
		cu->max_len_changed_item = len;
	structure->priv = diff_info__new(class__tag(new_structure),
					 new_cu, diff);
}

static int diff_tag_iterator(struct tag *tag, struct cu *cu, void *new_cu)
{
	if (tag__is_struct(tag))
		diff_struct(new_cu, tag__class(tag), cu);
	else if (tag__is_function(tag))
		diff_function(new_cu, tag__function(tag), cu);

	return 0;
}

static int find_new_functions_iterator(struct tag *tfunction, struct cu *cu,
				       void *old_cu)
{
	struct function *function = tag__function(tfunction);
	struct tag *old_function;
	const char *name;

	if (function->inlined)
		return 0;

	name = function__name(function, cu);
	old_function = cu__find_function_by_name(old_cu, name);

	if (old_function == NULL || tag__function(old_function)->inlined) {
		const size_t len = strlen(name);
		const int32_t diff = function__size(function);

		if (len > cu->max_len_changed_item)
			cu->max_len_changed_item = len;
		++cu->nr_functions_changed;
		cu->function_bytes_added += diff;
		function->priv = diff_info__new(old_function, cu, diff);
	}

	return 0;
}

static int find_new_classes_iterator(struct tag *tag, struct cu *cu, void *old_cu)
{
	struct class *class;
	size_t len;

	if (!tag__is_struct(tag))
		return 0;

	class = tag__class(tag);
	if (class__name(class) == NULL)
		return 0;

	if (class__size(class) == 0)
		return 0;

	if (cu__find_struct_by_name(old_cu,
				    class__name(class), 0, NULL) != NULL)
		return 0;

	class->priv = diff_info__new(NULL, NULL, 1);
	++cu->nr_structures_changed;

	len = strlen(class__name(class)) + sizeof("struct");
	if (len > cu->max_len_changed_item)
		cu->max_len_changed_item = len;
	return 0;
}

static int find_new_tags_iterator(struct tag *tag, struct cu *cu, void *old_cu)
{
	if (tag__is_function(tag)) {
		/*
		 * We're not interested in aliases, just real function definitions,
		 * where we'll know if the kind of inlining
		 */
		if (tag__function(tag)->abstract_origin != 0)
			return 0;
		return find_new_functions_iterator(tag, cu, old_cu);
	}
	return find_new_classes_iterator(tag, cu, old_cu);
}

static int cu_find_new_tags_iterator(struct cu *new_cu, void *old_cus)
{
	struct cu *old_cu = cus__find_cu_by_name(old_cus, new_cu->name);

	if (old_cu != NULL && cu__same_build_id(old_cu, new_cu))
		return 0;

	cu__for_each_tag(new_cu, find_new_tags_iterator, old_cu, NULL);
	return 0;
}

static int cu_diff_iterator(struct cu *cu, void *new_cus)
{
	struct cu *new_cu = cus__find_cu_by_name(new_cus, cu->name);

	if (new_cu != NULL && cu__same_build_id(cu, new_cu))
		return 0;
	cu__for_each_tag(cu, diff_tag_iterator, new_cu, NULL);

	return 0;
}

static void show_diffs_function(struct function *function, const struct cu *cu,
				const void *cookie)
{
	const struct diff_info *di = function->priv;

	printf("  %-*.*s | %+4d",
	       (int)cu->max_len_changed_item, (int)cu->max_len_changed_item,
	       function__name(function, cu), di->diff);

	if (!verbose) {
		putchar('\n');
		return;
	}

	if (di->tag == NULL)
		puts(cookie ? " (added)" : " (removed)");
	else {
		struct function *twin = tag__function(di->tag);

		if (twin->inlined)
			puts(cookie ? " (uninlined)" : " (inlined)");
		else if (strcmp(function__name(function, cu),
				function__name(twin, di->cu)) != 0)
			printf("%s: BRAIN FART ALERT: comparing %s to %s, "
			       "should be the same name\n", __FUNCTION__,
			       function__name(function, cu),
			       function__name(twin, di->cu));
		else {
			char proto[1024], twin_proto[1024];

			printf(" # %zd -> %zd", function__size(function),
			       function__size(twin));
			if (function->lexblock.nr_lexblocks !=
			    twin->lexblock.nr_lexblocks)
				printf(", lexblocks: %d -> %d",
				       function->lexblock.nr_lexblocks,
				       twin->lexblock.nr_lexblocks);
			if (function->lexblock.nr_inline_expansions !=
			    twin->lexblock.nr_inline_expansions)
				printf(", # inlines: %d -> %d",
				       function->lexblock.nr_inline_expansions,
				       twin->lexblock.nr_inline_expansions);
			if (function->lexblock.size_inline_expansions !=
			    twin->lexblock.size_inline_expansions)
				printf(", size inlines: %zd -> %zd",
				       function->lexblock.size_inline_expansions,
				       twin->lexblock.size_inline_expansions);

			if (strcmp(function__prototype(function, cu,
					     proto, sizeof(proto)),
				   function__prototype(twin, di->cu,
				   	     twin_proto, sizeof(twin_proto))) != 0)
				printf(", prototype: %s -> %s", proto, twin_proto);
			putchar('\n');
		}
	}
}

static void show_changed_member(char change, const struct class_member *member,
				const struct cu *cu)
{
	const struct tag *type = cu__find_type_by_id(cu, member->tag.type);
	char bf[128];

	tag__assert_search_result(type);
	printf("    %c%-26s %-21s /* %5u %5zd */\n",
	       change, tag__name(type, cu, bf, sizeof(bf)),
	       class_member__name(member),
	       member->offset, tag__size(type, cu));
}

static void show_nr_members_changes(const struct class *structure,
				    const struct cu *cu,
				    const struct class *new_structure,
				    const struct cu *new_cu)
{
	struct class_member *member;

	/* Find the removed ones */
	type__for_each_member(&structure->type, member) {
		struct class_member *twin =
			class__find_member_by_name(new_structure,
						   class_member__name(member));
		if (twin == NULL)
			show_changed_member('-', member, cu);
	}

	/* Find the new ones */
	type__for_each_member(&new_structure->type, member) {
		struct class_member *twin =
			class__find_member_by_name(structure,
						   class_member__name(member));
		if (twin == NULL)
			show_changed_member('+', member, new_cu);
	}
}

static void print_terse_type_changes(struct class *structure)
{
	const char *sep = "";

	printf("struct %s: ", class__name(structure));

	if (terse_type_changes & TCHANGEF__SIZE) {
		fputs("size", stdout);
		sep = ", ";
	}
	if (terse_type_changes & TCHANGEF__NR_MEMBERS) {
		printf("%snr_members", sep);
		sep = ", ";
	}
	if (terse_type_changes & TCHANGEF__TYPE) {
		printf("%stype", sep);
		sep = ", ";
	}
	if (terse_type_changes & TCHANGEF__OFFSET) {
		printf("%soffset", sep);
		sep = ", ";
	}
	if (terse_type_changes & TCHANGEF__BIT_OFFSET) {
		printf("%sbit_offset", sep);
		sep = ", ";
	}
	if (terse_type_changes & TCHANGEF__BIT_SIZE) {
		printf("%sbit_size", sep);
		sep = ", ";
	}
	if (terse_type_changes & TCHANGEF__PADDING) {
		printf("%spadding", sep);
		sep = ", ";
	}
	if (terse_type_changes & TCHANGEF__NR_HOLES) {
		printf("%snr_holes", sep);
		sep = ", ";
	}
	if (terse_type_changes & TCHANGEF__NR_BIT_HOLES)
		printf("%snr_bit_holes", sep);

	putchar('\n');
}

static void show_diffs_structure(struct class *structure,
				 const struct cu *cu)
{
	const struct diff_info *di = structure->priv;
	const struct class *new_structure;
	int diff;
	/*
	 * This is when the struct was not present in the new object file.
	 * Meaning that it either was not referenced or that it was completely
	 * removed.
	 */
	if (di == NULL)
		return;

	new_structure = tag__class(di->tag);
	/*
	 * If there is a diff_info but its di->tag is NULL we have a new structure,
	 * one that didn't appears in the old object. See find_new_classes_iterator.
	 */
	if (new_structure == NULL)
		diff = class__size(structure);
	else
		diff = class__size(new_structure) - class__size(structure);

	terse_type_changes = 0;

	if (!show_terse_type_changes)
		printf("  struct %-*.*s | %+4d\n",
		       (int)(cu->max_len_changed_item - sizeof("struct")),
		       (int)(cu->max_len_changed_item - sizeof("struct")),
		       class__name(structure), diff);

	if (diff != 0)
		terse_type_changes |= TCHANGEF__SIZE;

	if (!verbose && !show_terse_type_changes)
		return;

	if (new_structure == NULL)
		diff = -class__nr_members(structure);
	else
		diff = (class__nr_members(new_structure) -
		        class__nr_members(structure));
	if (diff != 0) {
		terse_type_changes |= TCHANGEF__NR_MEMBERS;
		if (!show_terse_type_changes) {
			printf("   nr_members: %+d\n", diff);
			if (new_structure != NULL)
				show_nr_members_changes(structure, cu,
							new_structure, di->cu);
		}
	}
	if (new_structure != NULL) {
		diff = (int)new_structure->padding - (int)structure->padding;
		if (diff) {
			terse_type_changes |= TCHANGEF__PADDING;
			if (!show_terse_type_changes)
				printf("   padding: %+d\n", diff);
		}
		diff = (int)new_structure->nr_holes - (int)structure->nr_holes;
		if (diff) {
			terse_type_changes |= TCHANGEF__NR_HOLES;
			if (!show_terse_type_changes)
				printf("   nr_holes: %+d\n", diff);
		}
		diff = ((int)new_structure->nr_bit_holes -
			(int)structure->nr_bit_holes);
		if (structure->nr_bit_holes != new_structure->nr_bit_holes) {
			terse_type_changes |= TCHANGEF__NR_BIT_HOLES;
			if (!show_terse_type_changes)
				printf("   nr_bit_holes: %+d\n", diff);
		}
		check_print_members_changes(structure, cu,
					    new_structure, di->cu, 1);
	}
	if (show_terse_type_changes)
		print_terse_type_changes(structure);
}

static int show_function_diffs_iterator(struct tag *tag, struct cu *cu,
					void *cookie)
{
	struct function *function = tag__function(tag);

	if (tag__is_function(tag) && function->priv != NULL)
		show_diffs_function(function, cu, cookie);
	return 0;
}

static int show_structure_diffs_iterator(struct tag *tag, struct cu *cu,
					 void *cookie __unused)
{
	struct class *class;

	if (!tag__is_struct(tag))
		return 0;

	class = tag__class(tag);
	if (class->priv != NULL) {
		const char *name = class__name(class);
		if (!strlist__has_entry(structs_printed, name)) {
			show_diffs_structure(class, cu);
			strlist__add(structs_printed, name);
		}
	}
	return 0;
}

static int cu_show_diffs_iterator(struct cu *cu, void *cookie)
{
	static int first_cu_printed;

	if (cu->nr_functions_changed == 0 &&
	    cu->nr_structures_changed == 0)
		return 0;

	if (first_cu_printed)
		putchar('\n');
	else
		first_cu_printed = 1;

	++total_cus_changed;

	printf("%s:\n", cu->name);

	if (show_terse_type_changes) {
		cu__for_each_tag(cu, show_structure_diffs_iterator,
				 NULL, NULL);
		return 0;
	}

	if (cu->nr_structures_changed != 0 && show_struct_diffs) {
		cu__for_each_tag(cu, show_structure_diffs_iterator,
				 NULL, NULL);
		printf(" %u struct%s changed\n", cu->nr_structures_changed,
		       cu->nr_structures_changed > 1 ? "s" : "");
	}

	if (cu->nr_functions_changed != 0 && show_function_diffs) {
		total_nr_functions_changed += cu->nr_functions_changed;

		cu__for_each_tag(cu, show_function_diffs_iterator, cookie, NULL);
		printf(" %u function%s changed", cu->nr_functions_changed,
		       cu->nr_functions_changed > 1 ? "s" : "");
		if (cu->function_bytes_added != 0) {
			total_function_bytes_added += cu->function_bytes_added;
			printf(", %zd bytes added", cu->function_bytes_added);
		}
		if (cu->function_bytes_removed != 0) {
			total_function_bytes_removed += cu->function_bytes_removed;
			printf(", %zd bytes removed",
			       cu->function_bytes_removed);
		}
		printf(", diff: %+zd",
		       cu->function_bytes_added - cu->function_bytes_removed);
		putchar('\n');
	}
	return 0;
}

static void print_total_function_diff(const char *filename)
{
	printf("\n%s:\n", filename);

	printf(" %u function%s changed", total_nr_functions_changed,
	       total_nr_functions_changed > 1 ? "s" : "");

	if (total_function_bytes_added != 0)
		printf(", %u bytes added", total_function_bytes_added);

	if (total_function_bytes_removed != 0)
		printf(", %u bytes removed", total_function_bytes_removed);
  
	printf(", diff: %+d",
	       (total_function_bytes_added -
	        total_function_bytes_removed));
	putchar('\n');
}

/* Name and version of program.  */
ARGP_PROGRAM_VERSION_HOOK_DEF = dwarves_print_version;

static const struct argp_option codiff__options[] = {
	{
		.key  = 's',
		.name = "structs",
		.doc  = "show struct diffs",
	},
	{
		.key  = 'f',
		.name = "functions",
		.doc  = "show function diffs",
	},
	{
		.key  = 't',
		.name = "terse_type_changes",
		.doc  = "show terse type changes",
	},
	{
		.key  = 'V',
		.name = "verbose",
		.doc  = "show diffs details",
	},
	{
		.name = NULL,
	}
};

static error_t codiff__options_parser(int key, char *arg __unused,
				      struct argp_state *state __unused)
{
	switch (key) {
	case 'f': show_function_diffs = 1;	break;
	case 's': show_struct_diffs = 1;	break;
	case 't': show_terse_type_changes = 1;	break;
	case 'V': verbose = 1;			break;
	default:  return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static const char codiff__args_doc[] = "OLD_FILE NEW_FILE";

static struct argp codiff__argp = {
	.options  = codiff__options,
	.parser	  = codiff__options_parser,
	.args_doc = codiff__args_doc,
};

int main(int argc, char *argv[])
{
	int remaining, err;
	struct cus *old_cus, *new_cus;
	char *old_filename, *new_filename;
	char *filenames[2];
	struct stat st;

	if (dwarves__init(0)) {
		fputs("codiff: insufficient memory\n", stderr);
		return EXIT_FAILURE;
	}

	if (argp_parse(&codiff__argp, argc, argv, 0, &remaining, NULL) ||
	    remaining < argc) {
		switch (argc - remaining) {
		case 2:	 old_filename = argv[remaining++];
			 new_filename = argv[remaining++]; break;
		case 1:
		default: goto failure;
		}
	} else {
failure:
		argp_help(&codiff__argp, stderr, ARGP_HELP_SEE, argv[0]);
		return EXIT_FAILURE;
	}

	if (show_function_diffs == 0 && show_struct_diffs == 0 &&
	    show_terse_type_changes == 0)
		show_function_diffs = show_struct_diffs = 1;

	structs_printed = strlist__new(false);
	old_cus = cus__new();
	new_cus = cus__new();
	if (old_cus == NULL || new_cus == NULL || structs_printed == NULL) {
		fputs("codiff: insufficient memory\n", stderr);
		return EXIT_FAILURE;
	}

	if (stat(old_filename, &st) != 0) {
		fprintf(stderr, "codiff: %s (%s)\n", strerror(errno), old_filename);
		return EXIT_FAILURE;
	}

	filenames[1] = NULL;

	/* If old_file is a character device, leave its cus empty */
	if (!S_ISCHR(st.st_mode)) {
		filenames[0] = old_filename;
		err = cus__loadfl(old_cus, filenames);
		if (err != 0) {
			cus__print_error_msg("codiff", old_cus, old_filename, err);
			return EXIT_FAILURE;
		}
	}

	if (stat(new_filename, &st) != 0) {
		fprintf(stderr, "codiff: %s (%s)\n", strerror(errno), new_filename);
		return EXIT_FAILURE;
	}

	/* If old_file is a character device, leave its cus empty */
	if (!S_ISCHR(st.st_mode)) {
		filenames[0] = new_filename;
		err = cus__loadfl(new_cus, filenames);
		if (err != 0) {
			cus__print_error_msg("codiff", new_cus, new_filename, err);
			return EXIT_FAILURE;
		}
	}

	cus__for_each_cu(old_cus, cu_diff_iterator, new_cus, NULL);
	cus__for_each_cu(new_cus, cu_find_new_tags_iterator, old_cus, NULL);
	cus__for_each_cu(old_cus, cu_show_diffs_iterator, NULL, NULL);
	cus__for_each_cu(new_cus, cu_show_diffs_iterator, (void *)1, NULL);

	if (total_cus_changed > 1) {
		if (show_function_diffs)
			print_total_function_diff(new_filename);
	}

	return EXIT_SUCCESS;
}
