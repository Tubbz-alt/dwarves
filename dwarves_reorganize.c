/*
  Copyright (C) 2006 Mandriva Conectiva S.A.
  Copyright (C) 2006 Arnaldo Carvalho de Melo <acme@mandriva.com>
  Copyright (C) 2007 Red Hat Inc.
  Copyright (C) 2007 Arnaldo Carvalho de Melo <acme@redhat.com>

  This program is free software; you can redistribute it and/or modify it
  under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.
*/

#include "list.h"
#include "dwarves_reorganize.h"
#include "dwarves.h"

void class__subtract_offsets_from(struct class *self, const struct cu *cu,
				  struct class_member *from,
				  const uint16_t size)
{
	struct class_member *member =
		list_prepare_entry(from, class__tags(self), tag.node);

	list_for_each_entry_continue(member, class__tags(self), tag.node)
		if (member->tag.tag == DW_TAG_member)
			member->offset -= size;

	if (self->padding != 0) {
		struct class_member *last_member =
					type__last_member(&self->type);
		const size_t last_member_size =
			class_member__size(last_member, cu);
		const ssize_t new_padding =
			(class__size(self) -
			 (last_member->offset + last_member_size));
		if (new_padding > 0)
			self->padding = new_padding;
		else
			self->padding = 0;
	}
}

void class__add_offsets_from(struct class *self, struct class_member *from,
			     const uint16_t size)
{
	struct class_member *member =
		list_prepare_entry(from, class__tags(self), tag.node);

	list_for_each_entry_continue(member, class__tags(self), tag.node)
		if (member->tag.tag == DW_TAG_member)
			member->offset += size;
}

/*
 * XXX: Check this more thoroughly. Right now it is used because I was
 * to lazy to do class__remove_member properly, adjusting alignments and
 * holes as we go removing fields. Ditto for class__add_offsets_from.
 */
void class__fixup_alignment(struct class *self, const struct cu *cu)
{
	struct class_member *pos, *last_member = NULL;
	size_t member_size;
	size_t power2;

	type__for_each_data_member(&self->type, pos) {
		member_size = class_member__size(pos, cu);

		if (last_member == NULL && pos->offset != 0) { /* paranoid! */
			class__subtract_offsets_from(self, cu, pos,
						     pos->offset - member_size);
			pos->offset = 0;
		} else if (last_member != NULL &&
			   last_member->hole >= cu->addr_size) {
			size_t dec = (last_member->hole / cu->addr_size) *
				     cu->addr_size;

			last_member->hole -= dec;
			if (last_member->hole == 0)
				--self->nr_holes;
			pos->offset -= dec;
			self->type.size -= dec;
			class__subtract_offsets_from(self, cu, pos, dec);
		} else for (power2 = cu->addr_size; power2 >= 2; power2 /= 2) {
			const size_t remainder = pos->offset % power2;

			if (member_size == power2) {
				if (remainder == 0) /* perfectly aligned */
					break;
				if (last_member->hole >= remainder) {
					last_member->hole -= remainder;
					if (last_member->hole == 0)
						--self->nr_holes;
					pos->offset -= remainder;
					class__subtract_offsets_from(self, cu, pos, remainder);
				} else {
					const size_t inc = power2 - remainder;

					if (last_member->hole == 0)
						++self->nr_holes;
					last_member->hole += inc;
					pos->offset += inc;
					self->type.size += inc;
					class__add_offsets_from(self, pos, inc);
				}
			}
		}
		 	
		last_member = pos;
	}

	if (last_member != NULL) {
		struct class_member *m =
		 type__find_first_biggest_size_base_type_member(&self->type, cu);
		size_t unpadded_size = last_member->offset + class_member__size(last_member, cu);
		size_t m_size = class_member__size(m, cu), remainder;

		/* google for struct zone_padding in the linux kernel for an example */
		if (m_size == 0)
			return;

		remainder = unpadded_size % m_size;
		if (remainder != 0) {
			self->padding = m_size - remainder;
			self->type.size = unpadded_size + self->padding;
		}
	}
}

static struct class_member *
	class__find_next_hole_of_size(struct class *class,
				      struct class_member *from,
				      const struct cu *cu, size_t size)
{
	struct class_member *member =
		list_prepare_entry(from, class__tags(class), tag.node);
	struct class_member *bitfield_head = NULL;

	list_for_each_entry_continue(member, class__tags(class), tag.node) {
		if (member->tag.tag != DW_TAG_member)
			continue;
		if (member->bit_size != 0) {
			if (bitfield_head == NULL)
				bitfield_head = member;
		} else
			bitfield_head = NULL;
		if (member->hole != 0) {
			const size_t member_size = class_member__size(member, cu);
			
			if (member_size != 0 && member_size <= size)
				return bitfield_head ? : member;
		}
	}

	return NULL;
}

static struct class_member *
	class__find_last_member_of_size(struct class *class,
					struct class_member *to,
					const struct cu *cu, size_t size)
{
	struct class_member *member;

	list_for_each_entry_reverse(member, class__tags(class), tag.node) {
		size_t member_size;

		if (member->tag.tag != DW_TAG_member)
			continue;

		if (member == to)
			break;
		/*
		 * Check if this is the first member of a bitfield.  It either
		 * has another member before it that is not part of the current
		 * bitfield or it is the first member of the struct.
		 */
		if (member->bit_size != 0 && member->offset != 0) {
			struct class_member *prev =
					list_entry(member->tag.node.prev,
						   struct class_member,
						   tag.node);
			if (prev->bit_size != 0)
				continue;

		}

		member_size = class_member__size(member, cu);
		if (member_size != 0 && member_size <= size)
			return member;
	}

	return NULL;
}

static struct class_member *
	class__find_next_bit_hole_of_size(struct class *class,
					  struct class_member *from,
					  size_t size)
{
	struct class_member *member =
		list_prepare_entry(from, class__tags(class), tag.node);

	list_for_each_entry_continue(member, class__tags(class), tag.node) {
		if (member->tag.tag != DW_TAG_member)
			continue;
		if (member->bit_hole != 0 &&
		    member->bit_size <= size)
		    return member;
	}
#if 0
	/*
	 * FIXME: Handle the case where the bit padding is on the same bitfield
	 * that we're looking, i.e. we can't combine a bitfield with itself,
	 * perhaps we should tag bitfields with a sequential, clearly marking
	 * each of the bitfields in advance, so that all the algoriths that
	 * have to deal with bitfields, moving them around, demoting, etc, can
	 * be simplified.
	 */
	/*
	 * Now look if the last member is a one member bitfield,
	 * i.e. if we have bit_padding
	 */
	if (class->bit_padding != 0)
		return type__last_member(&class->type);
#endif
	return NULL;
}

static void class__move_member(struct class *class, struct class_member *dest,
			       struct class_member *from, const struct cu *cu,
			       int from_padding, const int verbose, FILE *fp)
{
	const size_t from_size = class_member__size(from, cu);
	const size_t dest_size = class_member__size(dest, cu);
	const bool from_was_last = from->tag.node.next == class__tags(class);
	struct class_member *tail_from = from;
	struct class_member *from_prev = list_entry(from->tag.node.prev,
						    struct class_member,
						    tag.node);
	uint16_t orig_tail_from_hole = tail_from->hole;
	const uint16_t orig_from_offset = from->offset;
	/*
	 * Align 'from' after 'dest':
	 */
	const uint16_t offset = dest->hole % (from_size > cu->addr_size ?
						cu->addr_size : from_size);
	/*
	 * Set new 'from' offset, after 'dest->offset', aligned
	 */
	const uint16_t new_from_offset = dest->offset + dest_size + offset;

	if (verbose)
		fputs("/* Moving", fp);

	if (from->bit_size != 0) {
		struct class_member *pos =
				list_prepare_entry(from, class__tags(class),
						   tag.node);
		struct class_member *tmp;
		uint8_t orig_tail_from_bit_hole;
		LIST_HEAD(from_list);

		if (verbose)
			fprintf(fp, " bitfield('%s' ... ", from->name);
		list_for_each_entry_safe_from(pos, tmp, class__tags(class),
					      tag.node) {
			if (pos->tag.tag != DW_TAG_member)
				continue;
			/*
			 * Have we reached the end of the bitfield?
			 */
			if (pos->offset != orig_from_offset)
				break;
			tail_from = pos;
			orig_tail_from_hole = tail_from->hole;
			orig_tail_from_bit_hole = tail_from->bit_hole;
			pos->offset = new_from_offset;
			pos->hole = 0;
			pos->bit_hole = 0;
			list_move_tail(&pos->tag.node, &from_list);
		}
		tail_from->bit_hole = orig_tail_from_bit_hole;
		list_splice(&from_list, &dest->tag.node);
		if (verbose)
			fprintf(fp, "'%s')", tail_from->name);
	} else {
		if (verbose)
			fprintf(fp, " '%s'", from->name);
		/*
		 *  Remove 'from' from the list
		 */
		list_del(&from->tag.node);
		/*
		 * Add 'from' after 'dest':
		 */
		__list_add(&from->tag.node, &dest->tag.node,
			   dest->tag.node.next);
		from->offset = new_from_offset;
	}
		
	if (verbose)
		fprintf(fp, " from after '%s' to after '%s' */\n",
		       from_prev->name, dest->name);

	if (from_padding) {
		/*
		 * Check if we're eliminating the need for padding:
		 */
		if (orig_from_offset % cu->addr_size == 0) {
			/*
			 * Good, no need for padding anymore:
			 */
			class->type.size -= from_size + class->padding;
			class->padding = 0;
		} else {
			/*
			 * No, so just add from_size to the padding:
			 */
			class->padding += from_size;
			if (verbose)
				fprintf(fp, "/* adding %zd bytes from %s to "
					"the padding */\n",
					from_size, from->name);
		}
	} else if (from_was_last) {
		class->type.size -= from_size + class->padding;
		class->padding = 0;
	} else {
		/*
		 * See if we are adding a new hole that is bigger than
		 * sizeof(long), this may have problems with explicit alignment
		 * made by the programmer, perhaps we need A switch that allows
		 * us to avoid realignment, just using existing holes but
		 * keeping the existing alignment, anyway the programmer has to
		 * check the resulting rerganization before using it, and for
		 * automatic stuff such as the one that will be used for struct
		 * "views" in tools such as ctracer we are more interested in
		 * packing the subset as tightly as possible.
		 */
		if (orig_tail_from_hole + from_size >= cu->addr_size) {
			class->type.size -= cu->addr_size;
			class__subtract_offsets_from(class, cu, from_prev,
						     cu->addr_size);
		} else {
			/*
			 * Add the hole after 'from' + its size to the member
			 * before it:
			 */
			from_prev->hole += orig_tail_from_hole + from_size;
		}
		/*
		 * Check if we have eliminated a hole
		 */
		if (dest->hole == from_size)
			class->nr_holes--;
	}

	tail_from->hole = dest->hole - (from_size + offset);
	dest->hole = offset;

	if (verbose > 1) {
		class__find_holes(class, cu);
		class__fprintf(class, cu, NULL, fp);
		fputc('\n', fp);
	}
}

static void class__move_bit_member(struct class *class, const struct cu *cu,
				   struct class_member *dest,
				   struct class_member *from,
				   const int verbose, FILE *fp)
{
	struct class_member *from_prev = list_entry(from->tag.node.prev,
						    struct class_member,
						    tag.node);
	const uint8_t is_last_member = (from->tag.node.next ==
					class__tags(class));

	if (verbose)
		fprintf(fp, "/* Moving '%s:%u' from after '%s' to "
			"after '%s:%u' */\n",
			from->name, from->bit_size, from_prev->name,
			dest->name, dest->bit_size);
	/*
	 *  Remove 'from' from the list
	 */
	list_del(&from->tag.node);
	/*
	 * Add from after dest:
	 */
	__list_add(&from->tag.node,
		   &dest->tag.node,
		   dest->tag.node.next);

	/* Check if this was the last entry in the bitfield */
	if (from_prev->bit_size == 0) {
		size_t from_size = class_member__size(from, cu);
		/*
		 * Are we shrinking the struct?
		 */
		if (from_size + from->hole >= cu->addr_size) {
			class->type.size -= from_size + from->hole;
			class__subtract_offsets_from(class, cu, from_prev,
						     from_size + from->hole);
		} else if (is_last_member)
			class->padding += from_size;
		else
			from_prev->hole += from_size + from->hole;
		if (is_last_member) {
			/*
			 * Now we don't have bit_padding anymore
			 */
			class->bit_padding = 0;
		} else
			class->nr_bit_holes--;
	} else {
		/*
		 * Add add the holes after from + its size to the member
		 * before it:
		 */
		from_prev->bit_hole += from->bit_hole + from->bit_size;
		from_prev->hole = from->hole;
	}
	from->bit_hole = dest->bit_hole - from->bit_size;
	/*
	 * Tricky, what are the rules for bitfield layouts on this arch?
	 * Assume its IA32
	 */
	from->bit_offset = dest->bit_offset + dest->bit_size;
	/*
	 * Now both have the some offset:
	 */
	from->offset = dest->offset;
	dest->bit_hole = 0;
	from->hole = dest->hole;
	dest->hole = 0;
	if (verbose > 1) {
		class__find_holes(class, cu);
		class__fprintf(class, cu, NULL, fp);
		fputc('\n', fp);
	}
}

static void class__demote_bitfield_members(struct class *class,
					   struct class_member *from,
					   struct class_member *to,
					   const struct base_type *old_type,
					   const struct base_type *new_type)
{
	const uint8_t bit_diff = (old_type->size - new_type->size) * 8;
	struct class_member *member =
		list_prepare_entry(from, class__tags(class), tag.node);

	list_for_each_entry_from(member, class__tags(class), tag.node) {
		if (member->tag.tag != DW_TAG_member)
			continue;
		/*
		 * Assume IA32 bitfield layout
		 */
		member->bit_offset -= bit_diff;
		member->tag.type = new_type->tag.id;
		if (member == to)
			break;
		member->bit_hole = 0;
	}
}

static struct tag *cu__find_base_type_of_size(const struct cu *cu,
					      const size_t size)
{
	const char *type_name;

	switch (size) {
	case sizeof(unsigned char):
		type_name = "unsigned char"; break;
	case sizeof(unsigned short int):
		type_name = "short unsigned int"; break;
	case sizeof(unsigned int):
		type_name = "unsigned int"; break;
	case sizeof(unsigned long long):
		if (cu->addr_size == 8)
			type_name = "long unsigned int";
		else
			type_name = "long long unsigned int";
		break;
	default:
		return NULL;
	}

	return cu__find_base_type_by_name(cu, type_name);
}

static int class__demote_bitfields(struct class *class, const struct cu *cu,
				   const int verbose, FILE *fp)
{
	struct class_member *member;
	struct class_member *bitfield_head = NULL;
	const struct tag *old_type_tag, *new_type_tag;
	size_t current_bitfield_size = 0, size, bytes_needed, new_size;
	int some_was_demoted = 0;

	type__for_each_data_member(&class->type, member) {
		/*
		 * Check if we are moving away from a bitfield
		 */
		if (member->bit_size == 0) {
			current_bitfield_size = 0;
			bitfield_head = NULL;
		} else {
			if (bitfield_head == NULL) {
				bitfield_head = member;
				current_bitfield_size = member->bit_size;
			} else if (bitfield_head->offset != member->offset) {
				/*
				 * We moved from one bitfield to another, for
				 * now don't handle this case, just move on to
				 * the next bitfield, we may well move it to
				 * another place and then the first bitfield will
				 * be isolated and will be handled in the next
				 * pass.
				 */
				bitfield_head = member;
				current_bitfield_size = member->bit_size;
			} else
				current_bitfield_size += member->bit_size;
		}

		/*
		 * Have we got to the end of a bitfield with holes?
		 */
		if (member->bit_hole == 0)
			continue;

		size = class_member__size(member, cu);
	    	bytes_needed = (current_bitfield_size + 7) / 8;
		if (bytes_needed == size)
			continue;

		old_type_tag = cu__find_tag_by_id(cu, member->tag.type);
		new_type_tag = cu__find_base_type_of_size(cu, bytes_needed);

		if (new_type_tag == NULL) {
			fprintf(fp, "/* BRAIN FART ALERT! couldn't find a "
				    "%zd bytes base type */\n\n", bytes_needed);
			continue;
		}
		if (verbose)
			fprintf(fp, "/* Demoting bitfield ('%s' ... '%s') "
				"from '%s' to '%s' */\n",
				bitfield_head->name, member->name,
				tag__base_type(old_type_tag)->name,
				tag__base_type(new_type_tag)->name);

		class__demote_bitfield_members(class,
					       bitfield_head, member,	
					       tag__base_type(old_type_tag),
					       tag__base_type(new_type_tag));
		new_size = class_member__size(member, cu);
		member->hole = size - new_size;
		if (member->hole != 0)
			++class->nr_holes;
		member->bit_hole = new_size * 8 - current_bitfield_size;
		some_was_demoted = 1;
		/*
		 * Have we packed it so that there are no hole now?
		*/
		if (member->bit_hole == 0)
			--class->nr_bit_holes;
		if (verbose > 1) {
			class__find_holes(class, cu);
			class__fprintf(class, cu, NULL, fp);
			fputc('\n', fp);
		}
	}
	/*
	 * Now look if we have bit padding, i.e. if the the last member
	 * is a bitfield and its the sole member in this bitfield, i.e.
	 * if it wasn't already demoted as part of a bitfield of more than
	 * one member:
	 */
	member = type__last_member(&class->type);
	if (class->bit_padding != 0 && bitfield_head == member) {
		size = class_member__size(member, cu);
	    	bytes_needed = (member->bit_size + 7) / 8;
		if (bytes_needed < size) {
			old_type_tag =
				cu__find_tag_by_id(cu, member->tag.type);
			new_type_tag =
				cu__find_base_type_of_size(cu, bytes_needed);

			if (verbose)
				fprintf(fp, "/* Demoting bitfield ('%s') "
					"from '%s' to '%s' */\n",
					member->name,
					tag__base_type(old_type_tag)->name,
					tag__base_type(new_type_tag)->name);
			class__demote_bitfield_members(class,
						       member, member,	
						 tag__base_type(old_type_tag),
						 tag__base_type(new_type_tag));
			new_size = class_member__size(member, cu);
			member->hole = 0;
			/*
			 * Do we need byte padding?
			 */
			if (member->offset + new_size < class__size(class)) {
				class->padding = (class__size(class) -
						  (member->offset + new_size));
				class->bit_padding = 0;
				member->bit_hole = (new_size * 8 -
						    member->bit_size);
			} else {
				class->padding = 0;
				class->bit_padding = (new_size * 8 -
						      member->bit_size);
				member->bit_hole = 0;
			}
			some_was_demoted = 1;
			if (verbose > 1) {
				class__find_holes(class, cu);
				class__fprintf(class, cu, NULL, fp);
				fputc('\n', fp);
			}
		}
	}

	return some_was_demoted;
}

static void class__reorganize_bitfields(struct class *class,
					const struct cu *cu,
					const int verbose, FILE *fp)
{
	struct class_member *member, *brother;
restart:
	type__for_each_data_member(&class->type, member) {
		/* See if we have a hole after this member */
		if (member->bit_hole != 0) {
			/*
			 * OK, try to find a member that has a bit hole after
			 * it and that has a size that fits the current hole:
			*/
			brother =
			   class__find_next_bit_hole_of_size(class, member,
							     member->bit_hole);
			if (brother != NULL) {
				class__move_bit_member(class, cu,
						       member, brother,
						       verbose, fp);
				goto restart;
			}
		}
	}
}

static void class__fixup_bitfield_types(struct class *self,
					struct class_member *from,
					struct class_member *to_before,
					Dwarf_Off type)
{
	struct class_member *member =
		list_prepare_entry(from, class__tags(self), tag.node);

	list_for_each_entry_from(member, class__tags(self), tag.node) {
		if (member->tag.tag != DW_TAG_member)
			continue;
		if (member == to_before)
			break;
		member->tag.type = type;
	}
}

/*
 * Think about this pahole output a bit:
 *
 * [filo examples]$ pahole swiss_cheese cheese
 * / * <11b> /home/acme/git/pahole/examples/swiss_cheese.c:3 * /
 * struct cheese {
 * <SNIP>
 *       int         bitfield1:1;   / * 64 4 * /
 *       int         bitfield2:1;   / * 64 4 * /
 *
 *       / * XXX 14 bits hole, try to pack * /
 *       / * Bitfield WARNING: DWARF size=4, real size=2 * /
 *
 *       short int   d;             / * 66 2 * /
 * <SNIP>
 * 
 * The compiler (gcc 4.1.1 20070105 (Red Hat 4.1.1-51) in the above example),
 * Decided to combine what was declared as an int (4 bytes) bitfield but doesn't
 * uses even one byte with the next field, that is a short int (2 bytes),
 * without demoting the type of the bitfield to short int (2 bytes), so in terms
 * of alignment the real size is 2, not 4, to make things easier for the rest of
 * the reorganizing routines we just do the demotion ourselves, fixing up the
 * sizes.
*/
static void class__fixup_member_types(struct class *self, const struct cu *cu,
				      const uint8_t verbose, FILE *fp)
{
	struct class_member *pos, *bitfield_head = NULL;
	uint8_t fixup_was_done = 0;

	type__for_each_data_member(&self->type, pos) {
		/*
		 * Is this bitfield member?
		 */
		if (pos->bit_size != 0) {
			/*
			 * The first entry in a bitfield?
			 */
			if (bitfield_head == NULL)
				bitfield_head = pos;
			continue;
		}
		/*
		 * OK, not a bitfield member, but have we just passed
		 * by a bitfield?
		 */
		if (bitfield_head != NULL) {
			const uint16_t real_size = (pos->offset -
						  bitfield_head->offset);
			const size_t size = class_member__size(bitfield_head,
							       cu);
			if (real_size != size) {
				struct tag *new_type_tag =
					cu__find_base_type_of_size(cu,
								   real_size);
				if (new_type_tag == NULL) {
					fprintf(stderr, "pahole: couldn't find"
						" a base_type of %d bytes!\n",
						real_size);
					continue;
				}
				class__fixup_bitfield_types(self,
							    bitfield_head, pos,
							    new_type_tag->id);
				fixup_was_done = 1;
			}
		}
		bitfield_head = NULL;
	}
	if (verbose && fixup_was_done) {
		fprintf(fp, "/* bitfield types were fixed */\n");
		if (verbose > 1) {
			class__find_holes(self, cu);
			class__fprintf(self, cu, NULL, fp);
			fputc('\n', fp);
		}
	}
}

void class__reorganize(struct class *self, const struct cu *cu,
		       const int verbose, FILE *fp)
{
	struct class_member *member, *brother, *last_member;
	size_t last_member_size;

	class__fixup_member_types(self, cu, verbose, fp);

	while (class__demote_bitfields(self, cu, verbose, fp))
		class__reorganize_bitfields(self, cu, verbose, fp);
	
	/* Now try to combine holes */
restart:
	class__find_holes(self, cu);
	/*
	 * It can be NULL if this class doesn't have any data members,
	 * just inheritance entries
	 */
	last_member = type__last_member(&self->type);
	if (last_member == NULL)
		return;

	last_member_size = class_member__size(last_member, cu);

	type__for_each_data_member(&self->type, member) {
		/* See if we have a hole after this member */
		if (member->hole != 0) {
			/*
			 * OK, try to find a member that has a hole after it
			 * and that has a size that fits the current hole:
			*/
			brother = class__find_next_hole_of_size(self, member,
								cu,
								member->hole);
			if (brother != NULL) {
				struct class_member *brother_prev =
					    list_entry(brother->tag.node.prev,
						       struct class_member,
						       tag.node);
				/*
				 * If it the next member, avoid moving it closer,
				 * it could be a explicit alignment rule, like
				 * ____cacheline_aligned_in_smp in the Linux
				 * kernel.
				 */
				if (brother_prev != member) {
					class__move_member(self, member,
							   brother, cu, 0,
							   verbose, fp);
					goto restart;
				}
			}
			/*
			 * OK, but is there padding? If so the last member
			 * has a hole, if we are not at the last member and
			 * it has a size that is smaller than the current hole
			 * we can move it after the current member, reducing
			 * the padding or eliminating it altogether.
			 */
			if (self->padding > 0 &&
			    member != last_member &&
			    last_member_size != 0 &&
			    last_member_size <= member->hole) {
				class__move_member(self, member, last_member,
						   cu, 1, verbose, fp);
				goto restart;
			}
		}
	}

	/* Now try to move members at the tail to after holes */
	if (self->nr_holes == 0)
		return;

	type__for_each_data_member(&self->type, member) {
		/* See if we have a hole after this member */
		if (member->hole != 0) {
			brother = class__find_last_member_of_size(self, member,
								  cu,
								  member->hole);
			if (brother != NULL) {
				struct class_member *brother_prev =
					    list_entry(brother->tag.node.prev,
						       struct class_member,
						       tag.node);
				/*
				 * If it the next member, avoid moving it closer,
				 * it could be a explicit alignment rule, like
				 * ____cacheline_aligned_in_smp in the Linux
				 * kernel.
				 */
				if (brother_prev != member) {
					class__move_member(self, member,
							   brother, cu, 0,
							   verbose, fp);
					goto restart;
				}
			}
		}
	}
}
